// The connect retry for a workgroup resuming from idle suspension: which failures start another
// attempt, the attempt limit and the time budget, the wait it logs, and the handle each attempt
// gets. The connect's clock is scripted, so no case waits for the budget.

// catch.hpp comes first, as in this directory's other tests.
#include "catch.hpp"

#include "driver_source_hook.hpp"
#include "duckdb/common/exception.hpp"
#include "duckdb/common/numeric_utils.hpp"
#include "duckdb/main/database.hpp"
#include "fake_driver_fixture.hpp"
#include "thrown.hpp"

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <memory>
#include <string>
#include <vector>

using namespace duckdb;
using redshift_driver::testing::FakeDiag;
using redshift_test::FakeDriverFixture;
using redshift_test::Thrown;

namespace {

const std::string US_EAST_1_TARGET = redshift_test::ServerlessTarget("us-east-1");

//! The line the driver's load logs, ahead of every connect's own.
std::string LoadedLine() {
	return "Redshift driver loaded: test-fake fake " + DuckDB::Platform();
}

//! The line logged before attempt `attempt` of three.
std::string WaitLine(int attempt) {
	return "Redshift connect timed out; waiting for the workgroup to resume, attempt " + std::to_string(attempt) +
	       " of 3";
}

//! Makes every later connect read, on each call, the next of `readings` seconds after a fixed
//! start, repeating the last. A connect reads it once before its first attempt and once after each
//! failed one.
void ScriptClock(std::vector<long> readings) {
	auto next = std::make_shared<size_t>(0);
	const auto start = std::chrono::steady_clock::time_point();
	SetConnectClockForTesting([readings, next, start]() {
		const auto index = std::min(*next, readings.size() - 1);
		(*next)++;
		return start + std::chrono::seconds(readings[index]);
	});
}

//! What a connect to the workgroup throws once its attempts, the last failing with `sqlstate`,
//! have taken `seconds`.
Thrown Exhausted(long seconds, int attempts, const std::string &sqlstate) {
	return Thrown {ExceptionType::IO, "Could not connect to Redshift at " + US_EAST_1_TARGET +
	                                      ": the Redshift Serverless workgroup did not answer within " +
	                                      std::to_string(seconds) + " seconds over " + std::to_string(attempts) +
	                                      (attempts == 1 ? " connect attempt" : " connect attempts") +
	                                      ".\nIt may still be resuming from idle suspension, which can take a few "
	                                      "minutes; retry. If it keeps timing out, the workgroup may not "
	                                      "be publicly reachable from this host's network." +
	                                      redshift_test::DEFAULT_PORT_HINT + "\nSQLSTATE: " + sqlstate};
}

//! How many connection handles the fake was asked to free.
size_t ConnectionFrees(const FakeDriverFixture &fixture) {
	const auto &calls = fixture.fake.Calls();
	return NumericCast<size_t>(
	    std::count_if(calls.begin(), calls.end(), [](const redshift_driver::testing::FakeCall &call) {
		    return call.entry_point == "SQLFreeHandle" && call.detail == "connection";
	    }));
}

//! The calls from the first SQLDriverConnect up to the session setup's first statement, leaving
//! out the diagnostic reads.
std::vector<std::string> ConnectCalls(const FakeDriverFixture &fixture) {
	const auto calls = fixture.CallsAfter(0);
	const auto first = std::find(calls.begin(), calls.end(), "SQLDriverConnect");
	const auto setup = std::find(first, calls.end(), "SQLPrepare");
	// The setup statement's own allocation comes just before its prepare.
	return std::vector<std::string>(first, setup == calls.end() ? setup : setup - 1);
}

const FakeDiag HY000_TIMEOUT {"HY000", 0, "timeout expired", {}};
const FakeDiag HYT00_TIMEOUT {"HYT00", 0, "Timeout expired", {}};
const FakeDiag HYT01_TIMEOUT {"HYT01", 0, "Connection timeout expired", {}};

//! The addresses the workgroup's host resolved to when the retry was sized.
constexpr int64_t WORKGROUP_ADDRESSES = 3;

//! The LoginTimeout seconds the latest SQLDriverConnect was sent; 0 before the first.
int64_t LoginTimeoutSent(const FakeDriverFixture &fixture) {
	const std::string key = "LoginTimeout={";
	const auto sent = fixture.fake.LastConnectionString();
	const auto start = sent.find(key);
	return start == std::string::npos ? 0 : std::stoll(sent.substr(start + key.size()));
}

} // namespace

