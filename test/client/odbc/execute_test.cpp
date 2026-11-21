// Execute's parameters and row count on one connection to the fake driver: each parameter bound as
// text to its marker in order, a count that differs from the markers' refused before anything is
// bound or run, and the rows a statement with no result columns affected, also as redshift_query's
// rowcount column, and the server's reason for a feature
// it refuses.

// catch.hpp comes first, as in this directory's other tests.
#include "catch.hpp"

#include "extension_database.hpp"
#include "fake_driver_fixture.hpp"
#include "odbc/odbc_connection.hpp"
#include "thrown.hpp"

#include <cstddef>
#include <string>
#include <vector>

using namespace duckdb;
using redshift_driver::testing::FakeColumn;
using redshift_driver::testing::FakeResultSet;
using redshift_test::ATTACH_RS;
using redshift_test::DRIVER_FAILURE_TRAILER;
using redshift_test::ExtensionDatabase;
using redshift_test::FakeDriverFixture;
using redshift_test::Thrown;

namespace {

//! The refusal of `given` parameters for a statement with `markers` markers.
Thrown CountRefusal(int markers, int given) {
	return Thrown {ExceptionType::INVALID_INPUT, "the statement has " + std::to_string(markers) +
	                                                 " parameter marker(s) but " + std::to_string(given) +
	                                                 " parameter(s) were given; pass exactly one parameter for "
	                                                 "each marker, in marker order"};
}

//! A query with two markers, in the $n form the shared header promises.
const char *const TWO_MARKERS = "SELECT a FROM t WHERE a = $1 AND b = $2";
const char *const UPDATE = "UPDATE t SET a = 1";

//! One text column holding "x", in one row.
FakeResultSet OneRow() {
	FakeColumn column;
	column.name = "a";
	column.type_name = "CHARACTER VARYING";
	column.sql_type = SQL_VARCHAR;
	column.column_size = 256;
	FakeResultSet result;
	result.columns.push_back(column);
	result.rows.push_back({"x"});
	return result;
}

//! A statement with no result columns that affected `rows` rows.
FakeResultSet Affected(SQLLEN rows) {
	FakeResultSet result;
	result.row_count = rows;
	return result;
}

//! The detail the fake recorded for the latest call to `entry_point`.
std::string LastDetail(const FakeDriverFixture &fixture, const std::string &entry_point) {
	const auto &calls = fixture.fake.Calls();
	for (auto call = calls.rbegin(); call != calls.rend(); ++call) {
		if (call->entry_point == entry_point) {
			return call->detail;
		}
	}
	return "(no call to " + entry_point + ")";
}

} // namespace

TEST_CASE("Execute binds each parameter as text to its marker in order, sending the SQL text unchanged", "[odbc]") {
	FakeDriverFixture fixture;
	const std::string sql = "SELECT a FROM t WHERE a = $1 AND b = $2 AND c = $3";
	fixture.fake.AddResult(sql, {OneRow()}, 3);
	auto connection = fixture.OpenConnection();

	redshift_test::ExecuteStatement(*connection, sql, {"O'Brien'; DROP TABLE t; --", "$1", ""});
	CHECK(LastDetail(fixture, "SQLPrepare") == sql);
	CHECK(LastDetail(fixture, "SQLExecute") == "1=O'Brien'; DROP TABLE t; --\n2=$1\n3=");
}

TEST_CASE("Execute binds after counting the markers and describing the columns, then runs the statement", "[odbc]") {
	FakeDriverFixture fixture;
	fixture.fake.AddResult(TWO_MARKERS, {OneRow()}, 2);
	auto connection = fixture.OpenConnection();
	const auto before = fixture.fake.Calls().size();

	auto result = redshift_test::ExecuteStatement(*connection, TWO_MARKERS, {"a", "b"});
	CHECK(fixture.CallsAfter(before) == std::vector<std::string> {"SQLAllocHandle", "SQLPrepare", "SQLNumParams",
	                                                              "SQLNumResultCols", "SQLDescribeCol",
	                                                              "SQLColAttribute", "SQLBindParameter",
	                                                              "SQLBindParameter", "SQLExecute"});
}

