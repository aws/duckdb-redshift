// The closed state: when statement allocation, prepare, parameter count, column count, description or type name,
// parameter bind, execute or Execute's drain of further results fails, and the driver then reports the connection dead
// or cannot answer, or a record shows the loss, the connection frees its handles and throws the transport failure,
// which says a write may have taken effect only when the loss came once the statement ran (SQLExecute, or SQLExecDirect
// for one with no parameters whose shape the simple protocol keeps). When a result's fetch, cell read or drain at the
// end of its rows fails that way, the result throws the transport failure and marks the connection lost, and the
// connection tears down at its next Describe or Execute. From then on the connection keeps answering
// SessionHoldsState() and Target(), and refuses Describe and Execute with IOException without a driver call. A setup
// statement whose record shows the loss is here too.

// catch.hpp comes first, as in this directory's other tests.
#include "catch.hpp"

#include "fake_driver_fixture.hpp"
#include "odbc/bulk_fetch.hpp"
#include "thrown.hpp"

#include <string>
#include <vector>

using namespace duckdb;
using redshift_driver::testing::FakeColumn;
using redshift_driver::testing::FakeDiag;
using redshift_driver::testing::FakeResultSet;
using redshift_test::FakeDriverFixture;
using redshift_test::Thrown;

namespace {

//! A record of a lost connection by its class 08 state alone: its text is none libpq writes for a
//! dropped connection, and its host and address must not reach the message.
const FakeDiag LOST {"08S01", 0, "unexpected eof from relay-1.internal (10.1.2.3)", {}};

//! UsEast1Config()'s target, as the client's messages name it.
const std::string TARGET = redshift_test::ServerlessTarget("us-east-1");

//! How a transport failure's message on that target opens.
const std::string LOST_LEAD = "The connection to Redshift at " + TARGET + " was lost (SQLSTATE class ";
//! How it ends when the loss came once the statement ran, and when it came before.
const std::string NOT_REPLAYED = "The statement was not replayed, because a write it carried may already have taken "
                                 "effect; check before running it again.\nSQLSTATE: ";
const std::string NOT_REPLAYED_BEFORE_RUN = "The statement was not replayed.\nSQLSTATE: ";

//! The transport failure for LOST, once the statement ran and before it.
const std::string LOST_MESSAGE = LOST_LEAD + "08, connection exception). " + NOT_REPLAYED + "08S01";
const std::string LOST_BEFORE_RUN_MESSAGE =
    LOST_LEAD + "08, connection exception). " + NOT_REPLAYED_BEFORE_RUN + "08S01";

//! A record that does not show a lost connection, with a host and an address that must not reach the message.
const FakeDiag OUT_OF_MEMORY {"53200", 0, "out of memory on relay-1.internal (10.1.2.3)", {}};

//! The transport failure for OUT_OF_MEMORY on a connection the driver reports dead or cannot answer for, once the
//! statement ran and before it.
const std::string OUT_OF_MEMORY_LOST_MESSAGE = LOST_LEAD + "53, insufficient resources). " + NOT_REPLAYED + "53200";
const std::string OUT_OF_MEMORY_LOST_BEFORE_RUN_MESSAGE =
    LOST_LEAD + "53, insufficient resources). " + NOT_REPLAYED_BEFORE_RUN + "53200";

//! The statement failure for OUT_OF_MEMORY, by the phase it arrived in, on a connection the driver
//! reports alive: the client's own words, the class and the SQLSTATE, and none of the record's text.
const char *const ALIVE_PREPARE_MESSAGE =
    "Redshift refused the statement (SQLSTATE class 53, insufficient resources). The server's message for this class "
    "is not shown; act on the SQLSTATE.\nSQLSTATE: 53200";
const char *const ALIVE_DESCRIBE_MESSAGE =
    "Redshift did not describe the statement's result columns (SQLSTATE class 53, insufficient resources). The "
    "server's message for this class is not shown; act on the SQLSTATE.\nSQLSTATE: 53200";

//! The refusal of Describe or Execute once the connection is closed.
const std::string CLOSED_MESSAGE =
    "the connection to Redshift at " + TARGET + " was lost and is closed; open a new connection to continue";

//! What NextBatch says once the result's connection has closed.
const char *const CLOSED_RESULT =
    "this Redshift result's connection has closed; run the statement again on a new connection";

//! The transport failure for a generic record carrying libpq's text for a dropped connection.
const std::string DROPPED_LEAD = "The connection to Redshift at " + TARGET + " was lost";
const std::string DROPPED_CAUSE = " (Redshift, or the network between this host and Redshift, closed it). ";
const std::string DROPPED_MESSAGE = DROPPED_LEAD + DROPPED_CAUSE + NOT_REPLAYED + "HY000";

//! A generic record carrying libpq's text for a dropped connection, as the driver passes it on.
const FakeDiag DROPPED {"HY000", 0, "[Redshift][ODBC Driver][Server]server closed the connection unexpectedly", {}};

//! A query whose first column is long, so that column and every one after it are read with
//! SQLGetData.
const char *const LONG_QUERY = "SELECT a FROM t";

//! Scripts LONG_QUERY as one row of a long text column and then the `after` columns.
void ScriptLongColumn(FakeDriverFixture &fixture, std::vector<FakeColumn> after) {
	FakeResultSet rows;
	rows.columns.push_back(redshift_test::LongVarcharColumn("t"));
	rows.columns.insert(rows.columns.end(), after.begin(), after.end());
	rows.rows.push_back(std::vector<std::string>(rows.columns.size(), "1"));
	fixture.fake.AddResult(LONG_QUERY, {rows});
}

//! Makes the next SQLPrepare fail, with a record that shows no loss, on a connection the driver then reports dead.
void ScriptLostPrepare(FakeDriverFixture &fixture) {
	fixture.FailNextCall("SQLPrepare", SQL_ERROR, {OUT_OF_MEMORY});
	fixture.fake.SetConnectionDead(true);
}

//! Closes `connection` through a transport failure at its next prepare.
void LoseConnection(FakeDriverFixture &fixture, RedshiftConnection &connection) {
	ScriptLostPrepare(fixture);
	REQUIRE(redshift_test::Capture([&] { connection.Describe("SELECT 1"); }) ==
	        Thrown {ExceptionType::IO, OUT_OF_MEMORY_LOST_BEFORE_RUN_MESSAGE});
}

} // namespace

