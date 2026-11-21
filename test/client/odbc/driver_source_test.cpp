// When the client asks the driver source for a driver, and what it does with the answer.

// catch.hpp comes first, as in this directory's other tests.
#include "catch.hpp"

#include "duckdb.hpp"
#include "duckdb/common/string_util.hpp"
#include "extension_database.hpp"
#include "fake_driver_fixture.hpp"
#include "thrown.hpp"

#include <chrono>
#include <condition_variable>
#include <future>
#include <memory>
#include <mutex>
#include <string>

using namespace duckdb;
using redshift_driver::DriverErrorKind;
using redshift_driver::DriverLoadResult;
using redshift_driver::testing::FakeOdbcDriver;
using redshift_test::ATTACH_RS;
using redshift_test::ExtensionDatabase;
using redshift_test::FakeDriverFixture;
using redshift_test::Thrown;

namespace {

//! A secret of a type core DuckDB registers: creating one opens no connection either.
const char *const CREATE_SECRET = "CREATE SECRET s (TYPE http)";

//! A load failure's message, with a '%' and a '%s' that would change if it were used as a format.
const char *const LOAD_FAILURE = "every directory was refused: /opt/x (100% full), /tmp/%s (noexec)";

//! Counts the calls to HeldSource and holds each until the test releases them.
class SourceGate {
public:
	//! Whether `count` calls have arrived within `timeout`. Against the system clock, as
	//! FakeGate::WaitUntilHeldFor waits.
	bool WaitForCalls(int count, std::chrono::milliseconds timeout) {
		std::unique_lock<std::mutex> guard(lock);
		return changed.wait_until(guard, std::chrono::system_clock::now() + timeout, [&]() { return calls >= count; });
	}
	//! Lets every held call continue, and every later one through.
	void Release() {
		std::lock_guard<std::mutex> guard(lock);
		released = true;
		changed.notify_all();
	}
	//! Counts the call, then waits until the test releases it. Each test releases on every path.
	void Hold() {
		std::unique_lock<std::mutex> guard(lock);
		calls++;
		changed.notify_all();
		changed.wait(guard, [&]() { return released; });
	}

private:
	std::mutex lock;
	std::condition_variable changed;
	int calls = 0;
	bool released = false;
};

//! The gate HeldSource holds its calls at. A driver source is a plain function, so the gate is a
//! global, which each test that installs HeldSource replaces first.
std::shared_ptr<SourceGate> held_source_gate;

//! A driver source that holds each call at held_source_gate, then answers as the fake does.
DriverLoadResult HeldSource() {
	held_source_gate->Hold();
	return FakeOdbcDriver::Source();
}

} // namespace

TEST_CASE("redshift_query of SELECT 1 returns the fake driver's 1", "[odbc]") {
	FakeDriverFixture fixture;
	ExtensionDatabase database;
	database.Run(ATTACH_RS);

	auto result = database.Run("SELECT * FROM redshift_query('rs', 'SELECT 1')");
	CHECK(result->ColumnName(0).GetIdentifierName() == "?column?");
	auto chunk = result->Fetch();
	REQUIRE(chunk);
	REQUIRE(chunk->size() == 1);
	// Checked through strings, so this code names none of DuckDB's value types.
	CHECK(chunk->GetTypes()[0].ToString() == "INTEGER");
	CHECK(chunk->GetValue(0, 0).ToString() == "1");
	CHECK_FALSE(result->Fetch());
}

