//===----------------------------------------------------------------------===//
// LoadEmbeddedDriver: the one route to a driver. It hands the embedded set
// and the policy lists that driver_info.h generates to the platform's run-time sequence: on Linux and
// macOS driver_loader.cpp's, on Windows driver_loader_windows.cpp's, once the set's resources pass.
//===----------------------------------------------------------------------===//

#include "driver_info.h"

#if defined(_WIN32)
#include "driver_loader_windows.hpp"
#include "embedded_resources.hpp"
#include "sha256.hpp"

#include <mutex>

// The image this file is linked into, from the MSVC linker, so the set is read from the module that
// holds the loader whichever image that is (the extension, the shell, a test), with no call to fail.
extern "C" IMAGE_DOS_HEADER __ImageBase;

namespace duckdb {
namespace redshift_driver {
namespace detail {

bool ReadEmbeddedResources(HMODULE module, std::vector<EmbeddedResource> &files, DriverError &error) {
	struct Recorded {
		const char *name;
		size_t size;
		const char *sha256;
		const char *stem;
	};
#define REDSHIFT_RECORDED_FILE(file_name, file_size, file_sha256, stem) {file_name, file_size, file_sha256, #stem},
	static const Recorded recorded[] = {REDSHIFT_DRIVER_FILES(REDSHIFT_RECORDED_FILE)};
#undef REDSHIFT_RECORDED_FILE
	files.clear();
	for (const auto &file : recorded) {
		// The resource script names each file by its stem in upper case (driver_info.py).
		std::wstring resource;
		for (const char *c = file.stem; *c != '\0'; c++) {
			resource.push_back(static_cast<wchar_t>(*c >= 'a' && *c <= 'z' ? *c - 'a' + 'A' : *c));
		}
		const HRSRC found = FindResourceW(module, resource.c_str(), MAKEINTRESOURCEW(10)); // RT_RCDATA
		const HGLOBAL loaded = found ? LoadResource(module, found) : nullptr;
		const auto data = static_cast<const unsigned char *>(loaded ? LockResource(loaded) : nullptr);
		const size_t size = found ? SizeofResource(module, found) : 0;
		if (data == nullptr || size != file.size || Sha256Hex(data, size) != file.sha256) {
			error = DriverError();
			error.kind = DriverErrorKind::EMBEDDED_BYTES_CORRUPT;
			error.message =
			    std::string(file.name) +
			    " in the extension does not match the hash recorded when it was built; reinstall the extension";
			return false;
		}
		files.push_back({file.name, file.sha256, data, size});
	}
	return true;
}

windows_driver::LoaderConfig EmbeddedConfig(const std::vector<EmbeddedResource> &files) {
	windows_driver::LoaderConfig config;
	config.files = files; // EmbeddedResource is the loader's SetFile
	config.set_sha256 = REDSHIFT_DRIVER_SET_SHA256;
	config.identity.origin = DriverOrigin::EMBEDDED;
	config.identity.version = REDSHIFT_DRIVER_VERSION;
	config.identity.platform = REDSHIFT_DRIVER_PLATFORM;
	config.identity.development = REDSHIFT_DRIVER_DEVELOPMENT != 0;
	config.identity.set_sha256 = REDSHIFT_DRIVER_SET_SHA256;
	// Every refused name: a set DLL's C runtime copies the environment block when it loads.
	config.refused_environment = {REDSHIFT_DRIVER_REFUSED_ENVIRONMENT};
	config.overridden_environment = {REDSHIFT_DRIVER_OVERRIDDEN_ENVIRONMENT};
#define REDSHIFT_PREREQUISITE(dll, a, b, c, d) {dll, #a "." #b "." #c "." #d},
	config.prerequisites = {REDSHIFT_DRIVER_PREREQUISITE_VERSIONS(REDSHIFT_PREREQUISITE)};
#undef REDSHIFT_PREREQUISITE
	config.system_dbghelp = REDSHIFT_DRIVER_SYSTEM_DBGHELP != 0;
	return config;
}

namespace {

// One loader for the process, made at the first call whose resources pass, never at extension LOAD,
// and never destroyed: the driver it loaded stays in use until the process ends. Until then each call
// reads and checks the resources again, so a failure is retried.
windows_driver::DriverLoader *EmbeddedLoader(DriverError &error) {
	static std::mutex mutex;
	// NOLINTNEXTLINE(cppcoreguidelines-avoid-non-const-global-variables): never destroyed, on purpose
	static windows_driver::DriverLoader *loader = nullptr;
	const std::lock_guard<std::mutex> guard(mutex);
	if (loader == nullptr) {
		std::vector<EmbeddedResource> files;
		if (!ReadEmbeddedResources(reinterpret_cast<HMODULE>(&__ImageBase), files, error)) {
			return nullptr;
		}
		loader = new windows_driver::DriverLoader(EmbeddedConfig(files));
	}
	return loader;
}

} // namespace

} // namespace detail

DriverLoadResult LoadEmbeddedDriver() noexcept {
	DriverLoadResult result;
	windows_driver::DriverLoader *loader = nullptr;
	try {
		loader = detail::EmbeddedLoader(result.error);
		return loader == nullptr ? result : loader->Load(windows_driver::CurrentLoadInputs);
	} catch (...) {
		// Only a failure to make the loader is known to precede any load. Load reports its own failures,
		// so one that escapes it (copying a final result) may follow driver code: it asks for a restart.
		result = DriverLoadResult();
		if (loader == nullptr) {
			result.error.kind = DriverErrorKind::NO_USABLE_DIRECTORY;
			result.error.message = "the Redshift ODBC driver could not be prepared (out of memory or another "
			                       "internal error); try again";
		} else {
			result.error.kind = DriverErrorKind::LOAD_FAILED;
			result.error.message = "the Redshift ODBC driver could not be prepared (out of memory or another "
			                       "internal error); restart the program and connect again";
		}
	}
	return result;
}

// The plain allocation, since the driver reads no ODBCINI on Windows.
SQLRETURN AllocateEnvironmentHandle(const LoadedDriver &driver, SQLHENV *environment) noexcept {
	return windows_driver::AllocateEnvironment(driver, environment);
}

} // namespace redshift_driver
} // namespace duckdb

