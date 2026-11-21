// Statement failures on the embedded driver through redshift_query, with the live variables: a
// syntax error carries only its SQLSTATEs in extra_info, the server's first line shows, sanitized,
// and a long message is cut where the fake cuts one. Each statement only reads and fails, so nothing
// is written. Offline, the checks that decide whether a live case skips and where it connects.

// catch.hpp comes first, as in this directory's other tests.
#include "catch.hpp"

#include "call_trace.hpp"
#include "duckdb/common/string_util.hpp"
#include "extension_database.hpp"
#include "fake_driver_fixture.hpp"
#include "live_workgroup.hpp"
#include "thrown.hpp"

#include <string>

using namespace duckdb;
using redshift_test::CallTrace;
using redshift_test::Thrown;

namespace {

using ExtraInfo = unordered_map<string, string>;

//! What a statement the live workgroup refuses throws through redshift_query.
struct LiveFailure {
	Thrown thrown;
	ExtraInfo extra_info;
};

//! The client's message in `message`: DuckDB appends the redshift_query call's location after a blank
//! line to an error raised while binding it (query_error_context.cpp QueryErrorContext::Format).
string WithoutQueryLocation(const string &message) {
	return message.substr(0, message.find("\n\n"));
}

//! `sql`'s failure through redshift_query on the live workgroup, on the embedded driver, whose calls
//! CallTrace records.
LiveFailure FailureOf(const string &sql) {
	redshift_test::ResetOnExit reset;
	SetDriverSourceForTesting(&CallTrace::Source);
	CallTrace::Start(&redshift_driver::LoadEmbeddedDriver);
	redshift_test::LiveEndpoints endpoints;
	redshift_test::ExtensionDatabase database;
	database.Run(redshift_test::LiveWorkgroupAttach());
	auto result =
	    database.con.Query("SELECT * FROM redshift_query('rs', '" + StringUtil::Replace(sql, "'", "''") + "')");
	REQUIRE(result->HasError());
	const auto &error = result->GetErrorObject();
	return {Thrown {error.Type(), WithoutQueryLocation(error.RawMessage())}, error.ExtraInfo()};
}

//! The message of the last record SQLGetDiagRec read with `sqlstate`, as the driver wrote it.
string DriverMessage(const string &sqlstate) {
	const auto lead = "SQLGetDiagRec " + std::to_string(SQL_SUCCESS) + " " + sqlstate + " ";
	string message;
	for (const auto &call : CallTrace::Calls()) {
		if (StringUtil::StartsWith(call, lead)) {
			message = call.substr(lead.size());
		}
	}
	return message;
}

} // namespace

TEST_CASE("UnsetVariables names each unset variable in order, separated by spaces", "[odbc]") {
	redshift_driver::testing::ScopedEnvironmentVariable first {"REDSHIFT_TEST_PROBE_FIRST", nullptr};
	redshift_driver::testing::ScopedEnvironmentVariable set {"REDSHIFT_TEST_PROBE_SET", "x"};
	redshift_driver::testing::ScopedEnvironmentVariable last {"REDSHIFT_TEST_PROBE_LAST", nullptr};

	CHECK(redshift_test::UnsetVariables(
	          {"REDSHIFT_TEST_PROBE_FIRST", "REDSHIFT_TEST_PROBE_SET", "REDSHIFT_TEST_PROBE_LAST"}) ==
	      "REDSHIFT_TEST_PROBE_FIRST REDSHIFT_TEST_PROBE_LAST");
	CHECK(redshift_test::UnsetVariables({"REDSHIFT_TEST_PROBE_SET"}).empty());
}

//! A load that failed with `kind`.
redshift_driver::DriverLoadResult FailedLoad(redshift_driver::DriverErrorKind kind) {
	redshift_driver::DriverLoadResult load;
	load.error.kind = kind;
	return load;
}

//! The live variables set, to placeholder values, while it lives.
struct LiveVariablesSet {
	redshift_driver::testing::ScopedEnvironmentVariable region {redshift_test::LIVE_REGION_VARIABLE, "us-east-1"};
	redshift_driver::testing::ScopedEnvironmentVariable workgroup {redshift_test::LIVE_WORKGROUP_VARIABLE, "wg"};
	redshift_driver::testing::ScopedEnvironmentVariable account {redshift_test::LIVE_ACCOUNT_ID_VARIABLE, "1"};
};

TEST_CASE("With the live variables set, a live case skips only when the build has no embedded driver", "[odbc]") {
	LiveVariablesSet variables;
	const redshift_driver::LoadedDriver driver {};
	redshift_driver::DriverLoadResult loaded;
	loaded.driver = &driver;

	CHECK(redshift_test::LiveSkipReason(FailedLoad(redshift_driver::DriverErrorKind::NOT_BUILT)) ==
	      "skipped: this build has no embedded driver");
	CHECK(redshift_test::LiveSkipReason(FailedLoad(redshift_driver::DriverErrorKind::REFUSED_ENVIRONMENT)).empty());
	CHECK(redshift_test::LiveSkipReason(loaded).empty());
}

