// The ring of buffer sets a result fetches into:
// sets used in turn, one fetch a bound batch and never one ahead, a long column's values
// ending their set at its share of the budget, every set inside the budget and freed at the end of
// the rows, a long value held once and no memory kept across statements (Linux), and a fetch's
// warnings logged.

// catch.hpp comes first, as in this directory's other tests.
#include "catch.hpp"

#include "fake_driver_fixture.hpp"
#include "odbc/bulk_fetch.hpp"
#include "thrown.hpp"

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <iterator>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#if defined(__linux__)
#include <fstream>
#include <unistd.h>
#endif

using namespace duckdb;
using redshift_driver::testing::FakeColumn;
using redshift_driver::testing::FakeOdbcDriver;
using redshift_driver::testing::FakeResultSet;
using redshift_test::FakeDriverFixture;
using redshift_test::LongVarcharColumn;
using redshift_test::OneColumn;
using redshift_test::ReadBatches;
using redshift_test::StatementFreedWithin;
using redshift_test::Thrown;
using redshift_test::VarcharColumn;

namespace {

const char *const QUERY = "SELECT c FROM t";

//! A driver source over the fake that records the bind offset each SQLFetch reads, so a case sees
//! which set each fetch filled.
class BindOffsets {
public:
	static redshift_driver::DriverLoadResult Source() {
		auto result = FakeOdbcDriver::Source();
		if (result.ok()) {
			std::lock_guard<std::mutex> guard(lock);
			target = result.driver->api;
			wrapped = *result.driver;
			wrapped.api.SetStmtAttr = &RecordingSetStmtAttr;
			wrapped.api.Fetch = &RecordingFetch;
			offset = nullptr;
			offsets.clear();
			result.driver = &wrapped;
		}
		return result;
	}
	//! The offset each fetch read, oldest first: 0 before any offset pointer was set.
	static std::vector<SQLULEN> Offsets() {
		std::lock_guard<std::mutex> guard(lock);
		return offsets;
	}

private:
	static SQLRETURN SQL_API RecordingSetStmtAttr(SQLHSTMT statement, SQLINTEGER attribute, SQLPOINTER value,
	                                              SQLINTEGER length) {
		if (attribute == SQL_ATTR_ROW_BIND_OFFSET_PTR) {
			std::lock_guard<std::mutex> guard(lock);
			offset = static_cast<SQLULEN *>(value);
		}
		return target.SetStmtAttr(statement, attribute, value, length);
	}
	static SQLRETURN SQL_API RecordingFetch(SQLHSTMT statement) {
		{
			std::lock_guard<std::mutex> guard(lock);
			offsets.push_back(offset ? *offset : 0);
		}
		return target.Fetch(statement);
	}

	static inline std::mutex lock;
	static inline redshift_driver::OdbcApi target;
	static inline redshift_driver::LoadedDriver wrapped;
	static inline SQLULEN *offset = nullptr;
	static inline std::vector<SQLULEN> offsets;
};

//! An INTEGER column, bound before a long one.
FakeColumn IntegerColumn(const std::string &name) {
	FakeColumn column;
	column.name = name;
	column.type_name = "INTEGER";
	column.sql_type = SQL_INTEGER;
	column.column_size = 10;
	return column;
}

//! A driver source over the fake whose second SQLGetData call reports NULL, as a driver could after
//! the first part of a long value.
class NullSecondPart {
public:
	static redshift_driver::DriverLoadResult Source() {
		auto result = FakeOdbcDriver::Source();
		if (result.ok()) {
			std::lock_guard<std::mutex> guard(lock);
			target = result.driver->api;
			wrapped = *result.driver;
			wrapped.api.GetData = &NullOnSecondCall;
			calls = 0;
			result.driver = &wrapped;
		}
		return result;
	}

private:
	static SQLRETURN SQL_API NullOnSecondCall(SQLHSTMT statement, SQLUSMALLINT column, SQLSMALLINT c_type,
	                                          SQLPOINTER value, SQLLEN length, SQLLEN *indicator) {
		{
			std::lock_guard<std::mutex> guard(lock);
			if (++calls == 2) {
				*indicator = SQL_NULL_DATA;
				return SQL_SUCCESS;
			}
		}
		return target.GetData(statement, column, c_type, value, length, indicator);
	}

