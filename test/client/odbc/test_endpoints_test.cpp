// The end-to-end test variables. A build without the test-endpoint override
// refuses each one; a build with it takes the host and the URL together or not at all. A connect
// refuses first, before the trust search and the driver source, naming variables, never a value.
// The switch's configure and Makefile cases are in region_allowlist_test.cpp, by its helpers.

// catch.hpp comes first, as in this directory's other tests.
#include "catch.hpp"

#include "../interface/scoped_environment_variable.hpp"
#include "extension_database.hpp"
#include "fake_driver_fixture.hpp"
#include "odbc/connection_settings.hpp"
#include "odbc/odbc_connection.hpp"
#include "thrown.hpp"

#include <string>

#ifndef REDSHIFT_TEST_ENDPOINTS_CONFIGURED
#error "REDSHIFT_TEST_ENDPOINTS_CONFIGURED must say whether the configure set the switch; sources.cmake sets it"
#endif

using namespace duckdb;
using redshift_driver::DriverErrorKind;
using redshift_driver::testing::ScopedEnvironmentVariable;
using redshift_test::Capture;
using redshift_test::ExtensionDatabase;
using redshift_test::FakeDriverFixture;
using redshift_test::NoTestVariables;
using redshift_test::Thrown;

namespace {

//! Whether this build was configured with the switch, read from the configure, not from the client.
constexpr bool CONFIGURED_WITH_TEST_ENDPOINTS = REDSHIFT_TEST_ENDPOINTS_CONFIGURED != 0;

const std::string CASE_VARIABLE = TEST_CASE_VARIABLE;
const std::string URL_VARIABLE = TEST_URL_VARIABLE;
const std::string HOST_VARIABLE = TEST_HOST_VARIABLE;

//! Values that must reach no message.
const char *const CANARY_CASE = "canary-case";
const char *const CANARY_HOST = "canary-host.example";
const char *const CANARY_URL = "https://canary-url.example:8443";

Thrown NoOverrideRefusal(const std::string &name) {
	return Thrown {ExceptionType::INVALID_INPUT,
	               name + " is set, but this build has no test-endpoint override, so the connection would go to the "
	                      "endpoint the target derives and is refused. Unset it to connect."};
}

Thrown EmptyRefusal(const std::string &name) {
	return Thrown {ExceptionType::INVALID_INPUT,
	               name + " is set to an empty value, which would send the connection to the endpoint the target "
	                      "derives and is refused. Set it or unset it."};
}

Thrown MissingRefusal(const std::string &set, const std::string &missing) {
	return Thrown {ExceptionType::INVALID_INPUT,
	               set + " is set but " + missing +
	                   " is not, so the connection would go to the endpoint the target derives and is refused. Set " +
	                   HOST_VARIABLE + " and " + URL_VARIABLE + " together."};
}

Thrown LongRefusal(const std::string &name) {
	return Thrown {ExceptionType::INVALID_INPUT,
	               name + " is longer than the 255 bytes a test endpoint may have and is refused."};
}

//! What ReadTestEndpoints throws in a build with or without the override.
Thrown ReadThrown(bool test_endpoints_build) {
	return Capture([&] { ReadTestEndpoints(test_endpoints_build); });
}

const char *const TRUST_PATH = "/etc/pki/tls/certs/ca-bundle.crt";

//! `text` with its one `from` replaced by `to`.
std::string ReplacedOnce(std::string text, const std::string &from, const std::string &to) {
	const auto at = text.find(from);
	REQUIRE(at != std::string::npos);
	REQUIRE(text.find(from, at + 1) == std::string::npos);
	return text.replace(at, from.size(), to);
}

//! us-east-1's connection string `derived` with its Host and EndPointUrl replaced by `endpoints`'.
std::string WithTestEndpoints(const std::string &derived, const TestEndpoints &endpoints) {
	const auto with_host = ReplacedOnce(derived, "Host={" + redshift_test::ServerlessHost("us-east-1") + "};",
	                                    "Host={" + endpoints.host + "};");
	return ReplacedOnce(with_host, "EndPointUrl={https://redshift-serverless.us-east-1.amazonaws.com};",
	                    "EndPointUrl={" + endpoints.endpoint_url + "};");
}

} // namespace

TEST_CASE("A build without the test-endpoint override refuses each test variable, an empty one included", "[odbc]") {
	NoTestVariables none;
	for (const auto &name : {CASE_VARIABLE, URL_VARIABLE, HOST_VARIABLE}) {
		INFO("variable " << name);
		{
			ScopedEnvironmentVariable set(name.c_str(), CANARY_URL);
			CHECK(ReadThrown(false) == NoOverrideRefusal(name));
		}
		ScopedEnvironmentVariable empty(name.c_str(), "");
		CHECK(ReadThrown(false) == NoOverrideRefusal(name));
	}
}