TEST_CASE("A resume timeout that clears on the second attempt connects, after one wait on a new handle", "[odbc]") {
	FakeDriverFixture fixture;
	ScriptClock({0, 61});
	fixture.FailNextCall("SQLDriverConnect", SQL_ERROR, {HYT00_TIMEOUT});

	auto connection = fixture.OpenConnection();
	CHECK(fixture.CallsTo("SQLDriverConnect") == 2);
	CHECK(fixture.log_sink->Lines() == std::vector<std::string> {LoadedLine(), WaitLine(2)});
	// The failed attempt's handle is freed, with no rollback or disconnect, before the next is
	// allocated and set up.
	CHECK(ConnectCalls(fixture) == std::vector<std::string> {"SQLDriverConnect", "SQLFreeHandle", "SQLAllocHandle",
	                                                         "SQLSetConnectAttr", "SQLDriverConnect"});
	CHECK(ConnectionFrees(fixture) == 1);
	CHECK(fixture.fake.AllocatedHandles() == "environment, connection");
	CHECK(fixture.CallsTo("SQLExecute") == redshift_test::SessionSetupStatements().size());
}

TEST_CASE("A resume timeout that clears on the third attempt connects, after two waits", "[odbc]") {
	FakeDriverFixture fixture;
	ScriptClock({0, 61, 122});
	fixture.fake.FailCall("SQLDriverConnect", 1, SQL_ERROR, {HY000_TIMEOUT});
	fixture.fake.FailCall("SQLDriverConnect", 2, SQL_ERROR, {HYT01_TIMEOUT});

	auto connection = fixture.OpenConnection();
	CHECK(fixture.CallsTo("SQLDriverConnect") == 3);
	CHECK(fixture.log_sink->Lines() == std::vector<std::string> {LoadedLine(), WaitLine(2), WaitLine(3)});
	CHECK(ConnectionFrees(fixture) == 2);
	CHECK(fixture.fake.AllocatedHandles() == "environment, connection");
	connection.reset();
	CHECK(fixture.fake.AllocatedHandles() == "environment");
}

TEST_CASE("A resume timeout on every attempt fails after three, with the attempts and the seconds", "[odbc]") {
	FakeDriverFixture fixture;
	// Still inside the budget after the third, so only the attempt limit stops a fourth. Without
	// the limit, a fourth reads 200 and the budget ends it, failing a CHECK instead of looping.
	ScriptClock({0, 61, 122, 170, 200});
	fixture.fake.SetConnectOutcome(SQL_ERROR, {HYT00_TIMEOUT});

	CHECK(redshift_test::Capture([&] { fixture.OpenConnection(); }) == Exhausted(170, 3, "HYT00"));
	CHECK(fixture.CallsTo("SQLDriverConnect") == 3);
	// No wait is logged after the last attempt.
	CHECK(fixture.log_sink->Lines() == std::vector<std::string> {LoadedLine(), WaitLine(2), WaitLine(3)});
	// Each attempt's handle is allocated and freed once.
	CHECK(fixture.CallsTo("SQLSetConnectAttr") == 3);
	CHECK(ConnectionFrees(fixture) == 3);
	CHECK(fixture.fake.AllocatedHandles() == "environment");
}

TEST_CASE("A third attempt does not start once the first two have taken the 180-second budget", "[odbc]") {
	FakeDriverFixture fixture;
	ScriptClock({0, 90, 180});
	fixture.fake.SetConnectOutcome(SQL_ERROR, {HYT00_TIMEOUT});

	CHECK(redshift_test::Capture([&] { fixture.OpenConnection(); }) == Exhausted(180, 2, "HYT00"));
	CHECK(fixture.CallsTo("SQLDriverConnect") == 2);
	CHECK(fixture.log_sink->Lines() == std::vector<std::string> {LoadedLine(), WaitLine(2)});
	CHECK(fixture.fake.AllocatedHandles() == "environment");
}

