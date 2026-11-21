// Execute's shape check on one connection to the fake driver: the shape the caller bound is compared
// with the prepared statement's before anything is bound or run, and a difference in returns_rows,
// the column count, a column's name or type, or a DECIMAL's precision or scale is refused with
// shape_changed and no SQLSTATE, freeing the statement, leaving the session unmarked and the
// connection usable, and the refusal reaching a DuckDB user
// through redshift_query.

// catch.hpp comes first, as in this directory's other tests.
#include "catch.hpp"

#include "extension_database.hpp"
#include "fake_driver_fixture.hpp"
#include "odbc/odbc_connection.hpp"
#include "thrown.hpp"

#include <string>
#include <vector>

using namespace duckdb;
using redshift_driver::testing::FakeColumn;
using redshift_driver::testing::FakeResultSet;
using redshift_test::ATTACH_RS;
using redshift_test::ExtensionDatabase;
using redshift_test::FakeDriverFixture;
using redshift_test::Thrown;

namespace {

//! A query with one marker whose prepared shape is an INTEGER `a` and a NUMERIC(12,2) `amount`.
const char *const QUERY = "SELECT a, amount FROM t WHERE a = $1";
//! A procedure call, which may apply its effects whether or not it returns rows.
const char *const CALL = "CALL refresh_totals()";

//! What every refusal's message starts with, before the difference.
const std::string REFUSED = "the statement's result shape is not the one it was bound with, so it was not run: ";

FakeColumn Column(const std::string &name, const std::string &type_name, SQLSMALLINT sql_type, SQLULEN size = 0,
                  SQLSMALLINT digits = 0) {
	FakeColumn column;
	column.name = name;
	column.type_name = type_name;
	column.sql_type = sql_type;
	column.column_size = size;
	column.decimal_digits = digits;
	return column;
}

//! QUERY's result: one row of an INTEGER and a NUMERIC(12,2).
FakeResultSet QueryRows() {
	FakeResultSet result;
	result.columns = {Column("a", "INTEGER", SQL_INTEGER, 10), Column("amount", "NUMERIC", SQL_NUMERIC, 12, 2)};
	result.rows = {{"1", "12.34"}};
	return result;
}

//! CALL's result when it returns rows: one row of one INTEGER.
FakeResultSet CallRows() {
	FakeResultSet result;
	result.columns = {Column("refreshed", "INTEGER", SQL_INTEGER, 10)};
	result.rows = {{"3"}};
	return result;
}

//! CALL's result when it returns no rows.
FakeResultSet CallNoRows() {
	FakeResultSet result;
	result.row_count = 0;
	return result;
}

RedshiftColumn Bound(const std::string &name, RedshiftTypeId type, int32_t precision = 0, int32_t scale = 0) {
	RedshiftColumn column;
	column.name = name;
	column.type = type;
	column.precision = precision;
	column.scale = scale;
	return column;
}

//! A row-returning shape with `columns`.
RedshiftStatementShape Rows(std::vector<RedshiftColumn> columns) {
	RedshiftStatementShape shape;
	shape.returns_rows = true;
	shape.columns = std::move(columns);
	return shape;
}

//! The statements run so far: with SQLExecute, or, for one with no parameters, SQLExecDirect.
size_t Runs(const FakeDriverFixture &fixture) {
	return fixture.CallsTo("SQLExecute") + fixture.CallsTo("SQLExecDirect");
}

//! The shape QUERY was prepared with, written out rather than read from the client.
RedshiftStatementShape QueryShape() {
	return Rows({Bound("a", RedshiftTypeId::INTEGER), Bound("amount", RedshiftTypeId::DECIMAL, 12, 2)});
}

//! Executes `sql` with `params` against `bound` on a fresh connection and checks the refusal and
//! that nothing ran: the statement's handle is freed, nothing is bound or executed, the session stays
//! unmarked, and the same connection then runs the statement as the shape the client prepares.
void CheckRefused(FakeDriverFixture &fixture, const std::string &sql, const vector<string> &params,
                  const RedshiftStatementShape &bound, const std::string &difference) {
	auto connection = fixture.OpenConnection();
	const auto runs = Runs(fixture);
	const auto binds = fixture.CallsTo("SQLBindParameter");

	const auto error = redshift_test::CaptureError([&] { connection->Execute(sql, params, bound); });
	CHECK(Thrown {error.Type(), error.RawMessage()} == Thrown {ExceptionType::BINDER, REFUSED + difference});
	// redshift_error alone: a local comparison, so no sqlstate or sqlstates.
	const unordered_map<string, string> expected {{"redshift_error", "shape_changed"}};
	CHECK(error.ExtraInfo() == expected);
	CHECK(Runs(fixture) == runs);
	CHECK(fixture.CallsTo("SQLBindParameter") == binds);
	CHECK(fixture.EntryPoints().back() == "SQLFreeHandle");
	CHECK(fixture.fake.AllocatedHandles() == "environment, connection");
	CHECK_FALSE(connection->SessionHoldsState());

	auto result = redshift_test::ExecuteStatement(*connection, sql, params);
	CHECK(Runs(fixture) == runs + 1);
	CHECK(connection->SessionHoldsState());
}

} // namespace

