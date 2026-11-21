// The producer thread: it makes the fetches, cell reads and end-of-rows drain after Execute;
// destroying the result or the connection stops and joins it before a handle is freed; a failure it
// stores is raised by NextBatch, which asks the connection-dead question on the caller's thread; and
// a result whose producer has read the end of the rows stays open until its caller has read every
// set or destroys it.

// catch.hpp comes first, as in this directory's other tests.
#include "catch.hpp"

#include "call_trace.hpp"
#include "fake_driver_fixture.hpp"
#include "odbc/bulk_fetch.hpp"
#include "odbc/fetch_producer.hpp"
#include "thrown.hpp"

#include <algorithm>
#include <chrono>
#include <functional>
#include <future>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

using namespace duckdb;
using redshift_driver::testing::FakeColumn;
using redshift_driver::testing::FakeDiag;
using redshift_driver::testing::FakeGate;
using redshift_driver::testing::FakeOdbcDriver;
using redshift_driver::testing::FakeResultSet;
using redshift_test::CallTrace;
using redshift_test::FakeDriverFixture;
using redshift_test::ReadyWithin;
using redshift_test::StatementFreedWithin;
using redshift_test::Thrown;

namespace {

//! Long enough for a held call to arrive, or a join to finish, on a loaded machine.
constexpr std::chrono::milliseconds EXPECTED_WAIT {10000};
//! How long a case waits to see that a teardown is held in its join, long enough for it to have set
//! the stop flag first.
constexpr std::chrono::milliseconds HELD_WAIT {1000};

const char *const QUERY = "SELECT c FROM t";
const char *const LONG_QUERY = "SELECT v FROM t";
//! The rows of one bound set of an INTEGER or short VARCHAR column, MAX_BATCH_ROWS of them.
constexpr size_t SET_ROWS = MAX_BATCH_ROWS;

//! The fake driver behind CallTrace, so a case sees which thread made each call.
class ThreadFixture : public FakeDriverFixture {
public:
	ThreadFixture() {
		SetDriverSourceForTesting(&CallTrace::Source);
		CallTrace::Start(&FakeOdbcDriver::Source);
	}
};

//! The threads that made the traced calls to `entry_point` ("SQLFetch") after the first `skipped`.
std::vector<std::thread::id> ThreadsOf(const std::string &entry_point, size_t skipped = 0) {
	const auto calls = CallTrace::Calls();
	const auto threads = CallTrace::Threads();
	std::vector<std::thread::id> found;
	for (size_t index = skipped; index < calls.size(); index++) {
		if (calls[index].substr(0, calls[index].find(' ')) == entry_point) {
			found.push_back(threads[index]);
		}
	}
	return found;
}

//! The entry point of the latest traced call.
std::string LastTracedCall() {
	const auto calls = CallTrace::Calls();
	return calls.empty() ? std::string() : calls.back().substr(0, calls.back().find(' '));
}

//! Releases a gate when the case leaves its scope, on every path, so a failed REQUIRE never leaves the
//! producer held while a destructor joins it. Declared after the result and the connection, so it is
//! destroyed first.
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

//! What a teardown run beside a held call did: whether it returned while the call was held, and
//! whether it returned once the gate was released.
struct HeldTeardown {
	bool returned_while_held;
	bool joined;
};

//! Runs `teardown` on a thread of its own while `gate` holds a call, then releases the gate.
HeldTeardown TearDownWhileHeld(FakeGate &gate, const std::function<void()> &teardown) {
	auto done = std::async(std::launch::async, teardown);
	const bool returned_while_held = ReadyWithin(done, HELD_WAIT);
	gate.Release();
	return HeldTeardown {returned_while_held, ReadyWithin(done, EXPECTED_WAIT)};
}

FakeColumn IntegerColumn(const std::string &name) {
	FakeColumn column;
	column.name = name;
	column.type_name = "INTEGER";
	column.sql_type = SQL_INTEGER;
	column.column_size = 10;
	return column;
}

//! `count` INTEGER rows holding 0 to count - 1.
FakeResultSet Counting(size_t count) {
	std::vector<std::string> cells;
	for (size_t i = 0; i < count; i++) {
		cells.push_back(std::to_string(i));
	}
	return redshift_test::OneColumn(IntegerColumn("c"), cells);
}

//! `count` rows of one long VARCHAR holding "v0", "v1" and so on.
FakeResultSet LongRows(size_t count) {
	std::vector<std::string> cells;
	for (size_t i = 0; i < count; i++) {
		cells.push_back("v" + std::to_string(i));
	}
	return redshift_test::OneColumn(redshift_test::LongVarcharColumn("v"), cells);
}

//! Whether every row of `rows`, a batch's or ReadBatches', holds its own index, counting from `first`.
template <class ROWS>
bool CountsFrom(const ROWS &rows, int64_t first) {
	for (size_t row = 0; row < rows.size(); row++) {
		if (rows[row].at(0).integer != first + NumericCast<int64_t>(row)) {
			return false;
		}
	}
	return true;
}

//! The calls the fake received after the first `skipped`, the diagnostic reads included, which a
//! producer that missed the stop flag would make too.
std::vector<std::string> EveryCallAfter(const FakeDriverFixture &fixture, size_t skipped) {
	std::vector<std::string> names;
	const auto &calls = fixture.fake.Calls();
	for (size_t index = skipped; index < calls.size(); index++) {
		names.push_back(calls[index].entry_point);
	}
	return names;
}

//! The index, in the fake's calls, of the `nth` call to `entry_point`, counting from 1.
size_t IndexOfCall(const FakeDriverFixture &fixture, const std::string &entry_point, size_t nth) {
	const auto &calls = fixture.fake.Calls();
	for (size_t index = 0; index < calls.size(); index++) {
		if (calls[index].entry_point == entry_point && --nth == 0) {
			return index;
		}
	}
	return calls.size();
}

//! How many statement handles the fake was asked to free.
size_t StatementFrees(const FakeDriverFixture &fixture) {
	const auto details = fixture.DetailsOf("SQLFreeHandle");
	return NumericCast<size_t>(std::count(details.begin(), details.end(), "statement"));
}

//! A log sink that records the thread each line came from.
class ThreadLogSink : public RedshiftLogSink {
public:
	struct Line {
		std::string text;
		std::thread::id thread;
	};
	void Debug(const std::string &message) noexcept override {
		std::lock_guard<std::mutex> guard(lock);
		lines.push_back({message, std::this_thread::get_id()});
	}
	//! The lines that hold `text`.
	std::vector<Line> Holding(const std::string &text) const {
		std::lock_guard<std::mutex> guard(lock);
		std::vector<Line> found;
		for (const auto &line : lines) {
			if (line.text.find(text) != std::string::npos) {
				found.push_back(line);
			}
		}
		return found;
	}

private:
	mutable std::mutex lock;
	std::vector<Line> lines;
};

//! The refusal of Describe or Execute while a result is open.
const Thrown OPEN_RESULT_REFUSAL {ExceptionType::INVALID_INPUT,
                                  "a result from this Redshift connection is still open, and the connection holds one "
                                  "at a time; read that result to its end or close it, then try again"};
//! What NextBatch says once the result's connection has closed.
const Thrown CLOSED_RESULT {
    ExceptionType::IO, "this Redshift result's connection has closed; run the statement again on a new connection"};
//! What every NextBatch after a failed batch says.
const Thrown EARLIER_BATCH_FAILED {ExceptionType::EXECUTOR,
                                   "an earlier batch of this Redshift result failed, and the rest of its rows are not "
                                   "returned without it; run the statement again"};
//! A record that shows no loss, and the statement failures it gives on a fetch and on a cell read.
const FakeDiag DIVISION {"22012", 0, "ERROR:  division by zero", {}};
const Thrown DIVISION_FAILURE {ExceptionType::EXECUTOR,
                               "Redshift could not return the rest of the result: ERROR:  division by zero\n"
                               "SQLSTATE: 22012"};
const Thrown DIVISION_CELL_FAILURE {ExceptionType::EXECUTOR,
                                    "Redshift could not return column 1 of the current row: ERROR:  division by zero\n"
                                    "SQLSTATE: 22012"};
//! A record that shows the loss of the connection.
const FakeDiag LOST {"08S01", 0, "unexpected eof", {}};

//! Fails the `fetch`th fetch after Execute's with `record`, reads the first batch, and returns once
//! the producer, held reading the records, is past its last stop check, so a join finds the failure.
//! Returning lets the producer go.
void FailFetchAhead(FakeDriverFixture &fixture, RedshiftResult &result, size_t fetch, const FakeDiag &record) {
	fixture.fake.FailCall("SQLFetch", NumericCast<int>(fixture.CallsTo("SQLFetch") + fetch), SQL_ERROR, {record});
	auto gate = std::make_shared<FakeGate>();
	fixture.fake.BlockCall("SQLGetDiagRec", NumericCast<int>(fixture.CallsTo("SQLGetDiagRec") + 1), gate);
	const ReleaseOnExit release(gate);
	RedshiftBatch batch;
	REQUIRE(result.NextBatch(batch));
	REQUIRE(gate->WaitUntilHeldFor(EXPECTED_WAIT));
}

} // namespace