	static inline std::mutex lock;
	static inline redshift_driver::OdbcApi target;
	static inline redshift_driver::LoadedDriver wrapped;
	static inline size_t calls = 0;
};

//! `count` INTEGER rows holding 0 to count - 1.
FakeResultSet Counting(size_t count) {
	std::vector<std::string> cells;
	for (size_t i = 0; i < count; i++) {
		cells.push_back(std::to_string(i));
	}
	return OneColumn(IntegerColumn("c"), cells);
}

//! `count` rows of one long VARCHAR holding `value`.
FakeResultSet LongRows(size_t count, const std::string &value) {
	return OneColumn(LongVarcharColumn("c"), std::vector<std::string>(count, value));
}

//! Rows of one long VARCHAR holding `values`, in order.
FakeResultSet LongRows(const std::vector<std::string> &values) {
	return OneColumn(LongVarcharColumn("c"), values);
}

//! `count` rows of `width` long VARCHARs, each holding `value`.
FakeResultSet LongColumns(size_t width, size_t count, const std::string &value) {
	FakeResultSet result;
	for (size_t column = 0; column < width; column++) {
		result.columns.push_back(LongVarcharColumn("c" + std::to_string(column)));
	}
	result.rows.assign(count, std::vector<std::string>(width, value));
	return result;
}

//! The rows of `width` whole cells of `value_bytes` each that end a set at its 4 MiB share, the row
//! that reaches it included: each cell's length, bytes and RedshiftValue, and every slot that a
//! vector of the cells' strings has allocated on this standard library.
size_t RowsReachingShare(size_t width, size_t value_bytes) {
	const size_t share = MAX_BATCH_BYTES / RING_SETS;
	std::vector<std::string> slots;
	for (size_t rows = 1; rows < MAX_BATCH_ROWS; rows++) {
		// One at a time, as the cells are read, so the vector grows as the set's does.
		for (size_t cell = 0; cell < width; cell++) {
			slots.emplace_back();
		}
		const auto cells = rows * width * (sizeof(SQLLEN) + value_bytes + sizeof(RedshiftValue));
		if (cells + slots.capacity() * sizeof(std::string) >= share) {
			return rows;
		}
	}
	return MAX_BATCH_ROWS;
}

//! Whether `read` holds exactly `values`, a row each, in order.
bool TextsInOrder(const ReadBatches &read, const std::vector<std::string> &values) {
	if (read.rows.size() != values.size()) {
		return false;
	}
	for (idx_t row = 0; row < values.size(); row++) {
		if (read.rows[row][0].text != values[row]) {
			return false;
		}
	}
	return true;
}

//! A result of `width` VARCHAR(1024) columns, `count` rows of 1024-byte values.
FakeResultSet Wide(size_t width, size_t count) {
	FakeResultSet result;
	for (size_t i = 0; i < width; i++) {
		result.columns.push_back(VarcharColumn("c" + std::to_string(i), 1024));
	}
	result.rows.assign(count, std::vector<std::string>(width, std::string(1024, 'w')));
	return result;
}

//! `width` VARCHAR(1024) columns of 1024-byte values, then a long VARCHAR holding "v", over `count`
//! rows: a result with a long column whose bytes are mostly in its arenas.
FakeResultSet WideBeforeLong(size_t width, size_t count) {
	auto result = Wide(width, count);
	result.columns.push_back(LongVarcharColumn("v"));
	for (auto &row : result.rows) {
		row.push_back("v");
	}
	return result;
}

//! The INTEGER columns before NarrowCells' long one.
constexpr size_t NARROW_COLUMNS = 39;

//! NarrowCells' first value, so that every row's index has as many digits.
constexpr size_t FIRST_INDEX = 10000;

//! `count` rows of NARROW_COLUMNS INTEGERs, the first FIRST_INDEX plus the row's index and the rest 1,
//! or NULL in an odd row, then a long VARCHAR holding "v": a result with a long column whose cells are
//! many and small, so a set reaches its share by their number, not their bytes.
FakeResultSet NarrowCells(size_t count) {
	FakeResultSet result;
	for (size_t column = 0; column < NARROW_COLUMNS; column++) {
		result.columns.push_back(IntegerColumn("c" + std::to_string(column)));
	}
	result.columns.push_back(LongVarcharColumn("v"));
	for (size_t row = 0; row < count; row++) {
		std::vector<std::string> cells(NARROW_COLUMNS, "1");
		cells[0] = std::to_string(FIRST_INDEX + row);
		cells.push_back("v");
		result.rows.push_back(cells);
		std::vector<bool> nulls(NARROW_COLUMNS + 1, row % 2 == 1);
		nulls[0] = false;
		nulls[NARROW_COLUMNS] = false;
		result.nulls.push_back(nulls);
	}
	return result;
}

//! Whether `rows` are NarrowCells' rows, in order.
bool NarrowCellsInOrder(const std::vector<std::vector<RedshiftValue>> &rows) {
	for (idx_t row = 0; row < rows.size(); row++) {
		const auto &values = rows[row];
		if (values.size() != NARROW_COLUMNS + 1 || values[0].integer != NumericCast<int64_t>(FIRST_INDEX + row) ||
		    values[NARROW_COLUMNS].text != "v") {
			return false;
		}
		for (idx_t column = 1; column < NARROW_COLUMNS; column++) {
			const bool null = values[column].kind == RedshiftValueKind::NULL_VALUE;
			if (null != (row % 2 == 1) || (!null && values[column].integer != 1)) {
				return false;
			}
		}
	}
	return true;
}

//! Whether every row of `read` holds its own index, in order.
bool CountsInOrder(const ReadBatches &read) {
	for (idx_t row = 0; row < read.rows.size(); row++) {
		if (read.rows[row][0].integer != NumericCast<int64_t>(row)) {
			return false;
		}
	}
	return true;
}

//! What reading QUERY whole on `connection` held in fetch buffers, beyond what was held before it.
struct HeldRead {
	std::vector<std::vector<RedshiftValue>> rows;
	std::vector<idx_t> sizes;
	//! The most, sampled after each batch.
	idx_t most_held = 0;
	//! After the NextBatch that returned false, while the result is still open.
	idx_t held_at_end = 0;
};

HeldRead ReadHeld(RedshiftConnection &connection) {
	const auto before = HeldFetchBytes();
	auto result = redshift_test::ExecuteStatement(connection, QUERY, {});
	RedshiftBatch batch;
	HeldRead read;
	while (result->NextBatch(batch)) {
		read.rows.insert(read.rows.end(), std::make_move_iterator(batch.rows.begin()),
		                 std::make_move_iterator(batch.rows.end()));
		read.sizes.push_back(batch.rows.size());
		read.most_held = std::max(read.most_held, HeldFetchBytes() - before);
	}
	read.held_at_end = HeldFetchBytes() - before;
	return read;
}

//! HeldFetchBytes once the producer has freed the statement and the count has then held still for
//! 50 ms, against the system clock for at most 10 seconds: the producer frees the statement before it
//! counts the set that read the end of the rows.
idx_t SettledHeldBytes(const FakeDriverFixture &fixture) {
	REQUIRE(StatementFreedWithin(fixture, std::chrono::milliseconds(10000)));
	const auto deadline = std::chrono::system_clock::now() + std::chrono::seconds(10);
	auto held = HeldFetchBytes();
	while (std::chrono::system_clock::now() < deadline) {
		std::this_thread::sleep_for(std::chrono::milliseconds(50));
		const auto now = HeldFetchBytes();
		if (now == held) {
			break;
		}
		held = now;
	}
	return held;
}

//! What the result's fetch holds once `batches` batches are read and the producer has read the end
//! of the rows: the sets filled ahead, each waiting for its batch.
idx_t HeldWhenFilledAhead(const FakeDriverFixture &fixture, RedshiftConnection &connection, idx_t batches) {
	const auto before = HeldFetchBytes();
	auto result = redshift_test::ExecuteStatement(connection, QUERY, {});
	RedshiftBatch batch;
	for (idx_t read = 0; read < batches; read++) {
		REQUIRE(result->NextBatch(batch));
	}
	return SettledHeldBytes(fixture) - before;
}

// A sanitizer's allocator and shadow memory are not the program's, so no sanitized build reads the
// process's resident memory. GCC defines the first two macros, Clang reports the features.
#if defined(__SANITIZE_ADDRESS__) || defined(__SANITIZE_THREAD__)
#define REDSHIFT_SANITIZED_BUILD
#elif defined(__has_feature)
#if __has_feature(address_sanitizer) || __has_feature(thread_sanitizer)
#define REDSHIFT_SANITIZED_BUILD
#endif
#endif

#if defined(__linux__) && !defined(REDSHIFT_SANITIZED_BUILD)
#define REDSHIFT_RESIDENT_MEMORY_TEST
//! This process's resident memory now, in bytes, or -1 when it cannot be read.
int64_t ResidentBytes() {
	std::ifstream statm("/proc/self/statm");
	int64_t pages = 0;
	int64_t resident = 0;
	if (!(statm >> pages >> resident)) {
		return -1;
	}
	return resident * sysconf(_SC_PAGESIZE);
}

//! Sets the kernel's record of this process's peak resident memory to what it holds now (Linux 4.0).
bool ResetPeakResident() {
	std::ofstream clear_refs("/proc/self/clear_refs");
	clear_refs << "5";
	clear_refs.flush();
	return bool(clear_refs);
}

//! This process's peak resident memory since ResetPeakResident, in bytes, or -1 when it cannot be
//! read: VmHWM, in KiB.
int64_t PeakResidentBytes() {
	std::ifstream status("/proc/self/status");
	std::string line;
	while (std::getline(status, line)) {
		if (line.rfind("VmHWM:", 0) == 0) {
			return std::stoll(line.substr(6)) * 1024;
		}
	}
	return -1;
}

//! A VARBYTE column, which is long whatever size the driver reports.
FakeColumn VarbyteColumn(const std::string &name) {
	FakeColumn column;
	column.name = name;
	column.type_name = "VARBYTE";
	column.sql_type = SQL_LONGVARBINARY;
	return column;
}
#endif

//! How many lines `fixture`'s sink logged that start with `prefix`.
size_t LinesStarting(FakeDriverFixture &fixture, const std::string &prefix) {
	const auto lines = fixture.log_sink->Lines();
	return NumericCast<size_t>(
	    std::count_if(lines.begin(), lines.end(), [&](const std::string &line) { return line.rfind(prefix, 0) == 0; }));
}

} // namespace

