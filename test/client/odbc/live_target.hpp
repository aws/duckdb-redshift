//===----------------------------------------------------------------------===//
//                         DuckDB - Redshift
//
// test/client/odbc/live_target.hpp
//
// When a case that needs the embedded driver, or the live workgroup as well, can run: the build has
// the driver, and the live variables (test/README.md, "Live variables") are set.
//===----------------------------------------------------------------------===//

#pragma once

#include "catch.hpp"
#include "odbc_driver.hpp"

#include <cstdlib>
#include <string>
#include <vector>

namespace redshift_test {

//! The live variables that name the workgroup the live cases reach.
const std::vector<std::string> LIVE_VARIABLES {"REDSHIFT_TEST_LIVE_REGION", "REDSHIFT_TEST_LIVE_WORKGROUP",
                                               "REDSHIFT_TEST_LIVE_ACCOUNT_ID"};

//! Why a case that needs the embedded driver cannot run here, or empty when it can: the build links the
//! placeholder, which answers NOT_BUILT, rather than a driver.
inline std::string DriverSkipReason() {
	const auto load = duckdb::redshift_driver::LoadEmbeddedDriver();
	if (!load.ok() && load.error.kind == duckdb::redshift_driver::DriverErrorKind::NOT_BUILT) {
		return "skipped: this build has no embedded driver";
	}
	return std::string();
}

//! Why a live case cannot run here, or empty when it can: a live variable is unset, or DriverSkipReason.
inline std::string LiveSkipReason() {
	std::string unset;
	for (const auto &name : LIVE_VARIABLES) {
		if (std::getenv(name.c_str()) == nullptr) {
			unset += (unset.empty() ? "" : " ") + name;
		}
	}
	if (!unset.empty()) {
		return "skipped: unset: " + unset + " (test/README.md, \"Live variables\")";
	}
	return DriverSkipReason();
}

//! Whether the case skips, saying why, for a `reason` that is not empty.
inline bool SkippedFor(const std::string &reason) {
	if (reason.empty()) {
		return false;
	}
	SKIP_TEST(reason);
	return true;
}

//! SkippedFor, for a case that needs the embedded driver, or a build feature as well, such as the test
//! endpoints. Catch counts a skip as a pass, so with REDSHIFT_REQUIRE_DRIVER=1, which make
//! test_driver_e2e sets, any such skip fails instead.
inline bool DriverCaseSkippedFor(const std::string &reason) {
	const char *required = std::getenv("REDSHIFT_REQUIRE_DRIVER");
	if (!reason.empty() && required != nullptr && std::string(required) == "1") {
		FAIL("REDSHIFT_REQUIRE_DRIVER=1 requires the case to run, which would have " + reason);
	}
	return SkippedFor(reason);
}

} // namespace redshift_test