TEST_CASE("A third attempt starts at 179 seconds and may overrun the budget", "[odbc]") {
	FakeDriverFixture fixture;
	ScriptClock({0, 90, 179, 240});
	fixture.fake.SetConnectOutcome(SQL_ERROR, {HYT00_TIMEOUT});

	CHECK(redshift_test::Capture([&] { fixture.OpenConnection(); }) == Exhausted(240, 3, "HYT00"));
	CHECK(fixture.CallsTo("SQLDriverConnect") == 3);
}

TEST_CASE("On a host of three addresses a timed-out attempt leaves the budget room for a second", "[odbc]") {
	FakeDriverFixture fixture;
	// The driver spends one LoginTimeout on each address the host resolves to, so each timed-out
	// attempt takes one for each of the workgroup's addresses.
	SetConnectClockForTesting([&fixture]() {
		const auto attempts = NumericCast<int64_t>(fixture.CallsTo("SQLDriverConnect"));
		return std::chrono::steady_clock::time_point() +
		       std::chrono::seconds(WORKGROUP_ADDRESSES * LoginTimeoutSent(fixture) * attempts);
	});
	fixture.fake.SetConnectOutcome(SQL_ERROR, {HY000_TIMEOUT});

	CHECK(redshift_test::Capture([&] { fixture.OpenConnection(); }) == Exhausted(270, 2, "HY000"));
	CHECK(fixture.CallsTo("SQLDriverConnect") == 2);
	CHECK(fixture.log_sink->Lines() == std::vector<std::string> {LoadedLine(), WaitLine(2)});
}

TEST_CASE("The budget counts the attempt that just failed", "[odbc]") {
	FakeDriverFixture fixture;
	// The clock moves 90 seconds with each connect made, so a reading taken before an attempt
	// would leave that attempt out.
	SetConnectClockForTesting([&fixture]() {
		return std::chrono::steady_clock::time_point() +
		       std::chrono::seconds(90) * NumericCast<int64_t>(fixture.CallsTo("SQLDriverConnect"));
	});
	fixture.fake.SetConnectOutcome(SQL_ERROR, {HYT00_TIMEOUT});

	CHECK(redshift_test::Capture([&] { fixture.OpenConnection(); }) == Exhausted(180, 2, "HYT00"));
	CHECK(fixture.CallsTo("SQLDriverConnect") == 2);
}

TEST_CASE("A later attempt that fails otherwise is judged by its own records", "[odbc]") {
	FakeDriverFixture fixture;
	ScriptClock({0, 61, 62});
	fixture.fake.FailCall("SQLDriverConnect", 1, SQL_ERROR, {HYT00_TIMEOUT});
	fixture.fake.FailCall("SQLDriverConnect", 2, SQL_ERROR, {{"42501", 0, "permission denied", {}}});

	// A third attempt would succeed, so only the second attempt's own category stops the retry.
	CHECK(redshift_test::Capture([&] { fixture.OpenConnection(); }) ==
	      Thrown {ExceptionType::IO, "Could not connect to Redshift at " + US_EAST_1_TARGET +
	                                     ": the AWS identity is not allowed to connect to the workgroup.\nGrant it "
	                                     "redshift-serverless:GetCredentials on arn:aws:redshift-serverless:<region>:"
	                                     "<account>:workgroup/<workgroup-id>.\nSQLSTATE: 42501"});
	CHECK(fixture.CallsTo("SQLDriverConnect") == 2);
	CHECK(fixture.log_sink->Lines() == std::vector<std::string> {LoadedLine(), WaitLine(2)});
	CHECK(ConnectionFrees(fixture) == 2);
	CHECK(fixture.fake.AllocatedHandles() == "environment");
}