TEST_CASE("A failed prepare on a connection the driver reports dead gives the transport failure", "[odbc]") {
	FakeDriverFixture fixture;
	auto connection = fixture.OpenConnection();
	ScriptLostPrepare(fixture);

	CHECK(redshift_test::Capture([&] { connection->Describe("SELECT 1"); }) ==
	      Thrown {ExceptionType::IO, OUT_OF_MEMORY_LOST_BEFORE_RUN_MESSAGE});
}

TEST_CASE("A failed prepare on a connection the driver reports alive leaves the connection open", "[odbc]") {
	FakeDriverFixture fixture;
	auto connection = fixture.OpenConnection();
	fixture.FailNextCall("SQLPrepare", SQL_ERROR, {OUT_OF_MEMORY});

	REQUIRE(redshift_test::Capture([&] { connection->Describe("SELECT 1"); }) ==
	        Thrown {ExceptionType::BINDER, ALIVE_PREPARE_MESSAGE});
	CHECK(connection->Describe("SELECT 1").columns.size() == 1);
}

TEST_CASE("A connection-dead answer that cannot be read counts as a lost connection", "[odbc]") {
	FakeDriverFixture fixture;
	auto connection = fixture.OpenConnection();
	fixture.FailNextCall("SQLPrepare", SQL_ERROR, {OUT_OF_MEMORY});
	fixture.fake.FailCall("SQLGetConnectAttr", 1, SQL_ERROR, {});

	CHECK(redshift_test::Capture([&] { connection->Describe("SELECT 1"); }) ==
	      Thrown {ExceptionType::IO, OUT_OF_MEMORY_LOST_BEFORE_RUN_MESSAGE});
}

TEST_CASE("A failed statement-handle allocation on a dead connection reports the allocation's SQLSTATE", "[odbc]") {
	FakeDriverFixture fixture;
	auto connection = fixture.OpenConnection();
	// The record is the one the driver's allocation leaves (public source v2.3.0, rsconnect.cpp
	// RS_CONN_INFO::RS_SQLAllocStmt). The fake, like the driver, clears the connection handle's records
	// when asked whether it is dead.
	fixture.FailNextCall("SQLAllocHandle", SQL_ERROR, {{"HY001", 0, "Memory allocation error", {}}});
	fixture.fake.SetConnectionDead(true);

	CHECK(redshift_test::Capture([&] { redshift_test::ExecuteStatement(*connection, "SELECT 1", {}); }) ==
	      Thrown {ExceptionType::IO, LOST_LEAD + "HY, driver error). " + NOT_REPLAYED_BEFORE_RUN + "HY001"});
}

TEST_CASE("A failed statement-handle allocation on a connection the driver reports alive leaves the connection open",
          "[odbc]") {
	FakeDriverFixture fixture;
	auto connection = fixture.OpenConnection();
	fixture.FailNextCall("SQLAllocHandle", SQL_ERROR, {{"HY001", 0, "Memory allocation error", {}}});

	REQUIRE(redshift_test::Capture([&] { redshift_test::ExecuteStatement(*connection, "SELECT 1", {}); }).type ==
	        ExceptionType::EXECUTOR);
	CHECK(fixture.fake.AllocatedHandles() == "environment, connection");
	CHECK(connection->Describe("SELECT 1").columns.size() == 1);
}

TEST_CASE("A failed column count on a dead connection gives the transport failure", "[odbc]") {
	FakeDriverFixture fixture;
	auto connection = fixture.OpenConnection();
	fixture.fake.FailCall("SQLNumResultCols", 1, SQL_ERROR, {OUT_OF_MEMORY});
	fixture.fake.SetConnectionDead(true);

	CHECK(redshift_test::Capture([&] { connection->Describe("SELECT 1"); }) ==
	      Thrown {ExceptionType::IO, OUT_OF_MEMORY_LOST_BEFORE_RUN_MESSAGE});
}

