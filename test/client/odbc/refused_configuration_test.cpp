// A region, a profile or a target the client refuses never reaches the driver: the connect path
// throws InvalidInputException before it calls the driver source, so the fake records no call at
// all, no handle and no SQLDriverConnect. The values are the kinds the validators' own suites
// refuse: names from the other AWS partitions, a name with a host appended, profiles crafted to add
// a keyword, and targets that are missing, inconsistent, invalid or not reachable by this build.

// catch.hpp comes first, as in this directory's other tests.
#include "catch.hpp"

#include "../interface/scoped_environment_variable.hpp"
#include "fake_driver_fixture.hpp"
#include "settings_shape.hpp"
#include "thrown.hpp"

#include <string>
#include <vector>

using namespace duckdb;
using redshift_driver::testing::ScopedEnvironmentVariable;
using redshift_test::FakeDriverFixture;
using redshift_test::Thrown;

namespace {

const std::string REGION_MESSAGE =
    "Invalid value for parameter 'REGION': it is not an aws-partition region name. Use a lower-case region name from "
    "the aws partition, such as 'us-east-1'; the China, GovCloud, isolated and European Sovereign Cloud partitions "
    "are not supported, and a region newer than this extension's region list needs a newer extension";

//! The profile refusal for `reason`.
std::string ProfileMessage(const std::string &reason) {
	return "Invalid value for parameter 'PROFILE': it " + reason +
	       ". Use a profile name of at most 255 bytes, with no leading or trailing space and without NUL, an ASCII "
	       "control character or '}'";
}

//! What opening a connection with `config`, and `credentials` when given, threw, after checking that
//! the driver was never reached.
Thrown RefusalBeforeTheDriver(const RedshiftConfig &config,
                              optional_ptr<const RedshiftAwsCredentials> credentials = nullptr) {
	FakeDriverFixture fixture;
	const auto thrown = redshift_test::Capture([&] { OpenRedshiftConnection(config, credentials, fixture.log_sink); });
	CHECK(fixture.fake.SourceCalls() == 0);
	CHECK(fixture.fake.Calls().empty());
	CHECK(fixture.fake.AllocatedHandles().empty());
	CHECK(fixture.CallsTo("SQLDriverConnect") == 0);
	return thrown;
}

//! The refusal of a configuration with no target, or one outside the enum.
const Thrown NO_TARGET {ExceptionType::INVALID_INPUT,
                        "Redshift connection requires a target: a Serverless workgroup or a provisioned cluster"};

//! The refusal of a target for `reason`.
Thrown TargetRefusal(const std::string &reason) {
	return Thrown {ExceptionType::INVALID_INPUT, "Invalid Redshift target: " + reason};
}

//! The public build's refusal of every Serverless workgroup with no account.
const Thrown PUBLIC_ACCOUNTLESS_REFUSAL = TargetRefusal("a Serverless workgroup needs the account that owns it");

} // namespace

TEST_CASE("A region off the allowlist never reaches the driver", "[odbc]") {
	for (auto &region :
	     {"us-east-1.evil.example", "cn-north-1", "us-gov-west-1", "us-iso-east-1", "eu-isoe-west-1", "US-EAST-1"}) {
		INFO(region);
		const auto config = redshift_test::ServerlessConfig(region);
		CHECK(RefusalBeforeTheDriver(config) == Thrown {ExceptionType::INVALID_INPUT, REGION_MESSAGE});
	}
}

TEST_CASE("A profile crafted to add a keyword never reaches the driver", "[odbc]") {
	for (auto &profile : {"};SSLMode={disable", "x};Host={evil.example", "};EndPointUrl={https://evil.example",
	                      "};IAM={false", "a};Profile={other", "};LogLevel={6"}) {
		INFO(profile);
		auto config = redshift_test::UsEast1Config();
		config.profile = profile;
		CHECK(RefusalBeforeTheDriver(config) ==
		      Thrown {ExceptionType::INVALID_INPUT, ProfileMessage("holds '}', which ends a brace-quoted value")});
	}
}

TEST_CASE("A profile holding a control character or NUL never reaches the driver", "[odbc]") {
	auto config = redshift_test::UsEast1Config();
	config.profile = "prod\n;SSLMode=disable";
	CHECK(RefusalBeforeTheDriver(config) ==
	      Thrown {ExceptionType::INVALID_INPUT, ProfileMessage("holds an ASCII control character (0x0a)")});
	config.profile = std::string("prod\0;SSLMode=disable", 21);
	CHECK(RefusalBeforeTheDriver(config) ==
	      Thrown {ExceptionType::INVALID_INPUT,
	              ProfileMessage("holds a NUL byte, which truncates the connection string")});
}