TEST_CASE("Bound batches fill the four sets in turn, each fetch at its own bind offset", "[odbc]") {
	FakeDriverFixture fixture;
	fixture.fake.AddResult(QUERY, {Counting(5 * 2048 + 3)});
	SetDriverSourceForTesting(&BindOffsets::Source);
	auto connection = fixture.OpenConnection();
	ReadBatches read(*redshift_test::ExecuteStatement(*connection, QUERY, {}));

	CHECK(read.sizes == std::vector<idx_t> {2048, 2048, 2048, 2048, 2048, 3});
	CHECK(CountsInOrder(read));
	// Seven fetches, the last the end of the rows: sets 0, 1, 2, 3, 0, 1, 2.
	const auto offsets = BindOffsets::Offsets();
	REQUIRE(offsets.size() == 7);
	const auto set_bytes = offsets[1];
	CHECK(set_bytes > 0);
	CHECK(offsets == std::vector<SQLULEN> {0, set_bytes, 2 * set_bytes, 3 * set_bytes, 0, set_bytes, 2 * set_bytes});
}

TEST_CASE("On the same-thread ring a bound batch is one fetch, and only the NextBatch that returns false reads the end",
          "[odbc]") {
	FakeDriverFixture fixture;
	// On the caller's thread: the producer fetches ahead of NextBatch (producer_thread_test.cpp).
	SetFetchThreadForTesting(FetchThread::CALLER);
	fixture.fake.AddResult(QUERY, {Counting(10)});
	auto connection = fixture.OpenConnection();
	auto result = redshift_test::ExecuteStatement(*connection, QUERY, {});
	const auto fetches = fixture.CallsTo("SQLFetch");
	const auto frees = fixture.CallsTo("SQLFreeHandle");
	RedshiftBatch batch;

	REQUIRE(result->NextBatch(batch));
	CHECK(batch.rows.size() == 10);
	// The short set is not taken for the end: the statement is still held.
	CHECK(fixture.CallsTo("SQLFetch") - fetches == 1);
	CHECK(fixture.CallsTo("SQLFreeHandle") == frees);

	CHECK_FALSE(result->NextBatch(batch));
	CHECK(fixture.CallsTo("SQLFetch") - fetches == 2);
	CHECK(fixture.CallsTo("SQLFreeHandle") == frees + 1);
}

TEST_CASE("A result with a long column fills a set of 2048 rows, then a partial one", "[odbc]") {
	FakeDriverFixture fixture;
	fixture.fake.AddResult(QUERY, {LongRows(2049, "v")});
	auto connection = fixture.OpenConnection();
	ReadBatches read(*redshift_test::ExecuteStatement(*connection, QUERY, {}));

	CHECK(read.sizes == std::vector<idx_t> {2048, 1});
	REQUIRE(read.rows.size() == 2049);
	CHECK(read.rows.back()[0].text == "v");
}

TEST_CASE("A long value that fills a set's share of the budget ends the set early", "[odbc]") {
	FakeDriverFixture fixture;
	// Three of these pass a set's 4 MiB share; the row that reaches it stays in its set. The fifth set
	// is set 0 again, and each row's value is its own, so one left from the set's first use shows.
	std::vector<std::string> values;
	for (size_t row = 0; row < 13; row++) {
		values.push_back(std::string(1536 * 1024 + row, char('a' + row)));
	}
	fixture.fake.AddResult(QUERY, {LongRows(values)});
	auto connection = fixture.OpenConnection();
	ReadBatches read(*redshift_test::ExecuteStatement(*connection, QUERY, {}));

	CHECK(read.sizes == std::vector<idx_t> {3, 3, 3, 3, 1});
	CHECK(TextsInOrder(read, values));
}

TEST_CASE("A long column's rows keep their own values when the ring comes round to a set again", "[odbc]") {
	FakeDriverFixture fixture;
	// The fifth and sixth sets are sets 0 and 1 again; a later row's text is longer than the row's
	// that used the set before it.
	std::vector<std::string> values;
	for (size_t row = 0; row < 5 * 2048 + 7; row++) {
		values.push_back("v" + std::to_string(row));
	}
	fixture.fake.AddResult(QUERY, {LongRows(values)});
	auto connection = fixture.OpenConnection();
	ReadBatches read(*redshift_test::ExecuteStatement(*connection, QUERY, {}));

	CHECK(read.sizes == std::vector<idx_t> {2048, 2048, 2048, 2048, 2048, 7});
	CHECK(TextsInOrder(read, values));
}

