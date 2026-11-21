// Opening and closing one connection: the settings it sends, the order of its driver calls, the
// outcomes it accepts, the failures it classifies, and the handles it frees.

// catch.hpp comes first, as in this directory's other tests.
#include "catch.hpp"

#include "../interface/scoped_environment_variable.hpp"
#include "duckdb/common/string_util.hpp"
#include "duckdb/main/database.hpp"
#include "fake_driver_fixture.hpp"
#include "odbc/connection_settings.hpp"
#include "thrown.hpp"

#include <algorithm>
#include <chrono>
#include <future>
#include <memory>
#include <string>
#include <utility>
#include <vector>

using namespace duckdb;
using redshift_driver::testing::FakeDiag;
using redshift_driver::testing::FakeGate;
using redshift_driver::testing::ScopedEnvironmentVariable;
using redshift_test::FakeDriverFixture;
using redshift_test::ReadyWithin;
using redshift_test::Thrown;

namespace {

const std::string US_EAST_1_TARGET = redshift_test::ServerlessTarget("us-east-1");

//! How long a test waits for a call or a result it expects. A regression fails the case instead of
//! hanging it.
const std::chrono::milliseconds EXPECTED_WAIT(10000);

//! Times out the first three connects only. On the steady clock a connect with no attempt limit
//! would retry for the whole budget; here its fourth attempt connects and the case fails at once.
void FailThreeConnects(FakeDriverFixture &fixture) {
	for (int call = 1; call <= 3; call++) {
		fixture.fake.FailCall("SQLDriverConnect", call, SQL_ERROR, {FakeDiag {"HYT00", 0, "Timeout expired", {}}});
	}
}

//! The whole seconds a timeout message reports, or -1 when it reports none.
long ReportedSeconds(const std::string &message) {
	const std::string lead = "did not answer within ";
	const auto start = message.find(lead);
	return start == std::string::npos ? -1 : std::stol(message.substr(start + lead.size()));
}

//! What a connect to the workgroup throws once all three attempts have timed out, after `seconds`.
Thrown TimeoutFailure(long seconds) {
	return Thrown {ExceptionType::IO, std::string("Could not connect to Redshift at ") + US_EAST_1_TARGET +
	                                      ": the Redshift Serverless workgroup did not answer within " +
	                                      std::to_string(seconds) + (seconds == 1 ? " second" : " seconds") +
	                                      " over 3 connect attempts.\nIt may still be resuming from idle suspension, "
	                                      "which can take a few minutes; retry. If it keeps timing out, the "
	                                      "workgroup may not be publicly reachable from this host's network." +
	                                      redshift_test::DEFAULT_PORT_HINT + "\nSQLSTATE: HYT00"};
}

//! Whether the fake received a call to `entry_point`.
bool Called(const FakeDriverFixture &fixture, const std::string &entry_point) {
	const auto names = fixture.EntryPoints();
	return std::find(names.begin(), names.end(), entry_point) != names.end();
}

} // namespace

TEST_CASE("The driver receives the connection string BuildConnectionString builds with the resolved bundle", "[odbc]") {
	FakeDriverFixture fixture;
	auto connection = fixture.OpenConnection();

	TrustSource trust;
	trust.path = fixture.ca_bundle.path;
	CHECK(fixture.fake.LastConnectionString() == BuildConnectionString(redshift_test::UsEast1Config(), trust));
}

TEST_CASE("The bundle SSL_CERT_FILE names is sent as both TrustStore and CaFile", "[odbc]") {
	FakeDriverFixture fixture;
	auto connection = fixture.OpenConnection();

	const auto both = "TrustStore={" + fixture.ca_bundle.path + "};CaFile={" + fixture.ca_bundle.path + "};";
	CHECK(fixture.fake.LastConnectionString().find(both) != std::string::npos);
}

