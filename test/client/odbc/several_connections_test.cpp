// Several connections in one process: results on different connections read at the same time each
// return their own rows and count their own held bytes, and a fetch held on one connection holds no
// fetch, NextBatch, Execute or connect on another.

// catch.hpp comes first, as in this directory's other tests.
#include "catch.hpp"

#include "duckdb/common/numeric_utils.hpp"
#include "fake_driver_fixture.hpp"
#include "odbc/bulk_fetch.hpp"

#include <algorithm>
#include <chrono>
#include <future>
#include <memory>
#include <string>
#include <vector>

using namespace duckdb;
using redshift_driver::testing::FakeColumn;
using redshift_driver::testing::FakeGate;
using redshift_driver::testing::FakeResultSet;
using redshift_test::ExecuteStatement;
using redshift_test::FakeDriverFixture;
using redshift_test::LongVarcharColumn;
using redshift_test::OneColumn;
using redshift_test::ReadBatches;
using redshift_test::ReadyWithin;

namespace {

//! Long enough, on a loaded machine, for a read that waits on no other connection.
constexpr std::chrono::milliseconds EXPECTED_WAIT {10000};
//! How long a case waits for a read it expects to stay held. Correct code never ends it, so a
//! short wait cannot fail spuriously.
constexpr std::chrono::milliseconds UNEXPECTED_WAIT {200};
//! A long cell's length: several of the client's SQLGetData parts, so a part buffer shared across
//! results would mix cells.
constexpr size_t LONG_CELL_BYTES = 9000;

//! One key range of a split query, as a user runs it on a connection of its own: bounded or long
//! cells, with or without parameters, so both fill paths and both ways of running take part.
struct Slice {
	std::string sql;
	bool long_column;
	duckdb::vector<std::string> params;
	size_t rows;

	//! The cell of row `row`, unique to this slice, so a row read on the wrong connection shows.
	std::string Cell(size_t index, size_t row) const {
		const auto key = std::to_string(index * 1000000 + row);
		return long_column ? key + std::string(LONG_CELL_BYTES, static_cast<char>('a' + row % 26)) : key;
	}
};

//! Slice 1 spans more sets than a ring holds, so its producer is still running when a case stops it.
const std::vector<Slice> SLICES {
    {"SELECT c FROM t WHERE k > $1 AND k <= $2", false, {"0", "100"}, 3 * MAX_BATCH_ROWS + 5},
    {"SELECT c FROM t WHERE k > 100", false, {}, 9 * MAX_BATCH_ROWS + 7},
    {"SELECT v FROM t WHERE k > $1 AND k <= $2", true, {"200", "300"}, 700},
    {"SELECT v FROM t WHERE k > 300", true, {}, 900}};

FakeColumn IntegerColumn() {
	FakeColumn column;
	column.name = "c";
	column.type_name = "INTEGER";
	column.sql_type = SQL_INTEGER;
	column.column_size = 10;
	return column;
}

//! Scripts every slice on the fake.
void ScriptSlices(FakeDriverFixture &fixture) {
	for (size_t index = 0; index < SLICES.size(); index++) {
		const auto &slice = SLICES[index];
		std::vector<std::string> cells;
		for (size_t row = 0; row < slice.rows; row++) {
			cells.push_back(slice.Cell(index, row));
		}
		const auto column = slice.long_column ? LongVarcharColumn("v") : IntegerColumn();
		const FakeResultSet result = OneColumn(column, cells);
		fixture.fake.AddResult(slice.sql, {result}, NumericCast<SQLSMALLINT>(slice.params.size()));
	}
}

//! Whether `rows` are exactly slice `index`'s rows, in order.
bool HoldsSlice(const std::vector<std::vector<RedshiftValue>> &rows, size_t index) {
	const auto &slice = SLICES[index];
	if (rows.size() != slice.rows) {
		return false;
	}
	for (size_t row = 0; row < slice.rows; row++) {
		const auto &value = rows[row].at(0);
		const auto cell = slice.long_column ? value.text : std::to_string(value.integer);
		if (cell != slice.Cell(index, row)) {
			return false;
		}
	}
	return true;
}

//! Whether `result`, read to its end after the rows already in `read_so_far`, gives slice `index`.
bool ReadsSlice(RedshiftResult &result, size_t index, std::vector<std::vector<RedshiftValue>> read_so_far = {}) {
	const ReadBatches rest(result);
	read_so_far.insert(read_so_far.end(), rest.rows.begin(), rest.rows.end());
	return HoldsSlice(read_so_far, index);
}

//! Reads `result` to its end on a thread of its own.
std::future<bool> ReadSliceAsync(RedshiftResult &result, size_t index,
                                 std::vector<std::vector<RedshiftValue>> read_so_far = {}) {
	return std::async(std::launch::async,
	                  [&result, index, read_so_far]() { return ReadsSlice(result, index, read_so_far); });
}

//! Runs slice `index` on `connection` and reads it to its end, on a thread of its own.
std::future<bool> RunSliceAsync(RedshiftConnection &connection, size_t index) {
	return std::async(std::launch::async, [&connection, index]() {
		auto result = ExecuteStatement(connection, SLICES[index].sql, SLICES[index].params);
		return ReadsSlice(*result, index);
	});
}

//! Whether every read finished within EXPECTED_WAIT with its slice's rows. Each is then waited for
//! without a bound, so no result is destroyed under its reader: a deadlock still hangs the case,
//! as in this directory's other threaded cases, and a slow read fails it.
bool AllReadTheirSlices(std::vector<std::future<bool>> &reads) {
	bool all = true;
	for (auto &read : reads) {
		all = ReadyWithin(read, EXPECTED_WAIT) && all;
	}
	for (auto &read : reads) {
		all = read.get() && all;
	}
	return all;
}

//! Releases a gate when the case leaves its scope, on every path. Declared after the futures whose
//! threads the gate can hold, so it is destroyed, and the gate released, before they are joined.
class ReleaseOnExit {
public:
	explicit ReleaseOnExit(std::shared_ptr<FakeGate> gate_p) : gate(std::move(gate_p)) {
	}
	~ReleaseOnExit() {
		gate->Release();
	}
	ReleaseOnExit(const ReleaseOnExit &) = delete;
	ReleaseOnExit &operator=(const ReleaseOnExit &) = delete;

private:
	std::shared_ptr<FakeGate> gate;
};

} // namespace