TEST_CASE("On a result read to its end, every driver call after Execute is the producer's, in order", "[odbc]") {
	ThreadFixture fixture;
	fixture.fake.AddResult(QUERY, {Counting(3 * SET_ROWS + 5)});
	auto connection = fixture.OpenConnection();
	auto result = redshift_test::ExecuteStatement(*connection, QUERY, {});
	const auto executed = CallTrace::Calls().size();

	const redshift_test::ReadBatches read(*result);
	const auto threads = CallTrace::Threads();

	CHECK(read.sizes == std::vector<idx_t> {SET_ROWS, SET_ROWS, SET_ROWS, 5});
	CHECK(CountsFrom(read.rows, 0));
	REQUIRE(threads.size() > executed);
	const auto producer = threads[executed];
	CHECK(producer != std::this_thread::get_id());
	for (size_t index = executed; index < threads.size(); index++) {
		INFO(CallTrace::Calls()[index]);
		CHECK(threads[index] == producer);
	}
	CHECK(ThreadsOf("SQLFetch", executed).size() == 5);
	CHECK(ThreadsOf("SQLFreeHandle", executed) == std::vector<std::thread::id> {producer});
}

TEST_CASE("The test-only switch makes every call on the caller's thread, one fetch a NextBatch", "[odbc]") {
	ThreadFixture fixture;
	SetFetchThreadForTesting(FetchThread::CALLER);
	fixture.fake.AddResult(QUERY, {Counting(2 * SET_ROWS)});
	auto connection = fixture.OpenConnection();
	auto result = redshift_test::ExecuteStatement(*connection, QUERY, {});
	const auto executed = CallTrace::Calls().size();
	const auto caller = std::this_thread::get_id();

	RedshiftBatch batch;
	REQUIRE(result->NextBatch(batch));
	CHECK(ThreadsOf("SQLFetch", executed) == std::vector<std::thread::id> {caller});
	const redshift_test::ReadBatches rest(*result);

	CHECK(rest.sizes == std::vector<idx_t> {SET_ROWS});
	for (const auto &thread : CallTrace::Threads()) {
		CHECK(thread == caller);
	}
}