TEST_CASE("A failed column count on a connection the driver reports alive leaves the connection open", "[odbc]") {
	FakeDriverFixture fixture;
	auto connection = fixture.OpenConnection();
	fixture.fake.FailCall("SQLNumResultCols", 1, SQL_ERROR, {OUT_OF_MEMORY});

	REQUIRE(redshift_test::Capture([&] { connection->Describe("SELECT 1"); }) ==
	        Thrown {ExceptionType::BINDER, ALIVE_DESCRIBE_MESSAGE});
	CHECK(fixture.fake.AllocatedHandles() == "environment, connection");
	CHECK(connection->Describe("SELECT 1").columns.size() == 1);
}

TEST_CASE("A failed column description on a dead connection gives the transport failure", "[odbc]") {
	FakeDriverFixture fixture;
	auto connection = fixture.OpenConnection();
	fixture.fake.FailCall("SQLDescribeCol", 1, SQL_ERROR, {OUT_OF_MEMORY});
	fixture.fake.SetConnectionDead(true);

	CHECK(redshift_test::Capture([&] { connection->Describe("SELECT 1"); }) ==
	      Thrown {ExceptionType::IO, OUT_OF_MEMORY_LOST_BEFORE_RUN_MESSAGE});
}

TEST_CASE("A failed type-name read on a dead connection gives the transport failure", "[odbc]") {
	FakeDriverFixture fixture;
	auto connection = fixture.OpenConnection();
	fixture.fake.FailCall("SQLColAttribute", 1, SQL_ERROR, {OUT_OF_MEMORY});
	fixture.fake.SetConnectionDead(true);

	CHECK(redshift_test::Capture([&] { connection->Describe("SELECT 1"); }) ==
	      Thrown {ExceptionType::IO, OUT_OF_MEMORY_LOST_BEFORE_RUN_MESSAGE});
	CHECK(fixture.fake.AllocatedHandles() == "environment");
}

TEST_CASE("A failed column description on a connection the driver reports alive leaves the connection open", "[odbc]") {
	FakeDriverFixture fixture;
	auto connection = fixture.OpenConnection();
	fixture.fake.FailCall("SQLDescribeCol", 1, SQL_ERROR, {OUT_OF_MEMORY});

	REQUIRE(redshift_test::Capture([&] { connection->Describe("SELECT 1"); }) ==
	        Thrown {ExceptionType::BINDER, ALIVE_DESCRIBE_MESSAGE});
	CHECK(fixture.fake.AllocatedHandles() == "environment, connection");
	CHECK(connection->Describe("SELECT 1").columns.size() == 1);
}

TEST_CASE("A statement that fails to run on a dead connection gives the transport failure", "[odbc]") {
	FakeDriverFixture fixture;
	auto connection = fixture.OpenConnection();
	fixture.FailNextCall("SQLExecDirect", SQL_ERROR, {OUT_OF_MEMORY});
	fixture.fake.SetConnectionDead(true);

	CHECK(redshift_test::Capture([&] { redshift_test::ExecuteStatement(*connection, "SELECT 1", {}); }) ==
	      Thrown {ExceptionType::IO, OUT_OF_MEMORY_LOST_MESSAGE});
}

TEST_CASE("A failed parameter count on a dead connection gives the transport failure", "[odbc]") {
	FakeDriverFixture fixture;
	auto connection = fixture.OpenConnection();
	fixture.fake.FailCall("SQLNumParams", 1, SQL_ERROR, {OUT_OF_MEMORY});
	fixture.fake.SetConnectionDead(true);

	CHECK(redshift_test::Capture([&] { redshift_test::ExecuteStatement(*connection, "SELECT 1", {}); }) ==
	      Thrown {ExceptionType::IO, OUT_OF_MEMORY_LOST_BEFORE_RUN_MESSAGE});
	CHECK(fixture.fake.AllocatedHandles() == "environment");
}

TEST_CASE("A failed parameter bind on a dead connection gives the transport failure", "[odbc]") {
	FakeDriverFixture fixture;
	FakeResultSet no_columns;
	no_columns.row_count = 0;
	fixture.fake.AddResult("SET search_path TO $1", {no_columns}, 1);
	auto connection = fixture.OpenConnection();
	fixture.fake.FailCall("SQLBindParameter", 1, SQL_ERROR, {OUT_OF_MEMORY});
	fixture.fake.SetConnectionDead(true);

	CHECK(redshift_test::Capture([&] {
		      redshift_test::ExecuteStatement(*connection, "SET search_path TO $1", {"public"});
	      }) == Thrown {ExceptionType::IO, OUT_OF_MEMORY_LOST_BEFORE_RUN_MESSAGE});
	CHECK(fixture.fake.AllocatedHandles() == "environment");
	CHECK_FALSE(connection->SessionHoldsState());
}