TEST_CASE("Results on four connections read at the same time each return their own rows in order", "[odbc]") {
	FakeDriverFixture fixture;
	ScriptSlices(fixture);
	const auto held_before = HeldFetchBytes();
	std::vector<unique_ptr<RedshiftConnection>> connections;
	std::vector<unique_ptr<RedshiftResult>> results;
	for (size_t index = 0; index < SLICES.size(); index++) {
		connections.push_back(fixture.OpenConnection());
		results.push_back(ExecuteStatement(*connections.back(), SLICES[index].sql, SLICES[index].params));
	}

	// Each read's first NextBatch starts its result's producer, so the four fill at the same time.
	std::vector<std::future<bool>> reads;
	for (size_t index = 0; index < SLICES.size(); index++) {
		reads.push_back(ReadSliceAsync(*results[index], index));
	}

	CHECK(AllReadTheirSlices(reads));
	results.clear();
	connections.clear();
	CHECK(HeldFetchBytes() == held_before);
}

TEST_CASE("Four results held at once count, together, what each counts alone", "[odbc]") {
	FakeDriverFixture fixture;
	// Each NextBatch fills on the caller's thread, so what a result holds after its first batch is
	// fixed, and the four can be compared with each alone.
	SetFetchThreadForTesting(FetchThread::CALLER);
	ScriptSlices(fixture);
	const auto held_before = HeldFetchBytes();
	std::vector<idx_t> alone;
	for (size_t index = 0; index < SLICES.size(); index++) {
		auto connection = fixture.OpenConnection();
		auto result = ExecuteStatement(*connection, SLICES[index].sql, SLICES[index].params);
		RedshiftBatch batch;
		CHECK(result->NextBatch(batch));
		alone.push_back(HeldFetchBytes() - held_before);
	}
	const bool alone_freed = HeldFetchBytes() == held_before;

	std::vector<unique_ptr<RedshiftConnection>> connections;
	std::vector<unique_ptr<RedshiftResult>> results;
	for (size_t index = 0; index < SLICES.size(); index++) {
		connections.push_back(fixture.OpenConnection());
		results.push_back(ExecuteStatement(*connections.back(), SLICES[index].sql, SLICES[index].params));
		RedshiftBatch batch;
		CHECK(results.back()->NextBatch(batch));
	}
	const auto together = HeldFetchBytes() - held_before;
	results.resize(1);
	const auto held_by_one = HeldFetchBytes() - held_before;
	results.clear();
	connections.clear();

	CHECK(alone_freed);
	idx_t sum = 0;
	for (const auto bytes : alone) {
		CHECK(bytes > 0);
		sum += bytes;
	}
	CHECK(together >= sum);
	CHECK(together > *std::max_element(alone.begin(), alone.end()));
	CHECK(held_by_one == alone[0]);
	CHECK(HeldFetchBytes() == held_before);
}