TEST_CASE("redshift_query connects with its handle's target and profile, from ATTACH options", "[odbc]") {
	FakeDriverFixture fixture;
	ExtensionDatabase database;
	std::string region;
	std::string profile;
	std::string database_name;
	SECTION("ATTACH options") {
		region = "eu-west-1";
		profile = "analytics";
		database_name = "dev";
		database.Run("ATTACH '' AS rs (TYPE redshift, REGION 'eu-west-1', CREDENTIAL_PROVIDER profile, "
		             "PROFILE 'analytics', WORKGROUP 'analytics', ACCOUNT_ID '012345678901')");
	}
	SECTION("ATTACH options with a database") {
		region = "ap-southeast-2";
		profile = "team";
		database_name = "sales";
		database.Run("ATTACH '' AS rs (TYPE redshift, REGION 'ap-southeast-2', CREDENTIAL_PROVIDER profile, "
		             "PROFILE 'team', WORKGROUP 'analytics', ACCOUNT_ID '012345678901', DATABASE 'sales')");
	}

	database.Run("SELECT * FROM redshift_query('rs', 'SELECT 1')");
	const auto sent = fixture.fake.LastConnectionString();
	CHECK(StringUtil::StartsWith(sent,
	                             "Host={analytics.012345678901." + region + ".redshift-serverless.amazonaws.com};"));
	CHECK(sent.find(";Region={" + region + "};") != std::string::npos);
	CHECK(sent.find(";Workgroup={analytics};") != std::string::npos);
	CHECK(sent.find(";Database={" + database_name + "};") != std::string::npos);
	CHECK(StringUtil::EndsWith(sent, ";Profile={" + profile + "};"));
}

TEST_CASE("redshift_query on an ATTACH of a cluster by CLUSTER alone connects with its ClusterId and database, and no "
          "Host",
          "[odbc]") {
	FakeDriverFixture fixture;
	ExtensionDatabase database;
	database.Run("ATTACH '' AS rs (TYPE redshift, REGION 'us-west-2', CREDENTIAL_PROVIDER aws_default_chain, "
	             "CLUSTER 'warehouse-1', DATABASE 'sales')");

	database.Run("SELECT * FROM redshift_query('rs', 'SELECT 1')");
	const auto sent = fixture.fake.LastConnectionString();
	CHECK(StringUtil::StartsWith(sent, "EndPointUrl={https://redshift.us-west-2.amazonaws.com};Region={us-west-2};"
	                                   "Database={sales};ClusterId={warehouse-1};IAM={true};group_federation={1};"));
	CHECK_FALSE(SetsKeyword(sent, "Host"));
	CHECK_FALSE(SetsKeyword(sent, "Port"));
}

TEST_CASE("redshift_query on an ATTACH by a cluster's redshift:// URI connects to its host and port, with ClusterId",
          "[odbc]") {
	FakeDriverFixture fixture;
	ExtensionDatabase database;
	std::string port;
	SECTION("a port given") {
		port = "5440";
		database.Run("ATTACH 'redshift://warehouse-1.abc123xyz.us-west-2.redshift.amazonaws.com:5440/sales' AS rs "
		             "(TYPE redshift, CREDENTIAL_PROVIDER aws_default_chain)");
	}
	SECTION("no port, so Redshift's default") {
		port = "5439";
		database.Run("ATTACH 'redshift://warehouse-1.abc123xyz.us-west-2.redshift.amazonaws.com/sales' AS rs "
		             "(TYPE redshift, CREDENTIAL_PROVIDER aws_default_chain)");
	}

	database.Run("SELECT * FROM redshift_query('rs', 'SELECT 1')");
	const auto sent = fixture.fake.LastConnectionString();
	CHECK(StringUtil::StartsWith(sent, "Host={warehouse-1.abc123xyz.us-west-2.redshift.amazonaws.com};"
	                                   "EndPointUrl={https://redshift.us-west-2.amazonaws.com};Region={us-west-2};"
	                                   "Port={" +
	                                       port +
	                                       "};Database={sales};ClusterId={warehouse-1};IAM={true};"
	                                       "group_federation={1};SSLMode={verify-full};"));
	CHECK_FALSE(SetsKeyword(sent, "Workgroup"));
	CHECK_FALSE(SetsKeyword(sent, "Serverless"));
}

