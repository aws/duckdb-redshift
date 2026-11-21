// Describe, Execute and the text result on one connection to the fake driver. Describe's shape
// comes from the driver's column metadata through ClassifyColumn.

// catch.hpp comes first, as in this directory's other tests.
#include "catch.hpp"

#include "duckdb/common/numeric_utils.hpp"
#include "fake_driver_fixture.hpp"
#include "recorded_failures.hpp"
#include "thrown.hpp"

#include <algorithm>
#include <string>
#include <vector>

using namespace duckdb;
using redshift_driver::testing::FakeColumn;
using redshift_driver::testing::FakeDiag;
using redshift_driver::testing::FakeResultSet;
using redshift_test::DRIVER_FAILURE_TRAILER;
using redshift_test::FakeDriverFixture;
using redshift_test::LongVarcharColumn;
using redshift_test::Thrown;

namespace {

const char *const QUERY = "SELECT a, b FROM t";
//! What NextBatch says once the result's connection has closed.
const char *const CLOSED_RESULT =
    "this Redshift result's connection has closed; run the statement again on a new connection";
//! "SELECT 1" with a NUL byte and more text after it.
const std::string NUL_SQL("SELECT 1\0; DROP TABLE t", 23);
//! The refusal of SQL holding a NUL byte.
const char *const NUL_REFUSAL = "the SQL text holds a NUL byte, which Redshift cannot receive; remove it";

//! A column as the driver describes it: its name, type name and concise SQL type.
FakeColumn Column(const std::string &name, const std::string &type_name, SQLSMALLINT sql_type) {
	FakeColumn column;
	column.name = name;
	column.type_name = type_name;
	column.sql_type = sql_type;
	return column;
}

//! A result set with `columns` and no row, which is all Describe reads.
FakeResultSet ShapeOnly(std::vector<FakeColumn> columns) {
	FakeResultSet result;
	result.columns = std::move(columns);
	return result;
}

//! One long text column named `a` holding `values`, one row each.
FakeResultSet OneColumn(const std::vector<std::string> &values) {
	return redshift_test::OneColumn(LongVarcharColumn("a"), values);
}

//! A cell as its text, its integer in decimal, or "NULL".
std::string CellText(const RedshiftValue &value) {
	switch (value.kind) {
	case RedshiftValueKind::NULL_VALUE:
		return "NULL";
	case RedshiftValueKind::INTEGER:
		return std::to_string(value.integer);
	default:
		return value.text;
	}
}

//! The rows of `result`, each cell as CellText gives it, read until NextBatch returns false.
std::vector<std::vector<std::string>> ReadAll(RedshiftResult &result) {
	std::vector<std::vector<std::string>> rows;
	RedshiftBatch batch;
	while (result.NextBatch(batch)) {
		for (const auto &row : batch.rows) {
			std::vector<std::string> cells;
			for (const auto &value : row) {
				cells.push_back(CellText(value));
			}
			rows.push_back(cells);
		}
	}
	return rows;
}

//! The only value of a one-row, one-column result.
RedshiftValue OnlyValue(RedshiftConnection &connection, const std::string &sql) {
	auto result = redshift_test::ExecuteStatement(connection, sql, {});
	RedshiftBatch batch;
	REQUIRE(result->NextBatch(batch));
	REQUIRE(batch.rows.size() == 1);
	REQUIRE(batch.rows[0].size() == 1);
	return batch.rows[0][0];
}

//! `bytes` bytes cycling through the lower-case letters, so a part placed at the wrong offset shows.
std::string Letters(size_t bytes) {
	std::string text;
	for (size_t i = 0; i < bytes; i++) {
		text.push_back(NumericCast<char>('a' + i % 26));
	}
	return text;
}

} // namespace

TEST_CASE("Describe prepares the statement without executing it, keeping its handle", "[odbc]") {
	FakeDriverFixture fixture;
	fixture.fake.AddResult(QUERY, {OneColumn({"x"})});
	auto connection = fixture.OpenConnection();
	const auto before = fixture.fake.Calls().size();

	connection->Describe(QUERY);
	const auto names = fixture.EntryPoints();
	CHECK(std::vector<std::string>(names.begin() + NumericCast<std::ptrdiff_t>(before), names.end()) ==
	      std::vector<std::string> {"SQLAllocHandle", "SQLPrepare", "SQLNumResultCols", "SQLDescribeCol",
	                                "SQLColAttribute"});
	CHECK(fixture.fake.Calls()[before + 1].detail == QUERY);
	CHECK(fixture.fake.AllocatedHandles() == "environment, connection, statement");
}