TEST_CASE("Destroying a result while the producer is held joins it before the handle is freed", "[odbc]") {
	FakeDriverFixture fixture;
	fixture.fake.AddResult(QUERY, {Counting(6 * SET_ROWS)});
	auto connection = fixture.OpenConnection();
	auto result = redshift_test::ExecuteStatement(*connection, QUERY, {});
	const auto frees = StatementFrees(fixture);
	const auto start_bytes = HeldFetchBytes();
	// The second fetch is the producer's first ahead of the caller. The seventh reads the end of the
	// rows, after which a producer that missed the stop would drain and free the statement itself;
	// it waits for the caller to have given back three sets, so four batches are read first.
	const int held_fetch = GENERATE(2, 7);
	const int batches_before = held_fetch == 2 ? 1 : 4;
	// The second fetch passes with a warning: when the hold is there, the stopped producer reads none of
	// its records.
	fixture.fake.WarnCall("SQLFetch", 2, {{"01000", 0, "General warning", {}}});
	auto gate = std::make_shared<FakeGate>();
	fixture.fake.BlockCall("SQLFetch", held_fetch, gate);
	const ReleaseOnExit release(gate);
	RedshiftBatch batch;
	for (int read = 0; read < batches_before; read++) {
		REQUIRE(result->NextBatch(batch));
	}
	const bool held = gate->WaitUntilHeldFor(EXPECTED_WAIT);

	const auto teardown = TearDownWhileHeld(*gate, [&result]() { result.reset(); });

	CHECK(held);
	CHECK_FALSE(teardown.returned_while_held);
	REQUIRE(teardown.joined);
	const auto after_held = IndexOfCall(fixture, "SQLFetch", NumericCast<size_t>(held_fetch)) + 1;
	CHECK(EveryCallAfter(fixture, after_held) == std::vector<std::string> {"SQLFreeHandle"});
	CHECK(StatementFrees(fixture) - frees == 1);
	CHECK(HeldFetchBytes() == start_bytes);
	// The set the stop interrupted says nothing of the connection, which stays usable.
	CHECK(connection->Describe(QUERY).columns.size() == 1);
	connection.reset();
	CHECK(fixture.fake.AllocatedHandles() == "environment");
}

TEST_CASE("Destroying the connection while the producer is held joins it, then the result makes no driver call",
          "[odbc]") {
	FakeDriverFixture fixture;
	fixture.fake.AddResult(QUERY, {Counting(6 * SET_ROWS)});
	auto connection = fixture.OpenConnection();
	auto result = redshift_test::ExecuteStatement(*connection, QUERY, {});
	const auto frees = StatementFrees(fixture);
	auto gate = std::make_shared<FakeGate>();
	fixture.fake.BlockCall("SQLFetch", 2, gate);
	const ReleaseOnExit release(gate);
	RedshiftBatch batch;
	REQUIRE(result->NextBatch(batch));
	const bool held = gate->WaitUntilHeldFor(EXPECTED_WAIT);

	const auto teardown = TearDownWhileHeld(*gate, [&connection]() { connection.reset(); });

	CHECK(held);
	CHECK_FALSE(teardown.returned_while_held);
	REQUIRE(teardown.joined);
	const auto after_held = IndexOfCall(fixture, "SQLFetch", 2) + 1;
	CHECK(fixture.CallsAfter(after_held) ==
	      std::vector<std::string> {"SQLFreeHandle", "SQLEndTran", "SQLDisconnect", "SQLFreeHandle"});
	const auto closed_calls = fixture.fake.Calls().size();
	CHECK(redshift_test::Capture([&] { result->NextBatch(batch); }) == CLOSED_RESULT);
	result.reset();
	CHECK(fixture.fake.Calls().size() == closed_calls);
	CHECK(StatementFrees(fixture) - frees == 1);
	CHECK(fixture.fake.AllocatedHandles() == "environment");
}

TEST_CASE("NextBatch waits for the set the producer is filling", "[odbc]") {
	FakeDriverFixture fixture;
	fixture.fake.AddResult(LONG_QUERY, {LongRows(3)});
	auto connection = fixture.OpenConnection();
	auto result = redshift_test::ExecuteStatement(*connection, LONG_QUERY, {});
	auto gate = std::make_shared<FakeGate>();
	fixture.fake.BlockCall("SQLGetData", 2, gate);
	const ReleaseOnExit release(gate);
	RedshiftBatch batch;
	auto read = std::async(std::launch::async, [&]() { return result->NextBatch(batch); });
	const bool held = gate->WaitUntilHeldFor(EXPECTED_WAIT);
	const bool read_while_held = ReadyWithin(read, HELD_WAIT);
	gate->Release();

	CHECK(held);
	CHECK_FALSE(read_while_held);
	REQUIRE(ReadyWithin(read, EXPECTED_WAIT));
	CHECK(read.get());
	CHECK(batch.rows.size() == 3);
}

TEST_CASE("Destroying a long-column result while a cell read is held: the producer makes no further driver call",
          "[odbc]") {
	FakeDriverFixture fixture;
	// The second column, after the long one, is read with SQLGetData too: as text, or natively.
	const auto binding = GENERATE(NumberBinding::TEXT, NumberBinding::NATIVE);
	SetNumberBindingForTesting(binding);
	// More rows than one set holds, so the producer reads the second set while the caller holds the first.
	auto rows = LongRows(SET_ROWS + 3);
	rows.columns.push_back(IntegerColumn("n"));
	for (auto &row : rows.rows) {
		row.push_back("7");
	}
	fixture.fake.AddResult(LONG_QUERY, {rows});
	auto connection = fixture.OpenConnection();
	auto result = redshift_test::ExecuteStatement(*connection, LONG_QUERY, {});
	const auto frees = StatementFrees(fixture);
	auto gate = std::make_shared<FakeGate>();
	// A cell of the second set's first row: after its first, the next call reads its second cell;
	// after its second, the next row's fetch.
	const auto held_read = 2 * NumericCast<int>(SET_ROWS) + GENERATE(1, 2);
	fixture.fake.BlockCall("SQLGetData", held_read, gate);
	const ReleaseOnExit release(gate);
	RedshiftBatch batch;
	REQUIRE(result->NextBatch(batch));
	CHECK(batch.rows.size() == SET_ROWS);
	const bool held = gate->WaitUntilHeldFor(EXPECTED_WAIT);

	const auto teardown = TearDownWhileHeld(*gate, [&result]() { result.reset(); });

	CHECK(held);
	CHECK_FALSE(teardown.returned_while_held);
	REQUIRE(teardown.joined);
	const auto after_held = IndexOfCall(fixture, "SQLGetData", NumericCast<size_t>(held_read)) + 1;
	// The destructor's free alone.
	CHECK(EveryCallAfter(fixture, after_held) == std::vector<std::string> {"SQLFreeHandle"});
	CHECK(StatementFrees(fixture) - frees == 1);
}

