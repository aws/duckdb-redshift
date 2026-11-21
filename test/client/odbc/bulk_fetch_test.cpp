// The bulk fetch: each bounded column bound at its type's width over a row array the batch's
// byte budget sizes, every fetched cell's length and the fetch's records checked before a batch is
// returned, and a result with a long column fetched one row at a time, the columns before it bound
// and the rest read with SQLGetData.

// catch.hpp comes first, as in this directory's other tests.
#include "catch.hpp"

#include "duckdb/common/exception.hpp"
#include "duckdb/common/string_util.hpp"
#include "fake_driver_fixture.hpp"
#include "odbc/bulk_fetch.hpp"
#include "odbc/odbc_process_state.hpp"
#include "thrown.hpp"

#include <algorithm>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

using namespace duckdb;
using redshift_driver::testing::FakeColumn;
using redshift_driver::testing::FakeDiag;
using redshift_driver::testing::FakeOdbcDriver;
using redshift_driver::testing::FakeResultSet;
using redshift_test::DRIVER_FAILURE_TRAILER;
using redshift_test::FakeDriverFixture;
using redshift_test::LongVarcharColumn;
using redshift_test::OneColumn;
using redshift_test::ReadBatches;
using redshift_test::Thrown;
using redshift_test::VarcharColumn;

namespace {

const char *const QUERY = "SELECT c FROM t";

//! A column as the driver describes one of `type_name`, at `size` with `digits`.
FakeColumn Column(const std::string &type_name, SQLSMALLINT sql_type, SQLULEN size, SQLSMALLINT digits = 0) {
	FakeColumn column;
	column.name = "c";
	column.type_name = type_name;
	column.sql_type = sql_type;
	column.column_size = size;
	column.decimal_digits = digits;
	return column;
}

FakeColumn Integer() {
	return Column("INTEGER", SQL_INTEGER, 10);
}

//! `count` INTEGER rows holding 0 to count - 1, in order.
FakeResultSet Counting(size_t count) {
	std::vector<std::string> cells;
	for (size_t i = 0; i < count; i++) {
		cells.push_back(std::to_string(i));
	}
	return OneColumn(Integer(), cells);
}

//! The row array of 16 VARCHAR(1024) columns: a set's 4 MiB share of 16 MiB over 16 * (1025 + 8 +
//! sizeof(RedshiftValue)) + 2 bytes a row, 240 where a RedshiftValue is 56 bytes, as on libstdc++.
const idx_t WIDE_ROWS = MAX_BATCH_BYTES / RING_SETS / (2 + 16 * (1025 + 8 + sizeof(RedshiftValue)));

//! A result whose `width` columns are each a VARCHAR(1024), with `count` rows of short values.
FakeResultSet Wide(size_t width, size_t count) {
	FakeResultSet result;
	for (size_t i = 0; i < width; i++) {
		result.columns.push_back(VarcharColumn("c" + std::to_string(i), 1024));
	}
	result.rows.assign(count, std::vector<std::string>(width, "v"));
	return result;
}

//! The batches of QUERY scripted as `result`.
ReadBatches ReadQuery(FakeDriverFixture &fixture, const FakeResultSet &result) {
	fixture.fake.AddResult(QUERY, {result});
	auto connection = fixture.OpenConnection();
	return ReadBatches(*redshift_test::ExecuteStatement(*connection, QUERY, {}));
}

//! What the first NextBatch of QUERY threw, and the rows it left in the batch.
struct FirstBatch {
	ErrorData error;
	idx_t rows_left;
};

//! Whether a refusal carries no SQLSTATE: a refusal with records sets both keys.
bool CarriesNoSqlState(const ErrorData &error) {
	const auto &info = error.ExtraInfo();
	return info.count("sqlstate") == 0 && info.count("sqlstates") == 0;
}

//! The first batch of QUERY scripted as `result`, with `script` run on the fixture once QUERY has
//! executed, so its counts start after the session setup and Execute.
template <class SCRIPT>
FirstBatch FailFirstBatch(FakeDriverFixture &fixture, const FakeResultSet &result, SCRIPT &&script) {
	fixture.fake.AddResult(QUERY, {result});
	auto connection = fixture.OpenConnection();
	auto read = redshift_test::ExecuteStatement(*connection, QUERY, {});
	script();
	RedshiftBatch batch;
	const auto error = redshift_test::CaptureError([&] { read->NextBatch(batch); });
	return FirstBatch {error, batch.rows.size()};
}

FirstBatch FailFirstBatch(FakeDriverFixture &fixture, const FakeResultSet &result) {
	return FailFirstBatch(fixture, result, [] {});
}

//! How every refusal of a bound value that may not be whole ends.
const std::string NOT_RETURNED = ", so the batch was not returned rather than a value cut short; report it with the "
                                 "extension and driver versions";
//! The refusal of a value longer than column 1's buffer, which the driver's 01004 reported too.
const std::string LONGER_THAN_BUFFER =
    "Redshift returned a value in column 1 longer than this extension's buffer for it" + NOT_RETURNED +
    "\nSQLSTATE: HY000";

//! The varchar(300) bound values: `letters` letters, then the 4-byte U+1F600 when `emoji` is set.
std::string Text300(size_t letters, bool emoji) {
	return std::string(letters, 'a') + (emoji ? "\xF0\x9F\x98\x80" : "");
}

//! An INTEGER and then a VARCHAR(10), as columns 1 and 2, over `rows`.
FakeResultSet IntegerThenText(const std::vector<std::vector<std::string>> &rows) {
	FakeResultSet result;
	result.columns = {Integer(), VarcharColumn("d", 10)};
	result.rows = rows;
	return result;
}

//! `column`, named `name`.
FakeColumn Named(FakeColumn column, const std::string &name) {
	column.name = name;
	return column;
}

//! A VARBYTE, which is long whatever size the driver reports.
FakeColumn Varbyte() {
	return Column("VARBYTE", SQL_LONGVARBINARY, 0);
}

//! An INTEGER, then a long `column`, over `rows`.
FakeResultSet IntegerThenLong(const FakeColumn &column, const std::vector<std::vector<std::string>> &rows) {
	FakeResultSet result;
	result.columns = {Integer(), Named(column, "d")};
	result.rows = rows;
	return result;
}

//! A driver source over the fake whose SQLFetch ends the rows with SQL_SUCCESS and no row fetched,
//! which ODBC rules out: it asks for SQL_NO_DATA.
class SuccessAtEnd {
public:
	static redshift_driver::DriverLoadResult Source() {
		auto result = FakeOdbcDriver::Source();
		if (result.ok()) {
			std::lock_guard<std::mutex> guard(lock);
			target = result.driver->api;
			wrapped = *result.driver;
			wrapped.api.Fetch = &SuccessForNoData;
			result.driver = &wrapped;
		}
		return result;
	}

private:
	static SQLRETURN SQL_API SuccessForNoData(SQLHSTMT statement) {
		// The fake has already set the rows fetched to 0.
		const auto rc = target.Fetch(statement);
		return rc == SQL_NO_DATA ? SQL_SUCCESS : rc;
	}