TEST_CASE("SQLDriverConnect is given no output buffer and is told not to prompt", "[odbc]") {
	FakeDriverFixture fixture;
	auto connection = fixture.OpenConnection();

	CHECK_FALSE(fixture.fake.LastConnectOutput().has_buffer);
	CHECK(fixture.fake.LastConnectOutput().capacity == 0);
	CHECK(fixture.fake.LastConnectOutput().completion == SQL_DRIVER_NOPROMPT);
}

TEST_CASE("Closing a connection frees an open result's statement before the connection handle", "[odbc]") {
	FakeDriverFixture fixture;
	auto connection = fixture.OpenConnection();
	auto result = redshift_test::ExecuteStatement(*connection, "SELECT 1", {});
	const auto opened = fixture.fake.Calls().size();

	connection.reset();
	std::vector<std::string> freed;
	for (size_t index = opened; index < fixture.fake.Calls().size(); index++) {
		const auto &call = fixture.fake.Calls()[index];
		if (call.entry_point == "SQLFreeHandle") {
			freed.push_back(call.detail);
		}
	}
	CHECK(freed == std::vector<std::string> {"statement", "connection"});
}

TEST_CASE("A connection's driver calls run in a fixed order, from the environment to its own free", "[odbc]") {
	FakeDriverFixture fixture;
	{ auto connection = fixture.OpenConnection(); }

	const std::vector<std::string> setup_statement {"SQLAllocHandle", "SQLPrepare", "SQLExecute", "SQLFreeHandle"};
	std::vector<std::string> expected {"SQLAllocHandle", "SQLSetEnvAttr", "SQLAllocHandle", "SQLSetConnectAttr",
	                                   "SQLDriverConnect"};
	const auto setup_count = redshift_test::SessionSetupStatements().size();
	for (size_t statement = 0; statement < setup_count; statement++) {
		expected.insert(expected.end(), setup_statement.begin(), setup_statement.end());
	}
	expected.insert(expected.end(), {"SQLEndTran", "SQLDisconnect", "SQLFreeHandle"});
	CHECK(fixture.EntryPoints() == expected);
	CHECK(fixture.fake.AllocatedHandles() == "environment");
}

TEST_CASE("A connection selects ODBC 3 on the environment it allocates", "[odbc]") {
	FakeDriverFixture fixture;
	auto connection = fixture.OpenConnection();

	// SQL_ATTR_ODBC_VERSION is 200 and SQL_OV_ODBC3 is 3 (sqlext.h).
	CHECK(fixture.fake.Calls()[1].entry_point == "SQLSetEnvAttr");
	CHECK(fixture.fake.Calls()[1].detail == "200=3");
}

TEST_CASE("A connection asks for autocommit on its handle before it connects", "[odbc]") {
	FakeDriverFixture fixture;
	auto connection = fixture.OpenConnection();

	// SQL_ATTR_AUTOCOMMIT is 102 and SQL_AUTOCOMMIT_ON is 1 (sqlext.h).
	CHECK(fixture.fake.Calls()[3].entry_point == "SQLSetConnectAttr");
	CHECK(fixture.fake.Calls()[3].detail == "102=1");
}

TEST_CASE("A destroyed connection rolls back before it disconnects", "[odbc]") {
	FakeDriverFixture fixture;
	{ auto connection = fixture.OpenConnection(); }

	// SQL_ROLLBACK is 1 (sql.h). The rollback is the third call from the end, before the
	// disconnect and the free.
	const auto &calls = fixture.fake.Calls();
	REQUIRE(calls.size() >= 3);
	CHECK(calls[calls.size() - 3].entry_point == "SQLEndTran");
	CHECK(calls[calls.size() - 3].detail == "1");
}

TEST_CASE("Running a statement ends no transaction while the connection is open", "[odbc]") {
	FakeDriverFixture fixture;
	auto connection = fixture.OpenConnection();
	auto result = redshift_test::ExecuteStatement(*connection, "SELECT 1", {});
	RedshiftBatch batch;
	while (result->NextBatch(batch)) {
	}

	CHECK_FALSE(Called(fixture, "SQLEndTran"));
}

