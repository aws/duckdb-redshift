// The session setup: right after SQLDriverConnect, before any caller statement, a connection
// prepares and runs the statements that pin the session's output settings.
// They leave SessionHoldsState() false. A failure of any one fails the connect
// with an IOException that says the setup failed and that no caller statement was sent, and frees
// the connection's handles.

// catch.hpp comes first, as in this directory's other tests.
#include "catch.hpp"

#include "duckdb/common/numeric_utils.hpp"
#include "duckdb/common/unordered_map.hpp"
#include "duckdb/main/database.hpp"
#include "fake_driver_fixture.hpp"
#include "thrown.hpp"

#include <algorithm>
#include <string>
#include <vector>

using namespace duckdb;
using redshift_driver::testing::FakeDiag;
using redshift_test::FakeDriverFixture;
using redshift_test::Thrown;

namespace {

//! The setup statements as the fixture writes them out, kept separately from the client's `SESSION_SETUP`, so
//! that a change to that list fails a case.
const std::vector<std::string> SETUP = redshift_test::SessionSetupStatements();

//! A record that does not show a lost connection, so the driver's connection-dead answer decides, with a host and an
//! address that must not reach the message.
const FakeDiag OUT_OF_MEMORY {"53200", 0, "out of memory on relay-1.internal (10.1.2.3)", {}};

//! The SQL text of every SQLPrepare the fake received, oldest first.
std::vector<std::string> PreparedTexts(const FakeDriverFixture &fixture) {
	std::vector<std::string> texts;
	for (const auto &call : fixture.fake.Calls()) {
		if (call.entry_point == "SQLPrepare") {
			texts.push_back(call.detail);
		}
	}
	return texts;
}

//! The position in the call log of the first call to `entry_point`; the log's size when there is none.
size_t FirstCallTo(const FakeDriverFixture &fixture, const std::string &entry_point) {
	const auto names = fixture.EntryPoints();
	return NumericCast<size_t>(std::find(names.begin(), names.end(), entry_point) - names.begin());
}

//! What a connect throws when the setup statement `sql` fails with no record on a connection the
//! driver reports alive.
Thrown SetupRefusal(const std::string &sql) {
	return Thrown {ExceptionType::IO,
	               "Could not set up the Redshift session at " + redshift_test::ServerlessTarget("us-east-1") +
	                   ": the setup statement \"" + sql +
	                   "\" failed, and the driver returned no diagnostic record. The connection is closed, and no "
	                   "statement of yours was sent. Run the statement again, and if it fails the same way, report "
	                   "it with the extension and driver versions."};
}

//! What opening a connection threw.
Thrown OpenFailure(FakeDriverFixture &fixture) {
	return redshift_test::Capture([&] { fixture.OpenConnection(); });
}

} // namespace

TEST_CASE("A new connection prepares the setup statements, in order, before a caller's statement", "[odbc]") {
	FakeDriverFixture fixture;
	auto connection = fixture.OpenConnection();
	redshift_test::ExecuteStatement(*connection, "SELECT 1", {});

	auto expected = SETUP;
	expected.push_back("SELECT 1");
	CHECK(PreparedTexts(fixture) == expected);
}

TEST_CASE("The setup statements are prepared only after SQLDriverConnect has returned", "[odbc]") {
	FakeDriverFixture fixture;
	auto connection = fixture.OpenConnection();

	REQUIRE(FirstCallTo(fixture, "SQLPrepare") < fixture.EntryPoints().size());
	CHECK(FirstCallTo(fixture, "SQLDriverConnect") < FirstCallTo(fixture, "SQLPrepare"));
}

TEST_CASE("Each setup statement is prepared and run on a statement handle of its own, then freed", "[odbc]") {
	FakeDriverFixture fixture;
	auto connection = fixture.OpenConnection();

	const std::vector<std::string> one {"SQLAllocHandle", "SQLPrepare", "SQLExecute", "SQLFreeHandle"};
	std::vector<std::string> expected;
	for (size_t statement = 0; statement < SETUP.size(); statement++) {
		expected.insert(expected.end(), one.begin(), one.end());
	}
	CHECK(fixture.CallsAfter(FirstCallTo(fixture, "SQLDriverConnect") + 1) == expected);
	CHECK(fixture.fake.AllocatedHandles() == "environment, connection");
}

TEST_CASE("The session setup leaves SessionHoldsState() false", "[odbc]") {
	FakeDriverFixture fixture;
	auto connection = fixture.OpenConnection();

	CHECK_FALSE(connection->SessionHoldsState());
}

TEST_CASE("Every new connection runs the setup statements again", "[odbc]") {
	FakeDriverFixture fixture;
	{ auto first = fixture.OpenConnection(); }
	auto second = fixture.OpenConnection();

	auto expected = SETUP;
	expected.insert(expected.end(), SETUP.begin(), SETUP.end());
	CHECK(PreparedTexts(fixture) == expected);
}