	static inline std::mutex lock;
	static inline redshift_driver::OdbcApi target;
	static inline redshift_driver::LoadedDriver wrapped;
};

//! A driver source over the fake that reports column 1's bound length as 4 bytes when it is not NULL,
//! however wide its C type: a native number's length the client takes from its binding instead.
class ShortFirstLength {
public:
	static redshift_driver::DriverLoadResult Source() {
		auto result = FakeOdbcDriver::Source();
		if (result.ok()) {
			std::lock_guard<std::mutex> guard(lock);
			target = result.driver->api;
			wrapped = *result.driver;
			wrapped.api.BindCol = &RecordingBindCol;
			wrapped.api.Fetch = &ShortenedFetch;
			first_length = nullptr;
			result.driver = &wrapped;
		}
		return result;
	}

private:
	static SQLRETURN SQL_API RecordingBindCol(SQLHSTMT statement, SQLUSMALLINT column, SQLSMALLINT c_type,
	                                          SQLPOINTER value, SQLLEN capacity, SQLLEN *length) {
		if (column == 1) {
			std::lock_guard<std::mutex> guard(lock);
			first_length = length;
		}
		return target.BindCol(statement, column, c_type, value, capacity, length);
	}
	static SQLRETURN SQL_API ShortenedFetch(SQLHSTMT statement) {
		const auto rc = target.Fetch(statement);
		std::lock_guard<std::mutex> guard(lock);
		if (first_length && *first_length != SQL_NULL_DATA) {
			*first_length = 4;
		}
		return rc;
	}

	static inline std::mutex lock;
	static inline redshift_driver::OdbcApi target;
	static inline redshift_driver::LoadedDriver wrapped;
	static inline SQLLEN *first_length = nullptr;
};

//! A driver source over the fake whose SQLGetData reports a native number's length as 4 bytes when
//! it is not NULL, however wide its C type, as ShortFirstLength does for a bound one.
class ShortGetDataLength {
public:
	static redshift_driver::DriverLoadResult Source() {
		auto result = FakeOdbcDriver::Source();
		if (result.ok()) {
			std::lock_guard<std::mutex> guard(lock);
			target = result.driver->api;
			wrapped = *result.driver;
			wrapped.api.GetData = &ShortenedGetData;
			result.driver = &wrapped;
		}
		return result;
	}

private:
	static SQLRETURN SQL_API ShortenedGetData(SQLHSTMT statement, SQLUSMALLINT column, SQLSMALLINT c_type,
	                                          SQLPOINTER value, SQLLEN capacity, SQLLEN *length) {
		const auto rc = target.GetData(statement, column, c_type, value, capacity, length);
		if (c_type == SQL_C_SBIGINT && length && *length != SQL_NULL_DATA) {
			*length = 4;
		}
		return rc;
	}

	static inline std::mutex lock;
	static inline redshift_driver::OdbcApi target;
	static inline redshift_driver::LoadedDriver wrapped;
};

//! The line logged for SQL_GETDATA_EXTENSIONS, driver 2.3.0.0's 0xb unless a test sets another.
const char *const GETDATA_EXTENSIONS_LINE = "Redshift driver's SQL_GETDATA_EXTENSIONS: 0xb";

//! How many of `fixture`'s logged lines are `line`.
size_t LinesLogged(const FakeDriverFixture &fixture, const std::string &line) {
	const auto lines = fixture.log_sink->Lines();
	return NumericCast<size_t>(std::count(lines.begin(), lines.end(), line));
}

//! Keeps each SQL_GETDATA_EXTENSIONS line, marked when another thread could not take the process
//! state's lock while it was logged: a sink call can close the DuckDB database on its thread, so
//! the line must be logged after the lock is released.
class LockProbingSink : public RedshiftLogSink {
public:
	void Debug(const std::string &message) noexcept override {
		if (message.rfind("Redshift driver's SQL_GETDATA_EXTENSIONS", 0) != 0) {
			return;
		}
		bool taken = false;
		std::thread probe([&taken] {
			auto &lock = CurrentOdbcProcessState()->lock;
			taken = lock.try_lock();
			if (taken) {
				lock.unlock();
			}
		});
		probe.join();
		lock_guard<mutex> guard(lock);
		lines.push_back(taken ? message : message + " [logged under the process state's lock]");
	}
	std::vector<std::string> Lines() const {
		lock_guard<mutex> guard(lock);
		return lines;
	}

private:
	mutable mutex lock;
	std::vector<std::string> lines;
};

//! IntegerThenText with its VARCHAR scripted to report no total.
FakeResultSet IntegerThenTextOfNoTotal(const std::vector<std::vector<std::string>> &rows) {
	auto result = IntegerThenText(rows);
	result.columns[1].report_no_total = true;
	return result;
}

} // namespace

TEST_CASE("Each bounded type binds at the width its type sets, whatever size the driver reports", "[odbc]") {
	struct Case {
		RedshiftTypeId type;
		int32_t precision;
		SQLULEN reported;
		SQLSMALLINT c_type;
		SQLLEN width;
	};
	// The reported sizes are the pinned driver's (its getSize in rsutil.c, measured against the
	// driver); each width counts the NUL. The numbers are bound as text.
	const std::vector<Case> cases {
	    {RedshiftTypeId::BOOLEAN, 0, 1, SQL_C_CHAR, 33},      {RedshiftTypeId::SMALLINT, 0, 5, SQL_C_CHAR, 21},
	    {RedshiftTypeId::INTEGER, 0, 10, SQL_C_CHAR, 21},     {RedshiftTypeId::BIGINT, 0, 19, SQL_C_CHAR, 21},
	    {RedshiftTypeId::FLOAT, 0, 7, SQL_C_CHAR, 33},        {RedshiftTypeId::DOUBLE, 0, 15, SQL_C_CHAR, 33},
	    {RedshiftTypeId::DECIMAL, 38, 38, SQL_C_CHAR, 42},    {RedshiftTypeId::DECIMAL, 3, 3, SQL_C_CHAR, 7},
	    {RedshiftTypeId::DATE, 0, 10, SQL_C_CHAR, 14},        {RedshiftTypeId::TIME, 0, 8, SQL_C_CHAR, 16},
	    {RedshiftTypeId::TIMETZ, 0, 8, SQL_C_CHAR, 25},       {RedshiftTypeId::TIMESTAMP, 0, 26, SQL_C_CHAR, 30},
	    {RedshiftTypeId::TIMESTAMPTZ, 0, 26, SQL_C_CHAR, 39}, {RedshiftTypeId::VARCHAR, 0, 300, SQL_C_CHAR, 301},
	    {RedshiftTypeId::CHAR, 0, 1024, SQL_C_CHAR, 1025},
	};
	for (idx_t index = 0; index < cases.size(); index++) {
		const auto &bound = cases[index];
		RedshiftColumn column;
		column.type = bound.type;
		column.precision = bound.precision;
		INFO("case " << index << ", reported " << bound.reported);
		const auto binding = BindingOf(column, bound.reported, NumberBinding::TEXT);
		CHECK(binding.c_type == bound.c_type);
		CHECK(binding.width == bound.width);
	}
}

