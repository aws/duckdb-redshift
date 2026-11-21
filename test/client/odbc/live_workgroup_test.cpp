// The live helpers' checks that statement_error_live_test.cpp does not make: when a live case loads the
// driver, the attach it builds and the reset when it ends.

// catch.hpp comes first, as in this directory's other tests.
#include "catch.hpp"

#include "live_workgroup.hpp"

using namespace duckdb;

namespace {

//! The live variables set, to placeholder values, while it lives.
struct LiveVariablesSet {
	redshift_driver::testing::ScopedEnvironmentVariable region {redshift_test::LIVE_REGION_VARIABLE, "us-east-1"};
	redshift_driver::testing::ScopedEnvironmentVariable workgroup {redshift_test::LIVE_WORKGROUP_VARIABLE, "wg"};
	redshift_driver::testing::ScopedEnvironmentVariable account {redshift_test::LIVE_ACCOUNT_ID_VARIABLE, "1"};
};

} // namespace

TEST_CASE("A live case loads the driver only once every live variable is set", "[odbc]") {
	LiveVariablesSet variables;
	int loads = 0;
	const auto load = [&] {
		++loads;
		redshift_driver::DriverLoadResult result;
		result.error.kind = redshift_driver::DriverErrorKind::NOT_BUILT;
		return result;
	};
	{
		redshift_driver::testing::ScopedEnvironmentVariable workgroup {redshift_test::LIVE_WORKGROUP_VARIABLE, nullptr};
		CHECK(redshift_test::LiveSkipReasonLoadingLast(load) ==
		      "skipped: unset: REDSHIFT_TEST_LIVE_WORKGROUP (test/README.md, \"Live variables\")");
		CHECK(loads == 0);
	}

	CHECK(redshift_test::LiveSkipReasonLoadingLast(load) == "skipped: this build has no embedded driver");
	CHECK(loads == 1);

	// A case on the cluster reads its own variables: the workgroup's, all set, do not let it load.
	{
		redshift_driver::testing::ScopedEnvironmentVariable cluster {redshift_test::LIVE_CLUSTER_VARIABLE, nullptr};
		CHECK(redshift_test::LiveSkipReasonLoadingLast(load, redshift_test::LIVE_CLUSTER_VARIABLES) ==
		      "skipped: unset: REDSHIFT_TEST_LIVE_CLUSTER (test/README.md, \"Live variables\")");
		CHECK(loads == 1);
	}
	redshift_driver::testing::ScopedEnvironmentVariable cluster {redshift_test::LIVE_CLUSTER_VARIABLE, "cluster"};
	CHECK(redshift_test::LiveSkipReasonLoadingLast(load, redshift_test::LIVE_CLUSTER_VARIABLES) ==
	      "skipped: this build has no embedded driver");
	CHECK(loads == 2);
}

TEST_CASE("The live attach puts each live variable in its own option", "[odbc]") {
	LiveVariablesSet variables;
	redshift_driver::testing::ScopedEnvironmentVariable region {redshift_test::LIVE_REGION_VARIABLE, "eu-west-2"};

	CHECK(redshift_test::LiveWorkgroupAttach() == redshift_test::AttachRs("eu-west-2", "wg", "1"));
}

TEST_CASE("The live workgroup's config puts each live variable in its own field", "[odbc]") {
	LiveVariablesSet variables;
	redshift_driver::testing::ScopedEnvironmentVariable region {redshift_test::LIVE_REGION_VARIABLE, "eu-west-2"};

	const auto config = redshift_test::LiveWorkgroupConfig();
	CHECK(config.region == "eu-west-2");
	CHECK(config.target == RedshiftTarget::SERVERLESS);
	CHECK(config.workgroup == "wg");
	CHECK(config.account_id == "1");
}

TEST_CASE("A live case's process state is reset when it ends, freeing the driver's environment", "[odbc]") {
	redshift_test::FakeDriverFixture fixture;
	{
		redshift_test::ResetOnExit reset;
		{ auto connection = fixture.OpenConnection(); }
		CHECK(fixture.fake.AllocatedHandles() == "environment");
	}

	CHECK(fixture.fake.AllocatedHandles().empty());
}
