// What a user or a log can see when the driver echoes credentials or a parameter: the output scan
// of captured output (scan-output.py), and the connect, statement and teardown paths, failing and
// warning, with canary credentials and a canary parameter, whose exceptions, log lines and standard
// streams hold none of them.

// catch.hpp comes first, as in this directory's other tests.
#include "catch.hpp"

#include "canary_output.hpp"
#include "duckdb/common/error_data.hpp"
#include "driver_source_hook.hpp"
#include "fake_driver_fixture.hpp"
#include "live_target.hpp"
#include "thrown.hpp"
#include "trust_store_test_support.hpp"

#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <iterator>
#include <string>
#include <vector>

using duckdb::OpenRedshiftConnection;
using duckdb::RedshiftBatch;
using duckdb::redshift_driver::testing::FakeColumn;
using duckdb::redshift_driver::testing::FakeDiag;
using duckdb::redshift_driver::testing::FakeResultSet;
using redshift_test::CANARY_KEY_ID;
using redshift_test::CANARY_PARAMETER;
using redshift_test::CANARY_SECRET;
using redshift_test::CANARY_TOKEN;
using redshift_test::CapturedStandardStreams;
using redshift_test::FakeDriverFixture;
using redshift_test::ScanFiles;
using redshift_test::ScanOutput;
using redshift_test::SkippedFor;
using redshift_trust_test::TemporaryDirectory;