#else
#include "driver_loader_core.hpp"

#define REDSHIFT_DECLARE_EMBEDDED(name, size, sha256, stem)                                                            \
	extern "C" const unsigned char redshift_driver_##stem##_begin[];                                                   \
	extern "C" const unsigned char redshift_driver_##stem##_end[];
REDSHIFT_DRIVER_FILES(REDSHIFT_DECLARE_EMBEDDED)
#undef REDSHIFT_DECLARE_EMBEDDED

namespace duckdb {
namespace redshift_driver {
namespace detail {

LoaderConfig EmbeddedConfig() {
	LoaderConfig config;
#define REDSHIFT_EMBEDDED_FILE(file_name, file_size, file_sha256, stem)                                                \
	{                                                                                                                  \
		SetFile file;                                                                                                  \
		file.name = file_name;                                                                                         \
		file.sha256 = file_sha256;                                                                                     \
		file.data = redshift_driver_##stem##_begin;                                                                    \
		file.size = static_cast<size_t>(redshift_driver_##stem##_end - redshift_driver_##stem##_begin);                \
		config.set.files.push_back(file);                                                                              \
	}
	REDSHIFT_DRIVER_FILES(REDSHIFT_EMBEDDED_FILE)
#undef REDSHIFT_EMBEDDED_FILE
	config.set.set_sha256 = REDSHIFT_DRIVER_SET_SHA256;
	config.identity.origin = DriverOrigin::EMBEDDED;
	config.identity.version = REDSHIFT_DRIVER_VERSION;
	config.identity.platform = REDSHIFT_DRIVER_PLATFORM;
	config.identity.development = REDSHIFT_DRIVER_DEVELOPMENT != 0;
	config.identity.set_sha256 = REDSHIFT_DRIVER_SET_SHA256;
	config.refused_environment = {REDSHIFT_DRIVER_REFUSED_ENVIRONMENT};
	config.load_refused_environment = {REDSHIFT_DRIVER_LOAD_REFUSED_ENVIRONMENT};
	config.overridden_environment = {REDSHIFT_DRIVER_OVERRIDDEN_ENVIRONMENT};
	config.prerequisites = {REDSHIFT_DRIVER_HOST_PREREQUISITES};
#if defined(__linux__)
	config.prerequisite_hint = "it comes from the host's keyutils package (for example keyutils-libs or libkeyutils1)";
	// The libraries the embedded driver needs, read from its own bytes (the first file), found and
	// checked before anything is written. macOS has none to find: the driver links only system paths.
	config.needed = ElfNeededLibraries(config.set.files[0].data, config.set.files[0].size);
#endif
	return config;
}

namespace {

// One loader for the process, created at the first connection, never at extension LOAD, and never
// destroyed: the driver it loaded stays in use until the process ends.
DriverLoader &EmbeddedLoader() {
	// NOLINTNEXTLINE(cppcoreguidelines-avoid-non-const-global-variables): never destroyed, on purpose
	static auto *loader = new DriverLoader(EmbeddedConfig());
	return *loader;
}

} // namespace

} // namespace detail

DriverLoadResult LoadEmbeddedDriver() noexcept {
	try {
		// The base inputs are read only while no outcome is stored.
		return detail::EmbeddedLoader().Load(detail::CurrentLoadInputs);
	} catch (...) {
		// Creating the loader failed before anything was loaded, so the next call tries again.
		DriverLoadResult result;
		result.error = detail::InternalError(DriverErrorKind::NO_USABLE_DIRECTORY);
		return result;
	}
}

SQLRETURN AllocateEnvironmentHandle(const LoadedDriver &driver, SQLHENV *environment) noexcept {
	try {
		return detail::EmbeddedLoader().AllocateEnvironment(driver.api, environment);
	} catch (...) {
		return SQL_ERROR; // the loader could not be made; nothing was allocated
	}
}

} // namespace redshift_driver
} // namespace duckdb
#endif