TEST_CASE("Execute refuses a column whose type changed since bind, running nothing", "[odbc]") {
	FakeDriverFixture fixture;
	fixture.fake.AddResult(QUERY, {QueryRows()}, 1);
	auto bound = QueryShape();
	bound.columns[0].type = RedshiftTypeId::BIGINT;
	CheckRefused(fixture, QUERY, {"1"}, bound, "column 1 was bound as \"a\" BIGINT and is now \"a\" INTEGER");
}

TEST_CASE("Execute refuses a column whose name changed since bind, running nothing", "[odbc]") {
	FakeDriverFixture fixture;
	fixture.fake.AddResult(QUERY, {QueryRows()}, 1);
	auto bound = QueryShape();
	bound.columns[1].name = "Amount";
	CheckRefused(fixture, QUERY, {"1"}, bound,
	             "column 2 was bound as \"Amount\" DECIMAL(12,2) and is now \"amount\" DECIMAL(12,2)");
}

TEST_CASE("Execute refuses a statement whose column count changed since bind, running nothing", "[odbc]") {
	FakeDriverFixture fixture;
	fixture.fake.AddResult(QUERY, {QueryRows()}, 1);
	CheckRefused(fixture, QUERY, {"1"}, Rows({Bound("a", RedshiftTypeId::INTEGER)}),
	             "it was bound with 1 column and now has 2");
	auto wider = QueryShape();
	wider.columns.push_back(Bound("note", RedshiftTypeId::VARCHAR));
	CheckRefused(fixture, QUERY, {"1"}, wider, "it was bound with 3 columns and now has 2");
}

TEST_CASE("Execute refuses a DECIMAL whose precision or scale changed since bind, running nothing", "[odbc]") {
	FakeDriverFixture fixture;
	fixture.fake.AddResult(QUERY, {QueryRows()}, 1);
	auto precision = QueryShape();
	precision.columns[1].precision = 14;
	CheckRefused(fixture, QUERY, {"1"}, precision,
	             "column 2 was bound as \"amount\" DECIMAL(14,2) and is now \"amount\" DECIMAL(12,2)");
	auto scale = QueryShape();
	scale.columns[1].scale = 3;
	CheckRefused(fixture, QUERY, {"1"}, scale,
	             "column 2 was bound as \"amount\" DECIMAL(12,3) and is now \"amount\" DECIMAL(12,2)");
}

TEST_CASE("Execute refuses a CALL bound as returning no rows that now returns rows, running nothing", "[odbc]") {
	FakeDriverFixture fixture;
	fixture.fake.AddResult(CALL, {CallRows()});
	CheckRefused(fixture, CALL, {}, RedshiftStatementShape(),
	             "it was bound as a statement without a result set and now returns rows");
}

TEST_CASE("Execute refuses a CALL bound as returning rows that now returns none, running nothing", "[odbc]") {
	FakeDriverFixture fixture;
	fixture.fake.AddResult(CALL, {CallNoRows()});
	CheckRefused(fixture, CALL, {}, Rows({Bound("refreshed", RedshiftTypeId::INTEGER)}),
	             "it was bound as returning rows and now has no result set");
}