TEST_CASE("A caller's prepare and execute that succeed with information log each record redacted", "[odbc]") {
	FakeDriverFixture fixture;
	const std::string sql = "SELECT a FROM t WHERE a = $1";
	fixture.fake.AddResult(sql, {OneRow()}, 1);
	auto connection = fixture.OpenConnection();
	fixture.fake.WarnCall("SQLPrepare", NumericCast<int>(fixture.CallsTo("SQLPrepare")) + 1,
	                      {{"01000", 0, "NOTICE:  prepared", {}}});
	// A class 22 record's text is shown, so its line shows whether the value was
	// redacted; a class 01 notice's shows only its SQLSTATE.
	fixture.fake.WarnCall("SQLExecute", NumericCast<int>(fixture.CallsTo("SQLExecute")) + 1,
	                      {{"22001", 0, "ERROR:  value bound-value-1 too long", {}}});
	const auto logged = fixture.log_sink->Lines().size();

	redshift_test::ExecuteStatement(*connection, sql, {"bound-value-1"});
	const auto lines = fixture.log_sink->Lines();
	CHECK(std::vector<std::string>(lines.begin() + NumericCast<std::ptrdiff_t>(logged), lines.end()) ==
	      std::vector<std::string> {"Redshift driver diagnostic, SQLSTATE 01000",
	                                "Redshift driver diagnostic, SQLSTATE 22001: ERROR:  value <param 1> too long"});
}

TEST_CASE("Execute refuses fewer parameters than markers, binding and running nothing", "[odbc]") {
	FakeDriverFixture fixture;
	fixture.fake.AddResult(TWO_MARKERS, {OneRow()}, 2);
	auto connection = fixture.OpenConnection();
	const auto before = fixture.fake.Calls().size();

	CHECK(redshift_test::Capture([&] { redshift_test::ExecuteStatement(*connection, TWO_MARKERS, {"a"}); }) ==
	      CountRefusal(2, 1));
	CHECK(fixture.CallsAfter(before) ==
	      std::vector<std::string> {"SQLAllocHandle", "SQLPrepare", "SQLNumParams", "SQLFreeHandle"});
	CHECK(fixture.fake.AllocatedHandles() == "environment, connection");
}

TEST_CASE("Execute refuses more parameters than markers, binding and running nothing", "[odbc]") {
	FakeDriverFixture fixture;
	auto connection = fixture.OpenConnection();
	const auto before = fixture.fake.Calls().size();

	CHECK(redshift_test::Capture([&] { redshift_test::ExecuteStatement(*connection, "SELECT 1", {"a"}); }) ==
	      CountRefusal(0, 1));
	CHECK(fixture.CallsAfter(before) ==
	      std::vector<std::string> {"SQLAllocHandle", "SQLPrepare", "SQLNumParams", "SQLFreeHandle"});
	CHECK(fixture.fake.AllocatedHandles() == "environment, connection");
}

TEST_CASE("The marker count is the driver's, not the $ characters in the SQL text", "[odbc]") {
	FakeDriverFixture fixture;
	const std::string sql = "SELECT '$1' AS a";
	fixture.fake.AddResult(sql, {OneRow()}, 0);
	auto connection = fixture.OpenConnection();

	CHECK(redshift_test::Capture([&] { redshift_test::ExecuteStatement(*connection, sql, {"x"}); }) ==
	      CountRefusal(0, 1));
	CHECK(redshift_test::ExecuteStatement(*connection, sql, {})->RowCount() == -1);
}

TEST_CASE("A parameter-count refusal leaves the session unmarked and the connection usable", "[odbc]") {
	FakeDriverFixture fixture;
	fixture.fake.AddResult(TWO_MARKERS, {OneRow()}, 2);
	auto connection = fixture.OpenConnection();

	CHECK(redshift_test::Capture([&] { redshift_test::ExecuteStatement(*connection, TWO_MARKERS, {"a"}); }) ==
	      CountRefusal(2, 1));
	CHECK(redshift_test::Capture([&] {
		      redshift_test::ExecuteStatement(*connection, TWO_MARKERS, {"a", "b", "c"});
	      }) == CountRefusal(2, 3));
	CHECK_FALSE(connection->SessionHoldsState());
	CHECK(redshift_test::ExecuteStatement(*connection, TWO_MARKERS, {"a", "b"})->RowCount() == -1);
	CHECK(connection->SessionHoldsState());
}

