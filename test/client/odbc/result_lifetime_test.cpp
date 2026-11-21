// One open result per connection on the fake driver: Describe and Execute refused while a result is
// open, the result ending only when NextBatch first returns false or it is destroyed, and the
// further results a statement leaves after its first drained at the driver's end of data, where a
// further result set with columns fails the statement.

// catch.hpp comes first, as in this directory's other tests.
#include "catch.hpp"

#include "extension_database.hpp"
#include "fake_driver_fixture.hpp"
#include "thrown.hpp"

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

//! The refusal of Describe or Execute while a result is open.
const Thrown OPEN_RESULT_REFUSAL {ExceptionType::INVALID_INPUT,
                                  "a result from this Redshift connection is still open, and the connection holds one "
                                  "at a time; read that result to its end or close it, then try again"};
//! The failure of a statement that produced a further result set with columns.
const Thrown MULTIPLE_RESULT_SETS {ExceptionType::EXECUTOR,
                                   "the Redshift statement returned more than one result set, and this extension "
                                   "reads only one; run a statement that returns a single result set"};
//! The failure of a result whose drain of further results failed with a record of SQLSTATE `sqlstate`, in class HY.
Thrown FailedDrain(const std::string &sqlstate) {
	return Thrown {
	    ExceptionType::EXECUTOR,
	    "the Redshift driver failed while returning the rest of the result (SQLSTATE class HY, driver error), "
	    "and its message is not shown. The statement may have run; check before running it again." +
	        DRIVER_FAILURE_TRAILER + "\nSQLSTATE: " + sqlstate};
}
//! What NextBatch says once the result's connection has closed.
const Thrown CLOSED_RESULT {
    ExceptionType::IO, "this Redshift result's connection has closed; run the statement again on a new connection"};

//! A query whose one result set holds the rows "x" and "y".
const char *const QUERY = "SELECT a FROM t";
//! A query whose 2049 rows take more than one of the client's MAX_BATCH_ROWS (2048) batches, the size
//! DuckDB's default vector size happens to equal.
const char *const BIG_QUERY = "SELECT a FROM big";
//! A query whose second result set has columns.
const char *const TWO_RESULT_SETS = "CALL two_result_sets()";
//! A query whose second result has no columns.
const char *const QUERY_THEN_CLEANUP = "CALL query_then_cleanup()";
//! A statement with no result columns whose second result set has columns.
const char *const UPDATE_THEN_RESULT_SET = "CALL update_then_result_set()";
//! A statement with no result columns whose second result has none either.
const char *const UPDATE_THEN_CLEANUP = "CALL update_then_cleanup()";
//! A statement with no result columns and no further result.
const char *const UPDATE = "UPDATE t SET a = 1";

//! One text column named `a` holding `values`, one row each.
FakeResultSet TextRows(const std::vector<std::string> &values) {
	FakeColumn column;
	column.name = "a";
	column.type_name = "CHARACTER VARYING";
	column.sql_type = SQL_VARCHAR;
	column.column_size = 256;
	FakeResultSet result;
	result.columns.push_back(column);
	for (const auto &value : values) {
		result.rows.push_back({value});
	}
	return result;
}

//! A result with no columns that affected `rows` rows.
FakeResultSet Affected(SQLLEN rows) {
	FakeResultSet result;
	result.row_count = rows;
	return result;
}

//! A fixture with every statement above scripted.
class LifetimeFixture : public FakeDriverFixture {
public:
	LifetimeFixture() {
		fake.AddResult(QUERY, {TextRows({"x", "y"})});
		fake.AddResult(BIG_QUERY, {TextRows(std::vector<std::string>(2049, "z"))});
		fake.AddResult(TWO_RESULT_SETS, {TextRows({"x"}), TextRows({"second"})});
		fake.AddResult(QUERY_THEN_CLEANUP, {TextRows({"x"}), Affected(0)});
		fake.AddResult(UPDATE_THEN_RESULT_SET, {Affected(2), TextRows({"second"})});
		fake.AddResult(UPDATE_THEN_CLEANUP, {Affected(2), Affected(7)});
		fake.AddResult(UPDATE, {Affected(1)});
	}
};