TEST_CASE("A connect that returns SQL_SUCCESS_WITH_INFO with 01004 succeeds, logging its SQLSTATE alone", "[odbc]") {
	FakeDriverFixture fixture;
	fixture.fake.SetConnectOutcome(SQL_SUCCESS_WITH_INFO, {FakeDiag {"01004", 0, "String data, right truncated", {}}});

	auto connection = fixture.OpenConnection();
	CHECK(fixture.log_sink->Lines() ==
	      std::vector<std::string> {"Redshift driver loaded: test-fake fake " + DuckDB::Platform(),
	                                "Redshift driver diagnostic, SQLSTATE 01004"});
}

TEST_CASE("A connect warning in class 22 logs its SQLSTATE and none of the driver's text", "[odbc]") {
	FakeDriverFixture fixture;
	fixture.fake.SetConnectOutcome(
	    SQL_SUCCESS_WITH_INFO,
	    {FakeDiag {"22001", 0, "value too long at redshift.internal.example AKIAIOSFODNN7EXAMPLE", {}}});

	auto connection = fixture.OpenConnection();
	CHECK(fixture.log_sink->Lines() ==
	      std::vector<std::string> {"Redshift driver loaded: test-fake fake " + DuckDB::Platform(),
	                                "Redshift driver diagnostic, SQLSTATE 22001"});
}

TEST_CASE("An IAM-tagged connect failure gives the classified IOException, naming the target, no driver text and no "
          "port hint",
          "[odbc]") {
	FakeDriverFixture fixture;
	fixture.fake.SetConnectOutcome(
	    SQL_ERROR, {FakeDiag {"HY000", 0, "[Redshift][ODBC Driver][Server][860:8:IAMConnectionError]", {}}});

	const auto error = redshift_test::CaptureError([&] { fixture.OpenConnection(); });
	// An IAM call's failure, before any connect to the port, so no port hint.
	CHECK(Thrown {error.Type(), error.RawMessage()} ==
	      Thrown {ExceptionType::IO, std::string("Could not connect to Redshift at ") + US_EAST_1_TARGET +
	                                     ": the connection attempt failed.\nCheck the region and that this host can "
	                                     "reach the workgroup, then retry.\nSQLSTATE: HY000"});
	CHECK(error.ExtraInfo().at("redshift_driver") == "test-fake fake " + DuckDB::Platform());
}

TEST_CASE("A connect timeout on every attempt reports the three attempts made", "[odbc]") {
	FakeDriverFixture fixture;
	FailThreeConnects(fixture);

	const auto started = std::chrono::steady_clock::now();
	const auto thrown = redshift_test::Capture([&] { fixture.OpenConnection(); });
	const auto window = std::chrono::duration_cast<std::chrono::seconds>(std::chrono::steady_clock::now() - started);
	// A loaded machine can stall the fake's connect for a second or more.
	const auto seconds = ReportedSeconds(thrown.message);
	CHECK(seconds >= 0);
	CHECK(seconds <= window.count());
	CHECK(thrown == TimeoutFailure(seconds));
}

TEST_CASE("A connect timeout reports the whole seconds its attempts took, on the steady clock", "[odbc]") {
	FakeDriverFixture fixture;
	FailThreeConnects(fixture);
	auto gate = std::make_shared<FakeGate>();
	fixture.fake.BlockCall("SQLDriverConnect", 1, gate);

	const auto started = std::chrono::steady_clock::now();
	auto connect = std::async(std::launch::async, [&fixture]() {
		return redshift_test::Capture([&fixture]() { fixture.OpenConnection(); });
	});
	const bool held = gate->WaitUntilHeldFor(EXPECTED_WAIT);
	// Waiting this long on the held connect checks that it has not returned, and makes its first
	// attempt take more than a second; the other two fail at once.
	const bool returned_while_held = ReadyWithin(connect, std::chrono::milliseconds(1500));
	gate->Release();
	const bool returned = ReadyWithin(connect, EXPECTED_WAIT);
	const auto window = std::chrono::duration_cast<std::chrono::seconds>(std::chrono::steady_clock::now() - started);

	CHECK(held);
	CHECK_FALSE(returned_while_held);
	REQUIRE(returned);
	const auto thrown = connect.get();
	// The attempts lie inside the test's window, so a loaded machine can report more than 1 second.
	const auto seconds = ReportedSeconds(thrown.message);
	CHECK(seconds >= 1);
	CHECK(seconds <= window.count());
	CHECK(thrown == TimeoutFailure(seconds));
}