TEST_CASE("A fetch held on one connection holds no fetch, Execute or connect on another", "[odbc]") {
	// The held slice, bounded or long; the others run the remaining three.
	const size_t held_index = GENERATE(size_t(1), size_t(3));
	const size_t opened_index = held_index == 1 ? 3 : 1;
	FakeDriverFixture fixture;
	ScriptSlices(fixture);
	std::vector<unique_ptr<RedshiftConnection>> connections;
	for (size_t index = 0; index < 3; index++) {
		connections.push_back(fixture.OpenConnection());
	}
	unique_ptr<RedshiftConnection> opened_while_held;
	// A bounded set is one fetch and a long row is one, so this is the second bounded set or a long
	// row partway through: either way the reader waits in NextBatch for the set it is in.
	const auto &held_slice = SLICES[held_index];
	const size_t held_fetch = held_slice.long_column ? held_slice.rows / 2 : 2;
	auto gate = std::make_shared<FakeGate>();
	fixture.fake.BlockCall("SQLFetch", NumericCast<int>(fixture.CallsTo("SQLFetch") + held_fetch), gate);
	auto held_result = ExecuteStatement(*connections[0], held_slice.sql, held_slice.params);
	std::future<bool> held_read;
	std::vector<std::future<bool>> others;
	const ReleaseOnExit release(gate);

	held_read = ReadSliceAsync(*held_result, held_index);
	const bool held = gate->WaitUntilHeldFor(EXPECTED_WAIT);
	const bool held_read_ended = ReadyWithin(held_read, UNEXPECTED_WAIT);
	others.push_back(RunSliceAsync(*connections[1], 0));
	others.push_back(RunSliceAsync(*connections[2], 2));
	others.push_back(std::async(std::launch::async, [&]() {
		opened_while_held = fixture.OpenConnection();
		auto result = ExecuteStatement(*opened_while_held, SLICES[opened_index].sql, SLICES[opened_index].params);
		return ReadsSlice(*result, opened_index);
	}));
	// Read before the release, so only a read that waits on nothing held counts.
	std::vector<bool> done_while_held;
	for (auto &other : others) {
		done_while_held.push_back(ReadyWithin(other, EXPECTED_WAIT));
	}
	gate->Release();

	CHECK(held);
	CHECK_FALSE(held_read_ended);
	CHECK(done_while_held == std::vector<bool>(others.size(), true));
	CHECK(AllReadTheirSlices(others));
	std::vector<std::future<bool>> held_reads;
	held_reads.push_back(std::move(held_read));
	CHECK(AllReadTheirSlices(held_reads));
}

TEST_CASE("Destroying one connection's result before its end leaves another's read whole", "[odbc]") {
	FakeDriverFixture fixture;
	ScriptSlices(fixture);
	auto stopped = fixture.OpenConnection();
	auto reading = fixture.OpenConnection();
	auto stopped_result = ExecuteStatement(*stopped, SLICES[1].sql, SLICES[1].params);
	auto reading_result = ExecuteStatement(*reading, SLICES[3].sql, SLICES[3].params);
	RedshiftBatch stopped_batch;
	CHECK(stopped_result->NextBatch(stopped_batch));
	RedshiftBatch reading_batch;
	CHECK(reading_result->NextBatch(reading_batch));

	// Stops and joins a producer short of the end of its rows, which must leave the other's running.
	stopped_result.reset();
	std::vector<std::future<bool>> reads;
	reads.push_back(ReadSliceAsync(*reading_result, 3, {reading_batch.rows.begin(), reading_batch.rows.end()}));

	CHECK(AllReadTheirSlices(reads));
	auto again = ExecuteStatement(*stopped, SLICES[1].sql, SLICES[1].params);
	CHECK(ReadsSlice(*again, 1));
}