TEST_CASE("Describe reports each column's name and the type its driver type name gives", "[odbc]") {
	FakeDriverFixture fixture;
	fixture.fake.AddResult(QUERY, {ShapeOnly({Column("flag", "BOOL", SQL_BIT), Column("n", "INTEGER", SQL_INTEGER),
	                                          Column("t", "TIME", SQL_TYPE_TIME), Column("tz", "TIMETZ", SQL_TYPE_TIME),
	                                          Column("b", "VARBYTE", SQL_LONGVARBINARY),
	                                          Column("g", "GEOMETRY", SQL_LONGVARBINARY), LongVarcharColumn("s")})});
	auto connection = fixture.OpenConnection();

	const auto shape = connection->Describe(QUERY);
	CHECK(shape.returns_rows);
	REQUIRE(shape.columns.size() == 7);
	std::vector<std::string> names;
	for (const auto &column : shape.columns) {
		names.push_back(column.name);
	}
	CHECK(names == std::vector<std::string> {"flag", "n", "t", "tz", "b", "g", "s"});
	CHECK(shape.columns[0].type == RedshiftTypeId::BOOLEAN);
	CHECK(shape.columns[1].type == RedshiftTypeId::INTEGER);
	// TIME and TIMETZ share code 92, and VARBYTE and GEOMETRY share -4: only the name tells them apart.
	CHECK(shape.columns[2].type == RedshiftTypeId::TIME);
	CHECK(shape.columns[3].type == RedshiftTypeId::TIMETZ);
	CHECK(shape.columns[4].type == RedshiftTypeId::BLOB);
	CHECK(shape.columns[5].type == RedshiftTypeId::UNKNOWN);
	CHECK(shape.columns[6].type == RedshiftTypeId::VARCHAR);
}

TEST_CASE("Describe reports a column whose SQL type disagrees with its type name as UNKNOWN", "[odbc]") {
	FakeDriverFixture fixture;
	fixture.fake.AddResult(QUERY, {ShapeOnly({Column("n", "INTEGER", SQL_VARCHAR)})});
	auto connection = fixture.OpenConnection();

	const auto shape = connection->Describe(QUERY);
	REQUIRE(shape.columns.size() == 1);
	CHECK(shape.columns[0].type == RedshiftTypeId::UNKNOWN);
}

TEST_CASE("Describe reports a column the driver gives no type name as UNKNOWN", "[odbc]") {
	FakeDriverFixture fixture;
	fixture.fake.AddResult(QUERY, {ShapeOnly({Column("n", "", SQL_INTEGER)})});
	auto connection = fixture.OpenConnection();

	const auto shape = connection->Describe(QUERY);
	REQUIRE(shape.columns.size() == 1);
	CHECK(shape.columns[0].type == RedshiftTypeId::UNKNOWN);
}

TEST_CASE("Describe reports NUMERIC precision and scale as the driver reports them", "[odbc]") {
	FakeDriverFixture fixture;
	auto amount = Column("amount", "NUMERIC", SQL_NUMERIC);
	amount.column_size = 38;
	amount.decimal_digits = 9;
	auto too_wide = Column("too_wide", "NUMERIC", SQL_NUMERIC);
	too_wide.column_size = 39;
	fixture.fake.AddResult(QUERY, {ShapeOnly({amount, too_wide})});
	auto connection = fixture.OpenConnection();

	const auto shape = connection->Describe(QUERY);
	REQUIRE(shape.columns.size() == 2);
	CHECK(shape.columns[0].type == RedshiftTypeId::DECIMAL);
	CHECK(shape.columns[0].precision == 38);
	CHECK(shape.columns[0].scale == 9);
	CHECK(shape.columns[1].type == RedshiftTypeId::UNKNOWN);
}

TEST_CASE("A 255-byte type name is read whole", "[odbc]") {
	FakeDriverFixture fixture;
	fixture.fake.AddResult(QUERY, {ShapeOnly({Column("flag", "BOOL" + std::string(251, ' '), SQL_BIT)})});
	auto connection = fixture.OpenConnection();

	const auto shape = connection->Describe(QUERY);
	REQUIRE(shape.columns.size() == 1);
	CHECK(shape.columns[0].type == RedshiftTypeId::BOOLEAN);
}

TEST_CASE("A type name too long for its buffer is no type name, so the column is UNKNOWN", "[odbc]") {
	FakeDriverFixture fixture;
	// Cut to the buffer's 255 bytes, each name would read as BOOL.
	const std::vector<std::string> type_names {"BOOL" + std::string(251, ' ') + "X",
	                                           "BOOL" + std::string(300, ' ') + "X"};
	for (const auto &type_name : type_names) {
		fixture.fake.AddResult(QUERY, {ShapeOnly({Column("flag", type_name, SQL_BIT)})});
		auto connection = fixture.OpenConnection();

		const auto shape = connection->Describe(QUERY);
		REQUIRE(shape.columns.size() == 1);
		CHECK(shape.columns[0].type == RedshiftTypeId::UNKNOWN);
	}
}