TEST_CASE("A parameter holding a NUL byte is refused by its position, before any driver call", "[odbc]") {
	FakeDriverFixture fixture;
	fixture.fake.AddResult(TWO_MARKERS, {OneRow()}, 2);
	auto connection = fixture.OpenConnection();
	// An open result, which a refused Execute must not free.
	auto open_result = redshift_test::ExecuteStatement(*connection, TWO_MARKERS, {"a", "b"});
	const auto before = fixture.fake.Calls().size();

	CHECK(redshift_test::Capture([&] {
		      redshift_test::ExecuteStatement(*connection, TWO_MARKERS, {std::string("\0", 1), "b"});
	      }) == Thrown {ExceptionType::INVALID_INPUT,
	                    "parameter 1 holds a NUL byte, which Redshift cannot receive in a text parameter; remove it"});
	CHECK(redshift_test::Capture([&] {
		      redshift_test::ExecuteStatement(*connection, TWO_MARKERS, {"a", std::string("b\0c", 3)});
	      }) == Thrown {ExceptionType::INVALID_INPUT,
	                    "parameter 2 holds a NUL byte, which Redshift cannot receive in a text parameter; remove it"});
	CHECK(fixture.fake.Calls().size() == before);
}

TEST_CASE("A parameter is sendable up to the longest length the driver's int holds", "[odbc]") {
	CHECK(MAX_PARAMETER_BYTES == 2147483647);
}

TEST_CASE("A parameter longer than the driver can send is refused by its position", "[odbc]") {
	// The bound is passed in, so the refusal is reached without building a 2 GiB value. Execute's
	// call of it, before any driver call, is the NUL-byte case's.
	CHECK_NOTHROW(RefuseUnsendableParameters({"abcd", "efgh"}, 4));
	CHECK(redshift_test::Capture([] {
		      RefuseUnsendableParameters({"abcd", "efghi"}, 4);
	      }) == Thrown {ExceptionType::INVALID_INPUT,
	                    "parameter 2 is longer than the 4 bytes the Redshift driver can send; pass a shorter value"});
}

TEST_CASE("A failed parameter count gives BinderException, binding and running nothing", "[odbc]") {
	FakeDriverFixture fixture;
	fixture.fake.AddResult(TWO_MARKERS, {OneRow()}, 2);
	auto connection = fixture.OpenConnection();
	fixture.FailNextCall("SQLNumParams", SQL_ERROR, {{"HY010", 0, "Function sequence error", {}}});
	const auto before = fixture.fake.Calls().size();

	CHECK(redshift_test::Capture([&] {
		      redshift_test::ExecuteStatement(*connection, TWO_MARKERS, {"a", "b"});
	      }) ==
	      Thrown {ExceptionType::BINDER,
	              "the Redshift driver failed while reading the statement's parameter markers (SQLSTATE class HY, "
	              "driver error), and its message is not shown. Retry the statement." +
	                  DRIVER_FAILURE_TRAILER + "\nSQLSTATE: HY010"});
	CHECK(fixture.CallsAfter(before) == std::vector<std::string> {"SQLAllocHandle", "SQLPrepare", "SQLNumParams",
	                                                              "SQLGetConnectAttr", "SQLFreeHandle"});
	CHECK_FALSE(connection->SessionHoldsState());
}