//! The one-column result's cells, read until NextBatch returns false.
std::vector<std::string> CellsToEnd(RedshiftResult &result) {
	std::vector<std::string> cells;
	RedshiftBatch batch;
	while (result.NextBatch(batch)) {
		for (const auto &row : batch.rows) {
			cells.push_back(row.at(0).text);
		}
	}
	return cells;
}

} // namespace

TEST_CASE("Execute while a result is open is refused with InvalidInputException, making no driver call", "[odbc]") {
	LifetimeFixture fixture;
	auto connection = fixture.OpenConnection();
	auto result = redshift_test::ExecuteStatement(*connection, QUERY, {});
	const auto calls = fixture.fake.Calls().size();

	CHECK(redshift_test::Capture([&] { redshift_test::ExecuteStatement(*connection, QUERY, {}); }) ==
	      OPEN_RESULT_REFUSAL);
	CHECK(fixture.fake.Calls().size() == calls);
	CHECK(connection->SessionHoldsState());
}

TEST_CASE("Describe while a result is open is refused with InvalidInputException, making no driver call", "[odbc]") {
	LifetimeFixture fixture;
	auto connection = fixture.OpenConnection();
	auto result = redshift_test::ExecuteStatement(*connection, QUERY, {});
	const auto calls = fixture.fake.Calls().size();

	CHECK(redshift_test::Capture([&] { connection->Describe(QUERY); }) == OPEN_RESULT_REFUSAL);
	CHECK(fixture.fake.Calls().size() == calls);
}

TEST_CASE("A refused Describe or Execute leaves the open result to be read to its end", "[odbc]") {
	LifetimeFixture fixture;
	auto connection = fixture.OpenConnection();
	auto result = redshift_test::ExecuteStatement(*connection, QUERY, {});

	REQUIRE(redshift_test::Capture([&] { connection->Describe(QUERY); }) == OPEN_RESULT_REFUSAL);
	REQUIRE(redshift_test::Capture([&] { redshift_test::ExecuteStatement(*connection, QUERY, {}); }) ==
	        OPEN_RESULT_REFUSAL);
	CHECK(CellsToEnd(*result) == std::vector<std::string> {"x", "y"});
}

TEST_CASE("On the same-thread ring a bound result keeps its statement after its last rows, until the NextBatch that "
          "returns false",
          "[odbc]") {
	LifetimeFixture fixture;
	// On the caller's thread; the producer reads the end ahead, which producer_thread_test.cpp covers.
	SetFetchThreadForTesting(FetchThread::CALLER);
	auto connection = fixture.OpenConnection();
	auto result = redshift_test::ExecuteStatement(*connection, QUERY, {});
	RedshiftBatch batch;

	REQUIRE(result->NextBatch(batch));
	CHECK(batch.rows.size() == 2);
	// One fetch a batch: the end of the rows is not read ahead with them.
	CHECK(fixture.fake.AllocatedHandles() == "environment, connection, statement");
	CHECK(redshift_test::Capture([&] { connection->Describe(QUERY); }) == OPEN_RESULT_REFUSAL);
	CHECK_FALSE(result->NextBatch(batch));
	CHECK(fixture.fake.AllocatedHandles() == "environment, connection");
	CHECK(connection->Describe(QUERY).returns_rows);
}

TEST_CASE("Execute runs once the open result's NextBatch has returned false, while that result still exists",
          "[odbc]") {
	LifetimeFixture fixture;
	auto connection = fixture.OpenConnection();
	auto first = redshift_test::ExecuteStatement(*connection, QUERY, {});
	REQUIRE(CellsToEnd(*first) == std::vector<std::string> {"x", "y"});

	auto second = redshift_test::ExecuteStatement(*connection, QUERY, {});
	CHECK(CellsToEnd(*second) == std::vector<std::string> {"x", "y"});
}

