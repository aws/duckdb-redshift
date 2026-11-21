// The control-plane endpoint and Region, live in a second region: each target's control
// plane there answers a request signed for that region, and a workgroup only the live region holds is
// not found there. Nothing is created: the other targets do not exist, and only that workgroup may open.

// catch.hpp comes first, as in this directory's other tests.
#include "catch.hpp"

#include "live_workgroup.hpp"
#include "thrown.hpp"

#include <cstdlib>
#include <string>
#include <vector>

using namespace duckdb;

namespace {

//! The region the case reaches besides the live workgroup's.
constexpr const char *SECOND_REGION_VARIABLE = "REDSHIFT_TEST_LIVE_SECOND_REGION";

//! A name no workgroup or cluster of the live account has.
constexpr const char *ABSENT_TARGET = "duckdb-absent-target";

//! An account-backed workgroup, a cluster named alone and a cluster endpoint, each absent, in `region`.
std::vector<RedshiftConfig> AbsentTargets(const std::string &region) {
	RedshiftConfig workgroup;
	workgroup.region = region;
	workgroup.target = RedshiftTarget::SERVERLESS;
	workgroup.workgroup = ABSENT_TARGET;
	workgroup.account_id = std::getenv(redshift_test::LIVE_ACCOUNT_ID_VARIABLE);
	RedshiftConfig cluster;
	cluster.region = region;
	cluster.target = RedshiftTarget::PROVISIONED;
	cluster.cluster = ABSENT_TARGET;
	auto cluster_endpoint = cluster;
	cluster_endpoint.endpoint_label = "abc123def456";
	cluster_endpoint.endpoint_port = 5439;
	return {workgroup, cluster, cluster_endpoint};
}

//! Whether `error` is the connect failure for a target the control plane did not find, or for an
//! identity it refused: both need a request it authenticated, signed for its region.
bool ControlPlaneAnswered(const ErrorData &error, const std::string &noun) {
	const auto has = [&](const std::string &text) {
		return error.RawMessage().find(text) != std::string::npos;
	};
	return error.HasError() && error.Type() == ExceptionType::IO &&
	       (has(": the Redshift " + noun + " was not found.") ||
	        has(": the AWS identity is not allowed to connect to the " + noun + "."));
}

//! Names a workgroup of the live account that the live region holds and the second region does not.
constexpr const char *REGION_ONLY_WORKGROUP_VARIABLE = "REDSHIFT_TEST_LIVE_REGION_ONLY_WORKGROUP";

//! The workgroup REGION_ONLY_WORKGROUP_VARIABLE names, in `region`.
RedshiftConfig RegionOnlyWorkgroupIn(const std::string &region) {
	RedshiftConfig workgroup;
	workgroup.region = region;
	workgroup.target = RedshiftTarget::SERVERLESS;
	workgroup.workgroup = std::getenv(REGION_ONLY_WORKGROUP_VARIABLE);
	workgroup.account_id = std::getenv(redshift_test::LIVE_ACCOUNT_ID_VARIABLE);
	return workgroup;
}

//! Whether opening reached past the control plane, which needs the credentials it gave: the
//! connection opened (no error, which HasError decides), or the workgroup's endpoint refused the
//! login or did not answer.
bool PastControlPlane(const ErrorData &error) {
	const auto has = [&](const std::string &text) {
		return error.RawMessage().find(text) != std::string::npos;
	};
	return !error.HasError() ||
	       (error.Type() == ExceptionType::IO && (has(" did not answer") || has(" refused the login")));
}

//! What opening `config` on the embedded driver threw, with no error when it opened.
ErrorData OpenError(const RedshiftConfig &config) {
	return redshift_test::CaptureError(
	    [&] { OpenRedshiftConnection(config, make_shared_ptr<redshift_test::CapturingLogSink>()); });
}

//! What an open that ended in `error` shows: its message, or "opened".
std::string Outcome(const ErrorData &error) {
	return error.HasError() ? error.Message() : "opened";
}

//! Why a case that also needs `variables` cannot run, or empty when they are set.
std::string UnsetReason(const std::vector<std::string> &variables) {
	const auto unset = redshift_test::UnsetVariables(variables);
	return unset.empty() ? unset : "skipped: unset: " + unset + " (test/README.md, \"Live variables\")";
}

//! The region SECOND_REGION_VARIABLE names, which must differ from the live workgroup's. Call it
//! once both are set.
std::string RequireSecondRegion() {
	const std::string region = std::getenv(SECOND_REGION_VARIABLE);
	INFO(SECOND_REGION_VARIABLE << " must name a region other than the live workgroup's");
	REQUIRE((!region.empty() && region != std::getenv(redshift_test::LIVE_REGION_VARIABLE)));
	return region;
}

} // namespace

TEST_CASE("Live: a second region's control plane answers each target, signed for that region", "[odbc][live]") {
	// Checked before the driver loads, which installs its private copy.
	const auto skip = UnsetReason({SECOND_REGION_VARIABLE});
	if (!skip.empty()) {
		SKIP_TEST(skip);
		return;
	}
	SKIP_UNLESS_LIVE(redshift_driver::LoadEmbeddedDriver());
	RequireSecondRegion();
	redshift_test::ResetOnExit reset;
	SetDriverSourceForTesting(&redshift_driver::LoadEmbeddedDriver);
	// The derived endpoints, never the test ones, since those are what this checks.
	redshift_test::NoTestVariables derived;
	// The live workgroup's region first, as the control: the forms must hold in both.
	for (const auto *variable : {redshift_test::LIVE_REGION_VARIABLE, SECOND_REGION_VARIABLE}) {
		for (const auto &config : AbsentTargets(std::getenv(variable))) {
			const auto noun = config.target == RedshiftTarget::SERVERLESS ? "workgroup" : "cluster";
			const auto error = OpenError(config);
			INFO(config.Describe() << ": " << Outcome(error));
			CHECK(ControlPlaneAnswered(error, noun));
		}
	}
}

TEST_CASE("Live: a workgroup only the live region holds is found there and not in the second region", "[odbc][live]") {
	// Checked before the driver loads, which installs its private copy.
	const auto skip = UnsetReason({SECOND_REGION_VARIABLE, REGION_ONLY_WORKGROUP_VARIABLE});
	if (!skip.empty()) {
		SKIP_TEST(skip);
		return;
	}
	SKIP_UNLESS_LIVE(redshift_driver::LoadEmbeddedDriver());
	const auto second_region = RequireSecondRegion();
	redshift_test::ResetOnExit reset;
	SetDriverSourceForTesting(&redshift_driver::LoadEmbeddedDriver);
	redshift_test::NoTestVariables derived;
	// Found where it lives, so the not-found below comes from another region's control plane: a
	// driver that sent both requests to one region would find it twice.
	const auto found = OpenError(RegionOnlyWorkgroupIn(std::getenv(redshift_test::LIVE_REGION_VARIABLE)));
	INFO("in the live region: " << Outcome(found));
	REQUIRE(PastControlPlane(found));
	const auto absent = OpenError(RegionOnlyWorkgroupIn(second_region));
	INFO("in the second region: " << Outcome(absent));
	REQUIRE(absent.HasError());
	CHECK(absent.Type() == ExceptionType::IO);
	CHECK_THAT(absent.RawMessage(), Catch::Contains(": the Redshift workgroup was not found."));
}