TEST_CASE("redshift_query on an ATTACH by a workgroup endpoint's ARN or redshift:// URI connects to that endpoint",
          "[odbc]") {
	FakeDriverFixture fixture;
	ExtensionDatabase database;
	std::string database_name;
	SECTION("the long-form ARN, with DATABASE") {
		database_name = "analytics";
		database.Run("ATTACH 'arn:aws:redshift-serverless:eu-north-1:012345678901:workgroup/lbl/wg-a' AS rs "
		             "(TYPE redshift, CREDENTIAL_PROVIDER aws_default_chain, DATABASE 'analytics')");
	}
	SECTION("the redshift:// URI, with no database") {
		database_name = "dev";
		database.Run("ATTACH 'redshift://wg-a.lbl.eu-north-1.redshift-serverless.amazonaws.com' AS rs "
		             "(TYPE redshift, CREDENTIAL_PROVIDER aws_default_chain)");
	}

	database.Run("SELECT * FROM redshift_query('rs', 'SELECT 1')");
	const auto sent = fixture.fake.LastConnectionString();
	CHECK(StringUtil::StartsWith(sent, "Host={wg-a.lbl.eu-north-1.redshift-serverless.amazonaws.com};"));
	CHECK(sent.find(";Region={eu-north-1};") != std::string::npos);
	CHECK(sent.find(";Workgroup={wg-a};") != std::string::npos);
	CHECK(sent.find(";Database={" + database_name + "};") != std::string::npos);
}

TEST_CASE("Loading the extension and every statement that opens no connection leave the driver source uncalled",
          "[odbc]") {
	FakeDriverFixture fixture;
	ExtensionDatabase database;

	database.Run("SELECT 42");
	database.Run(CREATE_SECRET);
	database.Run("SELECT count(*) FROM duckdb_functions() WHERE function_name = 'redshift_query'");
	CHECK(fixture.fake.SourceCalls() == 0);
	CHECK(fixture.fake.Calls().empty());

	// ATTACH opens the attaching DuckDB connection's session, which loads the driver; nothing after it
	// calls the driver until that session closes.
	database.Run("ATTACH '' AS rs (TYPE redshift, REGION 'us-east-1', CREDENTIAL_PROVIDER aws_default_chain, "
	             "WORKGROUP 'analytics', ACCOUNT_ID '012345678901')");
	CHECK(fixture.fake.SourceCalls() == 1);
	const auto calls = fixture.fake.Calls().size();
	database.Run("SELECT count(*) FROM duckdb_databases() WHERE database_name = 'rs'");
	// An empty SQL argument is refused at bind, before the connection is used.
	CHECK(database.con.Query("SELECT * FROM redshift_query('rs', '')")->HasError());
	CHECK(fixture.fake.SourceCalls() == 1);
	CHECK(fixture.fake.Calls().size() == calls);
}

TEST_CASE("A failed driver source gives IOException with its message unchanged after the fixed prefix", "[odbc]") {
	FakeDriverFixture fixture;
	fixture.fake.FailLoad(DriverErrorKind::NO_USABLE_DIRECTORY, LOAD_FAILURE);

	CHECK(redshift_test::Capture([&] { fixture.OpenConnection(); }) ==
	      Thrown {ExceptionType::IO,
	              "the Redshift driver could not be loaded: every directory was refused: /opt/x (100% full), /tmp/%s "
	              "(noexec)"});
}

TEST_CASE("A failed driver source is called again by the next connect, which fails the same way", "[odbc]") {
	FakeDriverFixture fixture;
	fixture.fake.FailLoad(DriverErrorKind::NO_USABLE_DIRECTORY, LOAD_FAILURE);

	const auto first = redshift_test::Capture([&] { fixture.OpenConnection(); });
	const auto second = redshift_test::Capture([&] { fixture.OpenConnection(); });
	CHECK(fixture.fake.SourceCalls() == 2);
	CHECK(second == first);
	CHECK(second.type == ExceptionType::IO);
}