TEST_CASE("An ended result's NextBatch returns false, making no driver call, while a later result is open", "[odbc]") {
	LifetimeFixture fixture;
	auto connection = fixture.OpenConnection();
	auto first = redshift_test::ExecuteStatement(*connection, QUERY, {});
	REQUIRE(CellsToEnd(*first) == std::vector<std::string> {"x", "y"});
	auto second = redshift_test::ExecuteStatement(*connection, QUERY, {});
	const auto calls = fixture.fake.Calls().size();

	RedshiftBatch batch;
	CHECK_FALSE(first->NextBatch(batch));
	CHECK(fixture.fake.Calls().size() == calls);
	CHECK(CellsToEnd(*second) == std::vector<std::string> {"x", "y"});
}

TEST_CASE("Destroying an ended result leaves the result Execute returned after it open", "[odbc]") {
	LifetimeFixture fixture;
	auto connection = fixture.OpenConnection();
	auto first = redshift_test::ExecuteStatement(*connection, QUERY, {});
	REQUIRE(CellsToEnd(*first) == std::vector<std::string> {"x", "y"});
	auto second = redshift_test::ExecuteStatement(*connection, QUERY, {});
	const auto calls = fixture.fake.Calls().size();

	first.reset();
	CHECK(fixture.fake.Calls().size() == calls);
	CHECK(redshift_test::Capture([&] { redshift_test::ExecuteStatement(*connection, QUERY, {}); }) ==
	      OPEN_RESULT_REFUSAL);
	CHECK(CellsToEnd(*second) == std::vector<std::string> {"x", "y"});
}

TEST_CASE("A result destroyed before its end frees its statement, and the next Execute runs", "[odbc]") {
	LifetimeFixture fixture;
	auto connection = fixture.OpenConnection();
	auto result = redshift_test::ExecuteStatement(*connection, QUERY, {});
	const auto before = fixture.fake.Calls().size();

	result.reset();
	CHECK(fixture.CallsAfter(before) == std::vector<std::string> {"SQLFreeHandle"});
	CHECK(fixture.fake.AllocatedHandles() == "environment, connection");
	CHECK(CellsToEnd(*redshift_test::ExecuteStatement(*connection, QUERY, {})) == std::vector<std::string> {"x", "y"});
}

TEST_CASE("An ended result gives IOException from NextBatch, making no driver call, once its connection is gone",
          "[odbc]") {
	LifetimeFixture fixture;
	auto connection = fixture.OpenConnection();
	auto result = redshift_test::ExecuteStatement(*connection, QUERY, {});
	REQUIRE(CellsToEnd(*result) == std::vector<std::string> {"x", "y"});
	connection.reset();
	const auto calls = fixture.fake.Calls().size();

	RedshiftBatch batch;
	CHECK(redshift_test::Capture([&] { result->NextBatch(batch); }) == CLOSED_RESULT);
	CHECK(fixture.fake.Calls().size() == calls);
}

TEST_CASE("Every result a connection returned gives IOException from NextBatch once the connection is gone", "[odbc]") {
	LifetimeFixture fixture;
	auto connection = fixture.OpenConnection();
	auto first = redshift_test::ExecuteStatement(*connection, QUERY, {});
	REQUIRE(CellsToEnd(*first) == std::vector<std::string> {"x", "y"});
	auto second = redshift_test::ExecuteStatement(*connection, QUERY, {});
	connection.reset();
	const auto calls = fixture.fake.Calls().size();

	RedshiftBatch batch;
	CHECK(redshift_test::Capture([&] { first->NextBatch(batch); }) == CLOSED_RESULT);
	CHECK(redshift_test::Capture([&] { second->NextBatch(batch); }) == CLOSED_RESULT);
	first.reset();
	second.reset();
	CHECK(fixture.fake.Calls().size() == calls);
}

TEST_CASE("A result whose rows are all returned gives IOException from NextBatch once its connection is gone",
          "[odbc]") {
	LifetimeFixture fixture;
	auto connection = fixture.OpenConnection();
	auto result = redshift_test::ExecuteStatement(*connection, QUERY, {});
	RedshiftBatch batch;
	REQUIRE(result->NextBatch(batch));
	connection.reset();

	CHECK(redshift_test::Capture([&] { result->NextBatch(batch); }) == CLOSED_RESULT);
}