TEST_CASE("A fetch that fails in a later set is raised after the sets before it, asked about on the caller's thread",
          "[odbc]") {
	ThreadFixture fixture;
	fixture.fake.AddResult(QUERY, {Counting(6 * SET_ROWS)});
	auto connection = fixture.OpenConnection();
	auto result = redshift_test::ExecuteStatement(*connection, QUERY, {});
	const auto executed = CallTrace::Calls().size();
	const auto frees = StatementFrees(fixture);
	const bool dead = GENERATE(false, true);
	fixture.fake.FailCall("SQLFetch", 3, SQL_ERROR, {DIVISION});
	fixture.fake.SetConnectionDead(dead);
	RedshiftBatch batch;

	REQUIRE(result->NextBatch(batch));
	CHECK(CountsFrom(batch.rows, 0));
	REQUIRE(result->NextBatch(batch));
	CHECK(CountsFrom(batch.rows, NumericCast<int64_t>(SET_ROWS)));
	const auto raised = redshift_test::Capture([&] { result->NextBatch(batch); });
	const auto later = redshift_test::Capture([&] { result->NextBatch(batch); });
	const auto latest = redshift_test::Capture([&] { result->NextBatch(batch); });

	if (dead) {
		CHECK(raised.type == ExceptionType::IO);
		CHECK(later == CLOSED_RESULT);
	} else {
		CHECK(raised == DIVISION_FAILURE);
		CHECK(later == EARLIER_BATCH_FAILED);
	}
	CHECK(latest == later);
	// The failed fetch and its records are the producer's last calls; the connection-dead question,
	// the caller's, comes after them and is asked once.
	const auto threads = CallTrace::Threads();
	const auto caller = std::this_thread::get_id();
	CHECK(ThreadsOf("SQLFetch", executed).size() == 3);
	CHECK(ThreadsOf("SQLGetConnectAttr", executed) == std::vector<std::thread::id> {caller});
	CHECK(LastTracedCall() == "SQLGetConnectAttr");
	CHECK(threads[threads.size() - 2] != caller);
	result.reset();
	// The raised failure left its set, so the destructor does not ask again where it was raised as a copy.
	CHECK(ThreadsOf("SQLGetConnectAttr", executed).size() == 1);
	CHECK(StatementFrees(fixture) - frees == 1);
}

TEST_CASE("A cell read that fails on the producer names its column, asked about on the caller's thread", "[odbc]") {
	ThreadFixture fixture;
	fixture.fake.AddResult(LONG_QUERY, {LongRows(3)});
	auto connection = fixture.OpenConnection();
	auto result = redshift_test::ExecuteStatement(*connection, LONG_QUERY, {});
	const auto executed = CallTrace::Calls().size();
	fixture.FailNextCall("SQLGetData", SQL_ERROR, {DIVISION});
	RedshiftBatch batch;

	CHECK(redshift_test::Capture([&] { result->NextBatch(batch); }) == DIVISION_CELL_FAILURE);
	CHECK(redshift_test::Capture([&] { result->NextBatch(batch); }) == EARLIER_BATCH_FAILED);
	const auto caller = std::this_thread::get_id();
	CHECK(ThreadsOf("SQLGetConnectAttr", executed) == std::vector<std::thread::id> {caller});
	CHECK(ThreadsOf("SQLGetData", executed).front() != caller);
	CHECK(LastTracedCall() == "SQLGetConnectAttr");
}

TEST_CASE("A failed drain's records are read by the producer and its loss asked about once, after the join", "[odbc]") {
	ThreadFixture fixture;
	fixture.fake.AddResult(QUERY, {Counting(3)});
	auto connection = fixture.OpenConnection();
	auto result = redshift_test::ExecuteStatement(*connection, QUERY, {});
	const auto executed = CallTrace::Calls().size();
	fixture.FailNextCall("SQLMoreResults", SQL_ERROR, {{"08S01", 0, "unexpected eof", {}}});
	RedshiftBatch batch;
	REQUIRE(result->NextBatch(batch));
	CHECK(batch.rows.size() == 3);

	const auto raised = redshift_test::Capture([&] { result->NextBatch(batch); });

	CHECK(raised.type == ExceptionType::IO);
	const auto caller = std::this_thread::get_id();
	// SQLMoreResults, its records and the free on the producer; the question after them.
	CHECK(ThreadsOf("SQLMoreResults", executed).front() != caller);
	CHECK(ThreadsOf("SQLGetDiagRec", executed).front() != caller);
	CHECK(ThreadsOf("SQLFreeHandle", executed).front() != caller);
	CHECK(ThreadsOf("SQLGetConnectAttr", executed) == std::vector<std::thread::id> {caller});
	CHECK(LastTracedCall() == "SQLGetConnectAttr");
	// The destructor, which also marks a connection the drain lost, does not ask again.
	result.reset();
	CHECK(ThreadsOf("SQLGetConnectAttr", executed).size() == 1);
}

TEST_CASE("A result whose producer read the end with sets unread stays open until NextBatch returns false", "[odbc]") {
	FakeDriverFixture fixture;
	fixture.fake.AddResult(QUERY, {Counting(3 * SET_ROWS)});
	fixture.fake.AddResult("SELECT 2", {Counting(1)});
	auto connection = fixture.OpenConnection();
	auto result = redshift_test::ExecuteStatement(*connection, QUERY, {});
	const bool held_state = connection->SessionHoldsState();
	const auto start_bytes = HeldFetchBytes();
	RedshiftBatch batch;
	REQUIRE(result->NextBatch(batch));
	// Four sets fit the ring with none given back: three of rows and the end, so the producer drains
	// and frees the statement while two sets of rows are unread.
	REQUIRE(StatementFreedWithin(fixture, EXPECTED_WAIT));
	const auto frees_at_end = StatementFrees(fixture);

	CHECK(redshift_test::Capture([&] { redshift_test::ExecuteStatement(*connection, "SELECT 2", {}); }) ==
	      OPEN_RESULT_REFUSAL);
	CHECK(redshift_test::Capture([&] { connection->Describe("SELECT 2"); }) == OPEN_RESULT_REFUSAL);
	CHECK(connection->SessionHoldsState() == held_state);
	SECTION("read to the end") {
		REQUIRE(result->NextBatch(batch));
		CHECK(CountsFrom(batch.rows, NumericCast<int64_t>(SET_ROWS)));
		REQUIRE(result->NextBatch(batch));
		CHECK(CountsFrom(batch.rows, NumericCast<int64_t>(2 * SET_ROWS)));
		CHECK(redshift_test::Capture([&] { connection->Describe("SELECT 2"); }) == OPEN_RESULT_REFUSAL);
		CHECK_FALSE(result->NextBatch(batch));
	}
	SECTION("destroyed with sets unread") {
		result.reset();
	}
	CHECK(StatementFrees(fixture) == frees_at_end);
	CHECK(HeldFetchBytes() == start_bytes);
	auto next = redshift_test::ExecuteStatement(*connection, "SELECT 2", {});
	const redshift_test::ReadBatches read(*next);
	CHECK(read.sizes == std::vector<idx_t> {1});
}

