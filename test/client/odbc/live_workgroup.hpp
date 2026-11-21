//===----------------------------------------------------------------------===//
//                         DuckDB - Redshift
//
// test/client/odbc/live_workgroup.hpp
//
// What a [live] case in this directory needs to reach the workgroup the live variables name
// (test/README.md, "Live variables"). Include catch.hpp before this header.
//===----------------------------------------------------------------------===//

#pragma once

#include "driver_source_hook.hpp"
#include "extension_database.hpp"
#include "fake_driver_fixture.hpp"
#include "odbc/connection_settings.hpp"
#include "odbc_driver.hpp"

#include <cstdlib>
#include <string>
#include <vector>

namespace redshift_test {

//! The variables that name the live workgroup.
constexpr const char *LIVE_REGION_VARIABLE = "REDSHIFT_TEST_LIVE_REGION";
constexpr const char *LIVE_WORKGROUP_VARIABLE = "REDSHIFT_TEST_LIVE_WORKGROUP";
constexpr const char *LIVE_ACCOUNT_ID_VARIABLE = "REDSHIFT_TEST_LIVE_ACCOUNT_ID";
inline const std::vector<std::string> LIVE_WORKGROUP_VARIABLES {LIVE_REGION_VARIABLE, LIVE_WORKGROUP_VARIABLE,
                                                                LIVE_ACCOUNT_ID_VARIABLE};
//! The variables that name the live provisioned cluster.
constexpr const char *LIVE_CLUSTER_VARIABLE = "REDSHIFT_TEST_LIVE_CLUSTER";
inline const std::vector<std::string> LIVE_CLUSTER_VARIABLES {LIVE_REGION_VARIABLE, LIVE_CLUSTER_VARIABLE};

//! The variables in `names` that are unset, separated by spaces.
inline std::string UnsetVariables(const std::vector<std::string> &names) {
	std::string unset;
	for (const auto &name : names) {
		if (std::getenv(name.c_str()) == nullptr) {
			unset += (unset.empty() ? "" : " ") + name;
		}
	}
	return unset;
}

//! Why a live case cannot run here, or empty when it can: a live variable is unset, or `load`, the
//! embedded driver's load, shows a build with none. Any other load failure is the case's to report.
inline std::string LiveSkipReason(const duckdb::redshift_driver::DriverLoadResult &load,
                                  const std::vector<std::string> &variables = LIVE_WORKGROUP_VARIABLES) {
	const auto unset = UnsetVariables(variables);
	if (!unset.empty()) {
		return "skipped: unset: " + unset + " (test/README.md, \"Live variables\")";
	}
	if (!load.ok() && load.error.kind == duckdb::redshift_driver::DriverErrorKind::NOT_BUILT) {
		return "skipped: this build has no embedded driver";
	}
	return std::string();
}

//! LiveSkipReason, calling `load` only once every live variable is set: loading the embedded driver
//! installs its private copy, which a run without the live variables has no use for.
template <class LOAD>
std::string LiveSkipReasonLoadingLast(LOAD &&load,
                                      const std::vector<std::string> &variables = LIVE_WORKGROUP_VARIABLES) {
	if (!UnsetVariables(variables).empty()) {
		return LiveSkipReason(duckdb::redshift_driver::DriverLoadResult(), variables);
	}
	return LiveSkipReason(load(), variables);
}

// Ends a live case as skipped when LiveSkipReasonLoadingLast gives `load` a reason. A macro, because
// only a return in the case's own body ends the case. live_workgroup_test.cpp checks
// LiveSkipReasonLoadingLast itself: a case through the macro would be skipped, so the skip and the
// return are checked only by reading them.
#define SKIP_UNLESS_LIVE_ON(variables, load)                                                                           \
	do {                                                                                                               \
		const std::string live_skip_reason =                                                                           \
		    redshift_test::LiveSkipReasonLoadingLast([&] { return (load); }, (variables));                             \
		if (!live_skip_reason.empty()) {                                                                               \
			SKIP_TEST(live_skip_reason);                                                                               \
			return;                                                                                                    \
		}                                                                                                              \
	} while (false)
// SKIP_UNLESS_LIVE_ON for the live workgroup's variables.
#define SKIP_UNLESS_LIVE(load) SKIP_UNLESS_LIVE_ON(redshift_test::LIVE_WORKGROUP_VARIABLES, load)

//! The live workgroup's config. Call it only once LiveSkipReason is empty.
inline duckdb::RedshiftConfig LiveWorkgroupConfig() {
	duckdb::RedshiftConfig config;
	config.region = std::getenv(LIVE_REGION_VARIABLE);
	config.target = duckdb::RedshiftTarget::SERVERLESS;
	config.workgroup = std::getenv(LIVE_WORKGROUP_VARIABLE);
	config.account_id = std::getenv(LIVE_ACCOUNT_ID_VARIABLE);
	return config;
}

//! The ATTACH of `rs` to the live workgroup. Call it only once LiveSkipReason is empty.
inline std::string LiveWorkgroupAttach() {
	return AttachRs(std::getenv(LIVE_REGION_VARIABLE), std::getenv(LIVE_WORKGROUP_VARIABLE),
	                std::getenv(LIVE_ACCOUNT_ID_VARIABLE));
}

//! Whether a connect goes through the test endpoints: a test-endpoint build with both endpoint
//! variables set. Otherwise a connect is refused while one is set (ReadTestEndpoints).
inline bool ThroughTestEndpoints() {
	return duckdb::BuildHasTestEndpoints() && std::getenv(duckdb::TEST_HOST_VARIABLE) &&
	       std::getenv(duckdb::TEST_URL_VARIABLE);
}

//! While it lives, the end-to-end test variables are unset unless ThroughTestEndpoints, so a live
//! connect goes either through the test endpoints or where the target derives.
class LiveEndpoints {
public:
	LiveEndpoints() {
		if (!ThroughTestEndpoints()) {
			unset = duckdb::make_uniq<NoTestVariables>();
		}
	}

private:
	duckdb::unique_ptr<NoTestVariables> unset;
};

//! Resets the process state when the case ends, so the real driver's environment is freed.
struct ResetOnExit {
	~ResetOnExit() {
		duckdb::ResetProcessStateForTesting();
	}
};

} // namespace redshift_test