TEST_CASE("The client has the test-endpoint override exactly when the configure set the switch", "[odbc]") {
	CHECK(BuildHasTestEndpoints() == CONFIGURED_WITH_TEST_ENDPOINTS);
}

TEST_CASE("Either build, given no test variable, keeps the derived endpoints", "[odbc]") {
	NoTestVariables none;
	for (const bool test_endpoints_build : {false, true}) {
		const auto endpoints = ReadTestEndpoints(test_endpoints_build);
		CHECK(endpoints.host.empty());
		CHECK(endpoints.endpoint_url.empty());
	}
}

TEST_CASE("A build with the override takes the host and the URL together, with or without the case", "[odbc]") {
	NoTestVariables none;
	ScopedEnvironmentVariable url(URL_VARIABLE.c_str(), CANARY_URL);
	ScopedEnvironmentVariable host(HOST_VARIABLE.c_str(), CANARY_HOST);
	auto endpoints = ReadTestEndpoints(true);
	CHECK(endpoints.host == CANARY_HOST);
	CHECK(endpoints.endpoint_url == CANARY_URL);

	ScopedEnvironmentVariable test_case(CASE_VARIABLE.c_str(), CANARY_CASE);
	endpoints = ReadTestEndpoints(true);
	CHECK(endpoints.host == CANARY_HOST);
	CHECK(endpoints.endpoint_url == CANARY_URL);
}

TEST_CASE("A build with the override refuses an empty URL or host, whatever else is set", "[odbc]") {
	NoTestVariables none;
	{
		ScopedEnvironmentVariable url(URL_VARIABLE.c_str(), "");
		CHECK(ReadThrown(true) == EmptyRefusal(URL_VARIABLE));
		ScopedEnvironmentVariable host(HOST_VARIABLE.c_str(), CANARY_HOST);
		ScopedEnvironmentVariable test_case(CASE_VARIABLE.c_str(), CANARY_CASE);
		CHECK(ReadThrown(true) == EmptyRefusal(URL_VARIABLE));
	}
	ScopedEnvironmentVariable host(HOST_VARIABLE.c_str(), "");
	CHECK(ReadThrown(true) == EmptyRefusal(HOST_VARIABLE));
	ScopedEnvironmentVariable url(URL_VARIABLE.c_str(), CANARY_URL);
	CHECK(ReadThrown(true) == EmptyRefusal(HOST_VARIABLE));
}

TEST_CASE("A build with the override refuses a URL or host over 255 bytes, not one of 255", "[odbc]") {
	NoTestVariables none;
	const std::string longest(255, 'h');
	const std::string longer(256, 'h');
	ScopedEnvironmentVariable url(URL_VARIABLE.c_str(), longest.c_str());
	ScopedEnvironmentVariable host(HOST_VARIABLE.c_str(), longest.c_str());
	const auto endpoints = ReadTestEndpoints(true);
	CHECK(endpoints.endpoint_url == longest);
	CHECK(endpoints.host == longest);
	for (const auto &name : {URL_VARIABLE, HOST_VARIABLE}) {
		INFO("variable " << name);
		ScopedEnvironmentVariable too_long(name.c_str(), longer.c_str());
		CHECK(ReadThrown(true) == LongRefusal(name));
	}
}

TEST_CASE("A build with the override refuses the URL or the host without the other, and the case without both",
          "[odbc]") {
	NoTestVariables none;
	SECTION("the case alone, an empty one included") {
		{
			ScopedEnvironmentVariable empty(CASE_VARIABLE.c_str(), "");
			CHECK(ReadThrown(true) == MissingRefusal(CASE_VARIABLE, URL_VARIABLE));
		}
		ScopedEnvironmentVariable test_case(CASE_VARIABLE.c_str(), CANARY_CASE);
		CHECK(ReadThrown(true) == MissingRefusal(CASE_VARIABLE, URL_VARIABLE));
		ScopedEnvironmentVariable host(HOST_VARIABLE.c_str(), CANARY_HOST);
		CHECK(ReadThrown(true) == MissingRefusal(CASE_VARIABLE, URL_VARIABLE));
	}
	SECTION("the host alone") {
		ScopedEnvironmentVariable host(HOST_VARIABLE.c_str(), CANARY_HOST);
		CHECK(ReadThrown(true) == MissingRefusal(HOST_VARIABLE, URL_VARIABLE));
	}
	SECTION("the URL alone, or with the case") {
		ScopedEnvironmentVariable url(URL_VARIABLE.c_str(), CANARY_URL);
		CHECK(ReadThrown(true) == MissingRefusal(URL_VARIABLE, HOST_VARIABLE));
		ScopedEnvironmentVariable test_case(CASE_VARIABLE.c_str(), CANARY_CASE);
		CHECK(ReadThrown(true) == MissingRefusal(URL_VARIABLE, HOST_VARIABLE));
	}
}