TEST_CASE("A configuration with no target, or one outside the enum, never reaches the driver", "[odbc]") {
	auto config = redshift_test::UsEast1Config();
	config.target = RedshiftTarget::UNSPECIFIED;
	CHECK(RefusalBeforeTheDriver(config) == NO_TARGET);
	config.target = static_cast<RedshiftTarget>(3);
	CHECK(RefusalBeforeTheDriver(config) == NO_TARGET);
	// The region is checked before the target, so a refused region is the refusal reported.
	config.target = RedshiftTarget::UNSPECIFIED;
	config.region = "cn-north-1";
	CHECK(RefusalBeforeTheDriver(config) == Thrown {ExceptionType::INVALID_INPUT, REGION_MESSAGE});
}

TEST_CASE("A target whose fields do not fit it never reaches the driver", "[odbc]") {
	auto serverless = redshift_test::UsEast1Config();
	serverless.workgroup.clear();
	CHECK(RefusalBeforeTheDriver(serverless) == TargetRefusal("a Serverless target needs a workgroup"));
	serverless = redshift_test::UsEast1Config();
	serverless.cluster = "warehouse-1";
	CHECK(RefusalBeforeTheDriver(serverless) == TargetRefusal("a Serverless target takes no cluster"));

	RedshiftConfig provisioned;
	provisioned.region = "us-east-1";
	provisioned.target = RedshiftTarget::PROVISIONED;
	CHECK(RefusalBeforeTheDriver(provisioned) == TargetRefusal("a provisioned target needs a cluster"));
	provisioned.cluster = "warehouse-1";
	provisioned.workgroup = "analytics";
	CHECK(RefusalBeforeTheDriver(provisioned) == TargetRefusal("a provisioned target takes no workgroup"));
	provisioned.workgroup.clear();
	provisioned.account_id = "123456789012";
	CHECK(RefusalBeforeTheDriver(provisioned) == TargetRefusal("a provisioned target takes no account"));
}

TEST_CASE("An invalid cluster identifier or database never reaches the driver", "[odbc]") {
	auto config = redshift_test::ClusterConfig("us-east-1");
	config.cluster = "warehouse-1.evil.example";
	CHECK(
	    RefusalBeforeTheDriver(config) ==
	    Thrown {ExceptionType::INVALID_INPUT,
	            "Invalid value for parameter 'CLUSTER': use a Redshift cluster identifier of 1 to 63 characters, each "
	            "a lower-case letter, a digit or '-', starting with a letter, with no two '-' in a row and none at "
	            "the end"});
	config.cluster = "warehouse-1";
	config.database = "dev};SSLMode={disable";
	CHECK(RefusalBeforeTheDriver(config) ==
	      Thrown {ExceptionType::INVALID_INPUT,
	              "Invalid value for parameter 'DATABASE': it holds '}', which ends a brace-quoted value. Use a "
	              "database name of at most 127 bytes, with no leading or trailing space and without NUL, an ASCII "
	              "control character or '}'"});
}

TEST_CASE("An invalid workgroup, account ID or database never reaches the driver", "[odbc]") {
	auto config = redshift_test::UsEast1Config();
	config.workgroup = "x.evil.example";
	CHECK(
	    RefusalBeforeTheDriver(config) ==
	    Thrown {
	        ExceptionType::INVALID_INPUT,
	        "Invalid value for parameter 'WORKGROUP': use a Redshift Serverless workgroup name of 3 to 64 characters, "
	        "each a lower-case letter, a digit or '-'"});
	config = redshift_test::UsEast1Config();
	config.account_id = "1234567890123";
	CHECK(RefusalBeforeTheDriver(config) ==
	      Thrown {ExceptionType::INVALID_INPUT,
	              "Invalid value for parameter 'ACCOUNT_ID': use the 12-digit AWS account ID that owns the workgroup"});
	config = redshift_test::UsEast1Config();
	config.database = "};SSLMode={disable";
	CHECK(RefusalBeforeTheDriver(config) == Thrown {ExceptionType::INVALID_INPUT,
	                                                "Invalid value for parameter 'DATABASE': it holds '}', which ends "
	                                                "a brace-quoted value. Use a database name "
	                                                "of at most 127 bytes, with no leading or trailing space and "
	                                                "without NUL, an ASCII control character or '}'"});
}

TEST_CASE("A Serverless workgroup with no account that the target hook does not accept never reaches the driver",
          "[odbc]") {
	// Refused by either build's settings: the public build's refuse every one, and a preview
	// implementation's every one the target hook does not accept. The public words
	// come exactly when the configure expects no preview settings, so a wrong selection fails here.
	auto config = redshift_test::UsEast1Config();
	config.account_id.clear();
	const auto thrown = RefusalBeforeTheDriver(config);
	CHECK(thrown.type == ExceptionType::INVALID_INPUT);
	CHECK((thrown == PUBLIC_ACCOUNTLESS_REFUSAL) == !REDSHIFT_TEST_ACCOUNTLESS_PREVIEW);
}