TEST_CASE("A statement with no result columns stays open until NextBatch returns false", "[odbc]") {
	LifetimeFixture fixture;
	auto connection = fixture.OpenConnection();
	auto result = redshift_test::ExecuteStatement(*connection, UPDATE, {});
	CHECK(fixture.fake.AllocatedHandles() == "environment, connection");

	CHECK(redshift_test::Capture([&] { redshift_test::ExecuteStatement(*connection, UPDATE, {}); }) ==
	      OPEN_RESULT_REFUSAL);
	RedshiftBatch batch;
	CHECK_FALSE(result->NextBatch(batch));
	CHECK(redshift_test::ExecuteStatement(*connection, UPDATE, {})->RowCount() == 1);
}

TEST_CASE("A second result set with columns fails NextBatch after the first set's rows, with no SQLSTATE", "[odbc]") {
	LifetimeFixture fixture;
	auto connection = fixture.OpenConnection();
	auto result = redshift_test::ExecuteStatement(*connection, TWO_RESULT_SETS, {});
	RedshiftBatch batch;

	REQUIRE(result->NextBatch(batch));
	REQUIRE(batch.rows.size() == 1);
	CHECK(batch.rows[0][0].text == "x");
	const auto error = redshift_test::CaptureError([&] { result->NextBatch(batch); });
	CHECK((Thrown {error.Type(), error.RawMessage()}) == MULTIPLE_RESULT_SETS);
	CHECK(error.ExtraInfo().empty());
	CHECK(batch.rows.empty());
	CHECK(fixture.fake.AllocatedHandles() == "environment, connection");
}

TEST_CASE("A result failed by a second result set fails every later NextBatch too, making no driver call", "[odbc]") {
	LifetimeFixture fixture;
	auto connection = fixture.OpenConnection();
	auto result = redshift_test::ExecuteStatement(*connection, TWO_RESULT_SETS, {});
	RedshiftBatch batch;
	REQUIRE(result->NextBatch(batch));
	REQUIRE(redshift_test::Capture([&] { result->NextBatch(batch); }) == MULTIPLE_RESULT_SETS);
	const auto calls = fixture.fake.Calls().size();

	CHECK(redshift_test::Capture([&] { result->NextBatch(batch); }) == MULTIPLE_RESULT_SETS);
	CHECK(fixture.fake.Calls().size() == calls);
}

TEST_CASE("The next Execute runs once a result failed by a second result set is destroyed", "[odbc]") {
	LifetimeFixture fixture;
	auto connection = fixture.OpenConnection();
	auto result = redshift_test::ExecuteStatement(*connection, TWO_RESULT_SETS, {});
	RedshiftBatch batch;
	REQUIRE(result->NextBatch(batch));
	REQUIRE(redshift_test::Capture([&] { result->NextBatch(batch); }) == MULTIPLE_RESULT_SETS);

	result.reset();
	CHECK(CellsToEnd(*redshift_test::ExecuteStatement(*connection, QUERY, {})) == std::vector<std::string> {"x", "y"});
}

TEST_CASE("A result whose NextBatch failed stays open until it is destroyed", "[odbc]") {
	LifetimeFixture fixture;
	auto connection = fixture.OpenConnection();
	unique_ptr<RedshiftResult> result;
	RedshiftBatch batch;
	SECTION("A second result set with columns") {
		result = redshift_test::ExecuteStatement(*connection, TWO_RESULT_SETS, {});
		REQUIRE(result->NextBatch(batch));
	}
	SECTION("A failed drain at the end of the rows") {
		result = redshift_test::ExecuteStatement(*connection, QUERY, {});
		fixture.FailNextCall("SQLMoreResults", SQL_ERROR, {{"HY000", 0, "streaming cursor read failed", {}}});
		REQUIRE(result->NextBatch(batch));
	}
	SECTION("A failed fetch") {
		result = redshift_test::ExecuteStatement(*connection, QUERY, {});
		fixture.FailNextCall("SQLFetch", SQL_ERROR, {{"HY000", 0, "streaming cursor read failed", {}}});
	}
	SECTION("A failed bind of the row array") {
		result = redshift_test::ExecuteStatement(*connection, QUERY, {});
		fixture.FailNextCall("SQLBindCol", SQL_ERROR, {{"HY000", 0, "Memory allocation error", {}}});
	}
	REQUIRE_THROWS(result->NextBatch(batch));

	CHECK(redshift_test::Capture([&] { redshift_test::ExecuteStatement(*connection, QUERY, {}); }) ==
	      OPEN_RESULT_REFUSAL);
	result.reset();
	CHECK(CellsToEnd(*redshift_test::ExecuteStatement(*connection, QUERY, {})) == std::vector<std::string> {"x", "y"});
}