TEST_CASE("A long value's first part reserves the rest of it, so its string is allocated once", "[odbc]") {
	const std::string part(4096, 'p');
	const idx_t value_bytes = 1536 * 1024;
	std::string value;

	AppendLongPart(value, part.data(), part.size(), NumericCast<SQLLEN>(value_bytes));
	const auto reserved = value.capacity();
	// A standard library may round a reservation up by a few bytes, never to twice it.
	CHECK(reserved >= value_bytes);
	CHECK(reserved < value_bytes + 64);
	for (idx_t read = part.size(); read < value_bytes; read += part.size()) {
		AppendLongPart(value, part.data(), part.size(), NumericCast<SQLLEN>(value_bytes - read));
	}
	CHECK(value.size() == value_bytes);
	CHECK(value.capacity() == reserved);

	// A driver that cannot say what is left reserves nothing.
	std::string unknown;
	AppendLongPart(unknown, part.data(), part.size(), SQL_NO_TOTAL);
	CHECK(unknown == part);
}

TEST_CASE("A value that fits its arena leaves the arena's reservation as it is", "[odbc]") {
	// GCC 8's libstdc++ (RHEL 8's) shrinks a string to a smaller reserve(), so asking for one would
	// drop the reservation at every short value.
	std::string arena;
	arena.reserve(1024 * 1024);
	const auto reserved = arena.capacity();
	AppendLongPart(arena, "v", 1, 1);
	CHECK(arena == "v");
	CHECK(arena.capacity() == reserved);
	// So does a value read in two parts, whose first part gives the length still to come.
	AppendLongPart(arena, "w", 1, 2);
	AppendLongPart(arena, "x", 1, 1);
	CHECK(arena == "vwx");
	CHECK(arena.capacity() == reserved);
}

TEST_CASE("A wide result's sets fit the budget together until the end of the rows frees them", "[odbc]") {
	FakeDriverFixture fixture;
	fixture.fake.AddResult(QUERY, {Wide(16, 1000)});
	auto connection = fixture.OpenConnection();
	const auto read = ReadHeld(*connection);

	CHECK(read.rows.size() == 1000);
	CHECK(read.most_held > 3 * MAX_BATCH_BYTES / 4);
	CHECK(read.most_held <= MAX_BATCH_BYTES);
	CHECK(read.held_at_end == 0);
}

TEST_CASE("A long column's filled sets hold arenas of only their rows' lengths", "[odbc]") {
	FakeDriverFixture fixture;
	// On the caller's thread, so no set filled ahead holds strings when the counter is read.
	SetFetchThreadForTesting(FetchThread::CALLER);
	// Four of these, with their RedshiftValues, reach a set's share. The values are held in strings
	// of their own, so an arena holds only each row's length: a set filled ahead holds its strings
	// beside a few bytes of arena, not beside a whole share.
	fixture.fake.AddResult(QUERY, {LongRows(20, std::string(1024 * 1024 - 32, 'x'))});
	auto connection = fixture.OpenConnection();
	const auto read = ReadHeld(*connection);

	CHECK(read.rows.size() == 20);
	// The fifth set is set 0 again, its arena emptied first.
	CHECK(read.sizes == std::vector<idx_t> {4, 4, 4, 4, 4});
	// Under one set's reservation of a full batch's lengths, which each set would hold without the
	// give-back, and so well under a set's share.
	CHECK(read.most_held < MAX_BATCH_ROWS * sizeof(SQLLEN));
	CHECK(read.held_at_end == 0);
}

TEST_CASE("Values of one part fill a set's short values to 256 KiB, then take strings of their own", "[odbc]") {
	FakeDriverFixture fixture;
	// Values of 4095 bytes, the longest of one part: the first 64 of a set go into its short values,
	// which double as they come, and the rest take strings, about 1000 rows a set. Doubling on to
	// what the share leaves, the short values would count 4 MiB while holding about half of it, so
	// a set would end at about 513 rows.
	const auto value = std::string(4095, 'x');
	fixture.fake.AddResult(QUERY, {LongRows(4 * 1025 + 1, value)});
	auto connection = fixture.OpenConnection();
	const auto held = HeldWhenFilledAhead(fixture, *connection, 1);
	ReadBatches read(*redshift_test::ExecuteStatement(*connection, QUERY, {}));

	// Three full sets, each within its share, and set 0 again, holding the last rows.
	CHECK(held > MAX_BATCH_BYTES / 2);
	CHECK(held <= MAX_BATCH_BYTES);
	const auto string_rows =
	    MAX_BATCH_BYTES / RING_SETS / (value.size() + sizeof(SQLLEN) + sizeof(std::string) + sizeof(RedshiftValue));
	CHECK(read.sizes.front() > string_rows * 9 / 10);
}

TEST_CASE("A set's short values double from their first value as they grow, up to 256 KiB", "[odbc]") {
	FakeDriverFixture fixture;
	// 2048 values of one part a set, of `odd` and `even` bytes in turn, so the short values double
	// from the first: copied at a few values only, not at each, and never past their cap.
	idx_t odd = 0;
	idx_t even = 0;
	idx_t capacity = 0;
	idx_t most_capacity = 0;
	SECTION("below the cap") {
		// 153,600 bytes a set, in 204,800, 100 doubled eleven times. libc++ and MSVC round each
		// reservation up, which the doublings compound to about 12% more.
		odd = 100;
		even = 50;
		capacity = 100 << 11;
		most_capacity = capacity * 5 / 4;
	}
	SECTION("at the cap") {
		// 245,760 bytes a set: a doubling of 133,120 would pass 256 KiB, so it stops there.
		odd = 130;
		even = 110;
		capacity = idx_t(256) * 1024;
		most_capacity = capacity + 64;
	}
	std::vector<std::string> values;
	for (idx_t row = 0; row < RING_SETS * MAX_BATCH_ROWS; row++) {
		values.emplace_back(row % 2 == 0 ? odd : even, static_cast<char>('a' + row % 26));
	}
	fixture.fake.AddResult(QUERY, {LongRows(values)});
	auto connection = fixture.OpenConnection();
	const auto held = HeldWhenFilledAhead(fixture, *connection, 1);

	// Sets 1 to 3 filled ahead, each its lengths and its short values; set 0 refilled with none.
	const idx_t lengths = MAX_BATCH_ROWS * sizeof(SQLLEN);
	CHECK(held >= (RING_SETS - 1) * (lengths + capacity));
	CHECK(held <= (RING_SETS - 1) * (lengths + 64 + most_capacity) + 64);
	ReadBatches read(*redshift_test::ExecuteStatement(*connection, QUERY, {}));
	CHECK(TextsInOrder(read, values));
}