TEST_CASE("A transport failure frees the statement before it tears the connection down", "[odbc]") {
	FakeDriverFixture fixture;
	auto connection = fixture.OpenConnection();
	const auto opened = fixture.fake.Calls().size();

	LoseConnection(fixture, *connection);
	CHECK(fixture.CallsAfter(opened) == std::vector<std::string> {"SQLAllocHandle", "SQLPrepare", "SQLGetConnectAttr",
	                                                              "SQLFreeHandle", "SQLEndTran", "SQLDisconnect",
	                                                              "SQLFreeHandle"});
	CHECK(fixture.fake.AllocatedHandles() == "environment");
}

TEST_CASE("A transport failure at execute frees the statement before it tears the connection down", "[odbc]") {
	FakeDriverFixture fixture;
	auto connection = fixture.OpenConnection();
	fixture.FailNextCall("SQLExecDirect", SQL_ERROR, {LOST});
	fixture.fake.SetConnectionDead(true);
	const auto opened = fixture.fake.Calls().size();

	REQUIRE(redshift_test::Capture([&] { redshift_test::ExecuteStatement(*connection, "SELECT 1", {}); }) ==
	        Thrown {ExceptionType::IO, LOST_MESSAGE});
	// Driver 2.3.0 frees a connection's statements with it, so a later statement free would touch a
	// freed handle.
	const auto calls = fixture.CallsAfter(opened);
	REQUIRE(calls.size() >= 4);
	CHECK(std::vector<std::string>(calls.end() - 4, calls.end()) ==
	      std::vector<std::string> {"SQLFreeHandle", "SQLEndTran", "SQLDisconnect", "SQLFreeHandle"});
	CHECK(fixture.fake.AllocatedHandles() == "environment");
}

TEST_CASE("A connection handle whose free fails in a transport failure is not passed to the driver again", "[odbc]") {
	FakeDriverFixture fixture;
	auto connection = fixture.OpenConnection();
	// The statement's free comes first, then the connection handle's.
	fixture.fake.FailCall("SQLFreeHandle", NumericCast<int>(fixture.CallsTo("SQLFreeHandle")) + 2, SQL_ERROR, {});
	LoseConnection(fixture, *connection);
	const auto calls = fixture.fake.Calls().size();

	connection.reset();
	CHECK(fixture.fake.Calls().size() == calls);
	CHECK(fixture.fake.MisusedAbandonedHandles().empty());
}

TEST_CASE("A closed connection refuses with IOException before it checks the SQL or the parameters", "[odbc]") {
	FakeDriverFixture fixture;
	auto connection = fixture.OpenConnection();
	LoseConnection(fixture, *connection);
	const auto calls = fixture.fake.Calls().size();
	const std::string nul_sql("SELECT 1\0", 9);

	CHECK(redshift_test::Capture([&] { connection->Describe(nul_sql); }) == Thrown {ExceptionType::IO, CLOSED_MESSAGE});
	CHECK(redshift_test::Capture([&] { redshift_test::ExecuteStatement(*connection, nul_sql, {}); }) ==
	      Thrown {ExceptionType::IO, CLOSED_MESSAGE});
	CHECK(redshift_test::Capture([&] { redshift_test::ExecuteStatement(*connection, "SELECT $1", {"x"}); }) ==
	      Thrown {ExceptionType::IO, CLOSED_MESSAGE});
	CHECK(fixture.fake.Calls().size() == calls);
}

TEST_CASE("A closed connection refuses Describe with IOException, making no driver call", "[odbc]") {
	FakeDriverFixture fixture;
	auto connection = fixture.OpenConnection();
	LoseConnection(fixture, *connection);
	const auto calls = fixture.fake.Calls().size();

	CHECK(redshift_test::Capture([&] { connection->Describe("SELECT 1"); }) ==
	      Thrown {ExceptionType::IO, CLOSED_MESSAGE});
	CHECK(fixture.fake.Calls().size() == calls);
}

TEST_CASE("A closed connection refuses Execute with IOException, making no driver call", "[odbc]") {
	FakeDriverFixture fixture;
	auto connection = fixture.OpenConnection();
	LoseConnection(fixture, *connection);
	const auto calls = fixture.fake.Calls().size();

	CHECK(redshift_test::Capture([&] { redshift_test::ExecuteStatement(*connection, "SELECT 1", {}); }) ==
	      Thrown {ExceptionType::IO, CLOSED_MESSAGE});
	CHECK(fixture.fake.Calls().size() == calls);
}

TEST_CASE("A closed connection still answers that its session holds no state when no statement ran", "[odbc]") {
	FakeDriverFixture fixture;
	auto connection = fixture.OpenConnection();

	LoseConnection(fixture, *connection);
	CHECK_FALSE(connection->SessionHoldsState());
}

TEST_CASE("A closed connection still answers that its session holds state once a statement ran", "[odbc]") {
	FakeDriverFixture fixture;
	auto connection = fixture.OpenConnection();
	redshift_test::ExecuteStatement(*connection, "SELECT 1", {});

	LoseConnection(fixture, *connection);
	CHECK(connection->SessionHoldsState());
}