TEST_CASE("Describe of a statement with no result columns reports no rows", "[odbc]") {
	FakeDriverFixture fixture;
	FakeResultSet no_columns;
	no_columns.row_count = 0;
	fixture.fake.AddResult("SET search_path TO public", {no_columns});
	auto connection = fixture.OpenConnection();

	const auto shape = connection->Describe("SET search_path TO public");
	CHECK_FALSE(shape.returns_rows);
	CHECK(shape.columns.empty());
}

TEST_CASE("Describe of a statement Redshift refuses to prepare gives BinderException with its SQLSTATE", "[odbc]") {
	FakeDriverFixture fixture;
	auto connection = fixture.OpenConnection();
	fixture.FailNextCall("SQLPrepare", SQL_ERROR, {{"42601", 0, "ERROR:  syntax error at or near \"SELEC\"", {}}});

	const auto error = redshift_test::CaptureError([&] { connection->Describe("SELEC 1"); });
	CHECK(Thrown {error.Type(), error.RawMessage()} ==
	      Thrown {ExceptionType::BINDER,
	              "Redshift refused the statement: ERROR:  syntax error at or near \"SELEC\"\nSQLSTATE: 42601"});
	CHECK(error.ExtraInfo().at("sqlstate") == "42601");
	CHECK(error.ExtraInfo().at("sqlstates") == "42601");
}

TEST_CASE("A refused prepare leaves the connection open with no statement handle", "[odbc]") {
	FakeDriverFixture fixture;
	auto connection = fixture.OpenConnection();
	fixture.FailNextCall("SQLPrepare", SQL_ERROR, {{"42601", 0, "ERROR:  syntax error", {}}});

	REQUIRE(redshift_test::Capture([&] { connection->Describe("SELEC 1"); }).type == ExceptionType::BINDER);
	CHECK(fixture.fake.AllocatedHandles() == "environment, connection");
	CHECK(connection->Describe("SELECT 1").returns_rows);
}

TEST_CASE("Describe in an aborted transaction gives BinderException carrying 25P02", "[odbc]") {
	FakeDriverFixture fixture;
	auto connection = fixture.OpenConnection();
	fixture.FailNextCall("SQLPrepare", SQL_ERROR, {redshift_test::ABORTED_TRANSACTION_RECORD});

	const auto error = redshift_test::CaptureError([&] { connection->Describe(QUERY); });
	CHECK(Thrown {error.Type(), error.RawMessage()} ==
	      Thrown {ExceptionType::BINDER, "Redshift refused the statement: ERROR:  current transaction is aborted, "
	                                     "commands ignored until end of transaction block\nSQLSTATE: 25P02"});
	CHECK(error.ExtraInfo().at("sqlstate") == "25P02");
}

TEST_CASE("Describe gives the prepare refusal of a multi-statement string sent whole", "[odbc]") {
	FakeDriverFixture fixture;
	auto connection = fixture.OpenConnection();
	const auto opened = fixture.fake.Calls().size();
	fixture.FailNextCall("SQLPrepare", SQL_ERROR,
	                     {{"42601", 0, "ERROR:  cannot insert multiple commands into a prepared statement", {}}});

	CHECK(redshift_test::Capture([&] { connection->Describe("SELECT 1; SELECT 2"); }) ==
	      Thrown {ExceptionType::BINDER, "Redshift refused the statement: ERROR:  cannot insert multiple commands "
	                                     "into a prepared statement\nSQLSTATE: 42601"});
	CHECK(fixture.CallsAfter(opened) ==
	      std::vector<std::string> {"SQLAllocHandle", "SQLPrepare", "SQLGetConnectAttr", "SQLFreeHandle"});
	CHECK(fixture.fake.Calls()[opened + 1].detail == "SELECT 1; SELECT 2");
}