TEST_CASE("A long column's sets stay within the budget when bound cells sit between values of one part", "[odbc]") {
	FakeDriverFixture fixture;
	// A short bound VARCHAR, then a long value of one part, about 1900 a set, the first 129 in the
	// short values. A bound cell appended to a full arena doubles it as a string grows, so the values'
	// bytes are kept out of it: with them in it, a set of these rows would reach 7.5 MiB.
	FakeResultSet result;
	result.columns = {VarcharColumn("b", 13), LongVarcharColumn("v")};
	result.rows.assign(4 * MAX_BATCH_ROWS + 1, std::vector<std::string> {"b", std::string(2031, 'v')});
	fixture.fake.AddResult(QUERY, {result});
	auto connection = fixture.OpenConnection();
	const auto held = HeldWhenFilledAhead(fixture, *connection, 1);

	// Three full sets and set 0 again, holding the last rows.
	CHECK(held > MAX_BATCH_BYTES / 2);
	CHECK(held <= MAX_BATCH_BYTES);
}

TEST_CASE("A long column's sets refilled with longer values stay within the budget", "[odbc]") {
	FakeDriverFixture fixture;
	// Four sets of values of one part, then values of two parts, 517 a set, which a set refilled
	// after its short values counts against its share alone.
	auto result = LongRows(4 * 1025, std::string(4000, 's'));
	result.rows.insert(result.rows.end(), 3 * 519 + 1, std::vector<std::string> {std::string(8000, 'l')});
	fixture.fake.AddResult(QUERY, {result});
	auto connection = fixture.OpenConnection();
	const auto held = HeldWhenFilledAhead(fixture, *connection, RING_SETS);

	// Three sets of the longer values and the last one.
	CHECK(held > MAX_BATCH_BYTES / 2);
	CHECK(held <= MAX_BATCH_BYTES);
}

TEST_CASE("A long column's set frees its short values once its batch is built", "[odbc]") {
	FakeDriverFixture fixture;
	// On the caller's thread, so a built set holds only its arena, a length a row. Each set's first
	// 65 values of 4000 bytes fill its short values to 256 KiB: kept past their batches, the four sets'
	// short values would hold 1 MiB beside their arenas' 32 KiB of lengths.
	SetFetchThreadForTesting(FetchThread::CALLER);
	const auto value = std::string(4000, 's');
	fixture.fake.AddResult(QUERY, {LongRows(5 * 1025, value)});
	auto connection = fixture.OpenConnection();
	const auto read = ReadHeld(*connection);

	REQUIRE(read.rows.size() == 5 * 1025);
	CHECK(read.rows.back()[0].text == value);
	CHECK(read.most_held < 2 * RING_SETS * MAX_BATCH_ROWS * sizeof(SQLLEN));
	CHECK(read.held_at_end == 0);
}

TEST_CASE("Values of one part past what a set's short values may take get their own strings", "[odbc]") {
	FakeDriverFixture fixture;
	// The arena is reserved at 2048 bound VARCHAR(1024) cells, about half the share, and the short
	// values take at most 256 KiB, so the values of one part reach that cap after 65 rows of each
	// set, and the rest take strings of their own.
	FakeResultSet result;
	result.columns = {VarcharColumn("b", 1024), LongVarcharColumn("v")};
	for (idx_t row = 0; row < 4000; row++) {
		result.rows.push_back({"b", std::string(4000 - row % 7, static_cast<char>('a' + row % 26))});
	}
	fixture.fake.AddResult(QUERY, {result});
	auto connection = fixture.OpenConnection();
	const auto read = ReadHeld(*connection);

	REQUIRE(read.rows.size() == result.rows.size());
	for (idx_t row = 0; row < read.rows.size(); row++) {
		REQUIRE(read.rows[row][0].text == "b");
		REQUIRE(read.rows[row][1].text == result.rows[row][1]);
	}
}

TEST_CASE("A long-column result's arenas and bound row fit the budget together, however small its cells", "[odbc]") {
	FakeDriverFixture fixture;
	// The counter counts the arenas and the bound row, not the batch built beside them, so this shows
	// the bound row comes out of the budget before the sets share it. No row passes its share, so no
	// arena grows past its reservation between the samples.
	const size_t count = 5 * MAX_BATCH_ROWS;
	fixture.fake.AddResult(QUERY, {NarrowCells(count)});
	auto connection = fixture.OpenConnection();
	const auto read = ReadHeld(*connection);

	REQUIRE(read.rows.size() == count);
	CHECK(NarrowCellsInOrder(read.rows));
	// The cells, not the row cap, end each set, and every set but the last takes as many.
	REQUIRE(read.sizes.size() > RING_SETS);
	CHECK(read.sizes.front() < MAX_BATCH_ROWS);
	CHECK(NumericCast<size_t>(std::count(read.sizes.begin(), read.sizes.end() - 1, read.sizes.front())) ==
	      read.sizes.size() - 1);
	// Each arena keeps its rows' bound cells and lengths once filled. By the fourth batch every slot
	// has been filled once and only the last set can be partial, so three full sets' lengths at least.
	CHECK(read.most_held > (RING_SETS - 1) * read.sizes.front() * (NARROW_COLUMNS + 1) * sizeof(SQLLEN));
	CHECK(read.most_held <= MAX_BATCH_BYTES);
	CHECK(read.held_at_end == 0);
}