TEST_CASE("A later attempt's refused connection handle ends the connect as a pre-connect failure", "[odbc]") {
	FakeDriverFixture fixture;
	ScriptClock({0, 61});
	fixture.fake.FailCall("SQLDriverConnect", 1, SQL_ERROR, {HYT00_TIMEOUT});
	// The first allocation is the environment's, the second the first attempt's handle.
	fixture.fake.FailCall("SQLAllocHandle", 3, SQL_ERROR, {FakeDiag {"HY001", 0, "Memory allocation error", {}}});

	const auto error = redshift_test::CaptureError([&] { fixture.OpenConnection(); });
	CHECK(Thrown {error.Type(), error.RawMessage()} ==
	      Thrown {
	          ExceptionType::IO,
	          "the Redshift driver could not allocate a connection handle; run the statement again\nSQLSTATE: HY001"});
	CHECK(error.ExtraInfo().at("sqlstate") == "HY001");
	CHECK(fixture.CallsTo("SQLAllocHandle") == 3);
	CHECK(fixture.CallsTo("SQLDriverConnect") == 1);
	CHECK(fixture.log_sink->Lines() == std::vector<std::string> {LoadedLine(), WaitLine(2)});
	CHECK(ConnectionFrees(fixture) == 1);
	CHECK(fixture.fake.AllocatedHandles() == "environment");
}

TEST_CASE("A later attempt's refused autocommit ends the connect as a pre-connect failure", "[odbc]") {
	FakeDriverFixture fixture;
	ScriptClock({0, 61});
	fixture.fake.FailCall("SQLDriverConnect", 1, SQL_ERROR, {HYT00_TIMEOUT});
	fixture.fake.FailCall("SQLSetConnectAttr", 2, SQL_ERROR, {FakeDiag {"HY024", 0, "Invalid attribute value", {}}});

	const auto error = redshift_test::CaptureError([&] { fixture.OpenConnection(); });
	CHECK(Thrown {error.Type(), error.RawMessage()} ==
	      Thrown {ExceptionType::IO, "the Redshift driver refused to enable autocommit, which this extension needs; "
	                                 "report it with the extension and driver versions\nSQLSTATE: HY024"});
	CHECK(error.ExtraInfo().at("sqlstate") == "HY024");
	CHECK(fixture.CallsTo("SQLSetConnectAttr") == 2);
	CHECK(fixture.CallsTo("SQLDriverConnect") == 1);
	CHECK(fixture.log_sink->Lines() == std::vector<std::string> {LoadedLine(), WaitLine(2)});
	// The refused handle is freed too, as the first attempt's was.
	CHECK(ConnectionFrees(fixture) == 2);
	CHECK(fixture.fake.AllocatedHandles() == "environment");
}

TEST_CASE("An exhaustion reports the last attempt's SQLSTATE", "[odbc]") {
	FakeDriverFixture fixture;
	ScriptClock({0, 61, 122, 170});
	fixture.fake.FailCall("SQLDriverConnect", 1, SQL_ERROR, {HY000_TIMEOUT});
	fixture.fake.FailCall("SQLDriverConnect", 2, SQL_ERROR, {HYT00_TIMEOUT});
	fixture.fake.FailCall("SQLDriverConnect", 3, SQL_ERROR, {HYT01_TIMEOUT});

	CHECK(redshift_test::Capture([&] { fixture.OpenConnection(); }) == Exhausted(170, 3, "HYT01"));
	CHECK(fixture.CallsTo("SQLDriverConnect") == 3);
}

TEST_CASE("A first attempt that outlasts the budget is the only one", "[odbc]") {
	FakeDriverFixture fixture;
	ScriptClock({0, 200});
	fixture.fake.SetConnectOutcome(SQL_ERROR, {HYT00_TIMEOUT});

	CHECK(redshift_test::Capture([&] { fixture.OpenConnection(); }) == Exhausted(200, 1, "HYT00"));
	CHECK(fixture.CallsTo("SQLDriverConnect") == 1);
	CHECK(fixture.log_sink->Lines() == std::vector<std::string> {LoadedLine()});
}