TEST_CASE("Execute of a statement Redshift refuses to prepare gives BinderException, running nothing", "[odbc]") {
	FakeDriverFixture fixture;
	auto connection = fixture.OpenConnection();
	const auto opened = fixture.fake.Calls().size();
	fixture.FailNextCall("SQLPrepare", SQL_ERROR, {{"42P01", 0, "ERROR:  relation \"t\" does not exist", {}}});

	CHECK(redshift_test::Capture([&] { redshift_test::ExecuteStatement(*connection, QUERY, {}); }) ==
	      Thrown {ExceptionType::BINDER,
	              "Redshift refused the statement: ERROR:  relation \"t\" does not exist\nSQLSTATE: 42P01"});
	const auto names = fixture.CallsAfter(opened);
	CHECK(std::find(names.begin(), names.end(), "SQLExecute") == names.end());
	CHECK(std::find(names.begin(), names.end(), "SQLExecDirect") == names.end());
	CHECK_FALSE(connection->SessionHoldsState());
	CHECK(fixture.fake.AllocatedHandles() == "environment, connection");
}

TEST_CASE("A failed column count gives BinderException", "[odbc]") {
	FakeDriverFixture fixture;
	fixture.fake.FailCall("SQLNumResultCols", 1, SQL_ERROR, {});
	auto connection = fixture.OpenConnection();

	CHECK(redshift_test::Capture([&] { connection->Describe("SELECT 1"); }) ==
	      Thrown {ExceptionType::BINDER, "Redshift did not describe the statement's result columns, and the driver "
	                                     "returned no diagnostic record. Retry the statement."});
	CHECK(fixture.fake.AllocatedHandles() == "environment, connection");
}

TEST_CASE("Execute refuses a statement whose columns cannot be counted before running it", "[odbc]") {
	FakeDriverFixture fixture;
	fixture.fake.FailCall("SQLNumResultCols", 1, SQL_ERROR, {});
	auto connection = fixture.OpenConnection();
	const auto opened = fixture.fake.Calls().size();

	CHECK(redshift_test::Capture([&] { redshift_test::ExecuteStatement(*connection, "SELECT 1", {}); }) ==
	      Thrown {ExceptionType::BINDER, "Redshift did not describe the statement's result columns, and the driver "
	                                     "returned no diagnostic record. Retry the statement."});
	const auto names = fixture.CallsAfter(opened);
	CHECK(std::find(names.begin(), names.end(), "SQLExecute") == names.end());
	CHECK(std::find(names.begin(), names.end(), "SQLExecDirect") == names.end());
	CHECK_FALSE(connection->SessionHoldsState());
	CHECK(fixture.fake.AllocatedHandles() == "environment, connection");
}

TEST_CASE("A failed column description gives BinderException with its SQLSTATE", "[odbc]") {
	FakeDriverFixture fixture;
	fixture.fake.FailCall("SQLDescribeCol", 1, SQL_ERROR, {{"HY000", 0, "SQLDescribeCol: Invalid column number", {}}});
	auto connection = fixture.OpenConnection();

	const auto error = redshift_test::CaptureError([&] { connection->Describe("SELECT 1"); });
	CHECK(Thrown {error.Type(), error.RawMessage()} ==
	      Thrown {ExceptionType::BINDER,
	              "the Redshift driver failed while describing the statement's result columns (SQLSTATE class HY, "
	              "driver error), and its message is not shown. Retry the statement." +
	                  DRIVER_FAILURE_TRAILER + "\nSQLSTATE: HY000"});
	CHECK(error.ExtraInfo().at("sqlstate") == "HY000");
	CHECK(fixture.fake.AllocatedHandles() == "environment, connection");
}

TEST_CASE("A failed type-name read on a connection the driver reports alive gives BinderException", "[odbc]") {
	FakeDriverFixture fixture;
	fixture.fake.FailCall("SQLColAttribute", 1, SQL_ERROR, {{"HY091", 0, "Invalid descriptor field identifier", {}}});
	auto connection = fixture.OpenConnection();

	const auto error = redshift_test::CaptureError([&] { connection->Describe("SELECT 1"); });
	CHECK(Thrown {error.Type(), error.RawMessage()} ==
	      Thrown {ExceptionType::BINDER,
	              "the Redshift driver failed while describing the statement's result columns (SQLSTATE class HY, "
	              "driver error), and its message is not shown. Retry the statement." +
	                  DRIVER_FAILURE_TRAILER + "\nSQLSTATE: HY091"});
	CHECK(error.ExtraInfo().at("sqlstate") == "HY091");
	CHECK(fixture.fake.AllocatedHandles() == "environment, connection");
	CHECK(connection->Describe("SELECT 1").returns_rows);
}

TEST_CASE("A 255-byte column name is kept whole", "[odbc]") {
	FakeDriverFixture fixture;
	FakeResultSet result;
	result.columns = {LongVarcharColumn(Letters(255))};
	fixture.fake.AddResult(QUERY, {result});
	auto connection = fixture.OpenConnection();

	const auto shape = connection->Describe(QUERY);
	REQUIRE(shape.columns.size() == 1);
	CHECK(shape.columns[0].name == Letters(255));
}