TEST_CASE("A failed driver source carries no redshift_driver, sqlstate or sqlstates key", "[odbc]") {
	FakeDriverFixture fixture;
	fixture.fake.FailLoad(DriverErrorKind::LOAD_FAILED, "the platform loader refused the file");

	const auto extra_info = redshift_test::CaptureError([&] { fixture.OpenConnection(); }).ExtraInfo();
	CHECK(extra_info.count("redshift_driver") == 0);
	CHECK(extra_info.count("sqlstate") == 0);
	CHECK(extra_info.count("sqlstates") == 0);
}

TEST_CASE("A failed driver source logs nothing", "[odbc]") {
	FakeDriverFixture fixture;
	fixture.fake.FailLoad(DriverErrorKind::LOAD_FAILED, "the platform loader refused the file");

	CHECK_THROWS_AS(fixture.OpenConnection(), IOException);
	CHECK(fixture.log_sink->Lines().empty());
}

TEST_CASE("An ATTACH of a workgroup with no account that the target hook does not accept never calls the driver source",
          "[odbc]") {
	// The attach refuses it in either build, before the client sees it: only a preview implementation
	// accepts a workgroup with no account, and only the ones the target hook accepts.
	FakeDriverFixture fixture;
	ExtensionDatabase database;

	auto refused = database.con.Query("ATTACH '' AS rs (TYPE redshift, REGION 'us-east-1', "
	                                  "CREDENTIAL_PROVIDER aws_default_chain, WORKGROUP 'analytics')");
	REQUIRE(refused->HasError());
	CHECK(refused->GetErrorType() == ExceptionType::INVALID_CONFIGURATION);
	CHECK_THAT(refused->GetError(), Catch::Contains("Cannot attach rs: Missing required parameter 'ACCOUNT_ID'"));
	CHECK(fixture.fake.SourceCalls() == 0);
	CHECK(fixture.fake.Calls().empty());
}

TEST_CASE("An ATTACH whose target or database the client refuses fails before the driver source", "[odbc]") {
	// The attach's own checks read neither value's form, so the client's refusal, at the ATTACH's open,
	// is what a user meets, naming the handle and the parameter and never the value.
	const std::string workgroup_rule = "Invalid value for parameter 'WORKGROUP': use a Redshift Serverless "
	                                   "workgroup name of 3 to 64 characters, each a lower-case letter, a digit or '-'";
	FakeDriverFixture fixture;
	ExtensionDatabase database;
	std::string value;
	std::string target;
	std::string expected = workgroup_rule;
	SECTION("a workgroup under 3 characters") {
		value = "wg";
		target = "WORKGROUP 'wg', ACCOUNT_ID '012345678901'";
	}
	SECTION("a workgroup in upper case") {
		value = "Analytics-WG";
		target = "WORKGROUP 'Analytics-WG', ACCOUNT_ID '012345678901'";
	}
	SECTION("a cluster identifier in upper case") {
		value = "Warehouse-1";
		target = "CLUSTER 'Warehouse-1'";
		expected = "Invalid value for parameter 'CLUSTER': use a Redshift cluster identifier of 1 to 63 characters";
	}
	SECTION("a database holding '}'") {
		value = "sales};SSLMode={disable";
		target = "WORKGROUP 'analytics', ACCOUNT_ID '012345678901', DATABASE 'sales};SSLMode={disable'";
		expected = "Invalid value for parameter 'DATABASE': it holds '}', which ends a brace-quoted value";
	}
	auto refused = database.con.Query(
	    "ATTACH '' AS rs (TYPE redshift, REGION 'us-east-1', CREDENTIAL_PROVIDER aws_default_chain, " + target + ")");
	REQUIRE(refused->HasError());
	CHECK(refused->GetErrorType() == ExceptionType::INVALID_INPUT);
	CHECK_THAT(refused->GetError(), Catch::Contains("Cannot attach rs: " + expected));
	CHECK(refused->GetError().find(value) == std::string::npos);
	CHECK(fixture.fake.SourceCalls() == 0);
}