TEST_CASE("A parameter the driver refuses to bind gives an executor error, running nothing", "[odbc]") {
	FakeDriverFixture fixture;
	fixture.fake.AddResult(TWO_MARKERS, {OneRow()}, 2);
	auto connection = fixture.OpenConnection();
	fixture.FailNextCall("SQLBindParameter", SQL_ERROR, {{"HY001", 0, "Memory allocation error", {}}});

	CHECK(redshift_test::Capture([&] {
		      redshift_test::ExecuteStatement(*connection, TWO_MARKERS, {"a", "b"});
	      }) == Thrown {ExceptionType::EXECUTOR,
	                    "the Redshift driver failed while binding the statement's parameters (SQLSTATE class HY, "
	                    "driver error), and its message is not shown. Retry the statement." +
	                        DRIVER_FAILURE_TRAILER + "\nSQLSTATE: HY001"});
	CHECK(fixture.CallsTo("SQLExecute") == redshift_test::SessionSetupStatements().size());
	CHECK_FALSE(connection->SessionHoldsState());
	CHECK(fixture.fake.AllocatedHandles() == "environment, connection");
}

TEST_CASE("A bind failure's message shows a parameter's value only as its position", "[odbc]") {
	// Driver 2.3.0 posts no server record at bind; the execute path, where Redshift echoes values
	// (22P02), redacts through the same records read.
	FakeDriverFixture fixture;
	fixture.fake.AddResult(TWO_MARKERS, {OneRow()}, 2);
	auto connection = fixture.OpenConnection();
	fixture.fake.FailCall("SQLBindParameter", 2, SQL_ERROR,
	                      {{"22001", 0, "value too long for type character varying(4): token-12345678", {}}});

	CHECK(redshift_test::Capture([&] {
		      redshift_test::ExecuteStatement(*connection, TWO_MARKERS, {"a", "token-12345678"});
	      }) ==
	      Thrown {ExceptionType::EXECUTOR,
	              "the Redshift driver could not bind the statement's parameters: value too long for type character "
	              "varying(4): <param 2>\nSQLSTATE: 22001"});
}

TEST_CASE("A failed execute's message shows a parameter's value only as its position", "[odbc]") {
	FakeDriverFixture fixture;
	fixture.fake.AddResult(TWO_MARKERS, {OneRow()}, 2);
	auto connection = fixture.OpenConnection();
	fixture.FailNextCall("SQLExecute", SQL_ERROR, {{"22P02", 0, "invalid input syntax: token-12345678", {}}});

	CHECK(redshift_test::Capture([&] {
		      redshift_test::ExecuteStatement(*connection, TWO_MARKERS, {"a", "token-12345678"});
	      }) == Thrown {ExceptionType::EXECUTOR,
	                    "Redshift could not run the statement: invalid input syntax: <param 2>\nSQLSTATE: 22P02"});
}

TEST_CASE("A statement with no result columns reports the rows it affected", "[odbc]") {
	FakeDriverFixture fixture;
	fixture.fake.AddResult(UPDATE, {Affected(3)});
	auto connection = fixture.OpenConnection();

	CHECK(redshift_test::ExecuteStatement(*connection, UPDATE, {})->RowCount() == 3);
}

TEST_CASE("A command whose tag carries no count, such as SET, reports 0 rows", "[odbc]") {
	// Driver 2.3.0 reads the empty count of a SET or CREATE tag as 0 (rslibpq.c).
	FakeDriverFixture fixture;
	const std::string sql = "SET search_path TO public";
	fixture.fake.AddResult(sql, {Affected(0)});
	auto connection = fixture.OpenConnection();

	CHECK(redshift_test::ExecuteStatement(*connection, sql, {})->RowCount() == 0);
}

TEST_CASE("A result with rows reports no row count, whatever the driver counts", "[odbc]") {
	FakeDriverFixture fixture;
	auto rows = OneRow();
	rows.row_count = 1;
	fixture.fake.AddResult(TWO_MARKERS, {rows}, 2);
	auto connection = fixture.OpenConnection();

	CHECK(redshift_test::ExecuteStatement(*connection, TWO_MARKERS, {"a", "b"})->RowCount() == -1);
	CHECK(fixture.CallsTo("SQLRowCount") == 0);
}

TEST_CASE("A row count the driver fails to report is negative, and the statement still counts as run", "[odbc]") {
	FakeDriverFixture fixture;
	fixture.fake.AddResult(UPDATE, {Affected(3)});
	auto connection = fixture.OpenConnection();
	fixture.FailNextCall("SQLRowCount", SQL_ERROR, {{"HY009", 0, "Invalid use of null pointer", {}}});

	CHECK(redshift_test::ExecuteStatement(*connection, UPDATE, {})->RowCount() == -1);
	CHECK(connection->SessionHoldsState());
}