TEST_CASE("A statement that fails to run on a dead connection leaves the session holding state", "[odbc]") {
	FakeDriverFixture fixture;
	auto connection = fixture.OpenConnection();
	fixture.FailNextCall("SQLExecDirect", SQL_ERROR, {OUT_OF_MEMORY});
	fixture.fake.SetConnectionDead(true);

	REQUIRE(redshift_test::Capture([&] { redshift_test::ExecuteStatement(*connection, "SELECT 1", {}); }) ==
	        Thrown {ExceptionType::IO, OUT_OF_MEMORY_LOST_MESSAGE});
	CHECK(connection->SessionHoldsState());
}

TEST_CASE("A closed connection keeps the target it had before the failure", "[odbc]") {
	FakeDriverFixture fixture;
	auto connection = fixture.OpenConnection();

	LoseConnection(fixture, *connection);
	CHECK(connection->Target() == TARGET);
}

TEST_CASE("A closed connection's destructor makes no driver call", "[odbc]") {
	FakeDriverFixture fixture;
	auto connection = fixture.OpenConnection();
	LoseConnection(fixture, *connection);
	const auto calls = fixture.fake.Calls().size();

	connection.reset();
	CHECK(fixture.fake.Calls().size() == calls);
}

TEST_CASE("A result that ended before a transport failure gives IOException from NextBatch, making no driver call",
          "[odbc]") {
	FakeDriverFixture fixture;
	auto connection = fixture.OpenConnection();
	auto result = redshift_test::ExecuteStatement(*connection, "SELECT 1", {});
	RedshiftBatch batch;
	REQUIRE(result->NextBatch(batch));
	REQUIRE_FALSE(result->NextBatch(batch));
	LoseConnection(fixture, *connection);
	const auto calls = fixture.fake.Calls().size();

	CHECK(redshift_test::Capture([&] { result->NextBatch(batch); }) == Thrown {ExceptionType::IO, CLOSED_RESULT});
	result.reset();
	CHECK(fixture.fake.Calls().size() == calls);
}

TEST_CASE("A failed drain after a statement with no result columns on a dead connection gives the transport failure",
          "[odbc]") {
	FakeDriverFixture fixture;
	FakeResultSet affected;
	affected.row_count = 1;
	fixture.fake.AddResult("UPDATE t SET a = 1", {affected});
	auto connection = fixture.OpenConnection();
	fixture.FailNextCall("SQLMoreResults", SQL_ERROR, {OUT_OF_MEMORY});
	fixture.fake.SetConnectionDead(true);

	CHECK(redshift_test::Capture([&] { redshift_test::ExecuteStatement(*connection, "UPDATE t SET a = 1", {}); }) ==
	      Thrown {ExceptionType::IO, OUT_OF_MEMORY_LOST_MESSAGE});
	CHECK(fixture.fake.AllocatedHandles() == "environment");
}

TEST_CASE("A class 08 record closes the connection even when the driver reports it alive", "[odbc]") {
	FakeDriverFixture fixture;
	auto connection = fixture.OpenConnection();
	fixture.FailNextCall("SQLPrepare", SQL_ERROR, {LOST});

	REQUIRE(redshift_test::Capture([&] { connection->Describe("SELECT 1"); }) ==
	        Thrown {ExceptionType::IO, LOST_BEFORE_RUN_MESSAGE});
	CHECK(fixture.fake.AllocatedHandles() == "environment");
	CHECK(redshift_test::Capture([&] { connection->Describe("SELECT 1"); }) ==
	      Thrown {ExceptionType::IO, CLOSED_MESSAGE});
}

TEST_CASE("A dropped-connection text closes the connection even when the driver reports it alive", "[odbc]") {
	FakeDriverFixture fixture;
	auto connection = fixture.OpenConnection();
	fixture.FailNextCall("SQLExecDirect", SQL_ERROR, {DROPPED});

	REQUIRE(redshift_test::Capture([&] { redshift_test::ExecuteStatement(*connection, "SELECT 1", {}); }) ==
	        Thrown {ExceptionType::IO, DROPPED_MESSAGE});
	CHECK(fixture.fake.AllocatedHandles() == "environment");
}

TEST_CASE("A setup statement that fails with a class 08 record says the connection was lost, even reported alive",
          "[odbc]") {
	FakeDriverFixture fixture;
	fixture.fake.FailCall("SQLExecute", 1, SQL_ERROR, {LOST});

	CHECK(redshift_test::Capture([&] { fixture.OpenConnection(); }) ==
	      Thrown {ExceptionType::IO, "Could not set up the Redshift session at " + TARGET +
	                                     ": the connection was lost during the setup (SQLSTATE class 08, connection "
	                                     "exception). No statement of yours was sent; run the statement again.\n"
	                                     "SQLSTATE: 08S01"});
}

TEST_CASE("A failed fetch on a connection the driver reports dead gives the transport failure", "[odbc]") {
	FakeDriverFixture fixture;
	auto connection = fixture.OpenConnection();
	auto result = redshift_test::ExecuteStatement(*connection, "SELECT 1", {});
	fixture.FailNextCall("SQLFetch", SQL_ERROR, {OUT_OF_MEMORY});
	fixture.fake.SetConnectionDead(true);

	RedshiftBatch batch;
	CHECK(redshift_test::Capture([&] { result->NextBatch(batch); }) ==
	      Thrown {ExceptionType::IO, OUT_OF_MEMORY_LOST_MESSAGE});
}