TEST_CASE("A refused connect frees its connection handle with no rollback or disconnect", "[odbc]") {
	FakeDriverFixture fixture;
	fixture.fake.SetConnectOutcome(SQL_ERROR, {FakeDiag {"08001", 0, "could not connect", {}}});

	CHECK_THROWS_AS(fixture.OpenConnection(), IOException);
	CHECK_FALSE(Called(fixture, "SQLEndTran"));
	CHECK_FALSE(Called(fixture, "SQLDisconnect"));
	const auto entry_points = fixture.EntryPoints();
	REQUIRE(!entry_points.empty());
	CHECK(entry_points.back() == "SQLFreeHandle");
	CHECK(fixture.fake.AllocatedHandles() == "environment");
}

TEST_CASE("A connect to a workgroup the account does not have gives the not-found IOException", "[odbc]") {
	FakeDriverFixture fixture;
	fixture.fake.SetConnectOutcome(
	    SQL_ERROR, {FakeDiag {"HY000",
	                          0,
	                          "[Redshift][ODBC Driver][Server][860:8:IAMConnectionError]: ResourceNotFoundException: "
	                          "Serverless workgroup analytics not found.",
	                          {}}});

	CHECK(redshift_test::Capture([&] { fixture.OpenConnection(); }) ==
	      Thrown {ExceptionType::IO, "Could not connect to Redshift at " + US_EAST_1_TARGET +
	                                     ": the Redshift workgroup was not found.\nThe Redshift Serverless workgroup "
	                                     "may not exist or may not be available to the account; check WORKGROUP, "
	                                     "ACCOUNT_ID and REGION.\nSQLSTATE: HY000"});
}

TEST_CASE("A labelled workgroup endpoint that is not found or does not resolve gets a hint that names no cause",
          "[odbc]") {
	RedshiftConfig config;
	config.region = "us-east-2";
	config.target = RedshiftTarget::SERVERLESS;
	config.workgroup = "wg-a";
	config.endpoint_label = "lbl";
	const std::string target = "wg-a.lbl.us-east-2.redshift-serverless.amazonaws.com (region us-east-2, workgroup "
	                           "wg-a, endpoint label lbl)";
	const std::string hint = "\nCheck the region and the endpoint, and that this host can resolve and reach it.\n"
	                         "SQLSTATE: HY000";
	// libpq's answer, as driver 2.3.0.0 gave it, for a host a resolver said does not exist, which it can say
	// wrongly; then GetCredentials' for a workgroup that is not found.
	const std::vector<std::pair<std::string, std::string>> failures = {
	    {"[Redshift][ODBC Driver][Server]could not translate host name "
	     "\"wg-a.lbl.us-east-2.redshift-serverless.amazonaws.com\" to address: Name or service not known",
	     "the Redshift workgroup's host name could not be resolved."},
	    {"[Redshift][ODBC Driver][Server][860:8:IAMConnectionError]: ResourceNotFoundException: Serverless "
	     "workgroup wg-a not found.",
	     "the Redshift workgroup was not found."}};
	for (const auto &failure : failures) {
		INFO(failure.first);
		FakeDriverFixture fixture;
		fixture.fake.SetConnectOutcome(SQL_ERROR, {FakeDiag {"HY000", 0, failure.first, {}}});
		CHECK(redshift_test::Capture([&] { OpenRedshiftConnection(config, fixture.log_sink); }) ==
		      Thrown {ExceptionType::IO, "Could not connect to Redshift at " + target + ": " + failure.second + hint});
	}
}