TEST_CASE("A 256-byte column name is refused rather than cut", "[odbc]") {
	FakeDriverFixture fixture;
	FakeResultSet result;
	result.columns = {LongVarcharColumn(Letters(256))};
	fixture.fake.AddResult(QUERY, {result});
	auto connection = fixture.OpenConnection();

	CHECK(redshift_test::Capture([&] { connection->Describe(QUERY); }) ==
	      Thrown {ExceptionType::BINDER, "Redshift reported a name longer than 255 bytes for column 1; alias the "
	                                     "column to a shorter name"});
	CHECK(fixture.fake.AllocatedHandles() == "environment, connection");
}

TEST_CASE("A failed statement-handle allocation on a live connection is a statement failure, not IOException",
          "[odbc]") {
	FakeDriverFixture fixture;
	auto connection = fixture.OpenConnection();
	fixture.FailNextCall("SQLAllocHandle", SQL_ERROR, {FakeDiag {"HY001", 0, "Memory allocation error", {}}});

	const auto error =
	    redshift_test::CaptureError([&] { redshift_test::ExecuteStatement(*connection, "SELECT 1", {}); });
	CHECK(Thrown {error.Type(), error.RawMessage()} ==
	      Thrown {ExceptionType::EXECUTOR,
	              "the Redshift driver failed while allocating a statement handle (SQLSTATE class HY, driver error), "
	              "and its message is not shown. Retry the statement." +
	                  DRIVER_FAILURE_TRAILER + "\nSQLSTATE: HY001"});
	CHECK(error.ExtraInfo().at("sqlstate") == "HY001");
	CHECK(fixture.fake.AllocatedHandles() == "environment, connection");
	CHECK_FALSE(connection->SessionHoldsState());
	CHECK(connection->Describe("SELECT 1").returns_rows);
}

TEST_CASE("A failed statement-handle allocation in Describe is the same statement failure", "[odbc]") {
	FakeDriverFixture fixture;
	auto connection = fixture.OpenConnection();
	fixture.FailNextCall("SQLAllocHandle", SQL_ERROR, {FakeDiag {"HY001", 0, "Memory allocation error", {}}});

	// Not BinderException: running out of memory is not the user's SQL.
	CHECK(redshift_test::Capture([&] { connection->Describe("SELECT 1"); }) ==
	      Thrown {ExceptionType::EXECUTOR,
	              "the Redshift driver failed while allocating a statement handle (SQLSTATE class HY, driver error), "
	              "and its message is not shown. Retry the statement." +
	                  DRIVER_FAILURE_TRAILER + "\nSQLSTATE: HY001"});
	CHECK(fixture.fake.AllocatedHandles() == "environment, connection");
	CHECK(connection->Describe("SELECT 1").returns_rows);
}

TEST_CASE("Execute returns each row's cells as text, and NULL as NULL", "[odbc]") {
	FakeDriverFixture fixture;
	FakeResultSet result;
	result.columns = {LongVarcharColumn("a"), LongVarcharColumn("b")};
	result.rows = {{"1", "one"}, {"2", ""}, {"3", "three"}};
	result.nulls = {{false, false}, {false, true}, {false, false}};
	fixture.fake.AddResult(QUERY, {result});
	auto connection = fixture.OpenConnection();

	auto rows = ReadAll(*redshift_test::ExecuteStatement(*connection, QUERY, {}));
	CHECK(rows == std::vector<std::vector<std::string>> {{"1", "one"}, {"2", "NULL"}, {"3", "three"}});
}

TEST_CASE("A VARCHAR column's cells are read as SQL_C_CHAR", "[odbc]") {
	FakeDriverFixture fixture;
	fixture.fake.AddResult(QUERY, {OneColumn({"1"})});
	auto connection = fixture.OpenConnection();

	OnlyValue(*connection, QUERY);
	int reads = 0;
	for (const auto &call : fixture.fake.Calls()) {
		if (call.entry_point == "SQLGetData") {
			// Column 1, read as SQL_C_CHAR, which is 1.
			CHECK(call.detail == "1 1");
			reads++;
		}
	}
	CHECK(reads == 1);
}

TEST_CASE("An empty text value arrives as empty text, not NULL", "[odbc]") {
	FakeDriverFixture fixture;
	fixture.fake.AddResult(QUERY, {OneColumn({""})});
	auto connection = fixture.OpenConnection();

	const auto value = OnlyValue(*connection, QUERY);
	CHECK(value.kind == RedshiftValueKind::TEXT);
	CHECK(value.text.empty());
}

