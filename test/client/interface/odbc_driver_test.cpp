#include "odbc_driver.hpp"

#include "catch.hpp"

#include <set>
#include <string>
#include <utility>
#include <vector>

using namespace duckdb::redshift_driver;

namespace {

//! Never called: Complete() and FirstMissing() only look at whether a member is set.
template <class RESULT, class... ARGS>
RESULT NeverCalled(ARGS...) {
	return RESULT();
}

OdbcApi CompleteApi() {
	OdbcApi api;
#define SET_ENTRY_POINT(name) api.name = &NeverCalled;
	REDSHIFT_ODBC_ENTRY_POINTS(SET_ENTRY_POINT)
#undef SET_ENTRY_POINT
	return api;
}

//! FirstMissing() as a string, so a check that fails prints the name instead of a pointer.
std::string FirstMissingName(const OdbcApi &api) {
	const char *name = api.FirstMissing();
	return name == nullptr ? "(none)" : name;
}

DriverIdentity Identity(DriverOrigin origin, std::string version, bool development) {
	DriverIdentity identity;
	identity.origin = origin;
	identity.version = std::move(version);
	identity.platform = "linux_amd64";
	identity.development = development;
	return identity;
}

} // namespace

TEST_CASE("OdbcApi with every entry point set is complete", "[interface]") {
	const auto api = CompleteApi();
	CHECK(api.Complete());
	CHECK(api.FirstMissing() == nullptr);
}

//! Clears one member of a complete table, which must then be incomplete and name that member.
#define CHECK_CLEARED(name)                                                                                            \
	{                                                                                                                  \
		auto api = CompleteApi();                                                                                      \
		api.name = nullptr;                                                                                            \
		CHECK_FALSE(api.Complete());                                                                                   \
		CHECK(FirstMissingName(api) == "SQL" #name);                                                                   \
	}

TEST_CASE("OdbcApi with one entry point cleared names it as missing", "[interface]") {
	REDSHIFT_ODBC_ENTRY_POINTS(CHECK_CLEARED)
}
#undef CHECK_CLEARED

TEST_CASE("An empty OdbcApi names the first entry point in list order", "[interface]") {
	const OdbcApi api;
	CHECK_FALSE(api.Complete());
	CHECK(FirstMissingName(api) == "SQLAllocHandle");
}

TEST_CASE("DriverIdentity describes an embedded driver", "[interface]") {
	CHECK(Identity(DriverOrigin::EMBEDDED, "2.3.0.0", false).Describe() == "embedded 2.3.0.0 linux_amd64");
}

TEST_CASE("DriverIdentity marks a development lock", "[interface]") {
	// The flag is the lock's status, not its version: a development lock can pin the same release
	// as a release lock (above), and only the flag tells them apart.
	CHECK(Identity(DriverOrigin::EMBEDDED, "2.3.0.0", true).Describe() == "embedded 2.3.0.0 linux_amd64 (development)");
}

TEST_CASE("DriverIdentity describes the test fake", "[interface]") {
	CHECK(Identity(DriverOrigin::TEST_FAKE, "fake", false).Describe() == "test-fake fake linux_amd64");
}

TEST_CASE("DriverIdentity describes a by-path test driver", "[interface]") {
	CHECK(Identity(DriverOrigin::TEST_BY_PATH, "sha256:0123456789ab", false).Describe() ==
	      "test-by-path sha256:0123456789ab linux_amd64");
}

TEST_CASE("DriverIdentity describes an out-of-range origin as unknown", "[interface]") {
	// Pins the non-null fallback. GCC Release builds such as make test_client's do not flag an
	// origin left out of Describe(); make tidy_client, a Debug build's -Wall and Clang's default
	// do. When a new origin takes this value, move this value past it.
	CHECK(Identity(static_cast<DriverOrigin>(3), "2.3.0.0", false).Describe() == "unknown-origin 2.3.0.0 linux_amd64");
}

TEST_CASE("A LoadedDriver names no trust directory until a Windows loader fills it", "[interface]") {
	CHECK(LoadedDriver().trust_directory.empty());
}

TEST_CASE("Every DriverErrorKind has a distinct value", "[interface]") {
	const std::vector<DriverErrorKind> kinds {
	    DriverErrorKind::NOT_BUILT,           DriverErrorKind::EMBEDDED_BYTES_CORRUPT,
	    DriverErrorKind::NO_USABLE_DIRECTORY, DriverErrorKind::MISSING_PREREQUISITE,
	    DriverErrorKind::LOAD_FAILED,         DriverErrorKind::MISSING_ENTRY_POINT,
	    DriverErrorKind::REFUSED_ENVIRONMENT,
	};
	std::set<uint8_t> values;
	for (const auto kind : kinds) {
		values.insert(static_cast<uint8_t>(kind));
	}
	CHECK(values.size() == kinds.size());
}