TEST_CASE("A failed fetch whose connection-dead answer cannot be read gives the transport failure", "[odbc]") {
	FakeDriverFixture fixture;
	auto connection = fixture.OpenConnection();
	auto result = redshift_test::ExecuteStatement(*connection, "SELECT 1", {});
	fixture.FailNextCall("SQLFetch", SQL_ERROR, {OUT_OF_MEMORY});
	fixture.FailNextCall("SQLGetConnectAttr", SQL_ERROR, {});

	RedshiftBatch batch;
	CHECK(redshift_test::Capture([&] { result->NextBatch(batch); }) ==
	      Thrown {ExceptionType::IO, OUT_OF_MEMORY_LOST_MESSAGE});
}

TEST_CASE("A failed fetch with a class 08 record gives the transport failure on a connection reported alive",
          "[odbc]") {
	FakeDriverFixture fixture;
	auto connection = fixture.OpenConnection();
	auto result = redshift_test::ExecuteStatement(*connection, "SELECT 1", {});
	fixture.FailNextCall("SQLFetch", SQL_ERROR, {LOST});

	RedshiftBatch batch;
	CHECK(redshift_test::Capture([&] { result->NextBatch(batch); }) == Thrown {ExceptionType::IO, LOST_MESSAGE});
}

TEST_CASE("A failed cell read with a dropped-connection text gives the transport failure", "[odbc]") {
	FakeDriverFixture fixture;
	ScriptLongColumn(fixture, {});
	auto connection = fixture.OpenConnection();
	auto result = redshift_test::ExecuteStatement(*connection, LONG_QUERY, {});
	fixture.FailNextCall("SQLGetData", SQL_ERROR, {DROPPED});

	RedshiftBatch batch;
	CHECK(redshift_test::Capture([&] { result->NextBatch(batch); }) == Thrown {ExceptionType::IO, DROPPED_MESSAGE});
	// The read marked the connection lost: the result refuses, and the next call tears the connection down.
	CHECK(redshift_test::Capture([&] { result->NextBatch(batch); }) == Thrown {ExceptionType::IO, CLOSED_RESULT});
	result.reset();
	CHECK(redshift_test::Capture([&] { connection->Describe("SELECT 1"); }) ==
	      Thrown {ExceptionType::IO, CLOSED_MESSAGE});
	CHECK(fixture.fake.AllocatedHandles() == "environment");
}

TEST_CASE("A failed streaming read under libpq's last server SQLSTATE gives the transport failure", "[odbc]") {
	FakeDriverFixture fixture;
	auto connection = fixture.OpenConnection();
	auto result = redshift_test::ExecuteStatement(*connection, "SELECT 1", {});
	fixture.FailNextCall("SQLFetch", SQL_ERROR,
	                     {{"42P01",
	                       0,
	                       "[Redshift][ODBC Driver]An I/O error occurred while reading streaming cursor. Detail: "
	                       "server closed the connection unexpectedly",
	                       {}}});

	RedshiftBatch batch;
	CHECK(redshift_test::Capture([&] { result->NextBatch(batch); }) == Thrown {ExceptionType::IO, DROPPED_MESSAGE});
}

TEST_CASE("A failed cell read on a connection the driver reports dead gives the transport failure", "[odbc]") {
	FakeDriverFixture fixture;
	// The row's cell reads that succeed before the failing one, and what the failing one asks for: its
	// column and C type. The record names no column, so the call log shows which read failed.
	int reads_before = 0;
	std::string failed_read = "1 " + std::to_string(SQL_C_CHAR);
	SECTION("a text cell") {
		ScriptLongColumn(fixture, {});
	}
	SECTION("a fixed-size cell") {
		// A number is a fixed-size cell only on the native binding; the text binding reads it as text.
		SetNumberBindingForTesting(NumberBinding::NATIVE);
		FakeColumn column;
		column.name = "a";
		column.type_name = "INTEGER";
		column.sql_type = SQL_INTEGER;
		ScriptLongColumn(fixture, {column});
		reads_before = 1;
		failed_read = "2 " + std::to_string(SQL_C_SBIGINT);
	}
	auto connection = fixture.OpenConnection();
	auto result = redshift_test::ExecuteStatement(*connection, LONG_QUERY, {});
	fixture.fake.FailCall("SQLGetData", NumericCast<int>(fixture.CallsTo("SQLGetData")) + reads_before + 1, SQL_ERROR,
	                      {OUT_OF_MEMORY});
	fixture.fake.SetConnectionDead(true);

	RedshiftBatch batch;
	CHECK(redshift_test::Capture([&] { result->NextBatch(batch); }) ==
	      Thrown {ExceptionType::IO, OUT_OF_MEMORY_LOST_MESSAGE});
	CHECK(fixture.DetailsOf("SQLGetData").back() == failed_read);
}