TEST_CASE("A value that exactly fills one read arrives whole", "[odbc]") {
	FakeDriverFixture fixture;
	// A read's buffer is 4096 bytes, one of them the terminating NUL.
	fixture.fake.AddResult(QUERY, {OneColumn({Letters(4095)})});
	auto connection = fixture.OpenConnection();

	CHECK(OnlyValue(*connection, QUERY).text == Letters(4095));
}

TEST_CASE("A value longer than one read arrives whole", "[odbc]") {
	FakeDriverFixture fixture;
	fixture.fake.AddResult(QUERY, {OneColumn({Letters(10000)})});
	auto connection = fixture.OpenConnection();

	CHECK(OnlyValue(*connection, QUERY).text == Letters(10000));
}

TEST_CASE("A long value whose remaining length the driver does not report arrives whole", "[odbc]") {
	FakeDriverFixture fixture;
	auto result = OneColumn({Letters(10000)});
	result.columns[0].report_no_total = true;
	fixture.fake.AddResult(QUERY, {result});
	auto connection = fixture.OpenConnection();

	CHECK(OnlyValue(*connection, QUERY).text == Letters(10000));
}

TEST_CASE("A long value reaches the batch at its own size", "[odbc]") {
	FakeDriverFixture fixture;
	// One byte past 256 parts of 4095. The value is read into a string of its own, which its first
	// part's indicator reserves at the value's size and the batch takes over, so the capacity shows
	// the reservation: passing SQL_NO_TOTAL instead grows the string geometrically.
	const auto text = Letters(4095 * 256 + 1);
	fixture.fake.AddResult(QUERY, {OneColumn({text})});
	auto connection = fixture.OpenConnection();
	auto result = redshift_test::ExecuteStatement(*connection, QUERY, {});

	RedshiftBatch batch;
	REQUIRE(result->NextBatch(batch));
	const auto &value = batch.rows.at(0).at(0);
	CHECK(value.text == text);
	CHECK(value.text.capacity() < value.text.size() + 4096);
}

TEST_CASE("A result returns at most 2048 rows per batch", "[odbc]") {
	FakeDriverFixture fixture;
	fixture.fake.AddResult(QUERY, {OneColumn(std::vector<std::string>(2049, "x"))});
	auto connection = fixture.OpenConnection();
	auto result = redshift_test::ExecuteStatement(*connection, QUERY, {});

	RedshiftBatch batch;
	REQUIRE(result->NextBatch(batch));
	CHECK(batch.rows.size() == 2048);
	REQUIRE(result->NextBatch(batch));
	CHECK(batch.rows.size() == 1);
	CHECK_FALSE(result->NextBatch(batch));
	CHECK(batch.rows.empty());
}

TEST_CASE("Describe of SQL holding a NUL byte is refused before a statement handle is allocated", "[odbc]") {
	FakeDriverFixture fixture;
	auto connection = fixture.OpenConnection();
	const auto before = fixture.fake.Calls().size();

	CHECK(redshift_test::Capture([&] { connection->Describe(NUL_SQL); }) ==
	      Thrown {ExceptionType::INVALID_INPUT, NUL_REFUSAL});
	CHECK(fixture.fake.Calls().size() == before);
}

TEST_CASE("Execute of SQL holding a NUL byte is refused before a statement handle is allocated", "[odbc]") {
	FakeDriverFixture fixture;
	auto connection = fixture.OpenConnection();
	// Open, so a refusal that came after closing the open result would show.
	auto open = redshift_test::ExecuteStatement(*connection, "SELECT 1", {});
	const auto before = fixture.fake.Calls().size();

	CHECK(redshift_test::Capture([&] { redshift_test::ExecuteStatement(*connection, NUL_SQL, {}); }) ==
	      Thrown {ExceptionType::INVALID_INPUT, NUL_REFUSAL});
	CHECK(fixture.fake.Calls().size() == before);
	CHECK(ReadAll(*open) == std::vector<std::vector<std::string>> {{"1"}});
}

TEST_CASE("A DELETE that changes no row runs although the driver answers SQL_NO_DATA", "[odbc]") {
	FakeDriverFixture fixture;
	FakeResultSet no_rows_changed;
	no_rows_changed.row_count = 0;
	fixture.fake.AddResult("DELETE FROM t WHERE false", {no_rows_changed});
	auto connection = fixture.OpenConnection();

	auto result = redshift_test::ExecuteStatement(*connection, "DELETE FROM t WHERE false", {});
	CHECK(result->RowCount() == 0);
	CHECK(connection->SessionHoldsState());
	result.reset();
	CHECK(fixture.fake.AllocatedHandles() == "environment, connection");
}