namespace {

const char *const SELECT_PARAMETER = "SELECT $1::int AS a";

//! The records a driver that quotes what it was given could return for a connect: the connection
//! string's credentials in their keyword forms, and each credential bare.
std::vector<FakeDiag> ConnectEchoes() {
	return {{"28000",
	         0,
	         std::string("[Redshift][ODBC Driver] IAM authentication failed: AccessKeyID=") + CANARY_KEY_ID +
	             ";SecretAccessKey=" + CANARY_SECRET + ";SessionToken=" + CANARY_TOKEN,
	         {}},
	        {"HY000",
	         0,
	         std::string("{\"aws_secret_access_key\": \"") + CANARY_SECRET + "\", \"aws_session_token\": \"" +
	             CANARY_TOKEN + "\"}",
	         {}},
	        {"08001", 0, std::string("signing with ") + CANARY_KEY_ID + " " + CANARY_SECRET + " " + CANARY_TOKEN, {}}};
}

//! The records the server could return for a statement bound to CANARY_PARAMETER, under `sqlstate`:
//! the value echoed, and an access-key ID in an assignment. A statement never carries the secret
//! itself, which redaction cannot know bare.
std::vector<FakeDiag> StatementEchoes(const std::string &sqlstate) {
	return {{sqlstate,
	         0,
	         std::string("ERROR:  invalid input syntax for integer: \"") + CANARY_PARAMETER +
	             "\" signed as AccessKeyID=" + CANARY_KEY_ID,
	         {}},
	        {sqlstate, 0, std::string("context: $1 = '") + CANARY_PARAMETER + "'", {}}};
}

//! SELECT_PARAMETER's one INTEGER row.
FakeResultSet IntegerRow() {
	FakeColumn column;
	column.name = "a";
	column.type_name = "INTEGER";
	column.sql_type = SQL_INTEGER;
	column.column_size = 10;
	FakeResultSet result;
	result.columns.push_back(column);
	result.rows.push_back({"1"});
	return result;
}

//! What `call` threw, as a user and a log reader see it: its message and each extra_info entry, one
//! a line, or "returned" when it threw nothing, which HasError decides, so a std::exception that is
//! not DuckDB's is scanned too.
template <class CALL>
std::string ShownBy(CALL &&call) {
	const auto error = redshift_test::CaptureError(call);
	if (!error.HasError()) {
		return "returned\n";
	}
	std::string shown = error.RawMessage() + "\n";
	for (const auto &entry : error.ExtraInfo()) {
		shown += entry.first + "=" + entry.second + "\n";
	}
	return shown;
}

//! `lines`, one a line.
std::string Logged(const std::vector<std::string> &lines) {
	std::string logged;
	for (const auto &line : lines) {
		logged += line + "\n";
	}
	return logged;
}

//! A connect with the canary credentials that fails with ConnectEchoes, and what it shows.
std::string ConnectFailureShown() {
	FakeDriverFixture fixture;
	fixture.fake.SetConnectOutcome(SQL_ERROR, ConnectEchoes());
	auto config = redshift_test::UsEast1Config();
	config.aws_secret = "canaries";
	const auto credentials = redshift_test::CanaryCredentials();
	auto shown = ShownBy([&]() { OpenRedshiftConnection(config, &credentials, fixture.log_sink); });
	return shown + Logged(fixture.log_sink->Lines());
}

//! A connection opened with the canary credentials, warned with ConnectEchoes, through `fixture`.
duckdb::unique_ptr<duckdb::RedshiftConnection> WarnedConnection(FakeDriverFixture &fixture) {
	fixture.fake.SetConnectOutcome(SQL_SUCCESS_WITH_INFO, ConnectEchoes());
	fixture.fake.AddResult(SELECT_PARAMETER, {IntegerRow()}, 1);
	auto config = redshift_test::UsEast1Config();
	config.aws_secret = "canaries";
	const auto credentials = redshift_test::CanaryCredentials();
	return OpenRedshiftConnection(config, &credentials, fixture.log_sink);
}

//! SELECT_PARAMETER bound to CANARY_PARAMETER on `connection`, read to its end.
void ExecuteWithCanary(duckdb::RedshiftConnection &connection) {
	auto result = redshift_test::ExecuteStatement(connection, SELECT_PARAMETER, {CANARY_PARAMETER});
	RedshiftBatch batch;
	while (result->NextBatch(batch)) {
	}
}

//! On a warned connect, executes that fail and one that warns with StatementEchoes, then a teardown
//! whose disconnect fails with ConnectEchoes, and what each shows.
std::string WarnedConnectAndStatementsShown() {
	FakeDriverFixture fixture;
	auto connection = WarnedConnection(fixture);
	std::string shown;
	// Classes 22 and XX are classes whose server text is shown (XX its first line), class 58 one whose
	// is not.
	for (const auto &sqlstate : {"22P02", "XX000", "58030"}) {
		fixture.FailNextCall("SQLExecute", SQL_ERROR, StatementEchoes(sqlstate));
		shown += ShownBy([&]() { ExecuteWithCanary(*connection); });
	}
	fixture.fake.WarnCall("SQLExecute", duckdb::NumericCast<int>(fixture.CallsTo("SQLExecute")) + 1,
	                      StatementEchoes("01000"));
	shown += ShownBy([&]() { ExecuteWithCanary(*connection); });
	fixture.fake.FailCall("SQLDisconnect", 1, SQL_ERROR, ConnectEchoes());
	connection.reset();
	return shown + Logged(fixture.log_sink->Lines());
}

//! On a warned connect, a fetch the connection's loss fails with StatementEchoes, and what it shows.
std::string LostFetchShown() {
	FakeDriverFixture fixture;
	auto connection = WarnedConnection(fixture);
	fixture.FailNextCall("SQLFetch", SQL_ERROR, StatementEchoes("08S01"));
	auto shown = ShownBy([&]() { ExecuteWithCanary(*connection); });
	connection.reset();
	return shown + Logged(fixture.log_sink->Lines());
}

//! `path`'s whole content.
std::string ReadFile(const std::string &path) {
	std::ifstream in(path.c_str(), std::ios::binary);
	return std::string(std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>());
}

//! The scripted records' messages, one a line: what the driver handed the client.
std::string EchoedText() {
	std::string text;
	for (const auto &records : {ConnectEchoes(), StatementEchoes("22P02")}) {
		for (const auto &record : records) {
			text += record.message + "\n";
		}
	}
	return text;
}

//! The workgroup the live variables name, reached through the embedded driver while this lives,
//! through the test endpoints when the build and the environment give both, and otherwise with them
//! unset, as any other build would refuse them.
class LiveWorkgroup {
public:
	LiveWorkgroup() {
		duckdb::SetDriverSourceForTesting(&duckdb::redshift_driver::LoadEmbeddedDriver);
		if (!duckdb::BuildHasTestEndpoints() || !std::getenv(duckdb::TEST_HOST_VARIABLE) ||
		    !std::getenv(duckdb::TEST_URL_VARIABLE)) {
			no_test_variables = duckdb::make_uniq<redshift_test::NoTestVariables>();
		}
		config = redshift_test::ServerlessConfig(std::getenv("REDSHIFT_TEST_LIVE_REGION"));
		config.workgroup = std::getenv("REDSHIFT_TEST_LIVE_WORKGROUP");
		config.account_id = std::getenv("REDSHIFT_TEST_LIVE_ACCOUNT_ID");
	}
	~LiveWorkgroup() {
		duckdb::ResetProcessStateForTesting();
	}
	LiveWorkgroup(const LiveWorkgroup &) = delete;
	LiveWorkgroup &operator=(const LiveWorkgroup &) = delete;