TEST_CASE("A failed driver source leaves the rest of the extension working", "[odbc]") {
	FakeDriverFixture fixture;
	fixture.fake.FailLoad(DriverErrorKind::LOAD_FAILED, "the platform loader refused the file");
	ExtensionDatabase database;

	auto refused = database.con.Query(ATTACH_RS);
	REQUIRE(refused->HasError());
	CHECK(refused->GetErrorType() == ExceptionType::IO);
	// The message a user sees: the extension passes the driver loader's text through unchanged, after
	// the handle.
	CHECK_THAT(refused->GetError(), Catch::Contains("Cannot attach rs: the Redshift driver could not be loaded: the "
	                                                "platform loader refused the file"));
	database.Run("SELECT 42");
	database.Run(CREATE_SECRET);
	database.Run("DROP SECRET s");
}

TEST_CASE("After a successful load the driver source is not called again", "[odbc]") {
	FakeDriverFixture fixture;

	auto first = fixture.OpenConnection();
	auto second = fixture.OpenConnection();
	CHECK(fixture.fake.SourceCalls() == 1);
}

TEST_CASE("The driver's identity is logged once, by the connect that loads it", "[odbc]") {
	FakeDriverFixture fixture;

	auto first = fixture.OpenConnection();
	auto second = fixture.OpenConnection();
	CHECK(fixture.log_sink->Lines() ==
	      std::vector<std::string> {"Redshift driver loaded: test-fake fake " + DuckDB::Platform()});
}

TEST_CASE("A connect that starts while another is loading the driver waits for it, so the source is called once",
          "[odbc]") {
	FakeDriverFixture fixture;
	held_source_gate = std::make_shared<SourceGate>();
	SetDriverSourceForTesting(&HeldSource);

	auto first = std::async(std::launch::async, [&fixture]() { return fixture.OpenConnection(); });
	const bool first_arrived = held_source_gate->WaitForCalls(1, std::chrono::milliseconds(10000));
	auto second = std::async(std::launch::async, [&fixture]() { return fixture.OpenConnection(); });
	// Correct code keeps the second connect waiting for the lock, so this bounded wait runs out.
	const bool second_arrived = held_source_gate->WaitForCalls(2, std::chrono::milliseconds(200));
	held_source_gate->Release();
	const auto deadline = std::chrono::system_clock::now() + std::chrono::milliseconds(10000);
	const bool first_returned = first.wait_until(deadline) == std::future_status::ready;
	const bool second_returned = second.wait_until(deadline) == std::future_status::ready;

	CHECK(first_arrived);
	CHECK_FALSE(second_arrived);
	REQUIRE(first_returned);
	REQUIRE(second_returned);
	CHECK(first.get() != nullptr);
	CHECK(second.get() != nullptr);
	CHECK(fixture.fake.SourceCalls() == 1);
}

namespace {

//! An account-backed workgroup with the default chain, which a test opens directly.
RedshiftConfig WorkgroupConfig() {
	RedshiftConfig config;
	config.region = "us-east-1";
	config.target = RedshiftTarget::SERVERLESS;
	config.workgroup = "analytics";
	config.account_id = "012345678901";
	return config;
}

} // namespace