TEST_CASE("A connect to a cluster the region does not have gives the cluster's not-found hint", "[odbc]") {
	FakeDriverFixture fixture;
	fixture.fake.SetConnectOutcome(
	    SQL_ERROR, {FakeDiag {"HY000",
	                          0,
	                          "[Redshift][ODBC Driver][Server][860:8:IAMConnectionError]: ClusterNotFound: "
	                          "Cluster warehouse-1 not found.",
	                          {}}});

	CHECK(redshift_test::Capture(
	          [&] { OpenRedshiftConnection(redshift_test::ClusterConfig("us-east-1"), fixture.log_sink); }) ==
	      Thrown {ExceptionType::IO, "Could not connect to Redshift at " + redshift_test::ClusterTarget("us-east-1") +
	                                     ": the Redshift cluster was not found.\nThe Redshift cluster may not exist "
	                                     "or may be paused; check CLUSTER and REGION.\nSQLSTATE: HY000"});
}

TEST_CASE("A connection to a cluster endpoint whose login is refused names the other-account cause", "[odbc]") {
	// As the driver reports the server's refusal of another account's database credentials: libpq's state after
	// the driver's prefix.
	const FakeDiag refused {
	    "28000",
	    0,
	    "[Redshift][ODBC Driver][Server]28000:FATAL:  password authentication failed for user \"IAM:u\"",
	    {}};
	{
		FakeDriverFixture fixture;
		fixture.fake.SetConnectOutcome(SQL_ERROR, {refused});
		CHECK(redshift_test::Capture([&] {
			      OpenRedshiftConnection(redshift_test::ClusterEndpointConfig("us-east-1", 5439), fixture.log_sink);
		      }) == Thrown {ExceptionType::IO,
		                    "Could not connect to Redshift at warehouse-1 (region us-east-1, cluster "
		                    "warehouse-1, endpoint label abc123xyz, port 5439): the Redshift cluster "
		                    "refused the login.\nThe credentials may be for another account that also "
		                    "has a cluster of this name; use credentials for the account of the "
		                    "endpoint.\nSQLSTATE: 28000"});
	}
	// CLUSTER alone has no other-account hint, since the driver looked the cluster up in the credentials'
	// account: it gets the general sign-in hint, and the record, which names the database user, stays out.
	FakeDriverFixture fixture;
	fixture.fake.SetConnectOutcome(SQL_ERROR, {refused});
	const auto thrown = redshift_test::Capture(
	    [&] { OpenRedshiftConnection(redshift_test::ClusterConfig("us-east-1"), fixture.log_sink); });
	CHECK(thrown.message.find("the Redshift cluster refused the login.\nCheck that this AWS identity may sign in to "
	                          "the cluster's database, then retry.\nSQLSTATE: 28000") != std::string::npos);
	CHECK(thrown.message.find("IAM:u") == std::string::npos);
}

TEST_CASE("A refused autocommit request gives IOException naming the refusal, not a connect failure", "[odbc]") {
	FakeDriverFixture fixture;
	fixture.fake.FailCall("SQLSetConnectAttr", 1, SQL_ERROR, {FakeDiag {"HY024", 0, "Invalid attribute value", {}}});

	const auto error = redshift_test::CaptureError([&] { fixture.OpenConnection(); });
	CHECK(Thrown {error.Type(), error.RawMessage()} ==
	      Thrown {ExceptionType::IO, "the Redshift driver refused to enable autocommit, which this extension needs; "
	                                 "report it with the extension and driver versions\nSQLSTATE: HY024"});
	CHECK(error.ExtraInfo().at("redshift_driver") == "test-fake fake " + DuckDB::Platform());
	CHECK(error.ExtraInfo().at("sqlstate") == "HY024");
	CHECK(error.ExtraInfo().at("sqlstates") == "HY024");
}

