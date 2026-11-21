// How a connection leaves AWS credentials to the driver: the profile it passes or leaves out, one
// sign-in per connection and none after it, and the error when nothing resolves.

// catch.hpp comes first, as in this directory's other tests.
#include "catch.hpp"

#include "../interface/scoped_environment_variable.hpp"
#include "duckdb/common/string_util.hpp"
#include "fake_driver_fixture.hpp"
#include "thrown.hpp"

#include <string>

using namespace duckdb;
using redshift_driver::testing::FakeDiag;
using redshift_driver::testing::ScopedEnvironmentVariable;
using redshift_test::FakeDriverFixture;
using redshift_test::Thrown;

namespace {

//! UsEast1Config() with the named AWS profile `profile`.
RedshiftConfig UsEast1Profile(const std::string &profile) {
	auto config = redshift_test::UsEast1Config();
	config.profile = profile;
	return config;
}

//! Runs SELECT 1, which every fake scripts, and reads its result to the end.
void RunSelectOne(RedshiftConnection &connection) {
	connection.Describe("SELECT 1");
	auto result = redshift_test::ExecuteStatement(connection, "SELECT 1", {});
	RedshiftBatch batch;
	while (result->NextBatch(batch)) {
	}
}

} // namespace

TEST_CASE("With an empty profile the driver is sent no Profile setting, so it uses its default credential chain",
          "[odbc]") {
	FakeDriverFixture fixture;
	auto connection = fixture.OpenConnection();

	// Searched for directly, not through the client's own SetsKeyword, so this does not test it with itself.
	CHECK(fixture.fake.LastConnectionString().find("Profile=") == std::string::npos);
}

TEST_CASE("A named profile reaches the driver as written, with the spaces and punctuation inside it", "[odbc]") {
	FakeDriverFixture fixture;
	auto connection = OpenRedshiftConnection(UsEast1Profile("team analytics=prod;eu"), fixture.log_sink);

	CHECK(StringUtil::EndsWith(fixture.fake.LastConnectionString(), ";Profile={team analytics=prod;eu};"));
}

TEST_CASE("Each new connection signs in with a SQLDriverConnect of its own", "[odbc]") {
	FakeDriverFixture fixture;
	auto first = fixture.OpenConnection();
	auto second = fixture.OpenConnection();

	CHECK(fixture.CallsTo("SQLDriverConnect") == 2);
}

TEST_CASE("Statements on an open connection never sign in again or disconnect", "[odbc]") {
	FakeDriverFixture fixture;
	auto connection = fixture.OpenConnection();

	for (idx_t run = 0; run < 3; run++) {
		RunSelectOne(*connection);
	}
	CHECK(fixture.CallsTo("SQLDriverConnect") == 1);
	CHECK(fixture.CallsTo("SQLDisconnect") == 0);
}

TEST_CASE("A named profile that resolves no credentials fails the connect with the credentials IOException", "[odbc]") {
	FakeDriverFixture fixture;
	// The text driver 2.3.0.0 gave for a profile it could not find, as connect_error_test.cpp records.
	fixture.fake.SetConnectOutcome(
	    SQL_ERROR, {FakeDiag {"HY000",
	                          0,
	                          "[Redshift][ODBC Driver][Server][860:8:IAMConnectionError]: No AWS profile found: absent",
	                          {}}});

	CHECK(redshift_test::Capture([&] { OpenRedshiftConnection(UsEast1Profile("absent"), fixture.log_sink); }) ==
	      Thrown {ExceptionType::IO,
	              "Could not connect to Redshift at " + redshift_test::ServerlessTarget("us-east-1", "absent") +
	                  ": no usable AWS credentials were found through the AWS credential chain.\nCheck the credentials "
	                  "this process can reach: the default AWS credential chain, or the profile PROFILE names.\n"
	                  "SQLSTATE: HY000"});
}

TEST_CASE("An unsigned request's answer gives the credentials message with and without a profile", "[odbc]") {
	// The answer a failed credential_process is expected to leave, as it sends the request unsigned; this
	// is modelled on an empty chain's measured answer. It guards a phrase the client already matched.
	const std::string unsigned_answer = "[Redshift][ODBC Driver][Server][860:8:IAMConnectionError]: "
	                                    "MissingAuthenticationTokenException: Missing Authentication Token";
	const std::string message = ": no usable AWS credentials were found through the AWS credential chain.\nCheck the "
	                            "credentials this process can reach: the default AWS credential chain, or the profile "
	                            "PROFILE names.\nSQLSTATE: HY000";
	// Without the caller's AWS_PROFILE, which adds the role profile hint under the default chain
	// (connection_environment_test.cpp).
	ScopedEnvironmentVariable unset("AWS_PROFILE", nullptr);
	for (const std::string profile : {"from-process", ""}) {
		INFO("profile '" << profile << "'");
		FakeDriverFixture fixture;
		fixture.fake.SetConnectOutcome(SQL_ERROR, {FakeDiag {"HY000", 0, unsigned_answer, {}}});
		CHECK(redshift_test::Capture([&] { OpenRedshiftConnection(UsEast1Profile(profile), fixture.log_sink); }) ==
		      Thrown {ExceptionType::IO, "Could not connect to Redshift at " +
		                                     redshift_test::ServerlessTarget("us-east-1", profile) + message});
	}
}

TEST_CASE("An open with an AWS secret's expired credentials fails with the AWS secret's message, never the keys",
          "[odbc]") {
	FakeDriverFixture fixture;
	fixture.fake.SetConnectOutcome(
	    SQL_ERROR,
	    {FakeDiag {
	        "HY000", 0, "[Redshift][ODBC Driver][Server][860:8:IAMConnectionError]: ExpiredTokenException", {}}});
	auto config = redshift_test::UsEast1Config();
	config.aws_secret = "creds";
	const RedshiftAwsCredentials credentials {"AKIDEXPIREDCANARY001", "expired-secret-canary", "expired-token-canary"};

	const auto thrown = redshift_test::Capture([&] { OpenRedshiftConnection(config, &credentials, fixture.log_sink); });
	CHECK(thrown ==
	      Thrown {ExceptionType::IO,
	              "Could not connect to Redshift at " + redshift_test::ServerlessHost("us-east-1") +
	                  " (region us-east-1, credentials from AWS secret creds, workgroup " +
	                  redshift_test::TEST_WORKGROUP + ", account " + redshift_test::TEST_ACCOUNT_ID +
	                  "): the AWS credentials from AWS secret 'creds' were "
	                  "refused or have expired.\nThis extension does not refresh an AWS secret's credentials: "
	                  "check the secret's keys, or recreate the secret to get new temporary credentials (for "
	                  "the aws extension's default secret, reopen the database), then retry.\nSQLSTATE: HY000"});
	CHECK(thrown.message.find("credential chain") == std::string::npos);
	for (auto canary : {"AKIDEXPIREDCANARY", "expired-secret-canary", "expired-token-canary"}) {
		CHECK(thrown.message.find(canary) == std::string::npos);
	}
}