TEST_CASE("A batch that fails on the caller's thread joins the producer before NextBatch throws", "[odbc]") {
	FakeDriverFixture fixture;
	FakeResultSet rows = Counting(6 * SET_ROWS);
	// A value that is not an INTEGER's text fails the second batch while the producer is held filling
	// the third set.
	rows.rows[SET_ROWS][0] = "x";
	fixture.fake.AddResult(QUERY, {rows});
	auto connection = fixture.OpenConnection();
	auto result = redshift_test::ExecuteStatement(*connection, QUERY, {});
	const auto frees = StatementFrees(fixture);
	auto gate = std::make_shared<FakeGate>();
	fixture.fake.BlockCall("SQLFetch", 3, gate);
	const ReleaseOnExit release(gate);
	RedshiftBatch batch;
	REQUIRE(result->NextBatch(batch));
	REQUIRE(gate->WaitUntilHeldFor(EXPECTED_WAIT));
	auto read =
	    std::async(std::launch::async, [&]() { return redshift_test::Capture([&] { result->NextBatch(batch); }); });
	const bool thrown_while_held = ReadyWithin(read, HELD_WAIT);
	gate->Release();

	CHECK_FALSE(thrown_while_held);
	REQUIRE(ReadyWithin(read, EXPECTED_WAIT));
	CHECK(read.get().type == ExceptionType::EXECUTOR);
	const auto failed_calls = fixture.fake.Calls().size();
	CHECK(redshift_test::Capture([&] { result->NextBatch(batch); }) == EARLIER_BATCH_FAILED);
	result.reset();
	CHECK(EveryCallAfter(fixture, failed_calls) == std::vector<std::string> {"SQLFreeHandle"});
	CHECK(StatementFrees(fixture) - frees == 1);
	CHECK(connection->Describe(QUERY).columns.size() == 1);
}

TEST_CASE("A long-column batch that fails on the caller's thread frees the arenas once the producer is joined",
          "[odbc]") {
	FakeDriverFixture fixture;
	// Not UTF-8, so the first batch fails while the producer fills the next sets' arenas.
	FakeResultSet rows = LongRows(6 * SET_ROWS);
	rows.rows[0][0] = "\xff";
	fixture.fake.AddResult(LONG_QUERY, {rows});
	auto connection = fixture.OpenConnection();
	auto result = redshift_test::ExecuteStatement(*connection, LONG_QUERY, {});
	const auto frees = StatementFrees(fixture);
	const auto start_bytes = HeldFetchBytes();
	RedshiftBatch batch;

	CHECK(redshift_test::Capture([&] { result->NextBatch(batch); }).type == ExceptionType::EXECUTOR);
	CHECK(redshift_test::Capture([&] { result->NextBatch(batch); }) == EARLIER_BATCH_FAILED);
	result.reset();

	CHECK(StatementFrees(fixture) - frees == 1);
	CHECK(HeldFetchBytes() == start_bytes);
	CHECK(connection->Describe(LONG_QUERY).columns.size() == 1);
}

TEST_CASE("The producer fills at most the ring's four sets ahead of the caller", "[odbc]") {
	FakeDriverFixture fixture;
	fixture.fake.AddResult(QUERY, {Counting(8 * SET_ROWS)});
	auto connection = fixture.OpenConnection();
	auto result = redshift_test::ExecuteStatement(*connection, QUERY, {});
	auto gate = std::make_shared<FakeGate>();
	// With one set read and given back, the fifth fetch fills the last free set; the sixth waits.
	fixture.fake.BlockCall("SQLFetch", 6, gate);
	const ReleaseOnExit release(gate);
	RedshiftBatch batch;
	REQUIRE(result->NextBatch(batch));
	const bool held_with_ring_full = gate->WaitUntilHeldFor(HELD_WAIT);
	REQUIRE(result->NextBatch(batch));
	const bool held_once_set_returned = gate->WaitUntilHeldFor(EXPECTED_WAIT);
	gate->Release();

	CHECK_FALSE(held_with_ring_full);
	CHECK(held_once_set_returned);
	CHECK(CountsFrom(batch.rows, NumericCast<int64_t>(SET_ROWS)));
	const redshift_test::ReadBatches rest(*result);
	CHECK(rest.rows.size() == 6 * SET_ROWS);
	CHECK(CountsFrom(rest.rows, NumericCast<int64_t>(2 * SET_ROWS)));
}

TEST_CASE("Each batch is read from its own set while the producer fills the next", "[odbc]") {
	FakeDriverFixture fixture;
	// Each set's values a different length, so a batch read from the set being filled shows.
	std::vector<std::string> cells;
	for (size_t set = 0; set < 4; set++) {
		cells.insert(cells.end(), SET_ROWS, std::string(set + 1, 'a'));
	}
	fixture.fake.AddResult(QUERY, {redshift_test::OneColumn(redshift_test::VarcharColumn("c", 16), cells)});
	auto connection = fixture.OpenConnection();
	auto result = redshift_test::ExecuteStatement(*connection, QUERY, {});
	auto gate = std::make_shared<FakeGate>();
	// The third set's fetch, held with its set selected while the caller reads the second.
	fixture.fake.BlockCall("SQLFetch", 3, gate);
	const ReleaseOnExit release(gate);
	RedshiftBatch batch;
	REQUIRE(result->NextBatch(batch));
	REQUIRE(gate->WaitUntilHeldFor(EXPECTED_WAIT));

	REQUIRE(result->NextBatch(batch));
	gate->Release();

	REQUIRE(batch.rows.size() == SET_ROWS);
	CHECK(std::all_of(batch.rows.begin(), batch.rows.end(),
	                  [](const vector<RedshiftValue> &row) { return row[0].text == "aa"; }));
}