TEST_CASE("A refused autocommit request frees the handle without connecting", "[odbc]") {
	FakeDriverFixture fixture;
	fixture.fake.FailCall("SQLSetConnectAttr", 1, SQL_ERROR, {});

	CHECK_THROWS_AS(fixture.OpenConnection(), IOException);
	CHECK_FALSE(Called(fixture, "SQLDriverConnect"));
	CHECK(fixture.fake.AllocatedHandles() == "environment");
}

TEST_CASE("A failed environment allocation ends the connect at once with IOException", "[odbc]") {
	FakeDriverFixture fixture;
	fixture.fake.FailCall("SQLAllocHandle", 1, SQL_ERROR, {});

	const auto error = redshift_test::CaptureError([&] { fixture.OpenConnection(); });
	CHECK(Thrown {error.Type(), error.RawMessage()} ==
	      Thrown {ExceptionType::IO,
	              "the Redshift driver could not allocate an ODBC environment; run the statement again"});
	CHECK(error.ExtraInfo().at("redshift_driver") == "test-fake fake " + DuckDB::Platform());
	CHECK(fixture.EntryPoints() == std::vector<std::string> {"SQLAllocHandle"});
}

TEST_CASE("A refused ODBC version gives IOException with its SQLSTATE, leaving no handle allocated", "[odbc]") {
	FakeDriverFixture fixture;
	fixture.fake.FailCall("SQLSetEnvAttr", 1, SQL_ERROR, {FakeDiag {"HY092", 0, "Invalid attribute", {}}});

	const auto error = redshift_test::CaptureError([&] { fixture.OpenConnection(); });
	CHECK(Thrown {error.Type(), error.RawMessage()} ==
	      Thrown {ExceptionType::IO, "the Redshift driver refused ODBC version 3, which this extension needs; install "
	                                 "a published release of the extension\nSQLSTATE: HY092"});
	const unordered_map<string, string> expected {
	    {"sqlstate", "HY092"}, {"sqlstates", "HY092"}, {"redshift_driver", "test-fake fake " + DuckDB::Platform()}};
	CHECK(error.ExtraInfo() == expected);
	CHECK(fixture.fake.AllocatedHandles().empty());
}

TEST_CASE("A failed connection-handle allocation gives IOException with the environment's SQLSTATE", "[odbc]") {
	FakeDriverFixture fixture;
	fixture.fake.FailCall("SQLAllocHandle", 2, SQL_ERROR, {FakeDiag {"HY001", 0, "Memory allocation error", {}}});

	const auto error = redshift_test::CaptureError([&] { fixture.OpenConnection(); });
	CHECK(Thrown {error.Type(), error.RawMessage()} ==
	      Thrown {
	          ExceptionType::IO,
	          "the Redshift driver could not allocate a connection handle; run the statement again\nSQLSTATE: HY001"});
	const unordered_map<string, string> expected {
	    {"sqlstate", "HY001"}, {"sqlstates", "HY001"}, {"redshift_driver", "test-fake fake " + DuckDB::Platform()}};
	CHECK(error.ExtraInfo() == expected);
	CHECK(fixture.fake.AllocatedHandles() == "environment");
}

TEST_CASE("A failed connection-handle allocation with no record carries no SQLSTATE", "[odbc]") {
	FakeDriverFixture fixture;
	fixture.fake.FailCall("SQLAllocHandle", 2, SQL_ERROR, {});

	const auto error = redshift_test::CaptureError([&] { fixture.OpenConnection(); });
	CHECK(error.RawMessage() == "the Redshift driver could not allocate a connection handle; run the statement again");
	const unordered_map<string, string> expected {{"redshift_driver", "test-fake fake " + DuckDB::Platform()}};
	CHECK(error.ExtraInfo() == expected);
}