TEST_CASE("A number bound natively takes its C type's width, and every other type its text binding", "[odbc]") {
	struct Case {
		RedshiftTypeId type;
		SQLSMALLINT c_type;
		SQLLEN width;
	};
	const std::vector<Case> cases {
	    {RedshiftTypeId::BOOLEAN, SQL_C_BIT, 1},     {RedshiftTypeId::SMALLINT, SQL_C_SBIGINT, 8},
	    {RedshiftTypeId::INTEGER, SQL_C_SBIGINT, 8}, {RedshiftTypeId::BIGINT, SQL_C_SBIGINT, 8},
	    {RedshiftTypeId::FLOAT, SQL_C_DOUBLE, 8},    {RedshiftTypeId::DOUBLE, SQL_C_DOUBLE, 8},
	    {RedshiftTypeId::DATE, SQL_C_CHAR, 14},
	};
	for (idx_t index = 0; index < cases.size(); index++) {
		const auto &bound = cases[index];
		RedshiftColumn column;
		column.type = bound.type;
		INFO("case " << index);
		const auto binding = BindingOf(column, 10, NumberBinding::NATIVE);
		CHECK(binding.c_type == bound.c_type);
		CHECK(binding.width == bound.width);
	}
}

TEST_CASE("A character column reported with size 0, or longer than 1024 bytes, is long", "[odbc]") {
	for (const auto type : {RedshiftTypeId::VARCHAR, RedshiftTypeId::CHAR}) {
		RedshiftColumn column;
		column.type = type;
		CHECK(BindingOf(column, 0, DEFAULT_NUMBER_BINDING).IsLong());
		CHECK(BindingOf(column, 1025, DEFAULT_NUMBER_BINDING).IsLong());
		CHECK(BindingOf(column, 65535, DEFAULT_NUMBER_BINDING).IsLong());
		CHECK_FALSE(BindingOf(column, 1024, DEFAULT_NUMBER_BINDING).IsLong());
	}
	RedshiftColumn varbyte;
	varbyte.type = RedshiftTypeId::BLOB;
	CHECK(BindingOf(varbyte, 10, DEFAULT_NUMBER_BINDING).IsLong());
	CHECK(BindingOf(varbyte, 10, DEFAULT_NUMBER_BINDING).c_type == SQL_C_BINARY);
}

TEST_CASE("A text binding holds its width less the NUL, and a native one its whole width", "[odbc]") {
	RedshiftColumn varchar;
	varchar.type = RedshiftTypeId::VARCHAR;
	RedshiftColumn integer;
	integer.type = RedshiftTypeId::INTEGER;

	CHECK(BindingOf(varchar, 300, DEFAULT_NUMBER_BINDING).Holds() == 300);
	CHECK(BindingOf(integer, 10, NumberBinding::TEXT).Holds() == 20);
	CHECK(BindingOf(integer, 10, NumberBinding::NATIVE).Holds() == 8);
}

TEST_CASE("A column with no carrier has no binding", "[odbc]") {
#ifdef DUCKDB_CRASH_ON_ASSERT
	return; // InternalException aborts in this build
#endif
	RedshiftColumn unknown;

	CHECK_THROWS_AS(BindingOf(unknown, 10, DEFAULT_NUMBER_BINDING), InternalException);
	CHECK_THROWS_WITH(BindingOf(unknown, 10, DEFAULT_NUMBER_BINDING), Catch::Contains("no carrier reached the fetch"));
}

// Each row counts its buffers, one length indicator and one RedshiftValue per column, and its status.
TEST_CASE("The row array holds 2048 rows, fewer for a wide row, and at least 1", "[odbc]") {
	const ColumnBinding integer {SQL_C_SBIGINT, 8};
	const ColumnBinding varchar {SQL_C_CHAR, 1025};

	CHECK(RowArraySize({integer}) == 2048);
	CHECK(RowArraySize(std::vector<ColumnBinding>(16, varchar)) == WIDE_ROWS);
	// More than 16 MiB a row, which no Redshift row reaches.
	CHECK(RowArraySize(std::vector<ColumnBinding>(17000, varchar)) == 1);
	// A row one byte past half a set's share only with its status counted fits once, not twice.
	const auto half = NumericCast<SQLLEN>(MAX_BATCH_BYTES / RING_SETS / 2 - sizeof(SQLLEN) - sizeof(RedshiftValue) - 1);
	CHECK(RowArraySize({ColumnBinding {SQL_C_CHAR, half}}) == 1);
}

// A bound batch is the whole row array, so its RedshiftValues count against the budget: 1600 CHAR(1)
// columns, counting their buffers alone, hold about 23 MB of them in a set of 262 rows. Their text
// is not counted.
TEST_CASE("A bound batch's RedshiftValues stay within the budget, however narrow its cells", "[odbc]") {
	RedshiftColumn column;
	column.type = RedshiftTypeId::CHAR;
	const auto narrow = BindingOf(column, 1, DEFAULT_NUMBER_BINDING);
	const auto rows = RowArraySize(std::vector<ColumnBinding>(1600, narrow));

	CHECK(rows * 1600 * sizeof(RedshiftValue) <= MAX_BATCH_BYTES);
}

TEST_CASE("A result of bounded columns fetches 2048 rows a batch over one bound row array", "[odbc]") {
	FakeDriverFixture fixture;
	const auto read = ReadQuery(fixture, Counting(2049));

	CHECK(read.sizes == std::vector<idx_t> {2048, 1});
	REQUIRE(read.rows.size() == 2049);
	CHECK(read.rows[0][0].integer == 0);
	CHECK(read.rows[2048][0].integer == 2048);
	// SQL_ATTR_ROW_ARRAY_SIZE is 27 and SQL_ATTR_ROW_BIND_OFFSET_PTR 23; the array is set and bound
	// once, and each batch is one fetch, the third the one that reports the end.
	CHECK(fixture.DetailsOf("SQLSetStmtAttr") == std::vector<std::string> {"27=2048", "26", "25", "23"});
	CHECK(fixture.DetailsOf("SQLBindCol") == std::vector<std::string> {"1 1 21"});
	CHECK(fixture.CallsTo("SQLFetch") == 3);
}