TEST_CASE("A long-column set gives back its values' string slots once its batch is built", "[odbc]") {
	FakeDriverFixture fixture;
	// On the caller's thread, so a built set holds only its arena, 2048 lengths. Each value is two
	// parts, so a string of its own, 822 a set in 1024 slots of 32 bytes on libstdc++: kept past their
	// batches, the slots would hold twice what the four arenas do.
	SetFetchThreadForTesting(FetchThread::CALLER);
	const auto value = std::string(5000, 'v');
	fixture.fake.AddResult(QUERY, {LongRows(5 * 823, value)});
	auto connection = fixture.OpenConnection();
	const auto read = ReadHeld(*connection);

	REQUIRE(read.rows.size() == 5 * 823);
	CHECK(read.rows.back()[0].text == value);
	CHECK(read.most_held < 2 * RING_SETS * MAX_BATCH_ROWS * sizeof(SQLLEN));
	CHECK(read.held_at_end == 0);
}

TEST_CASE("A long value counts its bytes, length and RedshiftValue against the share, and its string if it has one",
          "[odbc]") {
	FakeDriverFixture fixture;
	// A row's bytes in all divide a set's 4 MiB share exactly, so a set that left out any part of a
	// value would take more rows. A value of one part (4095 bytes at most) is in the set's short
	// values, whose whole capacity counts, until they reach 256 KiB, and in a string after that, so
	// such a set takes fewer.
	idx_t row_bytes = 0;
	idx_t value_bytes = 0;
	bool exact = true;
	SECTION("one part, in the short values to their cap, then in strings") {
		row_bytes = 4096;
		value_bytes = row_bytes - sizeof(SQLLEN) - sizeof(RedshiftValue);
		exact = false;
	}
	SECTION("several parts, in a string") {
		row_bytes = 8192;
		value_bytes = row_bytes - sizeof(SQLLEN) - sizeof(std::string) - sizeof(RedshiftValue);
	}
	const auto rows = MAX_BATCH_BYTES / RING_SETS / row_bytes;
	fixture.fake.AddResult(QUERY, {LongRows(2 * rows, std::string(value_bytes, 'x'))});
	auto connection = fixture.OpenConnection();
	ReadBatches read(*redshift_test::ExecuteStatement(*connection, QUERY, {}));

	REQUIRE(read.sizes.size() >= 2);
	CHECK(read.sizes.front() <= rows);
	CHECK(read.sizes.front() > rows / 2);
	if (exact) {
		CHECK(read.sizes == std::vector<idx_t> {rows, rows});
	}
}

TEST_CASE("A set counts the string slots its values' vector has allocated, not only those in use", "[odbc]") {
	FakeDriverFixture fixture;
	// Values of 8000 bytes, each in two parts and so in a string of its own, as many rows as their
	// cells alone take to reach the 4 MiB share (519 on libstdc++). A vector of strings grows by a
	// factor, so past 512 strings it has room for 1024, and counting that room ends the set sooner.
	const size_t width = 1;
	const size_t value_bytes = 8000;
	const size_t cell_bytes = sizeof(SQLLEN) + value_bytes + sizeof(std::string) + sizeof(RedshiftValue);
	const size_t rows = MAX_BATCH_BYTES / RING_SETS / (width * cell_bytes) + 1;
	fixture.fake.AddResult(QUERY, {LongColumns(width, rows, std::string(value_bytes, 'x'))});
	auto connection = fixture.OpenConnection();
	ReadBatches read(*redshift_test::ExecuteStatement(*connection, QUERY, {}));

	REQUIRE(read.rows.size() == rows);
	CHECK(read.sizes.front() == RowsReachingShare(width, value_bytes));
	CHECK(read.sizes.front() < rows);
}

TEST_CASE("A set of values of one part and of several ends at its share, counting the short values and the strings",
          "[odbc]") {
	FakeDriverFixture fixture;
	// Every third value is in two parts, so in a string of its own, and the rest of one part, whose
	// first 256 KiB a set go into its short values and the rest into strings too: about 770 rows a
	// set.
	std::vector<std::string> values;
	for (idx_t row = 0; row < 3200; row++) {
		const idx_t bytes = (row % 3 == 0 ? idx_t(8000) : idx_t(4000)) - row % 7;
		values.emplace_back(bytes, static_cast<char>('a' + row % 26));
	}
	fixture.fake.AddResult(QUERY, {LongRows(values)});
	auto connection = fixture.OpenConnection();
	const auto held = HeldWhenFilledAhead(fixture, *connection, 1);
	ReadBatches read(*redshift_test::ExecuteStatement(*connection, QUERY, {}));

	// Three full sets and set 0 again, holding the last rows.
	CHECK(held > MAX_BATCH_BYTES / 2);
	CHECK(held <= MAX_BATCH_BYTES);
	CHECK(read.sizes.size() == 5);
	CHECK(TextsInOrder(read, values));
}

TEST_CASE("A set whose arena's reservation takes its whole share gives each long value a string of its own", "[odbc]") {
	FakeDriverFixture fixture;
	// Two bound VARCHAR(1024) cells a row reserve each arena at the whole share, which leaves the short
	// values no room, so each value of 4000 bytes takes a string: about 4.2 KB of the share a row, with
	// its lengths and RedshiftValues.
	FakeResultSet result;
	result.columns = {VarcharColumn("a", 1024), VarcharColumn("b", 1024), LongVarcharColumn("v")};
	for (idx_t row = 0; row < 4 * 1000 + 1; row++) {
		result.rows.push_back({"a", "b", std::string(4000 - row % 7, static_cast<char>('a' + row % 26))});
	}
	fixture.fake.AddResult(QUERY, {result});
	auto connection = fixture.OpenConnection();
	const auto held = HeldWhenFilledAhead(fixture, *connection, 1);
	ReadBatches read(*redshift_test::ExecuteStatement(*connection, QUERY, {}));

	REQUIRE(read.rows.size() == result.rows.size());
	CHECK(read.sizes.front() > 900);
	// The least a row counts: its three lengths, two bound bytes, its shortest value in a string, and
	// its RedshiftValues. Short values let past their room would take these values without a string,
	// so a set would take more rows than its share holds of these.
	const idx_t row_bytes = 3 * sizeof(SQLLEN) + 2 + (4000 - 6) + sizeof(std::string) + 3 * sizeof(RedshiftValue);
	CHECK(read.sizes.front() * row_bytes <= MAX_BATCH_BYTES / RING_SETS + row_bytes);
	// The sets filled ahead give back their arenas' unused reservations, so with their strings they
	// stay within the budget.
	CHECK(held > MAX_BATCH_BYTES / 2);
	CHECK(held <= MAX_BATCH_BYTES);
	for (idx_t row = 0; row < read.rows.size(); row++) {
		REQUIRE(read.rows[row][2].text == result.rows[row][2]);
	}
}