TEST_CASE("A pre-connect failure whose records could not be read says so, with no SQLSTATE", "[odbc]") {
	const unordered_map<string, string> expected {{"redshift_driver", "test-fake fake " + DuckDB::Platform()}};
	{
		INFO("a refused ODBC version");
		FakeDriverFixture fixture;
		fixture.fake.FailCall("SQLSetEnvAttr", 1, SQL_ERROR, {FakeDiag {"HY092", 0, "Invalid attribute", {}}});
		fixture.fake.FailCall("SQLGetDiagRec", 1, SQL_ERROR, {});
		const auto error = redshift_test::CaptureError([&] { fixture.OpenConnection(); });
		CHECK(error.RawMessage() == "the Redshift driver refused ODBC version 3, which this extension needs, and the "
		                            "driver's diagnostic records could not be read; install a published release of "
		                            "the extension");
		CHECK(error.ExtraInfo() == expected);
	}
	INFO("a failed connection-handle allocation");
	FakeDriverFixture fixture;
	fixture.fake.FailCall("SQLAllocHandle", 2, SQL_ERROR, {FakeDiag {"HY001", 0, "Memory allocation error", {}}});
	fixture.fake.FailCall("SQLGetDiagRec", 1, SQL_ERROR, {});
	const auto error = redshift_test::CaptureError([&] { fixture.OpenConnection(); });
	CHECK(error.RawMessage() == "the Redshift driver could not allocate a connection handle, and the driver's "
	                            "diagnostic records could not be read; run the statement again");
	CHECK(error.ExtraInfo() == expected);
}

TEST_CASE("A pre-connect failure with a record to act on does not say its records could not be read", "[odbc]") {
	FakeDriverFixture fixture;
	fixture.fake.FailCall("SQLAllocHandle", 2, SQL_ERROR,
	                      {FakeDiag {"HY001", 0, "Memory allocation error", {}}, FakeDiag {"HY000", 0, "second", {}}});
	fixture.fake.FailCall("SQLGetDiagRec", 2, SQL_ERROR, {});

	const auto error = redshift_test::CaptureError([&] { fixture.OpenConnection(); });
	CHECK(error.RawMessage() ==
	      "the Redshift driver could not allocate a connection handle; run the statement again\nSQLSTATE: HY001");
}

TEST_CASE("A connection to eu-west-1 sends the host of the workgroup in that region", "[odbc]") {
	FakeDriverFixture fixture;
	const auto config = redshift_test::ServerlessConfig("eu-west-1");

	auto connection = OpenRedshiftConnection(config, fixture.log_sink);
	CHECK(fixture.fake.LastConnectionString().find(
	          "Host={analytics.012345678901.eu-west-1.redshift-serverless.amazonaws.com};") == 0);
}

TEST_CASE("A denied connect to a Serverless workgroup names the Serverless action and resource", "[odbc]") {
	FakeDriverFixture fixture;
	fixture.fake.SetConnectOutcome(SQL_ERROR, {FakeDiag {"42501", 0, "permission denied", {}}});

	CHECK(redshift_test::Capture([&] { fixture.OpenConnection(); }) ==
	      Thrown {ExceptionType::IO, "Could not connect to Redshift at " + US_EAST_1_TARGET +
	                                     ": the AWS identity is not allowed to connect to the workgroup.\nGrant it "
	                                     "redshift-serverless:GetCredentials on arn:aws:redshift-serverless:<region>:"
	                                     "<account>:workgroup/<workgroup-id>.\nSQLSTATE: 42501"});
}

TEST_CASE("A connection to a cluster sends its provisioned settings, with the cluster as its target", "[odbc]") {
	FakeDriverFixture fixture;
	const auto config = redshift_test::ClusterConfig("us-east-1");

	auto connection = OpenRedshiftConnection(config, fixture.log_sink);
	CHECK(fixture.fake.LastConnectionString() == BuildConnectionString(config, TrustSource {fixture.ca_bundle.path}));
	CHECK(connection->Target() == redshift_test::ClusterTarget("us-east-1"));
}