TEST_CASE("A partial final batch returns exactly the rows fetched, in order", "[odbc]") {
	FakeDriverFixture fixture;
	const auto read = ReadQuery(fixture, Counting(2051));

	CHECK(read.sizes == std::vector<idx_t> {2048, 3});
	REQUIRE(read.rows.size() == 2051);
	for (idx_t row = 0; row < read.rows.size(); row++) {
		REQUIRE(read.rows[row][0].integer == NumericCast<int64_t>(row));
	}
}

TEST_CASE("A wide row gets a smaller row array rather than more memory", "[odbc]") {
	FakeDriverFixture fixture;
	const auto read = ReadQuery(fixture, Wide(16, WIDE_ROWS + 1));

	CHECK(read.sizes == std::vector<idx_t> {WIDE_ROWS, 1});
	CHECK(fixture.DetailsOf("SQLSetStmtAttr").at(0) == "27=" + std::to_string(WIDE_ROWS));
}

// The row is the rowset's last, so its records are the ones the driver keeps.
TEST_CASE("A row the driver reports in error fails its batch with that row's records", "[odbc]") {
	FakeDriverFixture fixture;
	auto result = Counting(3);
	result.row_status = {SQL_ROW_SUCCESS, SQL_ROW_SUCCESS, SQL_ROW_ERROR};
	result.row_diags[2] = {{"22012", 0, "division by zero", {}}};

	const auto batch = FailFirstBatch(fixture, result);

	CHECK((Thrown {batch.error.Type(), batch.error.RawMessage()}) ==
	      Thrown {ExceptionType::EXECUTOR, "Redshift could not return the rest of the result: division by zero\n"
	                                       "SQLSTATE: 22012"});
	CHECK(batch.rows_left == 0);
}

// A later row's read clears the failed row's records, as the driver's does, so the row status alone
// fails the batch.
TEST_CASE("A row in error before the rowset's last fails its batch with no record left", "[odbc]") {
	FakeDriverFixture fixture;
	auto result = Counting(3);
	result.row_status = {SQL_ROW_SUCCESS, SQL_ROW_ERROR, SQL_ROW_SUCCESS};
	result.row_diags[1] = {{"22012", 0, "division by zero", {}}};

	const auto batch = FailFirstBatch(fixture, result);

	CHECK((Thrown {batch.error.Type(), batch.error.RawMessage()}) ==
	      Thrown {ExceptionType::EXECUTOR, "Redshift could not return the rest of the result, and the driver returned "
	                                       "no diagnostic record. The statement may have run; check before running it "
	                                       "again."});
	CHECK(batch.rows_left == 0);
}

TEST_CASE("A row error on a connection the driver reports dead gives the transport failure", "[odbc]") {
	FakeDriverFixture fixture;
	auto result = Counting(2);
	result.row_status = {SQL_ROW_ERROR, SQL_ROW_SUCCESS};
	result.row_diags[0] = {{"22012", 0, "division by zero", {}}};

	const auto batch = FailFirstBatch(fixture, result, [&] { fixture.fake.SetConnectionDead(true); });

	CHECK(batch.error.Type() == ExceptionType::IO);
	CHECK(StringUtil::StartsWith(batch.error.RawMessage(), "The connection to Redshift at "));
	CHECK(batch.rows_left == 0);
}

TEST_CASE("A varchar(300) value of 300 bytes returns exactly from its 301-byte buffer", "[odbc]") {
	FakeDriverFixture fixture;
	const auto read = ReadQuery(fixture, OneColumn(VarcharColumn("c", 300), {Text300(300, false)}));

	REQUIRE(read.rows.size() == 1);
	CHECK(read.rows[0][0].text == Text300(300, false));
	CHECK(fixture.DetailsOf("SQLBindCol") == std::vector<std::string> {"1 1 301"});
}

TEST_CASE("A varchar(300) value of 301 bytes fails its batch with 01004, returning no row of it", "[odbc]") {
	FakeDriverFixture fixture;
	const auto batch = FailFirstBatch(fixture, OneColumn(VarcharColumn("c", 300), {"ok", Text300(301, false)}));

	CHECK((Thrown {batch.error.Type(), batch.error.RawMessage()}) ==
	      Thrown {ExceptionType::EXECUTOR, LONGER_THAN_BUFFER});
	CHECK(batch.error.ExtraInfo().at("sqlstates") == "01004");
	CHECK(batch.rows_left == 0);
}

TEST_CASE("A 4-byte character ending exactly at a varchar(300)'s capacity returns exactly", "[odbc]") {
	FakeDriverFixture fixture;
	const auto read = ReadQuery(fixture, OneColumn(VarcharColumn("c", 300), {Text300(296, true)}));

	REQUIRE(read.rows.size() == 1);
	CHECK(read.rows[0][0].text == Text300(296, true));
}

TEST_CASE("A 4-byte character ending a byte past a varchar(300)'s capacity fails its batch, none of it returned",
          "[odbc]") {
	FakeDriverFixture fixture;
	const auto batch = FailFirstBatch(fixture, OneColumn(VarcharColumn("c", 300), {Text300(297, true)}));

	CHECK((Thrown {batch.error.Type(), batch.error.RawMessage()}) ==
	      Thrown {ExceptionType::EXECUTOR, LONGER_THAN_BUFFER});
	CHECK(batch.error.ExtraInfo().at("sqlstates") == "01004");
	CHECK(batch.rows_left == 0);
}

TEST_CASE("A fetch that warns 01004 fails its batch even when every length fits", "[odbc]") {
	FakeDriverFixture fixture;
	auto result = OneColumn(VarcharColumn("c", 10), {"a", "b"});
	SECTION("every column bound") {
	}
	SECTION("a long column after the bound one") {
		result.columns.push_back(VarcharColumn("d", 0));
		result.rows = {{"a", "x"}, {"b", "y"}};
	}
	const auto batch = FailFirstBatch(fixture, result, [&] {
		fixture.fake.WarnCall("SQLFetch", NumericCast<int>(fixture.CallsTo("SQLFetch")) + 1,
		                      {{"01004", 0, "String data, right truncated", {}}});
	});

	CHECK((Thrown {batch.error.Type(), batch.error.RawMessage()}) ==
	      Thrown {ExceptionType::EXECUTOR,
	              "the Redshift driver cut a value to fit this extension's buffer for its column" + NOT_RETURNED +
	                  "\nSQLSTATE: HY000"});
	CHECK(batch.error.ExtraInfo().at("sqlstates") == "01004");
	CHECK(batch.rows_left == 0);
}

TEST_CASE("A fetch that warns with another SQLSTATE still returns its batch", "[odbc]") {
	FakeDriverFixture fixture;
	fixture.fake.AddResult(QUERY, {OneColumn(VarcharColumn("c", 10), {"a", "b"})});
	auto connection = fixture.OpenConnection();
	auto result = redshift_test::ExecuteStatement(*connection, QUERY, {});
	fixture.fake.WarnCall("SQLFetch", NumericCast<int>(fixture.CallsTo("SQLFetch")) + 1,
	                      {{"01000", 0, "General warning", {}}});

	const ReadBatches read(*result);
	CHECK(read.sizes == std::vector<idx_t> {2});
}