TEST_CASE("No connect failure but a resume timeout starts another attempt", "[odbc]") {
	const std::string iam = "[Redshift][ODBC Driver][Server][860:8:IAMConnectionError]: ";
	struct Case {
		std::vector<FakeDiag> diags;
		const char *what;
	};
	for (const auto &failure : std::vector<Case> {
	         {{{"HY000", 0, iam + "No AWS profile found: p", {}}}, "credentials"},
	         {{{"HY000", 0, "SSL error: certificate verify failed", {}}}, "certificate"},
	         {{{"42501", 0, "permission denied", {}}}, "permission"},
	         {{{"HY000", 0, iam + "ResourceNotFoundException: Serverless workgroup analytics not found.", {}}},
	          "workgroup not found"},
	         {{{"08001", 0, "could not connect to server", {}}}, "other"},
	         {{HYT00_TIMEOUT, {"HY000", 0, iam + "No AWS profile found: p", {}}},
	          "a timeout beside a credentials error"},
	         {{HYT00_TIMEOUT, {"HY000", 0, "certificate verify failed", {}}},
	          "a timeout beside a certificate error"}}) {
		INFO(failure.what);
		FakeDriverFixture fixture;
		// The budget never runs out, so only the failure's category stops the retry.
		ScriptClock({0});
		fixture.fake.SetConnectOutcome(SQL_ERROR, failure.diags);

		const auto thrown = redshift_test::Capture([&] { fixture.OpenConnection(); });
		CHECK(thrown.type == ExceptionType::IO);
		CHECK(thrown.message.find("connect attempt") == std::string::npos);
		CHECK(thrown.message.find("resum") == std::string::npos);
		CHECK(fixture.CallsTo("SQLDriverConnect") == 1);
		CHECK(fixture.log_sink->Lines() == std::vector<std::string> {LoadedLine()});
		CHECK(fixture.fake.AllocatedHandles() == "environment");
	}
}

TEST_CASE("A refused connect to a workgroup fails after one attempt, with no wait", "[odbc]") {
	FakeDriverFixture fixture;
	ScriptClock({0});
	// The driver's own answer to a closed port, as the closed-port smoke pins it on the real driver:
	// under HY000, the state a resume timeout also uses, so only its text keeps it from the retry.
	fixture.fake.SetConnectOutcome(
	    SQL_ERROR,
	    {FakeDiag {"HY000", 0, "[Redshift][ODBC Driver] could not connect to server: Connection refused", {}}});

	CHECK(redshift_test::Capture([&] { fixture.OpenConnection(); }) ==
	      Thrown {ExceptionType::IO, "Could not connect to Redshift at " + US_EAST_1_TARGET +
	                                     ": the connection attempt failed. The driver reported \"could not connect "
	                                     "to server: Connection refused\".\nCheck the region and that this host can "
	                                     "reach the workgroup, then retry." +
	                                     redshift_test::DEFAULT_PORT_HINT + "\nSQLSTATE: HY000"});
	CHECK(fixture.CallsTo("SQLDriverConnect") == 1);
	CHECK(fixture.log_sink->Lines() == std::vector<std::string> {LoadedLine()});
}

TEST_CASE("A connect failure's reason is redacted as the connect reads it from the driver", "[odbc]") {
	// Through the real read of the connect handle's records, not a record built for the test: each
	// credential form in the driver's text stays out of the reason the message shows.
	for (const auto *form : {"AKIAIOSFODNN7EXAMPLE", "placeholdertokenrunplaceholdertokenrunplaceholder"}) {
		INFO(form);
		FakeDriverFixture fixture;
		ScriptClock({0});
		fixture.fake.SetConnectOutcome(
		    SQL_ERROR, {FakeDiag {"HY000", 0, std::string("[Redshift][ODBC Driver] login failed for ") + form, {}}});
		const auto thrown = redshift_test::Capture([&] { fixture.OpenConnection(); });
		CHECK(thrown.message.find("The driver reported \"login failed for <redacted>\".") != std::string::npos);
		CHECK(thrown.message.find(form) == std::string::npos);
	}
}

TEST_CASE("A provisioned cluster's connect timeout fails after one attempt with no resume text", "[odbc]") {
	for (const auto &diag : {HY000_TIMEOUT, HYT00_TIMEOUT, HYT01_TIMEOUT}) {
		INFO(diag.sqlstate);
		FakeDriverFixture fixture;
		ScriptClock({0});
		fixture.fake.SetConnectOutcome(SQL_ERROR, {diag});

		const auto thrown = redshift_test::Capture(
		    [&] { OpenRedshiftConnection(redshift_test::ClusterConfig("us-east-1"), fixture.log_sink); });
		CHECK(thrown == Thrown {ExceptionType::IO,
		                        "Could not connect to Redshift at " + redshift_test::ClusterTarget("us-east-1") +
		                            ": the Redshift cluster did not answer.\nThe cluster may not be publicly "
		                            "reachable from this host's network; check its network access, then "
		                            "retry.\nSQLSTATE: " +
		                            diag.sqlstate});
		CHECK(fixture.CallsTo("SQLDriverConnect") == 1);
		CHECK(fixture.log_sink->Lines() == std::vector<std::string> {LoadedLine()});
	}
}

