#include "odbc_driver.hpp"

#include "duckdb/main/database.hpp"

#include "catch.hpp"

using namespace duckdb::redshift_driver;

TEST_CASE("Without a driver, LoadEmbeddedDriver fails with NOT_BUILT", "[interface]") {
	const auto result = LoadEmbeddedDriver();
	CHECK_FALSE(result.ok());
	CHECK(result.error.kind == DriverErrorKind::NOT_BUILT);
	CHECK(result.error.attempts.empty());
}

TEST_CASE("The NOT_BUILT message names DuckDB's platform", "[interface]") {
	// The placeholder derives the name itself; DuckDB's own answer catches a derivation that drifts.
	CHECK_THAT(LoadEmbeddedDriver().error.message, Catch::Contains(" " + duckdb::DuckDB::Platform() + " "));
}

#if defined(_WIN64) && defined(_MSC_VER)
TEST_CASE("The NOT_BUILT message of an MSVC x64 build names windows_amd64", "[interface]") {
	// An MSVC x64 build's refusal names windows_amd64, the platform DuckDB names for it.
	CHECK_THAT(LoadEmbeddedDriver().error.message, Catch::Contains(" windows_amd64 "));
}
#endif

TEST_CASE("LoadEmbeddedDriver gives the same NOT_BUILT answer on every call", "[interface]") {
	const auto first = LoadEmbeddedDriver();
	const auto second = LoadEmbeddedDriver();
	CHECK(second.driver == nullptr);
	CHECK(second.error.kind == first.error.kind);
	CHECK(second.error.message == first.error.message);
}

namespace {
SQLSMALLINT g_type = 0;
SQLHANDLE g_input = &g_type;
SQLRETURN g_result = SQL_SUCCESS;
SQLRETURN RecordingAllocHandle(SQLSMALLINT type, SQLHANDLE input, SQLHANDLE *output) {
	g_type = type;
	g_input = input;
	*output = &g_type;
	return g_result;
}
} // namespace

TEST_CASE("Without a driver, AllocateEnvironmentHandle is the given driver's own allocation", "[interface]") {
	// This build loads no driver, so only a test fake reaches it: the environment allocation, its result as it is.
	LoadedDriver driver;
	driver.api.AllocHandle = RecordingAllocHandle;
	for (const SQLRETURN result : {SQL_SUCCESS, SQL_SUCCESS_WITH_INFO, SQL_ERROR}) {
		g_result = result;
		SQLHENV environment = nullptr;
		CHECK(AllocateEnvironmentHandle(driver, &environment) == result);
		CHECK(g_type == SQL_HANDLE_ENV);
		CHECK(g_input == SQL_NULL_HANDLE);
		CHECK(environment == static_cast<SQLHENV>(&g_type));
	}
}