TEST_CASE("Destroying a result while its end-of-rows drain is held leaves the free to the destructor", "[odbc]") {
	ThreadFixture fixture;
	fixture.fake.AddResult(QUERY, {Counting(10)});
	auto connection = fixture.OpenConnection();
	auto result = redshift_test::ExecuteStatement(*connection, QUERY, {});
	const auto executed = CallTrace::Calls().size();
	auto gate = std::make_shared<FakeGate>();
	const auto held_drain = NumericCast<int>(fixture.CallsTo("SQLMoreResults")) + 1;
	fixture.fake.BlockCall("SQLMoreResults", held_drain, gate);
	const ReleaseOnExit release(gate);
	RedshiftBatch batch;
	REQUIRE(result->NextBatch(batch));
	const bool held = gate->WaitUntilHeldFor(EXPECTED_WAIT);

	std::thread::id destroyer;
	const auto teardown = TearDownWhileHeld(*gate, [&]() {
		destroyer = std::this_thread::get_id();
		result.reset();
	});

	CHECK(held);
	CHECK_FALSE(teardown.returned_while_held);
	REQUIRE(teardown.joined);
	CHECK(ThreadsOf("SQLFreeHandle", executed) == std::vector<std::thread::id> {destroyer});
	const auto after_held = IndexOfCall(fixture, "SQLMoreResults", NumericCast<size_t>(held_drain)) + 1;
	CHECK(EveryCallAfter(fixture, after_held) == std::vector<std::string> {"SQLFreeHandle"});
}

TEST_CASE("A result destroyed after its connection, with a failed drain unraised, makes no driver call", "[odbc]") {
	FakeDriverFixture fixture;
	fixture.fake.AddResult(QUERY, {Counting(3)});
	auto connection = fixture.OpenConnection();
	auto result = redshift_test::ExecuteStatement(*connection, QUERY, {});
	fixture.FailNextCall("SQLMoreResults", SQL_ERROR, {DIVISION});
	RedshiftBatch batch;
	REQUIRE(result->NextBatch(batch));
	// The producer has drained, failed and freed the statement; the failure waits for the next call.
	REQUIRE(StatementFreedWithin(fixture, EXPECTED_WAIT));
	connection.reset();
	const auto closed_calls = fixture.fake.Calls().size();

	result.reset();

	CHECK(fixture.fake.Calls().size() == closed_calls);
}

TEST_CASE("A fetch's warning is logged on the caller's thread, and an unread set's when the result goes", "[odbc]") {
	FakeDriverFixture fixture;
	fixture.fake.AddResult(QUERY, {Counting(6 * SET_ROWS)});
	auto sink = make_shared_ptr<ThreadLogSink>();
	auto connection = OpenRedshiftConnection(redshift_test::UsEast1Config(), sink);
	auto result = redshift_test::ExecuteStatement(*connection, QUERY, {});
	fixture.fake.WarnCall("SQLFetch", 1, {{"01000", 0, "General warning", {}}});
	fixture.fake.WarnCall("SQLFetch", 3, {{"01S02", 0, "Option value changed", {}}});
	auto gate = std::make_shared<FakeGate>();
	// Holding the fourth fetch shows the third, a set never read, has been made.
	fixture.fake.BlockCall("SQLFetch", 4, gate);
	const ReleaseOnExit release(gate);
	RedshiftBatch batch;
	REQUIRE(result->NextBatch(batch));
	REQUIRE(gate->WaitUntilHeldFor(EXPECTED_WAIT));
	const auto read_lines = sink->Holding("SQLSTATE 01000");
	CHECK(sink->Holding("SQLSTATE 01S02").empty());

	std::thread::id destroyer;
	const auto teardown = TearDownWhileHeld(*gate, [&]() {
		destroyer = std::this_thread::get_id();
		result.reset();
	});

	REQUIRE(teardown.joined);
	REQUIRE(read_lines.size() == 1);
	CHECK(read_lines[0].thread == std::this_thread::get_id());
	const auto unread_lines = sink->Holding("SQLSTATE 01S02");
	REQUIRE(unread_lines.size() == 1);
	CHECK(unread_lines[0].thread == destroyer);
	// The set that was read is not logged again.
	CHECK(sink->Holding("SQLSTATE 01000").size() == 1);
}

TEST_CASE("A long-column set filled ahead counts its strings until its batch takes them over", "[odbc]") {
	FakeDriverFixture fixture;
	// Four 1 MiB values reach a set's share, so eight rows make two sets.
	const std::string value(1024 * 1024 - 32, 'x');
	fixture.fake.AddResult(LONG_QUERY, {redshift_test::OneColumn(redshift_test::LongVarcharColumn("v"),
	                                                             std::vector<std::string>(8, value))});
	auto connection = fixture.OpenConnection();
	const auto before = HeldFetchBytes();
	auto result = redshift_test::ExecuteStatement(*connection, LONG_QUERY, {});
	RedshiftBatch batch;
	REQUIRE(result->NextBatch(batch));
	REQUIRE(batch.rows.size() == 4);

	// The producer has filled the second set once it has freed the statement at the end of the rows.
	REQUIRE(StatementFreedWithin(fixture, EXPECTED_WAIT));
	CHECK(HeldFetchBytes() - before >= 4 * value.size());
	REQUIRE(result->NextBatch(batch));
	CHECK(HeldFetchBytes() - before < value.size());
}

TEST_CASE("A producer that ends after its last fill answers that no set follows it", "[odbc]") {
	FetchProducer producer;
	producer.Start(RING_SETS, [](idx_t) { return false; });

	REQUIRE(producer.WaitForFilledSet());
	CHECK_FALSE(producer.WaitForFilledSet());
	producer.Stop();
}

TEST_CASE("A producer stopped while its ring is full answers that no set follows the ones it filled", "[odbc]") {
	FetchProducer producer;
	producer.Start(RING_SETS, [](idx_t) { return true; });
	// Taken without being given back, so the thread fills the ring and waits for a release.
	for (idx_t set = 0; set < RING_SETS; set++) {
		REQUIRE(producer.WaitForFilledSet());
	}

	producer.Stop();

	CHECK_FALSE(producer.WaitForFilledSet());
}