TEST_CASE("A set whose arena's reservation takes its whole share gives short values strings too", "[odbc]") {
	FakeDriverFixture fixture;
	// The same two bound VARCHAR(1024) cells, before values of 32 bytes, past a string's inline
	// buffer: 2048 rows a set, well within the share, and still each value in a string, as the
	// reservation leaves the short values no room.
	FakeResultSet result;
	result.columns = {VarcharColumn("a", 1024), VarcharColumn("b", 1024), LongVarcharColumn("v")};
	for (idx_t row = 0; row < RING_SETS * MAX_BATCH_ROWS + 1; row++) {
		result.rows.push_back({"a", "b", std::string(32, static_cast<char>('a' + row % 26))});
	}
	fixture.fake.AddResult(QUERY, {result});
	auto connection = fixture.OpenConnection();
	const auto held = HeldWhenFilledAhead(fixture, *connection, 1);

	// Sets 1 to 3 filled ahead, each its rows' lengths, bound bytes and strings. With the values in the
	// short values, the sets would hold about a third less.
	CHECK(held >= (RING_SETS - 1) * MAX_BATCH_ROWS * (3 * sizeof(SQLLEN) + 2 + 32 + sizeof(std::string)));
}

TEST_CASE("A set counts its short values' whole capacity, and grows them only into what its strings leave", "[odbc]") {
	FakeDriverFixture fixture;
	std::vector<std::string> values;
	idx_t most_rows = 0;
	idx_t least_rows = 0;
	const idx_t row_bytes = sizeof(SQLLEN) + sizeof(RedshiftValue);
	SECTION("strings first") {
		// 100 values in strings of 40,000 bytes leave the short values about 170 KB of the share, so
		// they double from 4000 bytes only to that: about 145 rows a set. Doubled to their cap past
		// what the strings leave, they would end the set at 133.
		values.assign(100, std::string(40000, 'l'));
		values.insert(values.end(), 200, std::string(4000, 's'));
		least_rows = 134;
		most_rows = 200;
	}
	SECTION("short values first") {
		// 33 values of one part grow the short values to 256,000 bytes, about half of it held, then
		// values in strings of 8000 bytes fill the rest of the share. Counting only the bytes held, a
		// set would take about 15 rows more.
		values.assign(33, std::string(4000, 's'));
		values.insert(values.end(), 600, std::string(8000, 'l'));
		least_rows = 434;
		most_rows =
		    33 + (MAX_BATCH_BYTES / RING_SETS - 256000 - 33 * row_bytes) / (8000 + sizeof(std::string) + row_bytes) + 1;
	}
	fixture.fake.AddResult(QUERY, {LongRows(values)});
	auto connection = fixture.OpenConnection();
	ReadBatches read(*redshift_test::ExecuteStatement(*connection, QUERY, {}));

	REQUIRE(read.sizes.size() >= 2);
	CHECK(read.sizes.front() >= least_rows);
	CHECK(read.sizes.front() <= most_rows);
	CHECK(TextsInOrder(read, values));
}

TEST_CASE("A long value reported NULL after its first part keeps no bytes for the next cell", "[odbc]") {
	FakeDriverFixture fixture;
	fixture.fake.AddResult(QUERY, {LongRows({std::string(5000, 'x'), "next"})});
	SetDriverSourceForTesting(&NullSecondPart::Source);
	auto connection = fixture.OpenConnection();
	ReadBatches read(*redshift_test::ExecuteStatement(*connection, QUERY, {}));

	REQUIRE(read.rows.size() == 2);
	CHECK(read.rows[0][0].kind == RedshiftValueKind::NULL_VALUE);
	CHECK(read.rows[1][0].text == "next");
}

TEST_CASE("A bound row past the whole budget still leaves each long-column set a share", "[odbc]") {
	FakeDriverFixture fixture;
	// More columns than Redshift allows, so the row of bound ones before the long one exceeds the
	// whole budget.
	FakeResultSet result;
	const size_t bound = MAX_BATCH_BYTES / 1024 + 1;
	for (size_t column = 0; column < bound; column++) {
		result.columns.push_back(VarcharColumn("c" + std::to_string(column), 1024));
	}
	result.columns.push_back(LongVarcharColumn("v"));
	std::vector<std::string> cells(bound, "a");
	cells.push_back("v");
	result.rows.assign(2, cells);
	fixture.fake.AddResult(QUERY, {result});
	auto connection = fixture.OpenConnection();
	const auto read = ReadHeld(*connection);

	REQUIRE(read.rows.size() == 2);
	CHECK(read.rows[1][bound].text == "v");
	CHECK(read.held_at_end == 0);
}

TEST_CASE("A result destroyed before its end gives back what its arenas held", "[odbc]") {
	FakeDriverFixture fixture;
	// Two sets, of 2048 rows and of 952, each row a value of one part.
	const idx_t rows = 3000;
	const idx_t value_bytes = 1000;
	fixture.fake.AddResult(QUERY, {LongRows(rows, std::string(value_bytes, 'v'))});
	auto connection = fixture.OpenConnection();
	const auto before = HeldFetchBytes();
	auto result = redshift_test::ExecuteStatement(*connection, QUERY, {});
	RedshiftBatch batch;
	REQUIRE(result->NextBatch(batch));
	REQUIRE(batch.rows.size() == MAX_BATCH_ROWS);
	// Each set's arena holds its rows' lengths, and the second set, filled but not read, its values.
	const idx_t unread = rows - MAX_BATCH_ROWS;
	REQUIRE(SettledHeldBytes(fixture) - before >= rows * sizeof(SQLLEN) + unread * value_bytes);

	result.reset();
	CHECK(HeldFetchBytes() == before);
}