	duckdb::RedshiftConfig config;
	duckdb::shared_ptr<redshift_test::CapturingLogSink> log_sink =
	    duckdb::make_shared_ptr<redshift_test::CapturingLogSink>();

private:
	duckdb::unique_ptr<redshift_test::NoTestVariables> no_test_variables;
};

//! What a cast of `value` to an integer shows when the server refuses it, on `connection`.
std::string FailedCastShown(duckdb::RedshiftConnection &connection, const std::string &value) {
	const std::string sql = "SELECT CAST($1 AS INTEGER) AS a";
	const auto shape = connection.Describe(sql);
	return ShownBy([&]() {
		auto result = connection.Execute(sql, {value}, shape);
		RedshiftBatch batch;
		while (result->NextBatch(batch)) {
		}
	});
}

} // namespace

TEST_CASE("The output scan reports each credential form and canary by line, never the text it found", "[odbc]") {
	TemporaryDirectory directory;
	const auto path = directory.File("output.txt", std::string("a clean line\n"
	                                                           "key AKIAIOSFODNN7EXAMPLE in use\n"
	                                                           "aws_secret_access_key=abc123\n"
	                                                           "{\"SessionToken\": \"tok123\"}\n"
	                                                           "pwd = {pass123}\n"
	                                                           "Password\\\":quoted123\n"
	                                                           "session ASIAIOSFODNN7EXAMPLE\n"
	                                                           "Password: //slashed123\n"
	                                                           "PWD: :colon123\n"
	                                                           "PWD=<redacted>after123\n"
	                                                           "Password\"\":doubled123\n"
	                                                           "{\"Password\"://urlish123}\n"
	                                                           "PWD=<redacted>...cut123\n"
	                                                           "PWD=<redacted>,comma123\n"
	                                                           "PWD=<redacted>}brace123\n"
	                                                           "aws_session_token=t1\n"
	                                                           "session_token: t2\n"
	                                                           "SecretAccessKey=t3\n"
	                                                           "Password'':single123\n"
	                                                           "PWD=//eq123\n"
	                                                           "PWD=a1;Password=b2\n"
	                                                           "Password://secret123\n"
	                                                           "PWD=:eq456\n") +
	                                                   "bound " + CANARY_PARAMETER + "\n");
	const auto scan = ScanOutput(path);

	CHECK(scan.exit_code == 1);
	// Line 21 has two assignments and is reported once.
	CHECK(scan.report == path + ":2: an access-key ID\n" + path + ":3: an unredacted aws_secret_access_key value\n" +
	                         path + ":4: an unredacted SessionToken value\n" + path + ":5: an unredacted pwd value\n" +
	                         path + ":6: an unredacted Password value\n" + path + ":7: an access-key ID\n" + path +
	                         ":8: an unredacted Password value\n" + path + ":9: an unredacted PWD value\n" + path +
	                         ":10: an unredacted PWD value\n" + path + ":11: an unredacted Password value\n" + path +
	                         ":12: an unredacted Password value\n" + path + ":13: an unredacted PWD value\n" + path +
	                         ":14: an unredacted PWD value\n" + path + ":15: an unredacted PWD value\n" + path +
	                         ":16: an unredacted aws_session_token value\n" + path +
	                         ":17: an unredacted session_token value\n" + path +
	                         ":18: an unredacted SecretAccessKey value\n" + path +
	                         ":19: an unredacted Password value\n" + path + ":20: an unredacted PWD value\n" + path +
	                         ":21: an unredacted PWD value\n" + path + ":22: an unredacted Password value\n" + path +
	                         ":23: an unredacted PWD value\n" + path + ":24: canary 4\n");
	for (const auto &found :
	     {"AKIAIOSFODNN7EXAMPLE", "abc123", "tok123", "pass123", "quoted123", "ASIAIOSFODNN7EXAMPLE", "slashed123",
	      "colon123", "after123", "doubled123", "urlish123", "cut123", "comma123", "brace123", "single123", "eq123",
	      "secret123", "eq456", CANARY_PARAMETER}) {
		CHECK(scan.report.find(found) == std::string::npos);
	}
}