TEST_CASE("A bound cell reporting SQL_NO_TOTAL fails its batch, with the fetch's 01004", "[odbc]") {
	FakeDriverFixture fixture;
	auto column = VarcharColumn("c", 10);
	column.report_no_total = true;
	auto result = OneColumn(column, {std::string(11, 'n')});
	SECTION("every column bound") {
	}
	SECTION("a long column after the bound one") {
		result.columns.push_back(VarcharColumn("d", 0));
		result.rows = {{std::string(11, 'n'), "x"}};
	}
	const auto batch = FailFirstBatch(fixture, result);

	CHECK((Thrown {batch.error.Type(), batch.error.RawMessage()}) ==
	      Thrown {ExceptionType::EXECUTOR, "the Redshift driver did not report the length of a value in column 1" +
	                                           NOT_RETURNED + "\nSQLSTATE: HY000"});
	CHECK(batch.error.ExtraInfo().at("sqlstates") == "01004");
	CHECK(batch.rows_left == 0);
}

TEST_CASE("A value in column 2 longer than its buffer fails its batch, naming column 2", "[odbc]") {
	FakeDriverFixture fixture;
	const auto batch = FailFirstBatch(fixture, IntegerThenText({{"1", std::string(11, 'v')}}));

	CHECK((Thrown {batch.error.Type(), batch.error.RawMessage()}) ==
	      Thrown {ExceptionType::EXECUTOR, "Redshift returned a value in column 2 longer than this extension's buffer "
	                                       "for it" +
	                                           NOT_RETURNED + "\nSQLSTATE: HY000"});
	CHECK(batch.error.ExtraInfo().at("sqlstates") == "01004");
	CHECK(batch.rows_left == 0);
}

TEST_CASE("A value in column 2 of unknown length fails its batch, naming column 2", "[odbc]") {
	FakeDriverFixture fixture;
	const auto batch = FailFirstBatch(fixture, IntegerThenTextOfNoTotal({{"1", std::string(11, 'v')}}));

	CHECK((Thrown {batch.error.Type(), batch.error.RawMessage()}) ==
	      Thrown {ExceptionType::EXECUTOR, "the Redshift driver did not report the length of a value in column 2" +
	                                           NOT_RETURNED + "\nSQLSTATE: HY000"});
	CHECK(batch.rows_left == 0);
}

// A later row's read clears the cut row's 01004, as the driver's does, so only the length indicator
// shows the cut, and the refusal has no SQLSTATE.
TEST_CASE("A cut value whose 01004 a later row cleared fails its batch from its length alone", "[odbc]") {
	FakeDriverFixture fixture;
	const auto batch = FailFirstBatch(fixture, IntegerThenText({{"1", std::string(11, 'v')}, {"2", "fits"}}));

	CHECK((Thrown {batch.error.Type(), batch.error.RawMessage()}) ==
	      Thrown {ExceptionType::EXECUTOR,
	              "Redshift returned a value in column 2 longer than this extension's buffer for it" + NOT_RETURNED});
	CHECK(CarriesNoSqlState(batch.error));
	CHECK(batch.rows_left == 0);
}

TEST_CASE("A value of unknown length whose 01004 a later row cleared fails its batch with no SQLSTATE", "[odbc]") {
	FakeDriverFixture fixture;
	const auto batch = FailFirstBatch(fixture, IntegerThenTextOfNoTotal({{"1", std::string(11, 'v')}, {"2", "fits"}}));

	CHECK((Thrown {batch.error.Type(), batch.error.RawMessage()}) ==
	      Thrown {ExceptionType::EXECUTOR,
	              "the Redshift driver did not report the length of a value in column 2" + NOT_RETURNED});
	CHECK(CarriesNoSqlState(batch.error));
	CHECK(batch.rows_left == 0);
}

// Another warning on the fetch says nothing about the cut, so it is not the refusal's SQLSTATE.
TEST_CASE("A cut found from its length alone carries no other warning's SQLSTATE", "[odbc]") {
	FakeDriverFixture fixture;
	const auto batch = FailFirstBatch(fixture, IntegerThenText({{"1", std::string(11, 'v')}, {"2", "fits"}}), [&] {
		fixture.fake.WarnCall("SQLFetch", NumericCast<int>(fixture.CallsTo("SQLFetch")) + 1,
		                      {{"01000", 0, "General warning", {}}});
	});

	CHECK((Thrown {batch.error.Type(), batch.error.RawMessage()}) ==
	      Thrown {ExceptionType::EXECUTOR,
	              "Redshift returned a value in column 2 longer than this extension's buffer for it" + NOT_RETURNED});
	CHECK(CarriesNoSqlState(batch.error));
}