TEST_CASE("A statement with no result columns that Redshift fails to run gives an executor error", "[odbc]") {
	FakeDriverFixture fixture;
	FakeResultSet no_columns;
	no_columns.row_count = 0;
	fixture.fake.AddResult("SET search_path TO public", {no_columns});
	auto connection = fixture.OpenConnection();
	fixture.FailNextCall("SQLExecDirect", SQL_ERROR, {});

	CHECK(redshift_test::Capture([&] {
		      redshift_test::ExecuteStatement(*connection, "SET search_path TO public", {});
	      }) == Thrown {ExceptionType::EXECUTOR,
	                    "Redshift could not run the statement, and the driver returned no "
	                    "diagnostic record. The statement may have run; check before running it again."});
	CHECK(fixture.fake.AllocatedHandles() == "environment, connection");
}

TEST_CASE("SQL_NO_DATA from running a statement with result columns gives an executor error", "[odbc]") {
	FakeDriverFixture fixture;
	auto connection = fixture.OpenConnection();
	fixture.FailNextCall("SQLExecDirect", SQL_NO_DATA, {});

	CHECK(redshift_test::Capture([&] { redshift_test::ExecuteStatement(*connection, "SELECT 1", {}); }) ==
	      Thrown {ExceptionType::EXECUTOR,
	              "Redshift could not run the statement, and the driver returned no "
	              "diagnostic record. The statement may have run; check before running it again."});
	CHECK(fixture.fake.AllocatedHandles() == "environment, connection");
}

TEST_CASE("A statement Redshift fails to run gives an executor error, leaving no statement handle", "[odbc]") {
	FakeDriverFixture fixture;
	auto connection = fixture.OpenConnection();
	fixture.FailNextCall("SQLExecDirect", SQL_ERROR, {FakeDiag {"42P01", 0, "relation does not exist", {}}});

	CHECK(redshift_test::Capture([&] { redshift_test::ExecuteStatement(*connection, "SELECT 1", {}); }) ==
	      Thrown {ExceptionType::EXECUTOR,
	              "Redshift could not run the statement: relation does not exist\nSQLSTATE: 42P01"});
	CHECK(fixture.fake.AllocatedHandles() == "environment, connection");
}

TEST_CASE("A failed fetch gives an executor error", "[odbc]") {
	FakeDriverFixture fixture;
	fixture.fake.FailCall("SQLFetch", 1, SQL_ERROR, {});
	auto connection = fixture.OpenConnection();
	auto result = redshift_test::ExecuteStatement(*connection, "SELECT 1", {});

	RedshiftBatch batch;
	CHECK(redshift_test::Capture([&] { result->NextBatch(batch); }) ==
	      Thrown {ExceptionType::EXECUTOR,
	              "Redshift could not return the rest of the result, and the driver returned "
	              "no diagnostic record. The statement may have run; check before running it again."});
}

TEST_CASE("A failed cell read gives the executor error naming the column, with its SQLSTATE", "[odbc]") {
	FakeDriverFixture fixture;
	fixture.fake.AddResult(QUERY, {OneColumn({"1"})});
	fixture.fake.FailCall("SQLGetData", 1, SQL_ERROR, {FakeDiag {"22003", 0, "Numeric value out of range", {}}});
	auto connection = fixture.OpenConnection();
	auto result = redshift_test::ExecuteStatement(*connection, QUERY, {});

	RedshiftBatch batch;
	CHECK(
	    redshift_test::Capture([&] { result->NextBatch(batch); }) ==
	    Thrown {ExceptionType::EXECUTOR,
	            "Redshift could not return column 1 of the current row: Numeric value out of range\nSQLSTATE: 22003"});
}

TEST_CASE("A cell read that fails with no record still names its column", "[odbc]") {
	FakeDriverFixture fixture;
	FakeResultSet result;
	result.columns = {LongVarcharColumn("a"), LongVarcharColumn("b")};
	result.rows = {{"1", "2"}};
	fixture.fake.AddResult("SELECT a, b FROM t", {result});
	// The second SQLGetData reads column 2. SQL_NO_DATA fails it too: a cell is never read past its end.
	SECTION("SQL_ERROR") {
		fixture.fake.FailCall("SQLGetData", 2, SQL_ERROR, {});
	}
	SECTION("SQL_NO_DATA") {
		fixture.fake.FailCall("SQLGetData", 2, SQL_NO_DATA, {});
	}
	auto connection = fixture.OpenConnection();
	auto read = redshift_test::ExecuteStatement(*connection, "SELECT a, b FROM t", {});

	RedshiftBatch batch;
	CHECK(redshift_test::Capture([&] { read->NextBatch(batch); }) ==
	      Thrown {ExceptionType::EXECUTOR,
	              "Redshift could not return column 2 of the current row, and the driver "
	              "returned no diagnostic record. The statement may have run; check before running it "
	              "again."});
}