TEST_CASE("The output scan reports a token, client secret, proxy password or master key assignment", "[odbc]") {
	TemporaryDirectory directory;
	const auto path = directory.File("output.txt", "web_identity_token=placeholder1\n"
	                                               "{\"token\": \"placeholder2\"}\n"
	                                               "CLIENT_SECRET = placeholder3\n"
	                                               "ProxyPwd={placeholder4}\n"
	                                               "https_proxy_password: placeholder5\n"
	                                               "master_symmetric_key=placeholder6\n");
	const auto scan = ScanOutput(path);

	CHECK(scan.exit_code == 1);
	CHECK(scan.report == path + ":1: an unredacted web_identity_token value\n" + path +
	                         ":2: an unredacted token value\n" + path + ":3: an unredacted CLIENT_SECRET value\n" +
	                         path + ":4: an unredacted ProxyPwd value\n" + path +
	                         ":5: an unredacted https_proxy_password value\n" + path +
	                         ":6: an unredacted master_symmetric_key value\n");
	CHECK(scan.report.find("placeholder") == std::string::npos);
}

TEST_CASE("The output scan finds nothing in redaction's markers or in a form that assigns nothing", "[odbc]") {
	TemporaryDirectory directory;
	const auto path = directory.File("output.txt", "AccessKeyID=<redacted>;PWD=<param 1>\n"
	                                               "SELECT pwd::text\n"
	                                               "Password:\n"
	                                               "PWD=;\n"
	                                               "XAKIAIOSFODNN7EXAMPLE and mysession_token=abc\n"
	                                               "PWD={<redacted>}\n"
	                                               "{\"aws_session_token\": \"<redacted>\", \"a\": 1}\n"
	                                               "PWD=<redacted>...\n"
	                                               "web_identity_token=<redacted>;token_type=bearer\n");
	const auto scan = ScanOutput(path);

	CHECK(scan.report.empty());
	CHECK(scan.exit_code == 0);
}

TEST_CASE("The output scan refuses a file it cannot read, as a usage error rather than a finding", "[odbc]") {
	TemporaryDirectory directory;
	const auto scan = ScanOutput(directory.Path("missing.txt"));

	CHECK(scan.exit_code == 2);
	CHECK_THAT(scan.report, Catch::Contains("cannot read " + directory.Path("missing.txt")));
}

TEST_CASE("The output scan refuses a file written as UTF-16, which its patterns cannot match", "[odbc]") {
	TemporaryDirectory directory;
	const std::string text = "PWD=placeholder\n";
	std::string utf16;
	for (const char c : text) {
		utf16 += std::string(1, c) + '\0';
	}
	const auto scan = ScanOutput(directory.File("output.txt", "a clean line\n" + utf16));

	CHECK(scan.exit_code == 2);
	CHECK_THAT(scan.report, Catch::Contains("cannot read " + directory.Path("output.txt") + ": line 2 holds a NUL"));
}

TEST_CASE("The output scan refuses a run given no file, which would find nothing", "[odbc]") {
	const auto scan = ScanFiles({}, {});

	CHECK(scan.exit_code == 2);
	CHECK_THAT(scan.report, Catch::Contains("FILE"));
}

TEST_CASE("The output scan numbers a case's own canaries after the shared ones", "[odbc]") {
	TemporaryDirectory directory;
	const auto scan = ScanOutput(directory.File("output.txt", "db canary-database\n"), {"canary-database"});

	CHECK(scan.exit_code == 1);
	CHECK(scan.report == directory.Path("output.txt") + ":1: canary 5\n");
}

TEST_CASE("The output scan refuses an empty canary, which every line would hold", "[odbc]") {
	TemporaryDirectory directory;
	const auto scan = ScanOutput(directory.File("output.txt", "x\n"), {""});

	CHECK(scan.exit_code == 2);
	CHECK_THAT(scan.report, Catch::Contains("a canary must not be empty"));
}

TEST_CASE("The scripted records hold every canary and credential form, so the canary run can fail", "[odbc]") {
	TemporaryDirectory directory;
	const auto scan = ScanOutput(directory.File("echoed.txt", EchoedText()));

	CHECK(scan.exit_code == 1);
	for (const auto &finding :
	     {"an access-key ID", "an unredacted AccessKeyID value", "an unredacted aws_secret_access_key value",
	      "canary 1", "canary 2", "canary 3", "canary 4"}) {
		CHECK_THAT(scan.report, Catch::Contains(finding));
	}
}