TEST_CASE("Each open with an AWS secret's credentials sends that open's keys, and a workgroup's port", "[odbc]") {
	FakeDriverFixture fixture;
	auto config = WorkgroupConfig();
	config.aws_secret = "creds";
	config.endpoint_port = 5440;
	auto sink = make_shared_ptr<redshift_test::CapturingLogSink>();
	RedshiftAwsCredentials first {"AKIDFIRSTKEY00000001", "first-secret-key", "first-token"};
	RedshiftAwsCredentials second {"AKIDSECONDKEY0000002", "second-secret-key", ""};
	{
		auto connection = OpenRedshiftConnection(config, &first, sink);
		const auto sent = fixture.fake.LastConnectionString();
		CHECK(sent.find(";AccessKeyID={AKIDFIRSTKEY00000001};SecretAccessKey={first-secret-key};SessionToken={first-"
		                "token};") != std::string::npos);
		CHECK(sent.find(";Port={5440};") != std::string::npos);
		CHECK_FALSE(SetsKeyword(sent, "Profile"));
		CHECK(connection->Target().find("credentials from AWS secret creds") != std::string::npos);
		for (auto canary : {"AKIDFIRSTKEY", "first-secret-key", "first-token"}) {
			CHECK(connection->Target().find(canary) == std::string::npos);
		}
	}
	auto connection = OpenRedshiftConnection(config, &second, sink);
	const auto sent = fixture.fake.LastConnectionString();
	CHECK(sent.find(";AccessKeyID={AKIDSECONDKEY0000002};SecretAccessKey={second-secret-key};") != std::string::npos);
	CHECK(sent.find("first") == std::string::npos);
	CHECK_FALSE(SetsKeyword(sent, "SessionToken"));
	for (auto &line : sink->Lines()) {
		for (auto canary : {"AKIDFIRSTKEY", "first-secret-key", "first-token", "AKIDSECONDKEY", "second-secret-key"}) {
			CHECK(line.find(canary) == std::string::npos);
		}
	}
}

TEST_CASE("An open whose AWS credentials or port the client refuses never calls the driver source", "[odbc]") {
	FakeDriverFixture fixture;
	auto sink = make_shared_ptr<redshift_test::CapturingLogSink>();
	auto config = WorkgroupConfig();
	RedshiftAwsCredentials credentials {"k", "s", ""};
	CHECK_THROWS_AS(OpenRedshiftConnection(config, &credentials, sink), InvalidInputException);
	config.aws_secret = "creds";
	CHECK_THROWS_AS(OpenRedshiftConnection(config, sink), InvalidInputException);
	auto cluster = WorkgroupConfig();
	cluster.target = RedshiftTarget::PROVISIONED;
	cluster.workgroup.clear();
	cluster.account_id.clear();
	cluster.cluster = "warehouse-1";
	cluster.endpoint_port = 5439;
	CHECK_THROWS_AS(OpenRedshiftConnection(cluster, sink), InvalidInputException);
	// A value rule, which Windows checks before the driver source as every platform does.
	RedshiftAwsCredentials braced {"AKIDEXAMPLEKEYID0001", "secret}key", ""};
	CHECK(redshift_test::Capture([&] { OpenRedshiftConnection(config, &braced, sink); }) ==
	      Thrown {ExceptionType::INVALID_INPUT, "Invalid AWS credentials from AWS secret 'creds': SecretAccessKey "
	                                            "holds '}', which ends a brace-quoted value"});
	CHECK(fixture.fake.SourceCalls() == 0);
}

TEST_CASE("redshift_query on an ATTACH by a workgroup URI with a port connects to that endpoint and port", "[odbc]") {
	FakeDriverFixture fixture;
	ExtensionDatabase database;
	database.Run("ATTACH 'redshift://wg-a.lbl.eu-north-1.redshift-serverless.amazonaws.com:5440' AS rs (TYPE "
	             "redshift, CREDENTIAL_PROVIDER aws_default_chain)");
	database.Run("SELECT * FROM redshift_query('rs', 'SELECT 1')");
	const auto sent = fixture.fake.LastConnectionString();
	CHECK(StringUtil::StartsWith(sent, "Host={wg-a.lbl.eu-north-1.redshift-serverless.amazonaws.com};"));
	CHECK(sent.find(";Port={5440};") != std::string::npos);
}