TEST_CASE("The test endpoints replace the host and the control-plane URL in the connection string alone", "[odbc]") {
	const auto config = redshift_test::UsEast1Config();
	const TrustSource trust {TRUST_PATH};
	const auto derived = BuildConnectionString(config, trust);
	CHECK(BuildConnectionString(config, trust, TestEndpoints()) == derived);
	const TestEndpoints endpoints {CANARY_HOST, CANARY_URL};
	CHECK(BuildConnectionString(config, trust, endpoints) == WithTestEndpoints(derived, endpoints));
}

TEST_CASE("A test endpoint goes through the encoder, so one that would add a setting is refused", "[odbc]") {
	const auto config = redshift_test::UsEast1Config();
	const TrustSource trust {TRUST_PATH};
	auto refusal = [](const std::string &keyword) {
		return Thrown {ExceptionType::INVALID_INPUT, "Invalid value for the Redshift connection setting '" + keyword +
		                                                 "': it holds '}', which ends a brace-quoted value"};
	};
	CHECK(Capture([&] {
		      BuildConnectionString(config, trust, TestEndpoints {"127.0.0.1};SSLMode={disable", CANARY_URL});
	      }) == refusal("Host"));
	CHECK(Capture([&] {
		      BuildConnectionString(config, trust, TestEndpoints {CANARY_HOST, "https://127.0.0.1};IAM={false"});
	      }) == refusal("EndPointUrl"));
}

TEST_CASE("ConnectHost names the test host when there is one, else the target's host", "[odbc]") {
	const auto target = ResolveTarget(redshift_test::UsEast1Config());
	CHECK(ConnectHost(target, TestEndpoints()) == redshift_test::ServerlessHost("us-east-1"));
	CHECK(ConnectHost(target, TestEndpoints {CANARY_HOST, CANARY_URL}) == CANARY_HOST);
}

TEST_CASE("A cluster named by CLUSTER alone takes the test URL alone, as it is sent no Host, and its target names the "
          "cluster",
          "[odbc]") {
	const auto config = redshift_test::ClusterConfig("us-east-1");
	const TrustSource trust {TRUST_PATH};
	const TestEndpoints endpoints {CANARY_HOST, CANARY_URL};
	CHECK(BuildConnectionString(config, trust, endpoints) ==
	      ReplacedOnce(BuildConnectionString(config, trust), "EndPointUrl={https://redshift.us-east-1.amazonaws.com};",
	                   std::string("EndPointUrl={") + CANARY_URL + "};"));
	const auto target = ResolveTarget(config);
	CHECK(TargetName(target, endpoints) == redshift_test::TEST_CLUSTER);
	CHECK(TargetName(target, TestEndpoints()) == redshift_test::TEST_CLUSTER);
	// A workgroup's name is its Host.
	CHECK(TargetName(ResolveTarget(redshift_test::UsEast1Config()), endpoints) == CANARY_HOST);
}

TEST_CASE("A cluster endpoint takes both test endpoints, as it is sent a Host, and its target names the cluster",
          "[odbc]") {
	const auto config = redshift_test::ClusterEndpointConfig("us-east-1", 5440);
	const TrustSource trust {TRUST_PATH};
	const TestEndpoints endpoints {CANARY_HOST, CANARY_URL};
	const auto host = std::string(redshift_test::TEST_CLUSTER) + ".abc123xyz.us-east-1.redshift.amazonaws.com";
	const auto with_host = ReplacedOnce(BuildConnectionString(config, trust), "Host={" + host + "};",
	                                    std::string("Host={") + CANARY_HOST + "};");
	CHECK(BuildConnectionString(config, trust, endpoints) ==
	      ReplacedOnce(with_host, "EndPointUrl={https://redshift.us-east-1.amazonaws.com};",
	                   std::string("EndPointUrl={") + CANARY_URL + "};"));
	CHECK(TargetName(ResolveTarget(config), endpoints) == redshift_test::TEST_CLUSTER);
}