TEST_CASE("A statement whose count the driver reports as -1 has no row count", "[odbc]") {
	// Driver 2.3.0 reports -1, with success, for a result with no count, such as an empty query's.
	FakeDriverFixture fixture;
	fixture.fake.AddResult(";", {Affected(-1)});
	auto connection = fixture.OpenConnection();

	CHECK(redshift_test::ExecuteStatement(*connection, ";", {})->RowCount() == -1);
	CHECK(fixture.CallsTo("SQLRowCount") == 1);
}

TEST_CASE("A statement with no result columns marks the session as holding state", "[odbc]") {
	FakeDriverFixture fixture;
	fixture.fake.AddResult(UPDATE, {Affected(3)});
	auto connection = fixture.OpenConnection();
	REQUIRE_FALSE(connection->SessionHoldsState());

	redshift_test::ExecuteStatement(*connection, UPDATE, {});
	CHECK(connection->SessionHoldsState());
}

TEST_CASE("redshift_query's rowcount column holds the rows a statement with no result columns affected", "[odbc]") {
	FakeDriverFixture fixture;
	fixture.fake.AddResult(UPDATE, {Affected(3)});
	ExtensionDatabase database;
	database.Run(ATTACH_RS);

	auto result = database.Run("SELECT * FROM redshift_query('rs', 'UPDATE t SET a = 1')");
	CHECK(result->ColumnName(0).GetIdentifierName() == "rowcount");
	auto chunk = result->Fetch();
	REQUIRE(chunk);
	REQUIRE(chunk->size() == 1);
	CHECK(chunk->GetValue(0, 0).ToString() == "3");
	// Checked through strings: client code names none of DuckDB's value types.
	REQUIRE(chunk->GetTypes().size() == 1);
	CHECK(chunk->GetTypes()[0].ToString() == "BIGINT");
}

TEST_CASE("redshift_query's rowcount column is NULL when the driver fails to report the count", "[odbc]") {
	FakeDriverFixture fixture;
	fixture.fake.AddResult(UPDATE, {Affected(3)});
	fixture.FailNextCall("SQLRowCount", SQL_ERROR, {{"HY009", 0, "Invalid use of null pointer", {}}});
	ExtensionDatabase database;
	database.Run(ATTACH_RS);

	auto result = database.Run("SELECT * FROM redshift_query('rs', 'UPDATE t SET a = 1')");
	auto chunk = result->Fetch();
	REQUIRE(chunk);
	REQUIRE(chunk->size() == 1);
	CHECK(chunk->GetValue(0, 0).IsNull());
	REQUIRE(chunk->GetTypes().size() == 1);
	CHECK(chunk->GetTypes()[0].ToString() == "BIGINT");
}

TEST_CASE("redshift_query binds each column with the type Describe reports", "[odbc]") {
	FakeDriverFixture fixture;
	FakeColumn date;
	date.name = "d";
	date.type_name = "DATE";
	date.sql_type = SQL_TYPE_DATE;
	FakeColumn amount;
	amount.name = "amount";
	amount.type_name = "NUMERIC";
	amount.sql_type = SQL_NUMERIC;
	amount.column_size = 10;
	amount.decimal_digits = 2;
	auto text = OneRow().columns[0];
	FakeResultSet result;
	result.columns = {date, amount, text};
	result.rows = {{"2024-03-15", "12.34", "x"}};
	fixture.fake.AddResult("SELECT d, amount, a FROM t", {result});
	ExtensionDatabase database;
	database.Run(ATTACH_RS);

	auto read = database.Run("SELECT * FROM redshift_query('rs', 'SELECT d, amount, a FROM t')");
	auto chunk = read->Fetch();
	REQUIRE(chunk);
	REQUIRE(chunk->size() == 1);
	const auto &types = chunk->GetTypes();
	REQUIRE(types.size() == 3);
	CHECK(types[0].ToString() == "DATE");
	CHECK(types[1].ToString() == "DECIMAL(10,2)");
	CHECK(types[2].ToString() == "VARCHAR");
	CHECK(chunk->GetValue(0, 0).ToString() == "2024-03-15");
	CHECK(chunk->GetValue(1, 0).ToString() == "12.34");
	CHECK(chunk->GetValue(2, 0).ToString() == "x");
}