TEST_CASE("A long-column batch that fails gives back what the result's arenas held", "[odbc]") {
	FakeDriverFixture fixture;
	// The first batch fills a set; the second fails on its one row, which is not valid UTF-8.
	std::vector<std::string> values(MAX_BATCH_ROWS, "v");
	values.push_back("bad \xC3\x28");
	fixture.fake.AddResult(QUERY, {LongRows(values)});
	auto connection = fixture.OpenConnection();
	const auto before = HeldFetchBytes();
	auto result = redshift_test::ExecuteStatement(*connection, QUERY, {});
	RedshiftBatch batch;
	REQUIRE(result->NextBatch(batch));
	// The first set's arena holds its rows' lengths.
	REQUIRE(HeldFetchBytes() - before >= MAX_BATCH_ROWS * sizeof(SQLLEN));

	REQUIRE(redshift_test::Capture([&] { result->NextBatch(batch); }) ==
	        Thrown {ExceptionType::EXECUTOR, "Redshift returned text in column 1 that is not valid UTF-8, so the batch "
	                                         "was not returned; check the column's data"});
	CHECK(HeldFetchBytes() == before);
	// What the failed batch gave back is not given back again when the result goes.
	result.reset();
	CHECK(HeldFetchBytes() == before);
}

TEST_CASE("A row that grows an arena past its share leaves it released once its batch is built", "[odbc]") {
	FakeDriverFixture fixture;
	// On the caller's thread, so no set filled ahead is counted when the batch is checked.
	SetFetchThreadForTesting(FetchThread::CALLER);
	// 1600 bound columns, as many as a Redshift table may have, before the long one: about 1.6 MiB of
	// cells a row, so the third row reaches a set's share and grows its arena past the reservation.
	const size_t bound = 1600;
	fixture.fake.AddResult(QUERY, {WideBeforeLong(bound, 4)});
	auto connection = fixture.OpenConnection();
	auto result = redshift_test::ExecuteStatement(*connection, QUERY, {});
	const auto before = HeldFetchBytes();
	RedshiftBatch batch;

	REQUIRE(result->NextBatch(batch));
	CHECK(batch.rows.size() == 3);
	// What is left is the bound row, about 1.6 MiB.
	CHECK(HeldFetchBytes() - before < MAX_BATCH_BYTES / RING_SETS);
}

#if defined(REDSHIFT_RESIDENT_MEMORY_TEST)
TEST_CASE("Long-column statements run again and again on one connection leave the process no larger", "[odbc]") {
	FakeDriverFixture fixture;
	// 64 MiB of 1 MiB values, held in strings of their own, and 16 MiB of bound cells, held in arenas:
	// either kept past its statement grows the process by megabytes a run.
	const char *const WIDE_QUERY = "SELECT w FROM t";
	fixture.fake.AddResult(QUERY, {LongRows(64, std::string(1024 * 1024, 'x'))});
	fixture.fake.AddResult(WIDE_QUERY, {WideBeforeLong(16, 1024)});
	auto connection = fixture.OpenConnection();
	const auto run = [&] {
		for (const auto sql : {QUERY, WIDE_QUERY}) {
			auto result = redshift_test::ExecuteStatement(*connection, sql, {});
			RedshiftBatch batch;
			while (result->NextBatch(batch)) {
			}
		}
	};
	// The first run brings the allocator's free lists and the fake's copies to their working size.
	run();
	const auto before = ResidentBytes();
	REQUIRE(before > 0);
	for (int runs = 0; runs < 8; runs++) {
		run();
	}

	// The fake logs every call, about 1 MiB a run here, which the bound leaves room for.
	const auto after = ResidentBytes();
	REQUIRE(after > 0);
	CHECK(after - before < int64_t(2 * MAX_BATCH_BYTES));
}

TEST_CASE("A long value is held once while its batch is built", "[odbc]") {
	FakeDriverFixture fixture;
	auto column = LongVarcharColumn("c");
	auto kind = RedshiftValueKind::TEXT;
	SECTION("text") {
	}
	SECTION("bytes") {
		column = VarbyteColumn("c");
		kind = RedshiftValueKind::BLOB;
	}
	// Past glibc's largest mmap threshold, 32 MiB, so each copy takes new pages, not freed heap.
	const int64_t value_bytes = 32 * 1024 * 1024;
	fixture.fake.AddResult(QUERY, {OneColumn(column, {std::string(NumericCast<size_t>(value_bytes), 'x')})});
	auto connection = fixture.OpenConnection();
	auto result = redshift_test::ExecuteStatement(*connection, QUERY, {});
	RedshiftBatch batch;
	REQUIRE(ResetPeakResident());
	const auto before = PeakResidentBytes();
	REQUIRE(before > 0);

	REQUIRE(result->NextBatch(batch));
	REQUIRE(batch.rows.size() == 1);
	CHECK(batch.rows[0][0].kind == kind);
	CHECK(batch.rows[0][0].text.size() == NumericCast<size_t>(value_bytes));
	// A second copy, an arena's or a value's, would take the peak to twice the value.
	const auto after = PeakResidentBytes();
	REQUIRE(after > 0);
	CHECK(after - before < value_bytes * 3 / 2);
}
#endif

TEST_CASE("A native number after an odd-width text column is bound at an aligned address", "[odbc]") {
	const ColumnBinding text {SQL_C_CHAR, 11};
	const ColumnBinding number {SQL_C_SBIGINT, sizeof(SQLBIGINT)};
	for (const SQLULEN rows : {SQLULEN(1), SQLULEN(3)}) {
		BoundRowArray buffers({text, number, text, number}, rows, RING_SETS);
		for (idx_t set = 0; set < RING_SETS; set++) {
			buffers.Select(set);
			CHECK(reinterpret_cast<uintptr_t>(buffers.Value(1, 0)) % alignof(SQLBIGINT) == 0);
			CHECK(reinterpret_cast<uintptr_t>(buffers.Value(3, 0)) % alignof(SQLBIGINT) == 0);
		}
	}
}

TEST_CASE("A fetch's warning other than 01004 is logged, on both paths", "[odbc]") {
	FakeDriverFixture fixture;
	SECTION("bound") {
		fixture.fake.AddResult(QUERY, {Counting(3)});
	}
	SECTION("with a long column") {
		fixture.fake.AddResult(QUERY, {LongRows(3, "v")});
	}
	auto connection = fixture.OpenConnection();
	auto result = redshift_test::ExecuteStatement(*connection, QUERY, {});
	fixture.fake.WarnCall("SQLFetch", NumericCast<int>(fixture.CallsTo("SQLFetch")) + 1,
	                      {{"01000", 0, "General warning", {}}});
	ReadBatches read(*result);

	CHECK(read.rows.size() == 3);
	CHECK(LinesStarting(fixture, "Redshift driver diagnostic, SQLSTATE 01000") == 1);
}