TEST_CASE("A connection holds no session state until Execute runs a statement", "[odbc]") {
	FakeDriverFixture fixture;
	auto connection = fixture.OpenConnection();
	CHECK_FALSE(connection->SessionHoldsState());

	connection->Describe("SELECT 1");
	CHECK_FALSE(connection->SessionHoldsState());
	redshift_test::ExecuteStatement(*connection, "SELECT 1", {});
	CHECK(connection->SessionHoldsState());
}

TEST_CASE("A statement that fails to run still marks the session as holding state", "[odbc]") {
	FakeDriverFixture fixture;
	auto connection = fixture.OpenConnection();
	fixture.FailNextCall("SQLExecDirect", SQL_ERROR, {});

	CHECK_THROWS(redshift_test::ExecuteStatement(*connection, "SELECT 1", {}));
	CHECK(connection->SessionHoldsState());
}

TEST_CASE("A drained result has freed its statement handle", "[odbc]") {
	FakeDriverFixture fixture;
	auto connection = fixture.OpenConnection();
	auto result = redshift_test::ExecuteStatement(*connection, "SELECT 1", {});

	CHECK(ReadAll(*result) == std::vector<std::vector<std::string>> {{"1"}});
	CHECK(fixture.fake.AllocatedHandles() == "environment, connection");
}

TEST_CASE("A connection closed after its result drained frees only its own handles", "[odbc]") {
	FakeDriverFixture fixture;
	auto connection = fixture.OpenConnection();
	auto result = redshift_test::ExecuteStatement(*connection, "SELECT 1", {});
	CHECK(ReadAll(*result) == std::vector<std::vector<std::string>> {{"1"}});
	const auto before = fixture.fake.Calls().size();

	connection.reset();
	const auto names = fixture.EntryPoints();
	CHECK(std::vector<std::string>(names.begin() + NumericCast<std::ptrdiff_t>(before), names.end()) ==
	      std::vector<std::string> {"SQLEndTran", "SQLDisconnect", "SQLFreeHandle"});
}

TEST_CASE("A connection closed after Describe frees the statement Describe kept, then its own handles", "[odbc]") {
	FakeDriverFixture fixture;
	auto connection = fixture.OpenConnection();
	connection->Describe("SELECT 1");
	const auto before = fixture.fake.Calls().size();

	connection.reset();
	const auto names = fixture.EntryPoints();
	CHECK(std::vector<std::string>(names.begin() + NumericCast<std::ptrdiff_t>(before), names.end()) ==
	      std::vector<std::string> {"SQLFreeHandle", "SQLEndTran", "SQLDisconnect", "SQLFreeHandle"});
	const auto frees = fixture.DetailsOf("SQLFreeHandle");
	CHECK(std::vector<std::string>(frees.end() - 2, frees.end()) ==
	      std::vector<std::string> {"statement", "connection"});
	CHECK(fixture.fake.AllocatedHandles() == "environment");
}

TEST_CASE("A statement handle whose free fails is not passed to the driver again", "[odbc]") {
	FakeDriverFixture fixture;
	auto connection = fixture.OpenConnection();
	auto result = redshift_test::ExecuteStatement(*connection, "SELECT 1", {});
	// The fake abandons a handle whose free it fails, and fails the test if it is called again.
	fixture.FailNextCall("SQLFreeHandle", SQL_ERROR, {});

	CHECK(ReadAll(*result) == std::vector<std::vector<std::string>> {{"1"}});
	result.reset();
	connection.reset();
	CHECK(fixture.fake.MisusedAbandonedHandles().empty());
}

TEST_CASE("A result that outlives its connection makes no driver call, and NextBatch gives IOException", "[odbc]") {
	FakeDriverFixture fixture;
	auto connection = fixture.OpenConnection();
	auto result = redshift_test::ExecuteStatement(*connection, "SELECT 1", {});
	connection.reset();
	const auto names = fixture.EntryPoints();
	// The statement goes first, then the connection's transaction, session and handle.
	CHECK(std::vector<std::string>(names.end() - 4, names.end()) ==
	      std::vector<std::string> {"SQLFreeHandle", "SQLEndTran", "SQLDisconnect", "SQLFreeHandle"});
	const auto calls = fixture.fake.Calls().size();

	RedshiftBatch batch;
	CHECK(redshift_test::Capture([&] { result->NextBatch(batch); }) == Thrown {ExceptionType::IO, CLOSED_RESULT});
	result.reset();
	CHECK(fixture.fake.Calls().size() == calls);
}