// A caller's change to a pinned setting is left in place: the setup never runs again
// on that connection, whatever the caller's SET or RESET changed.
TEST_CASE("After a caller's SET and RESET of a pinned setting the connection does not run the setup again", "[odbc]") {
	FakeDriverFixture fixture;
	redshift_driver::testing::FakeResultSet no_columns;
	no_columns.row_count = 0;
	fixture.fake.AddResult("SET extra_float_digits TO 0", {no_columns});
	fixture.fake.AddResult("RESET extra_float_digits", {no_columns});
	auto connection = fixture.OpenConnection();
	const std::vector<std::string> caller {"SET extra_float_digits TO 0", "SELECT 1", "RESET extra_float_digits",
	                                       "SELECT 1"};
	for (const auto &sql : caller) {
		redshift_test::ExecuteStatement(*connection, sql, {});
	}

	auto expected = SETUP;
	expected.insert(expected.end(), caller.begin(), caller.end());
	CHECK(PreparedTexts(fixture) == expected);
}

TEST_CASE("A setup statement Redshift refuses to prepare fails the connect with IOException naming it", "[odbc]") {
	FakeDriverFixture fixture;
	int failing = 0;
	SECTION("the first") {
		failing = 1;
	}
	SECTION("the second") {
		failing = 2;
	}
	SECTION("the third") {
		failing = 3;
	}
	SECTION("the fourth, the interval style") {
		failing = 4;
	}
	fixture.fake.FailCall("SQLPrepare", failing, SQL_ERROR, {});

	CHECK(OpenFailure(fixture) == SetupRefusal(SETUP.at(NumericCast<size_t>(failing - 1))));
	CHECK(fixture.fake.AllocatedHandles() == "environment");
}

TEST_CASE("A setup statement that fails to run fails the connect with IOException naming it", "[odbc]") {
	FakeDriverFixture fixture;
	int failing = 0;
	SECTION("the first") {
		failing = 1;
	}
	SECTION("the second") {
		failing = 2;
	}
	SECTION("the third") {
		failing = 3;
	}
	SECTION("the fourth, the interval style") {
		failing = 4;
	}
	fixture.fake.FailCall("SQLExecute", failing, SQL_ERROR, {});

	CHECK(OpenFailure(fixture) == SetupRefusal(SETUP.at(NumericCast<size_t>(failing - 1))));
	CHECK(fixture.fake.AllocatedHandles() == "environment");
}

TEST_CASE("A failed setup statement-handle allocation fails the connect with IOException naming it", "[odbc]") {
	FakeDriverFixture fixture;
	// The environment and the connection handle are the first two allocations.
	fixture.fake.FailCall("SQLAllocHandle", 3, SQL_ERROR, {});

	CHECK(OpenFailure(fixture) == SetupRefusal("SET extra_float_digits TO 2"));
	CHECK(fixture.fake.AllocatedHandles() == "environment");
}

TEST_CASE("A failed setup statement-handle allocation reports the SQLSTATE of the record it left", "[odbc]") {
	FakeDriverFixture fixture;
	// The record is the one the driver's allocation leaves on the connection handle (public source
	// v2.3.0, rsconnect.cpp RS_CONN_INFO::RS_SQLAllocStmt), which the fake, like the driver, clears when asked whether
	// the connection is dead.
	fixture.fake.FailCall("SQLAllocHandle", 3, SQL_ERROR, {{"HY001", 0, "Memory allocation error", {}}});

	CHECK(
	    OpenFailure(fixture) ==
	    Thrown {ExceptionType::IO,
	            "Could not set up the Redshift session at " + redshift_test::ServerlessTarget("us-east-1") +
	                ": the setup statement \"SET extra_float_digits TO 2\" failed (SQLSTATE class HY, driver error). "
	                "The connection is closed, and no statement of yours was sent. Run the statement again, and if it "
	                "fails the same way, report it with the extension and driver versions.\nSQLSTATE: HY001"});
}

TEST_CASE("A failed setup statement's message carries its record's SQLSTATE but none of the record's text", "[odbc]") {
	FakeDriverFixture fixture;
	fixture.fake.FailCall("SQLExecute", 2, SQL_ERROR,
	                      {{"42704", 0, "unrecognized configuration parameter on relay-1.internal (10.1.2.3)", {}}});

	CHECK(OpenFailure(fixture) ==
	      Thrown {ExceptionType::IO,
	              "Could not set up the Redshift session at " + redshift_test::ServerlessTarget("us-east-1") +
	                  ": the setup statement \"SET timezone TO 'UTC'\" failed (SQLSTATE class 42). The connection is "
	                  "closed, and no statement of yours was sent. Run the statement again, and if it fails the same "
	                  "way, report it with the extension and driver versions.\nSQLSTATE: 42704"});
}

TEST_CASE("A setup statement that fails on a dead connection says the connection was lost during the setup", "[odbc]") {
	FakeDriverFixture fixture;
	int failing = 0;
	SECTION("the second") {
		failing = 2;
	}
	SECTION("the fourth, the interval style") {
		failing = 4;
	}
	fixture.fake.FailCall("SQLExecute", failing, SQL_ERROR, {OUT_OF_MEMORY});
	fixture.fake.SetConnectionDead(true);

	CHECK(OpenFailure(fixture) ==
	      Thrown {ExceptionType::IO, "Could not set up the Redshift session at " +
	                                     redshift_test::ServerlessTarget("us-east-1") +
	                                     ": the connection was lost during the setup (SQLSTATE class 53, "
	                                     "insufficient resources). No statement of yours was sent; run the statement "
	                                     "again.\nSQLSTATE: 53200"});
	CHECK(fixture.fake.AllocatedHandles() == "environment");
}