TEST_CASE("A second result with no columns is drained, and the result ends normally", "[odbc]") {
	LifetimeFixture fixture;
	auto connection = fixture.OpenConnection();
	auto result = redshift_test::ExecuteStatement(*connection, QUERY_THEN_CLEANUP, {});

	CHECK(CellsToEnd(*result) == std::vector<std::string> {"x"});
	CHECK(fixture.CallsTo("SQLMoreResults") == 2);
	CHECK(fixture.fake.AllocatedHandles() == "environment, connection");
}

TEST_CASE("A failed drain at the end of the rows gives an executor error after the rows", "[odbc]") {
	LifetimeFixture fixture;
	auto connection = fixture.OpenConnection();
	auto result = redshift_test::ExecuteStatement(*connection, QUERY, {});
	fixture.FailNextCall("SQLMoreResults", SQL_ERROR, {{"HY000", 0, "streaming cursor read failed", {}}});

	RedshiftBatch batch;
	REQUIRE(result->NextBatch(batch));
	CHECK(batch.rows.size() == 2);
	CHECK(redshift_test::Capture([&] { result->NextBatch(batch); }) == FailedDrain("HY000"));
	CHECK(fixture.fake.AllocatedHandles() == "environment, connection");
}

TEST_CASE("A failed column count while draining at the end of the rows gives an executor error after the rows",
          "[odbc]") {
	LifetimeFixture fixture;
	auto connection = fixture.OpenConnection();
	auto result = redshift_test::ExecuteStatement(*connection, QUERY_THEN_CLEANUP, {});
	fixture.FailNextCall("SQLNumResultCols", SQL_ERROR, {{"HY009", 0, "Invalid use of null pointer", {}}});

	RedshiftBatch batch;
	REQUIRE(result->NextBatch(batch));
	CHECK(batch.rows.size() == 1);
	CHECK(redshift_test::Capture([&] { result->NextBatch(batch); }) == FailedDrain("HY009"));
}

TEST_CASE("A result whose drain failed fails every later NextBatch too, making no driver call", "[odbc]") {
	LifetimeFixture fixture;
	auto connection = fixture.OpenConnection();
	auto result = redshift_test::ExecuteStatement(*connection, TWO_RESULT_SETS, {});
	std::string sqlstate;
	SECTION("A failed SQLMoreResults") {
		fixture.FailNextCall("SQLMoreResults", SQL_ERROR, {{"HY000", 0, "streaming cursor read failed", {}}});
		sqlstate = "HY000";
	}
	SECTION("A failed SQLNumResultCols on the second result set") {
		fixture.FailNextCall("SQLNumResultCols", SQL_ERROR, {{"HY009", 0, "Invalid use of null pointer", {}}});
		sqlstate = "HY009";
	}
	RedshiftBatch batch;
	REQUIRE(result->NextBatch(batch));
	REQUIRE(batch.rows.size() == 1);
	CHECK(batch.rows[0][0].text == "x");
	REQUIRE(redshift_test::Capture([&] { result->NextBatch(batch); }) == FailedDrain(sqlstate));
	const auto calls = fixture.fake.Calls().size();

	CHECK(redshift_test::Capture([&] { result->NextBatch(batch); }) == FailedDrain(sqlstate));
	CHECK(redshift_test::Capture([&] { result->NextBatch(batch); }) == FailedDrain(sqlstate));
	CHECK(batch.rows.empty());
	CHECK(fixture.fake.Calls().size() == calls);
}

