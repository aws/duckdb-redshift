//===----------------------------------------------------------------------===//
// LoadEmbeddedDriver for an ODBC build that carries no driver: REDSHIFT_DRIVER=none, or a
// platform without a loader. The build still links, so the offline tests run with no driver
// download, and no published release carries it.
//===----------------------------------------------------------------------===//

#include "odbc_driver.hpp"

#define REDSHIFT_QUOTE_IMPL(x) #x
#define REDSHIFT_QUOTE(x)      REDSHIFT_QUOTE_IMPL(x)

namespace duckdb {
namespace redshift_driver {

namespace {

//! DuckDB's name for the platform this build targets. DuckDB's distribution builds name it;
//! otherwise it is derived as DuckDBPlatform() derives it for glibc Linux, macOS and MSVC Windows
//! on amd64 and arm64, and other hosts set DUCKDB_PLATFORM. Not DuckDB's own function: the driver
//! directory includes no DuckDB header, and that function is defined in its header, so a second
//! copy would not link.
std::string TargetPlatform() {
#ifdef DUCKDB_CUSTOM_PLATFORM
	return REDSHIFT_QUOTE(DUCKDB_CUSTOM_PLATFORM);
#else
#if defined(_WIN32)
	std::string os = "windows";
#elif defined(__APPLE__)
	std::string os = "osx";
#else
	std::string os = "linux";
#endif
#if defined(__aarch64__) || defined(__ARM_ARCH_ISA_A64)
	return os + "_arm64";
#else
	return os + "_amd64";
#endif
#endif
}

DriverLoadResult NotBuilt() {
	DriverLoadResult result;
	result.error.kind = DriverErrorKind::NOT_BUILT;
	result.error.message = "this build of the extension carries no Redshift ODBC driver for " + TargetPlatform() +
	                       " (it was built with REDSHIFT_DRIVER=none, or for a platform without a driver loader); "
	                       "install a published release of the extension on a platform it supports";
	return result;
}

} // namespace

DriverLoadResult LoadEmbeddedDriver() noexcept {
	// Nothing here can change while the process runs, so every call answers from one result.
	static const DriverLoadResult not_built = NotBuilt();
	return not_built;
}

// This build loads no driver of its own: what reaches this is a test's driver (the fake, which reads no
// odbc.ini, or the test-only by-path source's, a developer's own choice), so the allocation is made as it is.
SQLRETURN AllocateEnvironmentHandle(const LoadedDriver &driver, SQLHENV *environment) noexcept {
	return driver.api.AllocHandle(SQL_HANDLE_ENV, SQL_NULL_HANDLE, environment);
}

} // namespace redshift_driver
} // namespace duckdb