TEST_CASE("A live case skips naming the live variable that is unset", "[odbc]") {
	LiveVariablesSet variables;
	redshift_driver::testing::ScopedEnvironmentVariable workgroup {redshift_test::LIVE_WORKGROUP_VARIABLE, nullptr};

	CHECK(redshift_test::LiveSkipReason(FailedLoad(redshift_driver::DriverErrorKind::NOT_BUILT)) ==
	      "skipped: unset: REDSHIFT_TEST_LIVE_WORKGROUP (test/README.md, \"Live variables\")");
}

TEST_CASE("A live case keeps the test endpoint variables only when its connect goes through them", "[odbc]") {
	{
		redshift_driver::testing::ScopedEnvironmentVariable host {TEST_HOST_VARIABLE, nullptr};
		redshift_driver::testing::ScopedEnvironmentVariable url {TEST_URL_VARIABLE, "https://u"};
		CHECK_FALSE(redshift_test::ThroughTestEndpoints());
	}
	redshift_driver::testing::ScopedEnvironmentVariable host {TEST_HOST_VARIABLE, "h"};
	{
		redshift_driver::testing::ScopedEnvironmentVariable url {TEST_URL_VARIABLE, nullptr};
		CHECK_FALSE(redshift_test::ThroughTestEndpoints());
		redshift_test::LiveEndpoints endpoints;
		CHECK(std::getenv(TEST_HOST_VARIABLE) == nullptr);
	}
	redshift_driver::testing::ScopedEnvironmentVariable url {TEST_URL_VARIABLE, "https://u"};
	CHECK(redshift_test::ThroughTestEndpoints() == BuildHasTestEndpoints());
	redshift_test::LiveEndpoints endpoints;
	CHECK((std::getenv(TEST_HOST_VARIABLE) != nullptr) == BuildHasTestEndpoints());
}

// Prepare refuses it, and the driver writes the position in a second line of the message. DuckDB
// writes and then removes a position of its own for a bind error, so this checks that no other key
// appears; the offline cases check the position itself.
TEST_CASE("Live: a syntax error carries only its SQLSTATEs in extra_info, and its first line shows", "[odbc][live]") {
	SKIP_UNLESS_LIVE(redshift_driver::LoadEmbeddedDriver());
	const auto failure = FailureOf("SELECT 1 FROM");

	CHECK(failure.thrown ==
	      Thrown {ExceptionType::BINDER,
	              "Redshift refused the statement: ERROR:  syntax error at end of input\nSQLSTATE: 42601"});
	CHECK(failure.extra_info == ExtraInfo {{"sqlstate", "42601"}, {"sqlstates", "42601"}});
}

// From a column, so the cast fails when the statement runs rather than when it is prepared.
TEST_CASE("Live: an address a class 22 message echoes shows as <address>", "[odbc][live]") {
	SKIP_UNLESS_LIVE(redshift_driver::LoadEmbeddedDriver());
	const auto failure = FailureOf("SELECT CAST(v AS INTEGER) FROM (SELECT '10.0.0.1'::varchar AS v) t");

	CHECK(failure.thrown == Thrown {ExceptionType::EXECUTOR,
	                                "Redshift could not run the statement: ERROR:  invalid "
	                                "input syntax for integer: \"<address>\"\nSQLSTATE: 22P02"});
	CHECK(failure.extra_info == ExtraInfo {{"sqlstate", "22P02"}, {"sqlstates", "22P02"}});
}

// The fake keeps as much of a message as the driver does, and the client takes a message that long as
// cut, so a driver release that keeps more or less fails here. Each 10 bytes of the first value name
// their offset, so a failure shows where the driver cut; the second is cut inside a two-byte character.
TEST_CASE("Live: a long message is cut where the fake cuts one and shows the cut mark", "[odbc][live]") {
	SKIP_UNLESS_LIVE(redshift_driver::LoadEmbeddedDriver());
	string offsets;
	for (idx_t offset = 0; offset < 400; offset += 10) {
		offsets += StringUtil::Format("%09d|", offset);
	}
	string two_byte = "x";
	for (idx_t count = 0; count < 200; count++) {
		two_byte += "\xC3\xA9";
	}
	for (const auto &value : {offsets, two_byte}) {
		const auto failure = FailureOf("SELECT CAST(v AS INTEGER) FROM (SELECT '" + value + "'::varchar AS v) t");

		const string whole =
		    "[Redshift][ODBC Driver][Server]22P02:ERROR:  invalid input syntax for integer: \"" + value + "\"";
		CHECK(DriverMessage("22P02") ==
		      whole.substr(0, redshift_driver::testing::FakeOdbcDriver::DRIVER_MESSAGE_LIMIT));
		CHECK(failure.thrown == Thrown {ExceptionType::EXECUTOR,
		                                "Redshift could not run the statement: ERROR:  "
		                                "invalid input syntax for integer:...\nSQLSTATE: 22P02"});
	}
}
