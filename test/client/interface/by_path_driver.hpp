//===----------------------------------------------------------------------===//
//                         DuckDB - Redshift
//
// test/client/interface/by_path_driver.hpp
//
// A DriverSource over a driver library the developer supplies, for offline work with a real driver
// for offline work with a real driver, such as loading it by path and recording traces. Test-only: listed in this
// area's sources.cmake alone, so only the client suite links it and no shipped build contains it.
//===----------------------------------------------------------------------===//

#pragma once

#include "odbc_driver.hpp"

#include <dlfcn.h>
#include <sys/types.h>
#include <unistd.h>

#include <mutex>
#include <string>
#include <vector>

// A sanitizer runtime refuses a dlopen with RTLD_DEEPBIND, so a sanitized build (GCC's macro or
// Clang's __has_feature) leaves the flag out, and
// so does a platform without it. Defined here so the flags and their tests read one condition.
#if defined(__SANITIZE_ADDRESS__) || defined(__SANITIZE_THREAD__)
#define REDSHIFT_BY_PATH_SANITIZER_BUILD
#elif defined(__has_feature)
#if __has_feature(address_sanitizer) || __has_feature(thread_sanitizer)
#define REDSHIFT_BY_PATH_SANITIZER_BUILD
#endif
#endif
#if defined(RTLD_DEEPBIND) && !defined(REDSHIFT_BY_PATH_SANITIZER_BUILD)
#define REDSHIFT_BY_PATH_DEEPBIND
#endif

namespace duckdb {
namespace redshift_driver {
namespace testing {

//! Names the driver library's absolute path.
constexpr const char *BY_PATH_DRIVER_PATH_VARIABLE = "REDSHIFT_TEST_DRIVER_PATH";
//! Names the library's SHA-256 in lowercase hex, as sha256sum prints it.
constexpr const char *BY_PATH_DRIVER_SHA256_VARIABLE = "REDSHIFT_TEST_DRIVER_SHA256";

//! The flags a load uses: the driver loader's, with RTLD_DEEPBIND only where
//! REDSHIFT_BY_PATH_DEEPBIND is defined. Named and declared as the driver loader's own loader declares
//! it, so a test checks it.
int PlatformDlopenFlags();

//! Why a directory or file with this owner and mode may not be trusted, or an empty string:
//! owned by neither `trusted_uid` nor root, or writable by its group or by other users.
std::string FindOwnershipRefusal(uid_t owner, mode_t mode, uid_t trusted_uid);

//! Whether a directory above the library's own is exempt from FindOwnershipRefusal: root-owned and
//! sticky, such as /tmp. The driver loader's ancestor rule exempts such an ancestor but never the base
//! directory, and the library's own directory stands in for the base here.
bool IsTrustedStickyAncestor(uid_t owner, mode_t mode);

//! `text` with every absolute path cut to its file name, so the platform loader's text, which may
//! name a dependency or each file dyld tried, can go in a message.
//! A path starts the text or follows a space, '(' or a quote, and runs to its closing quote or the
//! next ':'; what follows its last '/' is kept.
std::string CutPathsToFileNames(const std::string &text);

//! The variables that make the platform loader try another file before the path it is given: on
//! macOS, dyld searches DYLD_LIBRARY_PATH, DYLD_FRAMEWORK_PATH and their fallbacks first, and tries
//! a suffixed name. None elsewhere: glibc opens a path with a slash as given, once '$' is refused.
std::vector<std::string> ListLoaderSearchVariables();

//! Loads the library the two variables name. A refusal before the platform loader (a relative path,
//! a '$', ':', quote or '..' in it, a link, a set loader search variable, a file that is not
//! regular, an owner or mode FindOwnershipRefusal refuses, a hash the pin does not match) is
//! NO_USABLE_DIRECTORY and the next call checks again; from the loader on, the outcome is final and
//! later calls return it before reading either variable.
class ByPathDriverLoader {
public:
	//! `trusted_uid` is the owner FindOwnershipRefusal accepts besides root; `search_variables` are
	//! the variables refused when set, even to an empty value.
	explicit ByPathDriverLoader(uid_t trusted_uid = ::getuid(),
	                            std::vector<std::string> search_variables = ListLoaderSearchVariables());
	ByPathDriverLoader(const ByPathDriverLoader &) = delete;
	ByPathDriverLoader &operator=(const ByPathDriverLoader &) = delete;

	//! A success names this loader's LoadedDriver, valid while the loader lives.
	DriverLoadResult Load();

private:
	void LoadMatchedFile(const std::string &path, const std::string &sha256);

	const uid_t trusted_uid;
	const std::vector<std::string> search_variables;
	std::mutex mutex;
	bool has_final_outcome = false;
	LoadedDriver driver;
	DriverLoadResult outcome;
};

//! The process's one ByPathDriverLoader, so the driver loads at most once per process; each
//! self-test makes its own loader. Its refused and overridden lists are empty, so a run through it
//! has no refused-variable protection: SSLKEYLOGFILE, for one, is honoured.
//! It skips the driver loader's noexec, ACL, file-system-type, driver-root, exact-file-set and prerequisite
//! checks too, so the library's directory may hold other files, and it loads the path it checked,
//! not the descriptor it hashed.
DriverLoadResult ByPathDriverSource();

} // namespace testing
} // namespace redshift_driver
} // namespace duckdb