TEST_CASE("A timeout in the session setup after connect starts no second connect", "[odbc]") {
	for (const auto &diag : {HYT00_TIMEOUT, HYT01_TIMEOUT}) {
		INFO(diag.sqlstate);
		FakeDriverFixture fixture;
		ScriptClock({0});
		fixture.FailNextCall("SQLExecute", SQL_ERROR, {diag});

		CHECK(redshift_test::Capture([&] { fixture.OpenConnection(); }) ==
		      Thrown {ExceptionType::IO, "Could not set up the Redshift session at " + US_EAST_1_TARGET +
		                                     ": the setup statement \"SET extra_float_digits TO 2\" failed (SQLSTATE "
		                                     "class HY, driver error). The connection is closed, and no statement of "
		                                     "yours was sent. Run the statement again, and if it fails the same way, "
		                                     "report it with the extension and driver versions.\nSQLSTATE: " +
		                                     diag.sqlstate});
		CHECK(fixture.CallsTo("SQLDriverConnect") == 1);
		CHECK(fixture.CallsTo("SQLExecute") == 1);
		CHECK(fixture.log_sink->Lines() == std::vector<std::string> {LoadedLine()});
	}
}

TEST_CASE("A statement that times out is neither run again nor followed by a second connect", "[odbc]") {
	for (const bool dead : {false, true}) {
		for (const auto &diag : {HYT00_TIMEOUT, HYT01_TIMEOUT}) {
			INFO(diag.sqlstate << (dead ? " on a dead connection" : " on a live connection"));
			FakeDriverFixture fixture;
			ScriptClock({0});
			auto connection = fixture.OpenConnection();
			fixture.fake.SetConnectionDead(dead);
			fixture.FailNextCall("SQLExecDirect", SQL_ERROR, {diag});
			const auto runs = fixture.CallsTo("SQLExecDirect");

			// A dead connection gives the transport failure, a live one the statement failure.
			const Thrown expected =
			    dead ? Thrown {ExceptionType::IO, "The connection to Redshift at " + US_EAST_1_TARGET +
			                                          " was lost (SQLSTATE class HY, driver error). The statement was "
			                                          "not replayed, because a write it carried may already have "
			                                          "taken effect; check before running it again.\nSQLSTATE: " +
			                                          diag.sqlstate}
			         : Thrown {ExceptionType::EXECUTOR,
			                   "the Redshift driver failed while running the statement (SQLSTATE class HY, driver "
			                   "error), and its message is not shown. The statement may have run; check before "
			                   "running it again." +
			                       redshift_test::DRIVER_FAILURE_TRAILER + "\nSQLSTATE: " + diag.sqlstate};
			CHECK(redshift_test::Capture([&] { redshift_test::ExecuteStatement(*connection, "SELECT 1", {}); }) ==
			      expected);
			CHECK(fixture.CallsTo("SQLExecDirect") == runs + 1);
			CHECK(fixture.CallsTo("SQLDriverConnect") == 1);
			CHECK(fixture.log_sink->Lines() == std::vector<std::string> {LoadedLine()});
		}
	}
}

TEST_CASE("A failed attempt's handle that cannot be freed is abandoned, and the next attempt gets a new one",
          "[odbc]") {
	FakeDriverFixture fixture;
	ScriptClock({0, 61});
	fixture.FailNextCall("SQLDriverConnect", SQL_ERROR, {HYT00_TIMEOUT});
	// The first free is the failed attempt's: nothing else is freed before the session setup.
	fixture.FailNextCall("SQLFreeHandle", SQL_ERROR, {});

	auto connection = fixture.OpenConnection();
	CHECK(fixture.CallsTo("SQLDriverConnect") == 2);
	CHECK(fixture.log_sink->Lines() ==
	      std::vector<std::string> {LoadedLine(),
	                                "Redshift connection teardown: freeing the connection handle failed, with no "
	                                "diagnostic record; the handle is abandoned",
	                                WaitLine(2)});
	CHECK(fixture.fake.AllocatedHandles() == "environment, connection");
}