TEST_CASE("Execute fails a statement with no result columns whose second result set has columns", "[odbc]") {
	LifetimeFixture fixture;
	auto connection = fixture.OpenConnection();

	const auto error =
	    redshift_test::CaptureError([&] { redshift_test::ExecuteStatement(*connection, UPDATE_THEN_RESULT_SET, {}); });
	CHECK((Thrown {error.Type(), error.RawMessage()}) == MULTIPLE_RESULT_SETS);
	CHECK(error.ExtraInfo().empty());
	CHECK(fixture.fake.AllocatedHandles() == "environment, connection");
	CHECK(connection->SessionHoldsState());
}

TEST_CASE("The next Execute runs after Execute failed a statement for its second result set", "[odbc]") {
	LifetimeFixture fixture;
	auto connection = fixture.OpenConnection();
	REQUIRE(redshift_test::Capture([&] { redshift_test::ExecuteStatement(*connection, UPDATE_THEN_RESULT_SET, {}); }) ==
	        MULTIPLE_RESULT_SETS);

	CHECK(CellsToEnd(*redshift_test::ExecuteStatement(*connection, QUERY, {})) == std::vector<std::string> {"x", "y"});
}

TEST_CASE("Execute drains a second result with no columns, reporting the first result's row count", "[odbc]") {
	LifetimeFixture fixture;
	auto connection = fixture.OpenConnection();

	auto result = redshift_test::ExecuteStatement(*connection, UPDATE_THEN_CLEANUP, {});
	CHECK(result->RowCount() == 2);
	CHECK(fixture.CallsTo("SQLMoreResults") == 2);
	RedshiftBatch batch;
	CHECK_FALSE(result->NextBatch(batch));
}

TEST_CASE("A failed drain after a statement with no result columns gives the fetch-phase statement failure", "[odbc]") {
	LifetimeFixture fixture;
	auto connection = fixture.OpenConnection();
	fixture.FailNextCall("SQLMoreResults", SQL_ERROR, {{"XX000", 0, "internal error at 10.1.2.3", {}}});

	CHECK(redshift_test::Capture([&] { redshift_test::ExecuteStatement(*connection, UPDATE, {}); }) ==
	      Thrown {ExceptionType::EXECUTOR,
	              "Redshift could not return the rest of the result: internal error at <address>\nSQLSTATE: XX000"});
	CHECK(fixture.fake.AllocatedHandles() == "environment, connection");
	CHECK(redshift_test::ExecuteStatement(*connection, UPDATE, {})->RowCount() == 1);
}

TEST_CASE("A failed drain's message shows a parameter's value only as its position", "[odbc]") {
	LifetimeFixture fixture;
	const char *const update_one = "UPDATE t SET a = $1";
	fixture.fake.AddResult(update_one, {Affected(1)}, 1);
	auto connection = fixture.OpenConnection();
	fixture.FailNextCall("SQLMoreResults", SQL_ERROR,
	                     {{"22001", 0, "value too long for type character varying(4): token-12345678", {}}});

	CHECK(
	    redshift_test::Capture([&] { redshift_test::ExecuteStatement(*connection, update_one, {"token-12345678"}); }) ==
	    Thrown {ExceptionType::EXECUTOR, "Redshift could not return the rest of the result: value too long for type "
	                                     "character varying(4): <param 1>\nSQLSTATE: 22001"});
}

TEST_CASE("redshift_query reports a fetch that lost the connection, then runs on a new one", "[odbc]") {
	LifetimeFixture fixture;
	fixture.FailNextCall("SQLFetch", SQL_ERROR, {{"08S01", 0, "Streaming cursor read error", {}}});
	ExtensionDatabase database;
	database.Run(ATTACH_RS);

	const auto lost = database.con.Query("SELECT * FROM redshift_query('rs', 'SELECT 1')");
	REQUIRE(lost->HasError());
	CHECK(lost->GetErrorObject().Type() == ExceptionType::IO);
	CHECK(lost->GetErrorObject().RawMessage().find("was lost") != std::string::npos);
	CHECK(lost->GetErrorObject().RawMessage().find("was not retried") != std::string::npos);
	// redshift_query discards a connection the client reports lost, so the
	// next query opens a new one and runs on it.
	const auto next = database.con.Query("SELECT * FROM redshift_query('rs', 'SELECT 1')");
	REQUIRE_FALSE(next->HasError());
	CHECK(fixture.fake.AllocatedHandles() == "environment, connection");
}

