//===----------------------------------------------------------------------===//
//                         DuckDB - Redshift
//
// src/transport/driver/driver_loader_windows.hpp
//
// The Windows run-time sequence behind LoadEmbeddedDriver: the
// environment block, the registry trace values, the VC++ runtime, the DbgHelp.dll check, the set's
// files held while they load, the post-load module check and the entry points. The interface header
// comes first.
//===----------------------------------------------------------------------===//

#pragma once

#include "odbc_driver.hpp"
#include "driver_windows.hpp"

#include <functional>
#include <mutex>
#include <string>
#include <vector>

namespace duckdb {
namespace redshift_driver {
namespace windows_driver {

// A VC++ runtime DLL the set imports, and the oldest file version it takes.
struct Prerequisite {
	std::string name;
	std::string minimum; // "a.b.c.d", from the lock
};

// What a load uses besides the process's own inputs; the embedded build fills it from its resources.
struct LoaderConfig {
	std::string set_sha256;
	std::vector<SetFile> files; // the driver, then its libraries in the lock's order, then the notices
	DriverIdentity identity;
	// Every refused name, whatever its phase: a set DLL's C runtime copies the block when it loads.
	std::vector<std::string> refused_environment;
	std::vector<OverriddenVariable> overridden_environment;
	std::vector<Prerequisite> prerequisites;
	// runtime-names.txt lists DbgHelp.dll, which the driver loads by bare name: refuse a copy from
	// outside System32, load System32's first, and require it alone afterwards.
	bool system_dbghelp = false;
};

struct LoadInputs {
	BaseInputs bases;
	std::string user; // CurrentUserSid()
	// Tests only: called with the set's paths, held open, just before the first LoadLibraryExW.
	void (*before_platform_loader)(const std::vector<std::string> &paths) = nullptr;
};
LoadInputs CurrentLoadInputs();

// True when the installed driver's LogLevel and Trace values count as off; false
// with REFUSED_ENVIRONMENT in `error` when either counts as on or the key cannot be read.
bool CheckRegistryTrace(DriverError &error);
// True unless a DbgHelp.dll is loaded from outside System32 or the
// process's modules cannot be listed; false with REFUSED_ENVIRONMENT in `error` then.
bool CheckDbgHelp(DriverError &error);
// True when each runtime the process would use is at its minimum; false with the
// final MISSING_PREREQUISITE in `error` otherwise.
bool CheckPrerequisites(const std::vector<Prerequisite> &prerequisites, DriverError &error);

// The sequence, serialised by a mutex. Every outcome from the first LoadLibraryExW on, a success's
// too, and MISSING_PREREQUISITE are stored and returned by every later call;
// any other failure is returned, and the next call runs again with the inputs it is given.
class DriverLoader {
public:
	explicit DriverLoader(LoaderConfig config);
	DriverLoadResult Load(const std::function<LoadInputs()> &inputs);

private:
	DriverLoadResult Attempt(const LoadInputs &inputs, bool &platform_loader_called);

	LoaderConfig config_;
	std::mutex mutex_;
	bool final_ = false;
	DriverLoadResult final_result_;
	LoadedDriver driver_;
};

// The plain allocation, since the driver reads no ODBCINI on Windows.
SQLRETURN AllocateEnvironment(const LoadedDriver &driver, SQLHENV *environment);

} // namespace windows_driver
} // namespace redshift_driver
} // namespace duckdb