TEST_CASE("A fetch failure whose server text quotes a dropped-connection text keeps the connection", "[odbc]") {
	FakeDriverFixture fixture;
	auto connection = fixture.OpenConnection();
	auto result = redshift_test::ExecuteStatement(*connection, "SELECT 1", {});
	fixture.FailNextCall("SQLFetch", SQL_ERROR,
	                     {{"22P02",
	                       0,
	                       "[Redshift][ODBC Driver][Server]22P02:ERROR:  invalid input syntax for integer: "
	                       "\"connection not open\"",
	                       {}}});

	RedshiftBatch batch;
	REQUIRE(redshift_test::Capture([&] { result->NextBatch(batch); }) ==
	        Thrown {ExceptionType::EXECUTOR, "Redshift could not return the rest of the result: ERROR:  invalid input "
	                                         "syntax for integer: \"connection not open\"\nSQLSTATE: 22P02"});
	result.reset();
	CHECK(connection->Describe("SELECT 1").columns.size() == 1);
}

TEST_CASE("A prepare refusal whose server text quotes a dropped-connection text keeps the connection", "[odbc]") {
	FakeDriverFixture fixture;
	auto connection = fixture.OpenConnection();
	fixture.FailNextCall("SQLPrepare", SQL_ERROR,
	                     {{"42P01",
	                       0,
	                       "[Redshift][ODBC Driver][Server]42P01:ERROR:  relation \"server closed the connection "
	                       "unexpectedly\" does not exist",
	                       {}}});

	REQUIRE(
	    redshift_test::Capture([&] { connection->Describe("SELECT 1"); }) ==
	    Thrown {ExceptionType::BINDER,
	            "Redshift refused the statement: ERROR:  relation \"server closed the connection unexpectedly\" does "
	            "not exist\nSQLSTATE: 42P01"});
	CHECK(fixture.fake.AllocatedHandles() == "environment, connection");
	CHECK(connection->Describe("SELECT 1").columns.size() == 1);
}

TEST_CASE("A failed fetch on a connection the driver reports alive leaves the connection open", "[odbc]") {
	FakeDriverFixture fixture;
	auto connection = fixture.OpenConnection();
	auto result = redshift_test::ExecuteStatement(*connection, "SELECT 1", {});
	fixture.FailNextCall("SQLFetch", SQL_ERROR, {{"22012", 0, "ERROR:  division by zero", {}}});

	RedshiftBatch batch;
	REQUIRE(redshift_test::Capture([&] { result->NextBatch(batch); }) ==
	        Thrown {ExceptionType::EXECUTOR,
	                "Redshift could not return the rest of the result: ERROR:  division by zero\nSQLSTATE: 22012"});
	result.reset();
	CHECK(connection->Describe("SELECT 1").columns.size() == 1);
}

TEST_CASE("A failed fetch's message shows a parameter's value only as its position", "[odbc]") {
	FakeDriverFixture fixture;
	fixture.fake.AddResult("SELECT a FROM t WHERE b = $1",
	                       {redshift_test::OneColumn(redshift_test::VarcharColumn("a", 16), {"x"})}, 1);
	auto connection = fixture.OpenConnection();
	auto result = redshift_test::ExecuteStatement(*connection, "SELECT a FROM t WHERE b = $1", {"token-12345678"});
	fixture.FailNextCall("SQLFetch", SQL_ERROR, {{"22P02", 0, "invalid input syntax: token-12345678", {}}});

	RedshiftBatch batch;
	CHECK(
	    redshift_test::Capture([&] { result->NextBatch(batch); }) ==
	    Thrown {ExceptionType::EXECUTOR,
	            "Redshift could not return the rest of the result: invalid input syntax: <param 1>\nSQLSTATE: 22P02"});
}

TEST_CASE("A failed drain at the end of the rows on a dead connection gives the transport failure after the rows",
          "[odbc]") {
	FakeDriverFixture fixture;
	auto connection = fixture.OpenConnection();
	auto result = redshift_test::ExecuteStatement(*connection, "SELECT 1", {});
	fixture.FailNextCall("SQLMoreResults", SQL_ERROR, {OUT_OF_MEMORY});
	fixture.fake.SetConnectionDead(true);

	RedshiftBatch batch;
	REQUIRE(result->NextBatch(batch));
	CHECK(batch.rows.size() == 1);
	CHECK(redshift_test::Capture([&] { result->NextBatch(batch); }) ==
	      Thrown {ExceptionType::IO, OUT_OF_MEMORY_LOST_MESSAGE});
	CHECK(redshift_test::Capture([&] { result->NextBatch(batch); }) == Thrown {ExceptionType::IO, CLOSED_RESULT});
}

TEST_CASE("After a fetch lost the connection, every later NextBatch gives IOException, making no driver call",
          "[odbc]") {
	FakeDriverFixture fixture;
	auto connection = fixture.OpenConnection();
	auto result = redshift_test::ExecuteStatement(*connection, "SELECT 1", {});
	fixture.FailNextCall("SQLFetch", SQL_ERROR, {LOST});
	RedshiftBatch batch;
	REQUIRE(redshift_test::Capture([&] { result->NextBatch(batch); }) == Thrown {ExceptionType::IO, LOST_MESSAGE});
	const auto calls = fixture.fake.Calls().size();

	CHECK(redshift_test::Capture([&] { result->NextBatch(batch); }) == Thrown {ExceptionType::IO, CLOSED_RESULT});
	CHECK(fixture.fake.Calls().size() == calls);
}