TEST_CASE("redshift_query fails a statement whose second result set has columns with the executor error", "[odbc]") {
	LifetimeFixture fixture;
	ExtensionDatabase database;
	database.Run(ATTACH_RS);

	auto result = database.con.Query("SELECT * FROM redshift_query('rs', 'CALL two_result_sets()')");
	REQUIRE(result->HasError());
	const auto &error = result->GetErrorObject();
	CHECK((Thrown {error.Type(), error.RawMessage()}) == MULTIPLE_RESULT_SETS);
}

TEST_CASE("redshift_query runs the next statement after a scan that never reached its end", "[odbc]") {
	LifetimeFixture fixture;
	ExtensionDatabase database;
	database.Run(ATTACH_RS);
	// Each scan below stops before the NextBatch that returns false, so its result ends only when
	// DuckDB destroys the scan, which must come before the next statement's bind.
	SECTION("A scan cut short by LIMIT") {
		// More rows than one batch, so the scan stops before the driver's end of data: one batch's
		// row-array fetch, never the one that would answer SQL_NO_DATA and finish the statement. On
		// the caller's thread, as the producer would read this short result's end ahead.
		// A connection takes the thread when it opens, so this runs on a DuckDB connection whose session
		// opens after the switch: the ATTACH's opened before it.
		SetFetchThreadForTesting(FetchThread::CALLER);
		Connection caller(database.db);
		const auto fetches = fixture.CallsTo("SQLFetch");
		REQUIRE_FALSE(caller.Query("SELECT * FROM redshift_query('rs', 'SELECT a FROM big') LIMIT 1")->HasError());
		CHECK(fixture.CallsTo("SQLFetch") - fetches == 1);
	}
	SECTION("A scan cut short by LIMIT, the producer reading ahead") {
		database.Run("SELECT * FROM redshift_query('rs', 'SELECT a FROM big') LIMIT 1");
	}
	SECTION("A statement with no result columns") {
		database.Run("SELECT * FROM redshift_query('rs', 'UPDATE t SET a = 1')");
	}
	SECTION("A scan failed by a second result set") {
		REQUIRE(database.con.Query("SELECT * FROM redshift_query('rs', 'CALL two_result_sets()')")->HasError());
	}

	auto next = database.Run("SELECT * FROM redshift_query('rs', 'SELECT a FROM t') ORDER BY a");
	auto chunk = next->Fetch();
	REQUIRE(chunk);
	REQUIRE(chunk->size() == 2);
	// Checked through strings: client code names none of DuckDB's value types.
	CHECK(chunk->GetValue(0, 0).ToString() == "x");
	CHECK(chunk->GetValue(0, 1).ToString() == "y");
}

TEST_CASE("The extension's fence refuses two scans of one handle before the connection's open-result refusal",
          "[odbc]") {
	LifetimeFixture fixture;
	ExtensionDatabase database;
	database.Run(ATTACH_RS);

	auto read = database.con.Query("SELECT * FROM redshift_query('rs', 'SELECT a FROM t') UNION ALL "
	                               "SELECT * FROM redshift_query('rs', 'SELECT a FROM t')");
	REQUIRE(read->HasError());
	const auto &error = read->GetErrorObject();
	CHECK(error.Type() == ExceptionType::BINDER);
	CHECK(error.RawMessage().find("more than once in the same DuckDB statement") != std::string::npos);
	// Bind refuses before any scan runs, so the connection's own refusal is never reached.
	CHECK(fixture.CallsTo("SQLExecute") == redshift_test::SessionSetupStatements().size());
	CHECK(fixture.CallsTo("SQLExecDirect") == 0);
}