TEST_CASE("Without the preview settings, no Serverless workgroup with no account reaches the driver",
          redshift_test::PUBLIC_SETTINGS_TAGS) {
	auto config = redshift_test::UsEast1Config();
	config.account_id.clear();
	CHECK(RefusalBeforeTheDriver(config) == PUBLIC_ACCOUNTLESS_REFUSAL);
	config.database = "sales";
	CHECK(RefusalBeforeTheDriver(config) == PUBLIC_ACCOUNTLESS_REFUSAL);
}

TEST_CASE("A refused region, profile or target is refused as such when the trust search would also fail", "[odbc]") {
	FakeDriverFixture fixture;
	// The trust search refuses an SSL_CERT_FILE that names no regular file.
	ScopedEnvironmentVariable unusable("SSL_CERT_FILE", "/dev/null");
	auto config = redshift_test::UsEast1Config();
	SECTION("a region") {
		config.region = "us-east-1.evil.example";
		CHECK(redshift_test::Capture([&] { OpenRedshiftConnection(config, fixture.log_sink); }) ==
		      Thrown {ExceptionType::INVALID_INPUT, REGION_MESSAGE});
	}
	SECTION("a profile") {
		config.profile = "};SSLMode={disable";
		CHECK(redshift_test::Capture([&] { OpenRedshiftConnection(config, fixture.log_sink); }) ==
		      Thrown {ExceptionType::INVALID_INPUT, ProfileMessage("holds '}', which ends a brace-quoted value")});
	}
	SECTION("a target") {
		config.target = RedshiftTarget::UNSPECIFIED;
		CHECK(redshift_test::Capture([&] { OpenRedshiftConnection(config, fixture.log_sink); }) == NO_TARGET);
	}
	CHECK(fixture.fake.SourceCalls() == 0);
}

TEST_CASE("A connect refuses the region, then the profile, then the target, so the first refused is reported",
          "[odbc]") {
	auto config = redshift_test::UsEast1Config();
	config.region = "us-east-1.evil.example";
	config.profile = "};SSLMode={disable";
	config.target = RedshiftTarget::UNSPECIFIED;
	CHECK(RefusalBeforeTheDriver(config) == Thrown {ExceptionType::INVALID_INPUT, REGION_MESSAGE});
	config.region = "us-east-1";
	CHECK(RefusalBeforeTheDriver(config) ==
	      Thrown {ExceptionType::INVALID_INPUT, ProfileMessage("holds '}', which ends a brace-quoted value")});
	config.profile = "";
	CHECK(RefusalBeforeTheDriver(config) == NO_TARGET);
}

TEST_CASE("AWS credentials, an AWS secret name or a port the client refuses never reach the driver", "[odbc]") {
	auto config = redshift_test::UsEast1Config();
	config.aws_secret = "creds";
	const std::string key_id = "AKIDEXAMPLEKEYID0001";
	const std::string secret_key = "secret-key";
	const RedshiftAwsCredentials refused[] = {
	    {"a}b", secret_key, ""},
	    {"", secret_key, ""},
	    {key_id, "", ""},
	    {key_id, std::string(256, 's'), ""},
	    {std::string(256, 'A'), secret_key, ""},
	    {key_id, secret_key, std::string(2048, 't')},
	    {key_id, secret_key, "a\nb"},
	};
	for (auto &credentials : refused) {
		INFO(credentials.access_key_id.substr(0, 8)
		     << "/" << credentials.secret_access_key.size() << "/" << credentials.session_token.size());
		CHECK(RefusalBeforeTheDriver(config, &credentials).type == ExceptionType::INVALID_INPUT);
	}
	const RedshiftAwsCredentials credentials {key_id, secret_key, ""};
	// An unusable AWS secret name, a profile beside it, and credentials without it, or it without them.
	auto bad_name = config;
	bad_name.aws_secret = std::string(256, 'n');
	CHECK(RefusalBeforeTheDriver(bad_name, &credentials).type == ExceptionType::INVALID_INPUT);
	auto with_profile = config;
	with_profile.profile = "p";
	CHECK(RefusalBeforeTheDriver(with_profile, &credentials).type == ExceptionType::INVALID_INPUT);
	CHECK(RefusalBeforeTheDriver(redshift_test::UsEast1Config(), &credentials).type == ExceptionType::INVALID_INPUT);
	CHECK(RefusalBeforeTheDriver(config).type == ExceptionType::INVALID_INPUT);
	// A port for a cluster named alone, with no endpoint label.
	auto cluster = redshift_test::ClusterConfig("us-east-1");
	cluster.endpoint_port = 5439;
	CHECK(RefusalBeforeTheDriver(cluster) == TargetRefusal("a cluster endpoint's label and port go together"));
}