TEST_CASE("A denied connect to a cluster names the provisioned actions on their resources", "[odbc]") {
	FakeDriverFixture fixture;
	fixture.fake.SetConnectOutcome(SQL_ERROR, {FakeDiag {"42501", 0, "permission denied", {}}});

	CHECK(redshift_test::Capture([&] {
		      OpenRedshiftConnection(redshift_test::ClusterConfig("us-east-1"), fixture.log_sink);
	      }) == Thrown {ExceptionType::IO,
	                    "Could not connect to Redshift at " + redshift_test::ClusterTarget("us-east-1") +
	                        ": the AWS identity is not allowed to connect to the cluster.\nGrant it "
	                        "redshift:DescribeClusters on arn:aws:redshift:<region>:<account>:cluster:<cluster> and "
	                        "redshift:GetClusterCredentialsWithIAM on "
	                        "arn:aws:redshift:<region>:<account>:dbname:<cluster>/<database>.\nSQLSTATE: 42501"});
	CHECK(fixture.CallsTo("SQLDriverConnect") == 1);
}

TEST_CASE("A connection to eu-west-1 names that region's host in its target", "[odbc]") {
	FakeDriverFixture fixture;
	const auto config = redshift_test::ServerlessConfig("eu-west-1");

	auto connection = OpenRedshiftConnection(config, fixture.log_sink);
	CHECK(connection->Target() == redshift_test::ServerlessTarget("eu-west-1"));
}

TEST_CASE("A named profile reaches the driver and the target", "[odbc]") {
	FakeDriverFixture fixture;
	auto config = redshift_test::UsEast1Config();
	config.profile = "analytics";

	auto connection = OpenRedshiftConnection(config, fixture.log_sink);
	CHECK(connection->Target() == redshift_test::ServerlessTarget("us-east-1", "analytics"));
	CHECK(StringUtil::EndsWith(fixture.fake.LastConnectionString(), ";Profile={analytics};"));
}

TEST_CASE("A trust source that cannot be used ends the connect before the driver source is called", "[odbc]") {
	FakeDriverFixture fixture;
	// A readable directory, which holds no certificate.
	ScopedEnvironmentVariable ssl_cert_file("SSL_CERT_FILE", "/");

	CHECK(redshift_test::Capture([&] { fixture.OpenConnection(); }) ==
	      Thrown {ExceptionType::IO,
	              "SSL_CERT_FILE names an empty file, a directory or a special file such as a device, so it holds no "
	              "CA certificates to verify the Redshift connection against. Set SSL_CERT_FILE to a file of PEM CA "
	              "certificates, or unset it to use the system CA certificates"});
	CHECK(fixture.fake.SourceCalls() == 0);
}

TEST_CASE("A connect to a database the target does not have fails with the database message, on each target",
          "[odbc]") {
	for (auto config : {redshift_test::UsEast1Config(), redshift_test::ClusterConfig("us-east-1")}) {
		INFO(config.Describe());
		FakeDriverFixture fixture;
		config.database = "secret_sales_db";
		// libpq's text, as driver 2.3.0.0 stamps it; its state was seen live on a cluster and a workgroup.
		fixture.fake.SetConnectOutcome(
		    SQL_ERROR,
		    {FakeDiag {"3D000",
		               0,
		               "[Redshift][ODBC Driver][Server]3D000:FATAL:  database \"secret_sales_db\" does not exist",
		               {}}});
		const auto thrown = redshift_test::Capture([&] { OpenRedshiftConnection(config, fixture.log_sink); });
		const std::string noun = config.target == RedshiftTarget::PROVISIONED ? "cluster" : "workgroup";
		CHECK(thrown.type == ExceptionType::IO);
		CHECK(thrown.message.find("): the database was not found on the Redshift " + noun + ".\nCheck DATABASE") !=
		      std::string::npos);
		CHECK(StringUtil::EndsWith(thrown.message, "\nSQLSTATE: 3D000"));
		// The name appears once, in the target's description, never from the driver's text.
		const auto first = thrown.message.find("secret_sales_db");
		CHECK(first != std::string::npos);
		CHECK(thrown.message.find("secret_sales_db", first + 1) == std::string::npos);
		CHECK(thrown.message.find("does not exist") == std::string::npos);
		// Not retried: one sign-in.
		CHECK(fixture.CallsTo("SQLDriverConnect") == 1);
	}
}