// Scripted at driver 2.3.0.0's sizes, each too small for its value: 10, 8, 8, 26, 26 and the
// precision.
TEST_CASE("Temporal and DECIMAL edge values return exactly at the widths their types set", "[odbc]") {
	FakeDriverFixture fixture;
	FakeResultSet result;
	result.columns = {Column("DATE", SQL_TYPE_DATE, 10),
	                  Column("TIME", SQL_TYPE_TIME, 8),
	                  Column("TIMETZ", SQL_TYPE_TIME, 8),
	                  Column("TIMESTAMP", SQL_TYPE_TIMESTAMP, 26),
	                  Column("TIMESTAMPTZ", SQL_TYPE_TIMESTAMP, 26),
	                  Column("NUMERIC", SQL_NUMERIC, 38, 10),
	                  Column("NUMERIC", SQL_NUMERIC, 16, 16),
	                  Column("NUMERIC", SQL_NUMERIC, 37, 37)};
	for (idx_t i = 0; i < result.columns.size(); i++) {
		result.columns[i].name = "c" + std::to_string(i);
	}
	// A stored negative NUMERIC(37,37) fills its 40 bytes, which is also its longest text.
	const std::string widest_numeric_37_37 = "-0.9999999999999999999999999999999999999";
	const std::vector<std::string> edges {"4713-01-01 BC",
	                                      "23:59:59.999999",
	                                      "23:59:59.999999+15:59:59",
	                                      "4713-01-01 00:00:00.123456 BC",
	                                      "4713-01-01 00:00:00.123456-04:56:02 BC",
	                                      "-9999999999999999999999999999.9999999999",
	                                      "-0.9999999999999999",
	                                      widest_numeric_37_37};
	// The strings Redshift returned for a stored value of these types (DATE, TIMETZ, TIMESTAMPTZ,
	// NUMERIC(p,p)), the second row's elsewhere. The TIMETZ is under the setup's UTC, the TIMESTAMPTZ
	// under New York, whose pre-1883 offset prints without its seconds: 35 of its 38 bytes.
	const std::vector<std::string> stored_new_york {"5874897-12-31",
	                                                "00:00:00",
	                                                "08:00:00.999999+00",
	                                                "2024-03-15 10:00:00",
	                                                "4713-01-01 00:00:00.123456-04:56 BC",
	                                                "0.0000000000",
	                                                "-0.9999999999999999",
	                                                widest_numeric_37_37};
	// The same stored TIMESTAMPTZ under `Asia/Kolkata`, +05:53 there and so 35 bytes as well, and
	// under the setup's UTC.
	constexpr idx_t timestamptz_column = 4;
	auto stored_kolkata = stored_new_york;
	stored_kolkata[timestamptz_column] = "4713-01-01 10:49:30.123456+05:53 BC";
	auto stored_utc = stored_new_york;
	stored_utc[timestamptz_column] = "4713-01-01 04:56:02.123456+00 BC";
	result.rows = {edges,
	               {"5874897-12-31", "00:00:00", "00:00:00+00", "2024-03-15 10:00:00", "2024-03-15 10:00:00+00",
	                "0.0000000000", "0.0000000000000000", "0.0000000000000000000000000000000000000"},
	               stored_new_york,
	               stored_kolkata,
	               stored_utc};

	const auto read = ReadQuery(fixture, result);

	REQUIRE(read.rows.size() == 5);
	for (idx_t i = 0; i < edges.size(); i++) {
		CHECK(read.rows[0][i].text == edges[i]);
		CHECK(read.rows[2][i].text == stored_new_york[i]);
		CHECK(read.rows[3][i].text == stored_kolkata[i]);
		CHECK(read.rows[4][i].text == stored_utc[i]);
	}
	CHECK(read.rows[1][0].text == "5874897-12-31");
	CHECK(fixture.DetailsOf("SQLBindCol") ==
	      std::vector<std::string> {"1 1 14", "2 1 16", "3 1 25", "4 1 30", "5 1 39", "6 1 42", "7 1 20", "8 1 41"});
}

TEST_CASE("A character column reported with size 0 is read with SQLGetData, not bound", "[odbc]") {
	FakeDriverFixture fixture;
	const auto text = std::string(2000, 't');
	const auto read = ReadQuery(fixture, OneColumn(VarcharColumn("c", 0), {text}));

	REQUIRE(read.rows.size() == 1);
	CHECK(read.rows[0][0].text == text);
	CHECK(fixture.DetailsOf("SQLBindCol").empty());
	CHECK(fixture.DetailsOf("SQLGetData") == std::vector<std::string> {"1 1"});
	// Fetched over a row array of 1, with the rows fetched and row status still read.
	CHECK(fixture.DetailsOf("SQLSetStmtAttr") == std::vector<std::string> {"27=1", "26", "25"});
}

TEST_CASE("A long column between bounded ones: those before it are bound, it and the rest read in order", "[odbc]") {
	FakeDriverFixture fixture;
	FakeResultSet result;
	result.columns = {Named(Integer(), "a"), VarcharColumn("b", 0), VarcharColumn("c", 10), Named(Integer(), "e")};
	result.rows = {{"1", "first", "x", "10"}, {"2", "second", "y", "20"}};

	const auto read = ReadQuery(fixture, result);

	REQUIRE(read.rows.size() == 2);
	for (idx_t row = 0; row < 2; row++) {
		CHECK(read.rows[row][0].integer == NumericCast<int64_t>(row + 1));
		CHECK(read.rows[row][1].text == result.rows[row][1]);
		CHECK(read.rows[row][2].text == result.rows[row][2]);
		CHECK(read.rows[row][3].integer == NumericCast<int64_t>(10 * (row + 1)));
	}
	// SQL_ATTR_ROW_ARRAY_SIZE is 27: SQLGetData, with no SQL_GD_BLOCK, reads only a row array of 1.
	CHECK(fixture.DetailsOf("SQLSetStmtAttr") == std::vector<std::string> {"27=1", "26", "25"});
	// The INTEGER before the long column is bound as text, SQL_C_CHAR (1), at 21 bytes.
	CHECK(fixture.DetailsOf("SQLBindCol") == std::vector<std::string> {"1 1 21"});
	// The bounded VARCHAR and the INTEGER after the long column are read too, both as SQL_C_CHAR.
	CHECK(fixture.DetailsOf("SQLGetData") == std::vector<std::string> {"2 1", "3 1", "4 1", "2 1", "3 1", "4 1"});
	CHECK(fixture.CallsTo("SQLFetch") == 3);
}

TEST_CASE("A long text value after a bound column arrives whole, with or without SQL_NO_TOTAL", "[odbc]") {
	FakeDriverFixture fixture;
	auto text = VarcharColumn("d", 0);
	SECTION("each part reporting the length still to come") {
	}
	SECTION("each cut part reporting SQL_NO_TOTAL") {
		text.report_no_total = true;
	}
	std::string value;
	for (idx_t i = 0; i < 10000; i++) {
		value.push_back(NumericCast<char>('a' + i % 26));
	}

	const auto read = ReadQuery(fixture, IntegerThenLong(text, {{"7", value}}));

	REQUIRE(read.rows.size() == 1);
	CHECK(read.rows[0][0].integer == 7);
	CHECK(read.rows[0][1].text == value);
	// Parts of 4095 bytes and the NUL: 4095, 4095 and the last 1810.
	CHECK(fixture.DetailsOf("SQLGetData") == std::vector<std::string> {"2 1", "2 1", "2 1"});
}

TEST_CASE("A VARBYTE after a bound column is read as SQL_C_BINARY, every byte exact", "[odbc]") {
	FakeDriverFixture fixture;
	const std::string bytes("\x00\xff\x10", 3);
	const std::string zeros(5000, '\0');

	const auto read = ReadQuery(fixture, IntegerThenLong(Varbyte(), {{"1", bytes}, {"2", zeros}, {"3", ""}}));

	REQUIRE(read.rows.size() == 3);
	for (const auto &row : read.rows) {
		CHECK(row[1].kind == RedshiftValueKind::BLOB);
	}
	CHECK(read.rows[0][1].text == bytes);
	CHECK(read.rows[1][1].text == zeros);
	CHECK(read.rows[2][1].text.empty());
	// SQL_C_BINARY is -2; the 5000 zeros take two reads, as a binary part keeps no byte for a NUL.
	CHECK(fixture.DetailsOf("SQLGetData") == std::vector<std::string> {"2 -2", "2 -2", "2 -2", "2 -2"});
	CHECK(fixture.DetailsOf("SQLBindCol") == std::vector<std::string> {"1 1 21"});
}