TEST_CASE("Starting a producer whose thread still runs is refused, and the running thread is kept", "[odbc]") {
	FetchProducer producer;
	producer.Start(RING_SETS, [](idx_t) { return true; });
	REQUIRE(producer.WaitForFilledSet());

	const auto thrown = redshift_test::Capture([&] { producer.Start(RING_SETS, [](idx_t) { return false; }); });

	CHECK(thrown.type == ExceptionType::INTERNAL);
	CHECK_THAT(thrown.message,
	           Catch::Contains("a Redshift result started its producer thread while the last one still ran"));
	// The first thread still fills: the set after the one taken is its, not the refused one's.
	CHECK(producer.WaitForFilledSet());
	producer.Stop();
}

TEST_CASE("A result destroyed with a failed drain unraised, its connection-dead question failing, marks the loss",
          "[odbc]") {
	FakeDriverFixture fixture;
	fixture.fake.AddResult(QUERY, {Counting(3)});
	auto connection = fixture.OpenConnection();
	auto result = redshift_test::ExecuteStatement(*connection, QUERY, {});
	fixture.FailNextCall("SQLMoreResults", SQL_ERROR, {DIVISION});
	RedshiftBatch batch;
	REQUIRE(result->NextBatch(batch));
	REQUIRE(StatementFreedWithin(fixture, EXPECTED_WAIT));
	// An answer that cannot be read counts as dead; the failed call returns a code.
	fixture.FailNextCall("SQLGetConnectAttr", SQL_ERROR, {});
	const auto asked = fixture.CallsTo("SQLGetConnectAttr");

	result.reset();

	CHECK(fixture.CallsTo("SQLGetConnectAttr") == asked + 1);
	CHECK(redshift_test::Capture([&] { connection->Describe(QUERY); }).type == ExceptionType::IO);
}

TEST_CASE("A result destroyed before it reaches a set whose fetch lost the connection marks the loss", "[odbc]") {
	FakeDriverFixture fixture;
	fixture.fake.AddResult(QUERY, {Counting((RING_SETS + 1) * SET_ROWS)});
	auto connection = fixture.OpenConnection();
	auto result = redshift_test::ExecuteStatement(*connection, QUERY, {});
	// The second or the last set's fetch fails with records that name the loss; the handle itself
	// still answers. With the refilled and caller's-thread cases below, every set holds the failure once.
	// Each value is a temporary: given a named one, Catch2's GENERATE recurses without end under MSVC's
	// default cast rules, where T(x) of an lvalue x of type T is an lvalue (warning C4717).
	FailFetchAhead(fixture, *result, GENERATE(idx_t {2}, idx_t {RING_SETS}), LOST);

	result.reset();

	const auto next = redshift_test::Capture([&] { connection->Describe(QUERY); });
	CHECK(next.type == ExceptionType::IO);
	CHECK_THAT(next.message, Catch::Contains("was lost and is closed"));
}

TEST_CASE("A result destroyed before it reaches a set whose fetch failed on a live connection leaves it open",
          "[odbc]") {
	FakeDriverFixture fixture;
	fixture.fake.AddResult(QUERY, {Counting(3 * SET_ROWS)});
	auto connection = fixture.OpenConnection();
	auto result = redshift_test::ExecuteStatement(*connection, QUERY, {});
	// Counted before the producer starts, which never asks the question.
	const auto asked = fixture.CallsTo("SQLGetConnectAttr");
	// The second set's fetch fails as a statement does, on a connection the driver reports alive.
	FailFetchAhead(fixture, *result, 2, DIVISION);

	result.reset();

	CHECK(fixture.CallsTo("SQLGetConnectAttr") == asked + 1);
	CHECK(connection->Describe(QUERY).columns.size() == 1);
}

namespace {

//! Redshift's answer that leaves the session unusable, as driver 2.3.0.0 writes it (connection_loss_test.cpp).
const FakeDiag UNUSABLE_SESSION {"XX000", 0, "[Redshift][ODBC Driver][Server]XX000:ERROR:  Internal Service Error", {}};

//! Whether `connection` refuses its next call as a closed connection does.
bool RefusesAsClosed(RedshiftConnection &connection) {
	const auto next = redshift_test::Capture([&] { connection.Describe(QUERY); });
	return next.type == ExceptionType::IO && next.message.find("was lost and is closed") != std::string::npos;
}

} // namespace

TEST_CASE("A result destroyed with a failed drain unraised that left the session unusable closes the connection",
          "[odbc]") {
	FakeDriverFixture fixture;
	fixture.fake.AddResult(QUERY, {Counting(3)});
	auto connection = fixture.OpenConnection();
	auto result = redshift_test::ExecuteStatement(*connection, QUERY, {});
	// The driver reports the connection alive: only the records say the session cannot be used.
	fixture.FailNextCall("SQLMoreResults", SQL_ERROR, {UNUSABLE_SESSION});
	RedshiftBatch batch;
	REQUIRE(result->NextBatch(batch));
	REQUIRE(StatementFreedWithin(fixture, EXPECTED_WAIT));

	result.reset();

	CHECK(RefusesAsClosed(*connection));
}

TEST_CASE("A result destroyed before it reaches a set whose fetch left the session unusable closes the connection",
          "[odbc]") {
	FakeDriverFixture fixture;
	fixture.fake.AddResult(QUERY, {Counting(3 * SET_ROWS)});
	auto connection = fixture.OpenConnection();
	auto result = redshift_test::ExecuteStatement(*connection, QUERY, {});
	FailFetchAhead(fixture, *result, 2, UNUSABLE_SESSION);

	result.reset();

	CHECK(RefusesAsClosed(*connection));
}

TEST_CASE("A result destroyed after a batch fails on the caller's thread marks a loss in the set after that batch",
          "[odbc]") {
	FakeDriverFixture fixture;
	FakeResultSet rows = Counting(4 * SET_ROWS);
	// A value that is not an INTEGER's text fails the second batch on the caller's thread.
	rows.rows[SET_ROWS][0] = "x";
	fixture.fake.AddResult(QUERY, {rows});
	auto connection = fixture.OpenConnection();
	auto result = redshift_test::ExecuteStatement(*connection, QUERY, {});
	// The third set's fetch, filled ahead, fails with records that name the loss.
	FailFetchAhead(fixture, *result, 3, LOST);
	RedshiftBatch batch;
	CHECK(redshift_test::Capture([&] { result->NextBatch(batch); }).type == ExceptionType::EXECUTOR);

	result.reset();

	const auto next = redshift_test::Capture([&] { connection->Describe(QUERY); });
	CHECK(next.type == ExceptionType::IO);
	CHECK_THAT(next.message, Catch::Contains("was lost and is closed"));
}