TEST_CASE("A connection a failed fetch lost is torn down at its next call, which it refuses", "[odbc]") {
	FakeDriverFixture fixture;
	auto connection = fixture.OpenConnection();
	auto result = redshift_test::ExecuteStatement(*connection, "SELECT 1", {});
	fixture.FailNextCall("SQLFetch", SQL_ERROR, {LOST});
	RedshiftBatch batch;
	REQUIRE(redshift_test::Capture([&] { result->NextBatch(batch); }) == Thrown {ExceptionType::IO, LOST_MESSAGE});
	const auto lost = fixture.fake.Calls().size();

	CHECK(redshift_test::Capture([&] { redshift_test::ExecuteStatement(*connection, "SELECT 1", {}); }) ==
	      Thrown {ExceptionType::IO, CLOSED_MESSAGE});
	CHECK(fixture.CallsAfter(lost) ==
	      std::vector<std::string> {"SQLFreeHandle", "SQLEndTran", "SQLDisconnect", "SQLFreeHandle"});
	CHECK(fixture.fake.AllocatedHandles() == "environment");
	CHECK(connection->SessionHoldsState());
	CHECK(connection->Target() == TARGET);
}

namespace {

//! One row of a long column, executed: its set reads the end of the rows with the row, so a caller
//! that stops at its rows has drained the further results. A bound result reads the end only in the
//! NextBatch that returns false.
duckdb::unique_ptr<RedshiftResult> ExecuteLongColumn(FakeDriverFixture &fixture, RedshiftConnection &connection) {
	const char *const sql = "SELECT v FROM t";
	fixture.fake.AddResult(sql, {redshift_test::OneColumn(redshift_test::LongVarcharColumn("v"), {"x"})});
	return redshift_test::ExecuteStatement(connection, sql, {});
}

} // namespace

TEST_CASE("A connection a failed drain lost is torn down at its next call, even when the result stops at its rows",
          "[odbc]") {
	FakeDriverFixture fixture;
	auto connection = fixture.OpenConnection();
	auto result = ExecuteLongColumn(fixture, *connection);
	fixture.FailNextCall("SQLMoreResults", SQL_ERROR, {LOST});
	RedshiftBatch batch;
	REQUIRE(result->NextBatch(batch));
	result.reset();
	const auto stopped = fixture.fake.Calls().size();

	CHECK(redshift_test::Capture([&] { connection->Describe("SELECT 1"); }) ==
	      Thrown {ExceptionType::IO, CLOSED_MESSAGE});
	CHECK(fixture.CallsAfter(stopped) == std::vector<std::string> {"SQLEndTran", "SQLDisconnect", "SQLFreeHandle"});
	CHECK(fixture.fake.AllocatedHandles() == "environment");
}

TEST_CASE("A failed drain that keeps the connection leaves it open when the result stops at its rows", "[odbc]") {
	FakeDriverFixture fixture;
	auto connection = fixture.OpenConnection();
	auto result = ExecuteLongColumn(fixture, *connection);
	fixture.FailNextCall("SQLMoreResults", SQL_ERROR, {OUT_OF_MEMORY});
	RedshiftBatch batch;
	REQUIRE(result->NextBatch(batch));
	result.reset();

	CHECK(connection->Describe("SELECT 1").columns.size() == 1);
}

TEST_CASE("A statement a transport failure ended is not run again", "[odbc]") {
	FakeDriverFixture fixture;
	auto connection = fixture.OpenConnection();
	const auto executes = fixture.CallsTo("SQLExecDirect");
	const auto prepares = fixture.CallsTo("SQLPrepare");
	SECTION("a failure while it runs") {
		fixture.FailNextCall("SQLExecDirect", SQL_ERROR, {LOST});
		REQUIRE(redshift_test::Capture([&] { redshift_test::ExecuteStatement(*connection, "SELECT 1", {}); }) ==
		        Thrown {ExceptionType::IO, LOST_MESSAGE});
	}
	SECTION("a failure while its rows are fetched") {
		auto result = redshift_test::ExecuteStatement(*connection, "SELECT 1", {});
		fixture.FailNextCall("SQLFetch", SQL_ERROR, {LOST});
		RedshiftBatch batch;
		REQUIRE(redshift_test::Capture([&] { result->NextBatch(batch); }) == Thrown {ExceptionType::IO, LOST_MESSAGE});
		REQUIRE(redshift_test::Capture([&] { connection->Describe("SELECT 1"); }) ==
		        Thrown {ExceptionType::IO, CLOSED_MESSAGE});
	}

	CHECK(fixture.CallsTo("SQLExecDirect") == executes + 1);
	CHECK(fixture.CallsTo("SQLPrepare") == prepares + 1);
}