TEST_CASE("redshift_query refuses a column Describe reports as UNKNOWN, running nothing", "[odbc]") {
	FakeDriverFixture fixture;
	FakeColumn shape;
	shape.name = "shape";
	shape.type_name = "GEOMETRY";
	shape.sql_type = SQL_LONGVARBINARY;
	FakeResultSet result;
	result.columns = {shape};
	result.rows = {{"x"}};
	fixture.fake.AddResult("SELECT shape FROM t", {result});
	ExtensionDatabase database;
	database.Run(ATTACH_RS);

	auto read = database.con.Query("SELECT * FROM redshift_query('rs', 'SELECT shape FROM t')");
	REQUIRE(read->HasError());
	const auto &error = read->GetErrorObject();
	CHECK(error.Type() == ExceptionType::BINDER);
	CHECK(error.RawMessage().find("shape") != std::string::npos);
	CHECK(error.RawMessage().find("has a type this extension does not support") != std::string::npos);
	// The session setup's statements are the only ones run.
	CHECK(fixture.CallsTo("SQLExecute") == redshift_test::SessionSetupStatements().size());
}

TEST_CASE("redshift_query binds its params to the statement's markers in order", "[odbc]") {
	FakeDriverFixture fixture;
	fixture.fake.AddResult(TWO_MARKERS, {OneRow()}, 2);
	ExtensionDatabase database;
	database.Run(ATTACH_RS);

	auto read = database.Run(std::string("SELECT * FROM redshift_query('rs', '") + TWO_MARKERS +
	                         "', params := ['first', 'second'])");
	auto chunk = read->Fetch();
	REQUIRE(chunk);
	REQUIRE(chunk->size() == 1);
	CHECK(chunk->GetValue(0, 0).ToString() == "x");
	CHECK(LastDetail(fixture, "SQLExecute") == "1=first\n2=second");
}

TEST_CASE("redshift_query's parameter-count refusal reaches the user, running nothing", "[odbc]") {
	FakeDriverFixture fixture;
	fixture.fake.AddResult(TWO_MARKERS, {OneRow()}, 2);
	ExtensionDatabase database;
	database.Run(ATTACH_RS);

	auto read = database.con.Query(std::string("SELECT * FROM redshift_query('rs', '") + TWO_MARKERS +
	                               "', params := ['only'])");
	REQUIRE(read->HasError());
	const auto &error = read->GetErrorObject();
	CHECK((Thrown {error.Type(), error.RawMessage()}) == CountRefusal(2, 1));
	// The session setup's statements are the only ones run.
	CHECK(fixture.CallsTo("SQLExecute") == redshift_test::SessionSetupStatements().size());
}

TEST_CASE("redshift_query shows why Redshift refused a temp table, a class 0A reason", "[odbc]") {
	FakeDriverFixture fixture;
	const std::string sql = "CREATE TEMP TABLE t (a INT)";
	fixture.fake.AddResult(sql, {Affected(0)});
	// The caller's statement has no parameters, so it runs with SQLExecDirect, unlike the setup's.
	fixture.fake.FailCall(
	    "SQLExecDirect", 1, SQL_ERROR,
	    {{"0A000", 0, "[Redshift][ODBC Driver][Server]0A000:ERROR:  Local storage is not supported", {}}});
	ExtensionDatabase database;
	database.Run(ATTACH_RS);

	auto result = database.con.Query("SELECT * FROM redshift_query('rs', '" + sql + "')");
	REQUIRE(result->HasError());
	const auto &error = result->GetErrorObject();
	CHECK((Thrown {error.Type(), error.RawMessage()}) ==
	      Thrown {ExceptionType::EXECUTOR,
	              "Redshift could not run the statement: ERROR:  Local storage is not supported\nSQLSTATE: 0A000"});
}