TEST_CASE("What the process writes to its standard streams is captured while a capture lives", "[odbc]") {
	TemporaryDirectory directory;
	const auto path = directory.Path("streams.out");
	{
		CapturedStandardStreams streams(path);
		// Flushed at once, as standard error is unbuffered, so the file keeps the order they were written in.
		std::fprintf(stdout, "to standard output\n");
		std::fflush(stdout);
		std::fprintf(stderr, "to standard error\n");
	}
	CHECK(ReadFile(path) == "to standard output\nto standard error\n");
}

TEST_CASE("No canary reaches an exception, a log line or a standard stream on a failing or warning path", "[odbc]") {
	TemporaryDirectory directory;
	const auto streams = directory.Path("streams.out");
	std::string shown;
	{
		CapturedStandardStreams capture(streams);
		shown = ConnectFailureShown() + WarnedConnectAndStatementsShown() + LostFetchShown();
	}
	const auto path = directory.File("shown.txt", shown + ReadFile(streams));
	INFO(shown);
	// Each path ran as scripted: the failed connect, the two failed executes and the lost fetch threw,
	// the warned execute returned, and the warnings and the teardown were logged.
	CHECK_THAT(shown, Catch::Contains("Could not connect to Redshift") && Catch::Contains("SQLSTATE: 22P02") &&
	                      Catch::Contains("SQLSTATE: XX000") && Catch::Contains("SQLSTATE: 08S01") &&
	                      Catch::Contains("returned") && Catch::Contains("SQLSTATE 01000") &&
	                      Catch::Contains("the disconnect failed"));
	const auto scan = ScanOutput(path);
	CHECK(scan.report.empty());
	CHECK(scan.exit_code == 0);
}

TEST_CASE("Live: a canary parameter the server echoes in a class 22 failure shows as its marker", "[odbc][live]") {
	if (SkippedFor(redshift_test::LiveSkipReason())) {
		return;
	}
	TemporaryDirectory directory;
	const auto streams = directory.Path("streams.out");
	std::string short_value_shown;
	std::string canary_shown;
	{
		CapturedStandardStreams capture(streams);
		LiveWorkgroup live;
		auto connection = OpenRedshiftConnection(live.config, live.log_sink);
		// A value under 8 bytes is left as the server wrote it, which shows the server echoes the value.
		short_value_shown = FailedCastShown(*connection, "abc");
		canary_shown = FailedCastShown(*connection, CANARY_PARAMETER) + Logged(live.log_sink->Lines());
	}
	// Scanned first: on a live session a leak can be a real credential, which no failure may print.
	const auto scan = ScanOutput(directory.File("shown.txt", short_value_shown + canary_shown + ReadFile(streams)));
	REQUIRE(scan.report.empty());
	CHECK(scan.exit_code == 0);
	INFO(short_value_shown << canary_shown);
	CHECK_THAT(short_value_shown, Catch::Contains("\"abc\"") && Catch::Contains("SQLSTATE: 22P02"));
	CHECK_THAT(canary_shown, Catch::Contains("\"<param 1>\"") && Catch::Contains("SQLSTATE: 22P02"));
}

TEST_CASE("Live: canary AWS credentials the service refuses reach no exception, log line or standard stream",
          "[odbc][live]") {
	if (SkippedFor(redshift_test::LiveSkipReason())) {
		return;
	}
	TemporaryDirectory directory;
	const auto streams = directory.Path("streams.out");
	std::string shown;
	{
		CapturedStandardStreams capture(streams);
		LiveWorkgroup live;
		live.config.aws_secret = "canaries";
		const auto credentials = redshift_test::CanaryCredentials();
		shown = ShownBy([&]() { OpenRedshiftConnection(live.config, &credentials, live.log_sink); }) +
		        Logged(live.log_sink->Lines());
	}
	const auto scan = ScanOutput(directory.File("shown.txt", shown + ReadFile(streams)));
	REQUIRE(scan.report.empty());
	CHECK(scan.exit_code == 0);
	INFO(shown);
	CHECK_THAT(shown, Catch::Contains("the AWS credentials from AWS secret 'canaries' were refused"));
}