TEST_CASE("A shape refusal prepares and describes the statement, then frees it", "[odbc]") {
	FakeDriverFixture fixture;
	fixture.fake.AddResult(QUERY, {QueryRows()}, 1);
	auto connection = fixture.OpenConnection();
	const auto before = fixture.fake.Calls().size();

	CHECK(redshift_test::Capture([&] { connection->Execute(QUERY, {"1"}, RedshiftStatementShape()); }).type ==
	      ExceptionType::BINDER);
	CHECK(fixture.CallsAfter(before) == std::vector<std::string> {"SQLAllocHandle", "SQLPrepare", "SQLNumParams",
	                                                              "SQLNumResultCols", "SQLDescribeCol",
	                                                              "SQLColAttribute", "SQLDescribeCol",
	                                                              "SQLColAttribute", "SQLFreeHandle"});
}

TEST_CASE("A shape refusal leaves a session that already holds state holding it", "[odbc]") {
	FakeDriverFixture fixture;
	fixture.fake.AddResult(CALL, {CallNoRows()});
	auto connection = fixture.OpenConnection();
	redshift_test::ExecuteStatement(*connection, CALL, {});
	REQUIRE(connection->SessionHoldsState());

	CHECK(redshift_test::Capture([&] {
		      connection->Execute(CALL, {}, Rows({Bound("refreshed", RedshiftTypeId::INTEGER)}));
	      }).type == ExceptionType::BINDER);
	CHECK(connection->SessionHoldsState());
}

TEST_CASE("Execute runs a statement whose prepared shape is the bound one", "[odbc]") {
	FakeDriverFixture fixture;
	fixture.fake.AddResult(QUERY, {QueryRows()}, 1);
	fixture.fake.AddResult(CALL, {CallNoRows()});
	auto connection = fixture.OpenConnection();

	{
		auto result = connection->Execute(QUERY, {"1"}, QueryShape());
		RedshiftBatch batch;
		REQUIRE(result->NextBatch(batch));
		REQUIRE(batch.rows.size() == 1);
		CHECK(batch.rows[0][0].integer == 1);
		CHECK(batch.rows[0][1].text == "12.34");
	}
	CHECK(connection->Execute(CALL, {}, RedshiftStatementShape())->RowCount() == 0);
	// QUERY has a parameter, so it runs with SQLExecute; CALL has none, so it runs with SQLExecDirect.
	CHECK(fixture.CallsTo("SQLExecute") == redshift_test::SessionSetupStatements().size() + 1);
	CHECK(fixture.CallsTo("SQLExecDirect") == 1);
	CHECK(connection->SessionHoldsState());
}

TEST_CASE("A prepared redshift_query whose columns changed since PREPARE is refused at EXECUTE, running nothing",
          "[odbc]") {
	FakeDriverFixture fixture;
	// EXECUTE comes a second after PREPARE, so the client prepares the statement again.
	redshift_test::ManualClock clock;
	const std::string sql = "SELECT a, amount FROM t";
	fixture.fake.AddResult(sql, {QueryRows()});
	ExtensionDatabase database;
	database.Run(ATTACH_RS);
	database.Run("PREPARE p AS SELECT * FROM redshift_query('rs', '" + sql + "')");
	// The table changes between PREPARE and EXECUTE: column a is now a BIGINT.
	auto changed = QueryRows();
	changed.columns[0] = Column("a", "BIGINT", SQL_BIGINT, 19);
	fixture.fake.AddResult(sql, {changed});
	clock.Advance(DESCRIBED_STATEMENT_REUSE);
	const auto runs = Runs(fixture);

	auto refused = database.con.Query("EXECUTE p");
	REQUIRE(refused->HasError());
	const auto &error = refused->GetErrorObject();
	CHECK((Thrown {error.Type(), error.RawMessage()}) ==
	      (Thrown {ExceptionType::BINDER,
	               "redshift_query: the statement on attach handle \"rs\" now returns different columns than when it "
	               "was bound, so this call was not run on Redshift, but other calls in the same DuckDB statement may "
	               "have run; check their effects on Redshift before running the statement again. If it is a "
	               "prepared statement, prepare it again. The client reported: " +
	                   REFUSED + "column 1 was bound as \"a\" INTEGER and is now \"a\" BIGINT"}));
	CHECK(Runs(fixture) == runs);

	// The handle stays usable: a statement bound now gets the new columns.
	auto read = database.Run("SELECT * FROM redshift_query('rs', '" + sql + "')");
	CHECK(redshift_test::FirstColumn(*read) == std::vector<std::string> {"1"});
	CHECK(Runs(fixture) == runs + 1);
}