TEST_CASE("Values of one part and of several, NULL and empty, keep their cells and order in a row", "[odbc]") {
	FakeDriverFixture fixture;
	FakeResultSet result;
	for (idx_t column = 0; column < 4; column++) {
		result.columns.push_back(VarcharColumn("c" + std::to_string(column), 0));
	}
	// 4095 bytes is one part with its NUL and 4096 is two, so each row mixes both kinds of read with a
	// NULL and an empty value, in a different order in each row.
	const auto one = std::string(4095, 'a');
	const auto two = std::string(4096, 'b');
	const auto three = std::string(9000, 'c');
	result.rows = {{one, two, "", "x"}, {"", three, "y", one}, {two, "z", three, ""}};
	result.nulls = {{false, false, false, true}, {true, false, false, false}, {false, false, false, false}};

	const auto read = ReadQuery(fixture, result);

	REQUIRE(read.rows.size() == 3);
	for (idx_t row = 0; row < 3; row++) {
		for (idx_t column = 0; column < 4; column++) {
			const auto &value = read.rows[row][column];
			if (result.nulls[row][column]) {
				CHECK(value.kind == RedshiftValueKind::NULL_VALUE);
				continue;
			}
			CHECK(value.kind == RedshiftValueKind::TEXT);
			CHECK(value.text == result.rows[row][column]);
		}
	}
}

TEST_CASE("A value cut in a bound column before a long one fails its batch with 01004", "[odbc]") {
	FakeDriverFixture fixture;
	FakeResultSet result;
	result.columns = {VarcharColumn("c", 10), VarcharColumn("d", 0)};
	result.rows = {{"ok", "x"}, {std::string(11, 'a'), "y"}};

	const auto batch = FailFirstBatch(fixture, result);

	CHECK((Thrown {batch.error.Type(), batch.error.RawMessage()}) ==
	      Thrown {ExceptionType::EXECUTOR, LONGER_THAN_BUFFER});
	CHECK(batch.error.ExtraInfo().at("sqlstates") == "01004");
	CHECK(batch.rows_left == 0);
}

TEST_CASE("A driver reporting SQL_GD_BLOCK still has a long column read over a row array of 1", "[odbc]") {
	FakeDriverFixture fixture;
	fixture.fake.SetInfo(SQL_GETDATA_EXTENSIONS, SQL_GD_ANY_COLUMN | SQL_GD_ANY_ORDER | SQL_GD_BLOCK | SQL_GD_BOUND);

	const auto read = ReadQuery(fixture, IntegerThenLong(VarcharColumn("d", 0), {{"1", "a"}, {"2", "b"}, {"3", "c"}}));

	CHECK(read.sizes == std::vector<idx_t> {3});
	CHECK(fixture.DetailsOf("SQLSetStmtAttr").at(0) == "27=1");
	CHECK(fixture.CallsTo("SQLFetch") == 4);
	CHECK(LinesLogged(fixture, "Redshift driver's SQL_GETDATA_EXTENSIONS: 0xf") == 1);
}

TEST_CASE("SQL_GETDATA_EXTENSIONS is read once a process, by the first result with a long column", "[odbc]") {
	FakeDriverFixture fixture;
	const char *const LONG_QUERY = "SELECT d FROM t";
	fixture.fake.AddResult(QUERY, {Counting(1)});
	fixture.fake.AddResult(LONG_QUERY, {OneColumn(VarcharColumn("c", 0), {"x"})});
	auto first = fixture.OpenConnection();
	const ReadBatches bounded(*redshift_test::ExecuteStatement(*first, QUERY, {}));
	CHECK(bounded.rows.size() == 1);
	CHECK(fixture.CallsTo("SQLGetInfo") == 0);

	const ReadBatches long_first(*redshift_test::ExecuteStatement(*first, LONG_QUERY, {}));
	auto second = fixture.OpenConnection();
	const ReadBatches long_second(*redshift_test::ExecuteStatement(*second, LONG_QUERY, {}));
	CHECK(long_first.rows.size() + long_second.rows.size() == 2);

	// SQL_GETDATA_EXTENSIONS is 81.
	CHECK(fixture.DetailsOf("SQLGetInfo") == std::vector<std::string> {"81"});
	CHECK(LinesLogged(fixture, GETDATA_EXTENSIONS_LINE) == 1);
}

TEST_CASE("A failed SQL_GETDATA_EXTENSIONS read is tried again by the next result with a long column", "[odbc]") {
	FakeDriverFixture fixture;
	fixture.fake.FailCall("SQLGetInfo", 1, SQL_ERROR, {});
	fixture.fake.AddResult(QUERY, {OneColumn(VarcharColumn("c", 0), {"x"})});
	auto connection = fixture.OpenConnection();

	const ReadBatches failed(*redshift_test::ExecuteStatement(*connection, QUERY, {}));
	REQUIRE(failed.rows.size() == 1);
	CHECK(failed.rows[0][0].text == "x");
	CHECK(LinesLogged(fixture, "Redshift driver's SQL_GETDATA_EXTENSIONS could not be read; a long column is read "
	                           "one row a fetch either way") == 1);

	const ReadBatches retried(*redshift_test::ExecuteStatement(*connection, QUERY, {}));
	CHECK(retried.rows.size() == 1);
	CHECK(fixture.CallsTo("SQLGetInfo") == 2);
	CHECK(LinesLogged(fixture, GETDATA_EXTENSIONS_LINE) == 1);
}

TEST_CASE("The SQL_GETDATA_EXTENSIONS line is logged with the process state's lock released", "[odbc]") {
	FakeDriverFixture fixture;
	std::string line = GETDATA_EXTENSIONS_LINE;
	SECTION("a read value") {
	}
	SECTION("a failed read") {
		fixture.fake.FailCall("SQLGetInfo", 1, SQL_ERROR, {});
		line = "Redshift driver's SQL_GETDATA_EXTENSIONS could not be read; a long column is read one row a fetch "
		       "either way";
	}
	fixture.fake.AddResult(QUERY, {OneColumn(VarcharColumn("c", 0), {"x"})});
	auto sink = make_shared_ptr<LockProbingSink>();
	auto connection = OpenRedshiftConnection(redshift_test::UsEast1Config(), sink);

	const ReadBatches read(*redshift_test::ExecuteStatement(*connection, QUERY, {}));

	CHECK(read.rows.size() == 1);
	CHECK(sink->Lines() == std::vector<std::string> {line});
}

TEST_CASE("A NULL in each bound carrier arrives as NULL", "[odbc]") {
	FakeDriverFixture fixture;
	FakeResultSet result;
	result.columns = {Column("BOOL", SQL_BIT, 1), Integer(), Column("DOUBLE PRECISION", SQL_DOUBLE, 15),
	                  Column("NUMERIC", SQL_NUMERIC, 10, 2), VarcharColumn("c", 10)};
	for (idx_t i = 0; i < result.columns.size(); i++) {
		result.columns[i].name = "c" + std::to_string(i);
	}
	result.rows = {{"1", "1", "1", "1.00", "x"}};
	result.nulls = {{true, true, true, true, true}};

	const auto read = ReadQuery(fixture, result);

	REQUIRE(read.rows.size() == 1);
	for (const auto &value : read.rows[0]) {
		CHECK(value.kind == RedshiftValueKind::NULL_VALUE);
	}
	// Each column bound at its own width, so a reported size reaches its own column.
	CHECK(fixture.DetailsOf("SQLBindCol") ==
	      std::vector<std::string> {"1 1 33", "2 1 21", "3 1 33", "4 1 14", "5 1 11"});
}