TEST_CASE("A result destroyed before it reads a refilled set whose fetch lost the connection marks the loss",
          "[odbc]") {
	FakeDriverFixture fixture;
	fixture.fake.AddResult(QUERY, {Counting((RING_SETS + 2) * SET_ROWS)});
	auto connection = fixture.OpenConnection();
	auto result = redshift_test::ExecuteStatement(*connection, QUERY, {});
	// With one set read and given back, the fetch after the ring's first round fills that set's place
	// again, and fails with records that name the loss.
	FailFetchAhead(fixture, *result, RING_SETS + 1, LOST);

	result.reset();

	const auto next = redshift_test::Capture([&] { connection->Describe(QUERY); });
	CHECK(next.type == ExceptionType::IO);
	CHECK_THAT(next.message, Catch::Contains("was lost and is closed"));
}

TEST_CASE("A result destroyed after its connection, with a lost connection in a set unread, makes no driver call",
          "[odbc]") {
	FakeDriverFixture fixture;
	fixture.fake.AddResult(QUERY, {Counting(3 * SET_ROWS)});
	auto connection = fixture.OpenConnection();
	auto result = redshift_test::ExecuteStatement(*connection, QUERY, {});
	FailFetchAhead(fixture, *result, 2, LOST);
	// Closing the connection joins the producer, so the set holds the failure; nothing is left to ask.
	connection.reset();
	const auto closed_calls = fixture.fake.Calls().size();

	result.reset();

	CHECK(fixture.fake.Calls().size() == closed_calls);
}

TEST_CASE("A result destroyed during a fetch that then fails asks once whether the fetch lost the connection",
          "[odbc]") {
	FakeDriverFixture fixture;
	fixture.fake.AddResult(QUERY, {Counting(6 * SET_ROWS)});
	auto connection = fixture.OpenConnection();
	auto result = redshift_test::ExecuteStatement(*connection, QUERY, {});
	// The producer's first fetch ahead is held until the stop has come, then fails with records that
	// name the loss, or as a statement does on a connection the driver reports alive.
	const bool lost = GENERATE(true, false);
	const auto held_fetch = NumericCast<int>(fixture.CallsTo("SQLFetch") + 2);
	fixture.fake.FailCall("SQLFetch", held_fetch, SQL_ERROR, {lost ? LOST : DIVISION});
	auto gate = std::make_shared<FakeGate>();
	fixture.fake.BlockCall("SQLFetch", held_fetch, gate);
	const ReleaseOnExit release(gate);
	RedshiftBatch batch;
	REQUIRE(result->NextBatch(batch));
	REQUIRE(gate->WaitUntilHeldFor(EXPECTED_WAIT));
	const auto asked = fixture.CallsTo("SQLGetConnectAttr");

	const auto teardown = TearDownWhileHeld(*gate, [&result]() { result.reset(); });

	CHECK_FALSE(teardown.returned_while_held);
	REQUIRE(teardown.joined);
	CHECK(fixture.CallsTo("SQLGetConnectAttr") == asked + 1);
	if (lost) {
		const auto next = redshift_test::Capture([&] { connection->Describe(QUERY); });
		CHECK(next.type == ExceptionType::IO);
		CHECK_THAT(next.message, Catch::Contains("was lost and is closed"));
	} else {
		CHECK(connection->Describe(QUERY).columns.size() == 1);
	}
}

TEST_CASE("A result destroyed during a cell read that then fails asks once whether the read lost the connection",
          "[odbc]") {
	FakeDriverFixture fixture;
	// The second column, after the long one, is read with SQLGetData too: as text, or natively.
	const auto binding = GENERATE(NumberBinding::TEXT, NumberBinding::NATIVE);
	SetNumberBindingForTesting(binding);
	auto rows = LongRows(SET_ROWS + 3);
	rows.columns.push_back(IntegerColumn("n"));
	for (auto &row : rows.rows) {
		row.push_back("7");
	}
	fixture.fake.AddResult(LONG_QUERY, {rows});
	auto connection = fixture.OpenConnection();
	auto result = redshift_test::ExecuteStatement(*connection, LONG_QUERY, {});
	// A cell of the second set's first row, read ahead of the caller, is held until the stop has come,
	// then fails with records that name the loss, or as a statement does on a connection the driver
	// reports alive: the long cell, read in parts, or the one after it, read natively or as text.
	const bool lost = GENERATE(true, false);
	const auto held_read = 2 * NumericCast<int>(SET_ROWS) + GENERATE(1, 2);
	fixture.fake.FailCall("SQLGetData", held_read, SQL_ERROR, {lost ? LOST : DIVISION});
	auto gate = std::make_shared<FakeGate>();
	fixture.fake.BlockCall("SQLGetData", held_read, gate);
	const ReleaseOnExit release(gate);
	RedshiftBatch batch;
	REQUIRE(result->NextBatch(batch));
	CHECK(batch.rows.size() == SET_ROWS);
	REQUIRE(gate->WaitUntilHeldFor(EXPECTED_WAIT));
	const auto asked = fixture.CallsTo("SQLGetConnectAttr");

	const auto teardown = TearDownWhileHeld(*gate, [&result]() { result.reset(); });

	CHECK_FALSE(teardown.returned_while_held);
	REQUIRE(teardown.joined);
	CHECK(fixture.CallsTo("SQLGetConnectAttr") == asked + 1);
	if (lost) {
		const auto next = redshift_test::Capture([&] { connection->Describe(LONG_QUERY); });
		CHECK(next.type == ExceptionType::IO);
		CHECK_THAT(next.message, Catch::Contains("was lost and is closed"));
	} else {
		CHECK(connection->Describe(LONG_QUERY).columns.size() == 2);
	}
}