TEST_CASE("A setup failure's extra_info holds the SQLSTATE of the record it acted on, and the driver", "[odbc]") {
	FakeDriverFixture fixture;
	const std::string driver = "test-fake fake " + DuckDB::Platform();
	duckdb::unordered_map<duckdb::string, duckdb::string> expected;
	SECTION("a setup statement Redshift refused") {
		fixture.fake.FailCall("SQLExecute", 2, SQL_ERROR,
		                      {{"HY000", 0, "generic", {}}, {"42704", 0, "unrecognized configuration parameter", {}}});
		expected = {{"sqlstate", "42704"}, {"sqlstates", "HY000,42704"}, {"redshift_driver", driver}};
	}
	SECTION("a connection lost during the setup") {
		fixture.fake.FailCall("SQLExecute", 2, SQL_ERROR, {OUT_OF_MEMORY});
		fixture.fake.SetConnectionDead(true);
		expected = {{"sqlstate", "53200"}, {"sqlstates", "53200"}, {"redshift_driver", driver}};
	}
	SECTION("a setup failure with no record") {
		fixture.fake.FailCall("SQLExecute", 2, SQL_ERROR, {});
		expected = {{"redshift_driver", driver}};
	}

	const auto error = redshift_test::CaptureError([&] { fixture.OpenConnection(); });
	REQUIRE(error.Type() == ExceptionType::IO);
	CHECK(error.ExtraInfo() == expected);
}

TEST_CASE("A setup statement the driver answers with SQL_NO_DATA fails the connect", "[odbc]") {
	// Stricter than Execute, where SQL_NO_DATA marks a statement that changed no row: a SET is not one.
	FakeDriverFixture fixture;
	fixture.fake.FailCall("SQLExecute", 1, SQL_NO_DATA, {});

	CHECK(OpenFailure(fixture) == SetupRefusal(SETUP[0]));
	CHECK(fixture.fake.AllocatedHandles() == "environment");
}

TEST_CASE("A setup statement that succeeds with a warning counts as run, and its records are logged", "[odbc]") {
	FakeDriverFixture fixture;
	SECTION("a prepare that warns") {
		fixture.fake.WarnCall("SQLPrepare", 2, {{"01000", 0, "notice from relay-1.internal", {}}});
	}
	SECTION("an execute that warns") {
		fixture.fake.WarnCall("SQLExecute", 2, {{"01000", 0, "notice from relay-1.internal", {}}});
	}

	auto connection = fixture.OpenConnection();
	CHECK(PreparedTexts(fixture) == SETUP);
	// After the driver's identity: the one record, by its SQLSTATE, with none of its class 01 text.
	CHECK(fixture.log_sink->Lines() ==
	      std::vector<std::string> {"Redshift driver loaded: test-fake fake " + DuckDB::Platform(),
	                                "Redshift driver diagnostic, SQLSTATE 01000"});
}

TEST_CASE("A failed setup statement frees its statement, then ends the session it connected", "[odbc]") {
	FakeDriverFixture fixture;
	auto expected = SetupRefusal("SET extra_float_digits TO 2");
	SECTION("on a connection the driver reports alive") {
	}
	SECTION("on a connection the driver reports dead") {
		fixture.fake.SetConnectionDead(true);
		expected =
		    Thrown {ExceptionType::IO,
		            "Could not set up the Redshift session at " + redshift_test::ServerlessTarget("us-east-1") +
		                ": the connection was lost during the setup, and the driver returned no diagnostic record. "
		                "No statement of yours was sent; run the statement again."};
	}
	fixture.fake.FailCall("SQLExecute", 1, SQL_ERROR, {});

	REQUIRE(OpenFailure(fixture) == expected);
	CHECK(fixture.CallsAfter(FirstCallTo(fixture, "SQLExecute")) ==
	      std::vector<std::string> {"SQLExecute", "SQLGetConnectAttr", "SQLFreeHandle", "SQLEndTran", "SQLDisconnect",
	                                "SQLFreeHandle"});
}

TEST_CASE("A connect after a failed setup runs the setup again", "[odbc]") {
	FakeDriverFixture fixture;
	fixture.fake.FailCall("SQLExecute", 1, SQL_ERROR, {});
	REQUIRE(OpenFailure(fixture) == SetupRefusal("SET extra_float_digits TO 2"));

	auto connection = fixture.OpenConnection();
	const auto texts = PreparedTexts(fixture);
	REQUIRE(texts.size() >= SETUP.size());
	CHECK(std::vector<std::string>(texts.end() - NumericCast<std::ptrdiff_t>(SETUP.size()), texts.end()) == SETUP);
	CHECK_FALSE(connection->SessionHoldsState());
}