TEST_CASE("A native number read after a long column arrives NULL where it is NULL, and the next rows exact", "[odbc]") {
	FakeDriverFixture fixture;
	SetNumberBindingForTesting(NumberBinding::NATIVE);
	FakeResultSet result;
	result.columns = {LongVarcharColumn("c"), Named(Integer(), "d")};
	result.rows = {{"a", "1"}, {"b", "2"}, {"c", "3"}, {"d", "4"}, {"e", "5"}};
	result.nulls = {{false, false}, {false, true}, {false, false}, {false, true}, {false, false}};

	const auto read = ReadQuery(fixture, result);

	REQUIRE(read.rows.size() == 5);
	for (idx_t row = 0; row < 5; row++) {
		CHECK(read.rows[row][0].text == result.rows[row][0]);
		if (row % 2 == 1) {
			CHECK(read.rows[row][1].kind == RedshiftValueKind::NULL_VALUE);
		} else {
			CHECK(read.rows[row][1].integer == NumericCast<int64_t>(row + 1));
		}
	}
	// SQL_C_SBIGINT is -25: the INTEGER is read natively with SQLGetData, after the long column.
	CHECK(fixture.DetailsOf("SQLGetData").at(1) == "2 -25");
}

TEST_CASE("A native number bound before a long column keeps its whole width, whatever length the driver reports",
          "[odbc]") {
	FakeDriverFixture fixture;
	// Each needs all 8 bytes.
	fixture.fake.AddResult(QUERY, {IntegerThenLong(LongVarcharColumn("d"),
	                                               {{"7", "a"}, {"-2", "b"}, {"1099511627776", "c"}, {"0", "e"}})});
	// After the source, which resets the process state the binding is part of.
	SetDriverSourceForTesting(&ShortFirstLength::Source);
	SetNumberBindingForTesting(NumberBinding::NATIVE);
	auto connection = fixture.OpenConnection();
	const ReadBatches read(*redshift_test::ExecuteStatement(*connection, QUERY, {}));

	REQUIRE(read.rows.size() == 4);
	CHECK(read.rows[0][0].integer == 7);
	CHECK(read.rows[1][0].integer == -2);
	CHECK(read.rows[2][0].integer == int64_t(1) << 40);
	CHECK(read.rows[3][0].integer == 0);
	CHECK(read.rows[3][1].text == "e");
}

TEST_CASE("A native number read after a long column keeps its whole width, whatever length the driver reports",
          "[odbc]") {
	FakeDriverFixture fixture;
	FakeResultSet result;
	result.columns = {LongVarcharColumn("c"), Named(Integer(), "d"), VarcharColumn("e", 10)};
	// Each needs all 8 bytes; a short stored length would fill the rest from the next cell's, in
	// every row, as the VARCHAR's follows it.
	result.rows = {{"a", "7", "x"}, {"b", "-2", "y"}, {"c", "1099511627776", "z"}, {"d", "0", "w"}};
	fixture.fake.AddResult(QUERY, {result});
	// After the source, which resets the process state the binding is part of.
	SetDriverSourceForTesting(&ShortGetDataLength::Source);
	SetNumberBindingForTesting(NumberBinding::NATIVE);
	auto connection = fixture.OpenConnection();
	const ReadBatches read(*redshift_test::ExecuteStatement(*connection, QUERY, {}));

	REQUIRE(read.rows.size() == 4);
	CHECK(read.rows[0][1].integer == 7);
	CHECK(read.rows[1][1].integer == -2);
	CHECK(read.rows[2][1].integer == int64_t(1) << 40);
	CHECK(read.rows[3][1].integer == 0);
	CHECK(read.rows[3][2].text == "w");
	// SQL_C_SBIGINT is -25: the INTEGER is read natively with SQLGetData, after the long column.
	CHECK(fixture.DetailsOf("SQLGetData").at(1) == "2 -25");
}

TEST_CASE("A fetch that ends the rows with SQL_SUCCESS and no row still frees the statement", "[odbc]") {
	FakeDriverFixture fixture;
	SECTION("bound") {
		fixture.fake.AddResult(QUERY, {Counting(3)});
	}
	SECTION("with a long column") {
		fixture.fake.AddResult(QUERY, {IntegerThenLong(LongVarcharColumn("d"), {{"1", "a"}, {"2", "b"}, {"3", "c"}})});
	}
	SetDriverSourceForTesting(&SuccessAtEnd::Source);
	auto connection = fixture.OpenConnection();
	const auto before = HeldFetchBytes();
	auto result = redshift_test::ExecuteStatement(*connection, QUERY, {});
	const auto frees = fixture.CallsTo("SQLFreeHandle");

	const ReadBatches read(*result);

	CHECK(read.rows.size() == 3);
	CHECK(fixture.CallsTo("SQLFreeHandle") == frees + 1);
	CHECK(HeldFetchBytes() == before);
}

TEST_CASE("A row array the driver refuses to set up fails the batch as a fetch failure", "[odbc]") {
	FakeDriverFixture fixture;
	FakeResultSet result = Counting(1);
	const char *entry_point = "SQLSetStmtAttr";
	int nth = 1;
	SECTION("the row-array size") {
	}
	SECTION("the rows-fetched pointer") {
		nth = 2;
	}
	SECTION("the row-status pointer") {
		nth = 3;
	}
	SECTION("the bind-offset pointer that selects each set") {
		nth = 4;
	}
	SECTION("a column's binding") {
		result.columns.push_back(VarcharColumn("c", 10));
		result.columns.back().name = "d";
		result.rows[0].push_back("x");
		entry_point = "SQLBindCol";
		nth = 2;
	}

	const auto batch = FailFirstBatch(fixture, result, [&] {
		fixture.fake.FailCall(entry_point, NumericCast<int>(fixture.CallsTo(entry_point)) + nth, SQL_ERROR,
		                      {{"HY000", 0, "Memory allocation error", {}}});
	});

	CHECK((Thrown {batch.error.Type(), batch.error.RawMessage()}) ==
	      Thrown {ExceptionType::EXECUTOR,
	              "the Redshift driver failed while returning the rest of the result (SQLSTATE class HY, driver "
	              "error), and its message is not shown. The statement may have run; check before running it again." +
	                  DRIVER_FAILURE_TRAILER + "\nSQLSTATE: HY000"});
}