TEST_CASE("A connect with a test variable set is refused before the trust search, the source and any driver call",
          "[odbc]") {
	FakeDriverFixture fixture;
	// Each later step would refuse on its own: the region, the trust source and the load.
	auto config = redshift_test::UsEast1Config();
	config.region = "not a region";
	ScopedEnvironmentVariable trust("SSL_CERT_FILE", "/nonexistent/redshift-ca.pem");
	fixture.fake.FailLoad(DriverErrorKind::NO_USABLE_DIRECTORY, "no usable directory");
	ScopedEnvironmentVariable test_case(CASE_VARIABLE.c_str(), CANARY_CASE);

	const auto expected =
	    CONFIGURED_WITH_TEST_ENDPOINTS ? MissingRefusal(CASE_VARIABLE, URL_VARIABLE) : NoOverrideRefusal(CASE_VARIABLE);
	CHECK(Capture([&] { OpenRedshiftConnection(config, fixture.log_sink); }) == expected);
	CHECK(fixture.fake.SourceCalls() == 0);
	CHECK(fixture.fake.Calls().empty());
	CHECK(fixture.log_sink->Lines().empty());
}

TEST_CASE("A connect with both test endpoints set goes to them only in a build with the override", "[odbc]") {
	FakeDriverFixture fixture;
	ScopedEnvironmentVariable url(URL_VARIABLE.c_str(), "https://127.0.0.1:9");
	ScopedEnvironmentVariable host(HOST_VARIABLE.c_str(), "127.0.0.1");
	if (!CONFIGURED_WITH_TEST_ENDPOINTS) {
		CHECK(Capture([&] { fixture.OpenConnection(); }) == NoOverrideRefusal(URL_VARIABLE));
		CHECK(fixture.fake.SourceCalls() == 0);
		CHECK(fixture.fake.Calls().empty());
		return;
	}
	auto connection = fixture.OpenConnection();
	CHECK(connection->Target().rfind("127.0.0.1 (", 0) == 0);
	CHECK(fixture.fake.LastConnectionString() ==
	      BuildConnectionString(redshift_test::UsEast1Config(), TrustSource {fixture.ca_bundle.path},
	                            TestEndpoints {"127.0.0.1", "https://127.0.0.1:9"}));
}

TEST_CASE("A connect given test endpoints goes to them, whatever the build", "[odbc]") {
	FakeDriverFixture fixture;
	// Passed in, as a build with the override hands on the endpoints it reads, so any build runs it.
	const auto config = redshift_test::UsEast1Config();
	const TestEndpoints endpoints {"127.0.0.1", "https://127.0.0.1:9"};
	OdbcConnection connection(config, fixture.log_sink, endpoints);
	CHECK(connection.Target() == "127.0.0.1 (" + config.Describe() + ")");
	CHECK(fixture.fake.LastConnectionString() ==
	      WithTestEndpoints(BuildConnectionString(config, TrustSource {fixture.ca_bundle.path}), endpoints));
}

TEST_CASE("A fixture's connect ignores the test variables the caller's environment holds", "[odbc]") {
	ScopedEnvironmentVariable test_case(CASE_VARIABLE.c_str(), CANARY_CASE);
	ScopedEnvironmentVariable url(URL_VARIABLE.c_str(), CANARY_URL);
	ScopedEnvironmentVariable host(HOST_VARIABLE.c_str(), CANARY_HOST);
	FakeDriverFixture fixture;
	auto connection = fixture.OpenConnection();
	CHECK(connection->Target() == redshift_test::ServerlessTarget("us-east-1"));
}

TEST_CASE("An ATTACH with the test-case variable set fails with the refusal, before the driver source", "[odbc]") {
	FakeDriverFixture fixture;
	ExtensionDatabase database;
	ScopedEnvironmentVariable test_case(CASE_VARIABLE.c_str(), CANARY_CASE);

	// The ATTACH's connect is the one refused.
	auto result = database.con.Query(redshift_test::ATTACH_RS);
	REQUIRE(result->HasError());
	const auto expected =
	    CONFIGURED_WITH_TEST_ENDPOINTS ? MissingRefusal(CASE_VARIABLE, URL_VARIABLE) : NoOverrideRefusal(CASE_VARIABLE);
	CHECK(result->GetErrorType() == ExceptionType::INVALID_INPUT);
	CHECK(result->GetError().find(expected.message) != std::string::npos);
	CHECK(result->GetError().find(CANARY_CASE) == std::string::npos);
	CHECK(fixture.fake.SourceCalls() == 0);
}
