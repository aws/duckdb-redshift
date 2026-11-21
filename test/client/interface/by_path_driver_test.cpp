#include "by_path_driver.hpp"
#include "scoped_environment_variable.hpp"

#include "duckdb/main/database.hpp"
#include "mbedtls_wrapper.hpp"

#include "catch.hpp"

#include <dlfcn.h>
#include <fcntl.h>
#include <limits.h>
#include <stdlib.h>
#include <sys/stat.h>
#include <sys/statvfs.h>
#include <unistd.h>
#ifdef __linux__
// Only RunChildCase and CountOpenDescriptors use these, and both are Linux-only.
// On Apple, <spawn.h> must not follow DuckDB's helper.hpp, which redefines __restrict.
#include <dirent.h>
#include <spawn.h>
#include <sys/wait.h>
#endif

#include <atomic>
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <string>
#include <thread>
#include <vector>

using duckdb::idx_t;
using namespace duckdb::redshift_driver;
using namespace duckdb::redshift_driver::testing;

namespace {

const char *const WRONG_PIN = "0000000000000000000000000000000000000000000000000000000000000000";

std::string ComputeSha256(const std::string &bytes) {
	duckdb_mbedtls::MbedTlsWrapper::SHA256State state;
	state.AddString(bytes);
	char hex[duckdb_mbedtls::MbedTlsWrapper::SHA256_HASH_LENGTH_TEXT];
	state.FinishHex(hex);
	return std::string(hex, sizeof(hex));
}

//! The whole file, which the test needs to exist.
std::string ReadFile(const std::string &path) {
	std::ifstream file(path.c_str(), std::ios::binary | std::ios::ate);
	REQUIRE(file.good());
	std::string bytes(static_cast<size_t>(file.tellg()), '\0');
	file.seekg(0);
	file.read(&bytes[0], static_cast<std::streamsize>(bytes.size()));
	REQUIRE(file.good());
	return bytes;
}

//! The file's mode is set, not taken from the umask, which under 002 would make it group-writable.
void WriteFile(const std::string &path, const std::string &bytes) {
	std::ofstream file(path.c_str(), std::ios::binary);
	file.write(bytes.data(), static_cast<std::streamsize>(bytes.size()));
	file.close();
	REQUIRE(file.good());
	REQUIRE(::chmod(path.c_str(), 0644) == 0);
}

//! Whether `needle` appears in the file. Read in chunks that overlap by one byte less than the
//! needle, so a match across a chunk boundary is still found and no artefact is held in memory
//! whole.
bool FileContains(const std::string &path, const std::string &needle) {
	std::ifstream file(path.c_str(), std::ios::binary);
	REQUIRE(file.good());
	const idx_t overlap = needle.size() - 1;
	std::string window;
	std::vector<char> chunk(1 << 20);
	while (file.read(chunk.data(), static_cast<std::streamsize>(chunk.size())) || file.gcount() > 0) {
		window.append(chunk.data(), static_cast<size_t>(file.gcount()));
		if (window.find(needle) != std::string::npos) {
			return true;
		}
		if (window.size() > overlap) {
			window.erase(0, window.size() - overlap);
		}
	}
	return false;
}

//! /tmp with its links resolved, so a host whose /tmp is a symbolic link checks the real
//! directory. Every temporary directory here is made under it.
std::string FindTemporaryRoot() {
	char resolved[PATH_MAX];
	REQUIRE(realpath("/tmp", resolved) != nullptr);
	return resolved;
}

//! Why this host cannot run the cases that check real directories, or an empty string. Inside a
//! user namespace such as `unshare -r`, the files of the real root show as owned by the overflow
//! user, so the source refuses / itself.
std::string FindHostSkipReason() {
	struct stat root;
	if (::lstat("/", &root) != 0) {
		return "/ cannot be read";
	}
	if (root.st_uid != 0 && root.st_uid != ::getuid()) {
		return "/ is owned by uid " + std::to_string(root.st_uid) +
		       ", neither root nor this user, as inside a user namespace (unshare -r); run make test_client "
		       "outside one";
	}
	return std::string();
}

//! Why this host cannot run the cases that load a library copied under the temporary root, or an
//! empty string.
std::string FindLoadingSkipReason() {
	const std::string host_reason = FindHostSkipReason();
	if (!host_reason.empty()) {
		return host_reason;
	}
#ifdef ST_NOEXEC
	struct statvfs file_system;
	const std::string root = FindTemporaryRoot();
	if (::statvfs(root.c_str(), &file_system) == 0 && (file_system.f_flag & ST_NOEXEC) != 0) {
		return root + " is mounted noexec, so no library copied there can be loaded";
	}
#endif
	return std::string();
}

// Ends the case as skipped, so the totals count it, naming why this host cannot run it. A macro,
// because only a return in the case's own body ends the case.
#define SKIP_CASE_IF(reason_expression)                                                                                \
	do {                                                                                                               \
		const std::string skip_reason = (reason_expression);                                                           \
		if (!skip_reason.empty()) {                                                                                    \
			SKIP_TEST("skipped: " + skip_reason);                                                                      \
			return;                                                                                                    \
		}                                                                                                              \
	} while (false)

//! A new directory under `parent`, removed again with everything added to it, even when the
//! test fails half way through filling it.
class TemporaryDirectory {
public:
	explicit TemporaryDirectory(const std::string &parent) {
		std::string pattern = parent + "/redshift-by-path-XXXXXX";
		REQUIRE(mkdtemp(&pattern[0]) != nullptr);
		path = pattern;
	}
	~TemporaryDirectory() {
		// A test may have made the directory unusable to its owner's tools.
		chmod(path.c_str(), 0700);
		for (idx_t i = entries.size(); i > 0; i--) {
			chmod(entries[i - 1].c_str(), 0700);
			::remove(entries[i - 1].c_str());
		}
		rmdir(path.c_str());
	}
	TemporaryDirectory(const TemporaryDirectory &) = delete;
	TemporaryDirectory &operator=(const TemporaryDirectory &) = delete;

	const std::string &Path() const {
		return path;
	}

	//! The path of `name` inside, removed with the directory whether or not the test creates it.
	std::string Register(const std::string &name) {
		entries.push_back(path + "/" + name);
		return entries.back();
	}

private:
	std::string path;
	std::vector<std::string> entries;
};

//! A copy of `bytes` named librsodbc64.so in a new directory of its own, so no two cases load one
//! file and each case sees only its own load.
class TemporaryLibrary {
public:
	explicit TemporaryLibrary(const std::string &bytes, const std::string &parent = FindTemporaryRoot())
	    : directory(parent), path(directory.Register("librsodbc64.so")), sha256(ComputeSha256(bytes)) {
		WriteFile(path, bytes);
	}

	const std::string &Directory() const {
		return directory.Path();
	}
	const std::string &Path() const {
		return path;
	}
	const std::string &Sha256() const {
		return sha256;
	}

	//! A symbolic link beside the library, pointing at `target`.
	std::string AddSymbolicLink(const std::string &name, const std::string &target) {
		const std::string link = directory.Register(name);
		REQUIRE(::symlink(target.c_str(), link.c_str()) == 0);
		return link;
	}

	//! A directory beside the library, so a test can name something that is not a file.
	std::string AddSubdirectory(const std::string &name) {
		const std::string child = directory.Register(name);
		REQUIRE(::mkdir(child.c_str(), 0700) == 0);
		return child;
	}

	//! A file named `name` beside the library, holding `bytes`.
	std::string AddFile(const std::string &name, const std::string &bytes) {
		const std::string file = directory.Register(name);
		WriteFile(file, bytes);
		return file;
	}

	//! A FIFO beside the library, which an open without O_NONBLOCK would wait on for a writer.
	std::string AddFifo(const std::string &name) {
		const std::string fifo = directory.Register(name);
		REQUIRE(::mkfifo(fifo.c_str(), 0600) == 0);
		return fifo;
	}

private:
	TemporaryDirectory directory;
	const std::string path;
	const std::string sha256;
};

//! Whether the platform loader has this file mapped into the process. RTLD_LOCAL, because on macOS a
//! dlopen without it makes a library loaded with RTLD_LOCAL global, even with RTLD_NOLOAD.
bool IsMapped(const std::string &path) {
	void *library = dlopen(path.c_str(), RTLD_NOW | RTLD_NOLOAD | RTLD_LOCAL);
	if (library != nullptr) {
		dlclose(library);
	}
	return library != nullptr;
}

//! The loader's answer for `path` pinned to `pin`, with the environment restored afterwards.
DriverLoadResult LoadThrough(ByPathDriverLoader &loader, const std::string &path, const std::string &pin) {
	ScopedEnvironmentVariable path_variable(BY_PATH_DRIVER_PATH_VARIABLE, path.c_str());
	ScopedEnvironmentVariable pin_variable(BY_PATH_DRIVER_SHA256_VARIABLE, pin.c_str());
	return loader.Load();
}

//! The answer of a loader of its own for `path`, for a case that expects a refusal.
DriverLoadResult RefuseThrough(const std::string &path, const std::string &pin) {
	ByPathDriverLoader loader;
	const DriverLoadResult result = LoadThrough(loader, path, pin);
	REQUIRE_FALSE(result.ok());
	return result;
}

//! The loader's answer for `library` pinned to its own SHA-256.
DriverLoadResult LoadPinned(ByPathDriverLoader &loader, const TemporaryLibrary &library) {
	return LoadThrough(loader, library.Path(), library.Sha256());
}

//! The loader's answer with both variables unset.
DriverLoadResult LoadUnset(ByPathDriverLoader &loader) {
	ScopedEnvironmentVariable path_variable(BY_PATH_DRIVER_PATH_VARIABLE, nullptr);
	ScopedEnvironmentVariable pin_variable(BY_PATH_DRIVER_SHA256_VARIABLE, nullptr);
	return loader.Load();
}

//! The driver a load that must succeed names; the load's error is printed when it does not.
const LoadedDriver &RequireLoaded(const DriverLoadResult &result) {
	INFO(result.error.message);
	REQUIRE(result.ok());
	return *result.driver;
}

//! The one attempt a failure carries, so a check that fails prints the reason.
std::string GetOnlyAttemptReason(const DriverLoadResult &result) {
	INFO(result.error.message);
	REQUIRE(result.error.attempts.size() == 1);
	return result.error.attempts[0].reason;
}

//! Checks a refusal of the variables' values: NO_USABLE_DIRECTORY with no attempt, and a message
//! that starts with `rule` and holds no '/', so neither a path nor a value.
void CheckVariableRefusal(const DriverLoadResult &result, const std::string &rule) {
	INFO(result.error.message);
	CHECK(result.error.kind == DriverErrorKind::NO_USABLE_DIRECTORY);
	CHECK_THAT(result.error.message, Catch::StartsWith(rule));
	CHECK(result.error.message.find('/') == std::string::npos);
	CHECK(result.error.attempts.empty());
}

//! Checks a refusal of a checked path: NO_USABLE_DIRECTORY, a message that gives `message_rule`
//! and holds none of the directories down to the attempt's, and one attempt whose reason is
//! `attempt_reason`. A reason such as "Input/output error" may hold a '/'.
void CheckPathRefusal(const DriverLoadResult &result, const std::string &message_rule,
                      const std::string &attempt_reason) {
	INFO(result.error.message);
	CHECK(result.error.kind == DriverErrorKind::NO_USABLE_DIRECTORY);
	CHECK_THAT(result.error.message, Catch::Contains("refused before loading: " + message_rule + ";"));
	CHECK(GetOnlyAttemptReason(result) == attempt_reason);
	const std::string directory = result.error.attempts[0].directory + "/";
	for (auto slash = directory.find('/', 1); slash != std::string::npos; slash = directory.find('/', slash + 1)) {
		CHECK_THAT(result.error.message, !Catch::Contains(directory.substr(0, slash)));
	}
}

//! Why `directory` is not a root-owned sticky directory, which the walk passes over, or an empty
//! string.
std::string FindTrustedAncestorSkipReason(const std::string &directory) {
	struct stat status;
	REQUIRE(::lstat(directory.c_str(), &status) == 0);
	if (!IsTrustedStickyAncestor(status.st_uid, status.st_mode)) {
		return directory + " is not a root-owned sticky directory on this host";
	}
	return std::string();
}

#ifdef __linux__
//! The name of the hidden case a child process runs, so a case can spend the child's
//! ByPathDriverSource and leave this process's unloaded.
const char *const CHILD_CASE = "ByPathDriverSource in a child keeps its first outcome for the process";

//! This suite's exit status running CHILD_CASE alone with the two variables set, or -1 when the
//! child did not exit. Its output goes to /dev/null.
int RunChildCase(const std::string &path, const std::string &pin) {
	ScopedEnvironmentVariable path_variable(BY_PATH_DRIVER_PATH_VARIABLE, path.c_str());
	ScopedEnvironmentVariable pin_variable(BY_PATH_DRIVER_SHA256_VARIABLE, pin.c_str());
	std::string program = "/proc/self/exe";
	std::string case_name = CHILD_CASE;
	char *arguments[] = {&program[0], &case_name[0], nullptr};
	posix_spawn_file_actions_t actions;
	REQUIRE(posix_spawn_file_actions_init(&actions) == 0);
	REQUIRE(posix_spawn_file_actions_addopen(&actions, STDOUT_FILENO, "/dev/null", O_WRONLY, 0) == 0);
	REQUIRE(posix_spawn_file_actions_addopen(&actions, STDERR_FILENO, "/dev/null", O_WRONLY, 0) == 0);
	pid_t child;
	const int spawned = posix_spawn(&child, program.c_str(), &actions, nullptr, arguments, environ);
	posix_spawn_file_actions_destroy(&actions);
	REQUIRE(spawned == 0);
	int status = 0;
	REQUIRE(waitpid(child, &status, 0) == child);
	return WIFEXITED(status) ? WEXITSTATUS(status) : -1;
}

//! How many descriptors this process holds open, as /proc lists them.
idx_t CountOpenDescriptors() {
	DIR *descriptors = opendir("/proc/self/fd");
	REQUIRE(descriptors != nullptr);
	idx_t count = 0;
	while (readdir(descriptors) != nullptr) {
		count++;
	}
	closedir(descriptors);
	return count;
}
#endif

//! Why this host has no root-owned, sticky, group-writable temporary root, or an empty string.
std::string FindStickyRootSkipReason(const std::string &root) {
	struct stat status;
	REQUIRE(::lstat(root.c_str(), &status) == 0);
	if (status.st_uid != 0 || (status.st_mode & S_ISVTX) == 0 || (status.st_mode & S_IWGRP) == 0) {
		return root + " is not a root-owned, sticky, group-writable directory on this host";
	}
	return std::string();
}

//! The C library this process runs on, with its links resolved, for the arm of the ownership check
//! that accepts root: an unprivileged test cannot create a root-owned file, so it borrows this one.
std::string FindRootOwnedLibrary() {
	void *libc = dlopen("libc.so.6", RTLD_NOW | RTLD_NOLOAD);
	if (libc == nullptr) {
		return std::string();
	}
	// Looked up through the library's own handle, as the executable may hold a PLT entry instead.
	void *symbol = dlsym(libc, "getpid");
	Dl_info info;
	char resolved[PATH_MAX];
	const bool found = symbol != nullptr && dladdr(symbol, &info) != 0 && info.dli_fname != nullptr &&
	                   realpath(info.dli_fname, resolved) != nullptr;
	dlclose(libc);
	return found ? std::string(resolved) : std::string();
}

//! Why `library` is not a root-owned regular file on this host, or an empty string.
std::string FindRootOwnedLibrarySkipReason(const std::string &library) {
	struct stat status;
	if (library.empty() || ::lstat(library.c_str(), &status) != 0 || status.st_uid != 0 || !S_ISREG(status.st_mode)) {
		return "the C library '" + library + "' is not a root-owned regular file on this host";
	}
	return std::string();
}

//! Why the developer's own driver cannot be loaded, or an empty string once both variables are set.
std::string FindDeveloperDriverSkipReason() {
	if (std::getenv(BY_PATH_DRIVER_PATH_VARIABLE) == nullptr ||
	    std::getenv(BY_PATH_DRIVER_SHA256_VARIABLE) == nullptr) {
		return std::string("set ") + BY_PATH_DRIVER_PATH_VARIABLE + " and " + BY_PATH_DRIVER_SHA256_VARIABLE +
		       " to a driver library to run it";
	}
	return std::string();
}

} // namespace

TEST_CASE("FindOwnershipRefusal refuses an owner that is neither this user nor root", "[interface]") {
	CHECK(FindOwnershipRefusal(1234, 0755, 1000) == "is owned by another user");
	CHECK(FindOwnershipRefusal(1000, 0755, 1000).empty());
	CHECK(FindOwnershipRefusal(0, 0755, 1000).empty());
}

TEST_CASE("FindOwnershipRefusal refuses a mode its group or other users can write", "[interface]") {
	CHECK(FindOwnershipRefusal(1000, 0775, 1000) == "is writable by its group");
	CHECK(FindOwnershipRefusal(1000, 0757, 1000) == "is writable by other users");
	CHECK(FindOwnershipRefusal(0, 01777, 1000) == "is writable by its group");
}

TEST_CASE("Only a sticky directory root owns is a trusted ancestor", "[interface]") {
	CHECK(IsTrustedStickyAncestor(0, 01777));
	CHECK_FALSE(IsTrustedStickyAncestor(1000, 01777));
	CHECK_FALSE(IsTrustedStickyAncestor(0, 0777));
}

TEST_CASE("CutPathsToFileNames cuts every absolute path to its file name", "[interface]") {
	// dyld's form, with a quoted list, the Cryptexes prefix and a directory holding a space.
	CHECK(CutPathsToFileNames("dlopen(/Users/a b/lib.dylib, 0x0006): tried: '/Users/a b/lib.dylib' (not a mach-o "
	                          "file), '/System/Volumes/Preboot/Cryptexes/OS/Users/a b/lib.dylib' (no such file)") ==
	      "dlopen(lib.dylib, 0x0006): tried: 'lib.dylib' (not a mach-o file), 'lib.dylib' (no such file)");
	CHECK(CutPathsToFileNames("Library not loaded: @rpath/libz.dylib\n  Referenced from: /opt/d/lib.dylib\n  Reason: "
	                          "tried: '/usr/lib/libz.dylib' (no such file)") ==
	      "Library not loaded: @rpath/libz.dylib\n  Referenced from: lib.dylib\n  Reason: tried: 'libz.dylib' (no such "
	      "file)");
	CHECK(CutPathsToFileNames("/lib64/libssl.so.3: version `OPENSSL_3.2.0' not found (required by /opt/d/lib.so)") ==
	      "libssl.so.3: version `OPENSSL_3.2.0' not found (required by lib.so)");
	// dyld's older form starts a path after a tab.
	CHECK(CutPathsToFileNames("no suitable image found.  Did find:\n\t/opt/d/lib.dylib: mach-o, but wrong "
	                          "architecture") ==
	      "no suitable image found.  Did find:\n\tlib.dylib: mach-o, but wrong architecture");
	// A slash within a word starts no path.
	CHECK(CutPathsToFileNames("/opt/d/lib.so: I/O error") == "lib.so: I/O error");
	CHECK(CutPathsToFileNames("libkeyutils.so.1: cannot open shared object file") ==
	      "libkeyutils.so.1: cannot open shared object file");
}

TEST_CASE("The by-path source asks for both variables when the pin is unset", "[interface]") {
	ScopedEnvironmentVariable path(BY_PATH_DRIVER_PATH_VARIABLE, "/opt/driver/librsodbc64.so");
	ScopedEnvironmentVariable pin(BY_PATH_DRIVER_SHA256_VARIABLE, nullptr);
	ByPathDriverLoader loader;
	const auto result = loader.Load();
	CheckVariableRefusal(result, "no test driver is named");
	CHECK_THAT(result.error.message,
	           Catch::Contains(BY_PATH_DRIVER_PATH_VARIABLE) && Catch::Contains(BY_PATH_DRIVER_SHA256_VARIABLE));
}

TEST_CASE("The by-path source asks for both variables when the path is unset", "[interface]") {
	ScopedEnvironmentVariable path(BY_PATH_DRIVER_PATH_VARIABLE, nullptr);
	ScopedEnvironmentVariable pin(BY_PATH_DRIVER_SHA256_VARIABLE, WRONG_PIN);
	ByPathDriverLoader loader;
	CheckVariableRefusal(loader.Load(), "no test driver is named");
}

TEST_CASE("ByPathDriverSource reads the two variables", "[interface]") {
	// With the developer's variables set, the last case may already have made the process's source
	// final, so this one runs only without them.
	SKIP_CASE_IF(FindDeveloperDriverSkipReason().empty() ? "the developer's driver is named" : "");
	ScopedEnvironmentVariable path(BY_PATH_DRIVER_PATH_VARIABLE, nullptr);
	ScopedEnvironmentVariable pin(BY_PATH_DRIVER_SHA256_VARIABLE, nullptr);
	CheckVariableRefusal(ByPathDriverSource(), "no test driver is named");
}

TEST_CASE("The by-path source refuses a relative path, which the platform loader would search for", "[interface]") {
	CheckVariableRefusal(RefuseThrough("librsodbc64.so", WRONG_PIN),
	                     "REDSHIFT_TEST_DRIVER_PATH is not an absolute path");
}

TEST_CASE("The by-path source refuses a pin that is not 64 lowercase hex digits", "[interface]") {
	const std::string pin = ComputeSha256("any bytes");
	for (const std::string &malformed : {std::string(), pin.substr(0, 63), pin + "0", std::string(64, 'A')}) {
		INFO("pin '" << malformed << "'");
		CheckVariableRefusal(RefuseThrough("/opt/driver/librsodbc64.so", malformed),
		                     "REDSHIFT_TEST_DRIVER_SHA256 is not 64 lowercase hex digits");
	}
}

TEST_CASE("The by-path source refuses a '$' in the path, which the platform loader would expand", "[interface]") {
	SKIP_CASE_IF(FindLoadingSkipReason());
	// The checks would read the literal $LIB directory and the loader the lib64 one, which holds a
	// different library, as glibc expands $LIB to lib64 on an RPM-based host.
	TemporaryLibrary checked(ReadFile(REDSHIFT_TEST_PARTIAL_STUB_DRIVER));
	checked.AddSubdirectory("$LIB");
	checked.AddSubdirectory("lib64");
	const std::string checked_copy =
	    checked.AddFile("$LIB/librsodbc64.so", ReadFile(REDSHIFT_TEST_PARTIAL_STUB_DRIVER));
	const std::string loaded_copy = checked.AddFile("lib64/librsodbc64.so", ReadFile(REDSHIFT_TEST_STUB_DRIVER));
	CheckVariableRefusal(RefuseThrough(checked_copy, checked.Sha256()), "REDSHIFT_TEST_DRIVER_PATH contains a '$'");
	CHECK_FALSE(IsMapped(loaded_copy));
}

TEST_CASE("The by-path source refuses a '..' component in the path, as the driver loader does", "[interface]") {
	for (const std::string &path : {std::string("/opt/../librsodbc64.so"), std::string("/opt/driver/..")}) {
		INFO("path '" << path << "'");
		CheckVariableRefusal(RefuseThrough(path, WRONG_PIN), "REDSHIFT_TEST_DRIVER_PATH has a '..' component");
	}
}

TEST_CASE("The by-path source refuses a ':' or a quote in the path, which would split it in a message", "[interface]") {
	for (const std::string &path : {std::string("/opt/a:b/librsodbc64.so"), std::string("/opt/a'b/librsodbc64.so")}) {
		INFO("path '" << path << "'");
		CheckVariableRefusal(RefuseThrough(path, WRONG_PIN), "REDSHIFT_TEST_DRIVER_PATH contains a ':' or a quote");
	}
}

TEST_CASE("The by-path source lets a name that only contains '..' through to the directory checks", "[interface]") {
	SKIP_CASE_IF(FindHostSkipReason());
	CheckPathRefusal(RefuseThrough("/redshift-by-path-absent../..librsodbc64.so", WRONG_PIN),
	                 "its directory cannot be read (No such file or directory)",
	                 "/redshift-by-path-absent.. cannot be read (No such file or directory)");
}

TEST_CASE("The by-path source refuses a set loader search variable, naming it and not its value", "[interface]") {
	const char *const variable = "REDSHIFT_INTERFACE_TEST_SEARCH_VARIABLE";
	ByPathDriverLoader loader(::getuid(), {variable});
	// An empty value counts as set.
	for (const char *value : {"/redshift-by-path-search", ""}) {
		INFO("value '" << value << "'");
		ScopedEnvironmentVariable search(variable, value);
		CheckVariableRefusal(LoadThrough(loader, "/redshift-by-path-absent/librsodbc64.so", WRONG_PIN),
		                     std::string(variable) + " is set");
	}
	// Unset, the same loader goes on to the directory checks.
	ScopedEnvironmentVariable search(variable, nullptr);
	const auto result = LoadThrough(loader, "/redshift-by-path-absent/librsodbc64.so", WRONG_PIN);
	CHECK(result.error.kind == DriverErrorKind::NO_USABLE_DIRECTORY);
	CHECK(result.error.attempts.size() == 1);
}

TEST_CASE("ListLoaderSearchVariables names dyld's seven search variables on macOS and none elsewhere", "[interface]") {
#ifdef __APPLE__
	CHECK(ListLoaderSearchVariables() ==
	      std::vector<std::string> {"DYLD_LIBRARY_PATH", "DYLD_FALLBACK_LIBRARY_PATH", "DYLD_IMAGE_SUFFIX",
	                                "DYLD_VERSIONED_LIBRARY_PATH", "DYLD_FRAMEWORK_PATH",
	                                "DYLD_FALLBACK_FRAMEWORK_PATH", "DYLD_VERSIONED_FRAMEWORK_PATH"});
#else
	CHECK(ListLoaderSearchVariables().empty());
#endif
}

#ifdef __APPLE__
TEST_CASE("The by-path source refuses each dyld search variable on macOS", "[interface]") {
	for (const std::string &name : ListLoaderSearchVariables()) {
		INFO(name);
		ScopedEnvironmentVariable search(name.c_str(), "/redshift-by-path-search");
		CheckVariableRefusal(RefuseThrough("/redshift-by-path-absent/librsodbc64.dylib", WRONG_PIN), name + " is set");
	}
}
#endif

TEST_CASE("The by-path source refuses a library whose SHA-256 is not the pinned one, before loading it",
          "[interface]") {
	SKIP_CASE_IF(FindHostSkipReason());
	TemporaryLibrary stub(ReadFile(REDSHIFT_TEST_STUB_DRIVER));
	const auto result = RefuseThrough(stub.Path(), WRONG_PIN);
	// The message names the variable and the rule, never the variable's value; the path is in the
	// attempt alone.
	const std::string rule =
	    "librsodbc64.so has SHA-256 " + stub.Sha256() + ", not the one REDSHIFT_TEST_DRIVER_SHA256 pins";
	CheckPathRefusal(result, rule, rule);
	CHECK_THAT(result.error.message, Catch::StartsWith("the library REDSHIFT_TEST_DRIVER_PATH names was refused"));
	REQUIRE(result.error.attempts.size() == 1);
	CHECK(result.error.attempts[0].directory == stub.Directory());
	CHECK_FALSE(IsMapped(stub.Path()));
}

TEST_CASE("The by-path source refuses a library that is not there, with the reason", "[interface]") {
	SKIP_CASE_IF(FindHostSkipReason());
	TemporaryLibrary stub(ReadFile(REDSHIFT_TEST_STUB_DRIVER));
	CheckPathRefusal(RefuseThrough(stub.Directory() + "/absent.so", WRONG_PIN),
	                 "absent.so cannot be opened (No such file or directory)",
	                 "absent.so cannot be opened (No such file or directory)");
}

TEST_CASE("The by-path source refuses a path that is not a regular file", "[interface]") {
	SKIP_CASE_IF(FindHostSkipReason());
	TemporaryLibrary stub(ReadFile(REDSHIFT_TEST_STUB_DRIVER));
	CheckPathRefusal(RefuseThrough(stub.AddSubdirectory("not-a-library.so"), WRONG_PIN),
	                 "not-a-library.so is not a regular file", "not-a-library.so is not a regular file");
}

TEST_CASE("The by-path source refuses a FIFO without waiting for a writer", "[interface]") {
	SKIP_CASE_IF(FindHostSkipReason());
	TemporaryLibrary stub(ReadFile(REDSHIFT_TEST_STUB_DRIVER));
	CheckPathRefusal(RefuseThrough(stub.AddFifo("fifo.so"), WRONG_PIN), "fifo.so is not a regular file",
	                 "fifo.so is not a regular file");
}

TEST_CASE("The by-path source refuses a symbolic link to the library", "[interface]") {
	SKIP_CASE_IF(FindHostSkipReason());
	TemporaryLibrary stub(ReadFile(REDSHIFT_TEST_STUB_DRIVER));
	// Pinned to the library's own hash, so only the link can be the reason it is refused.
	CheckPathRefusal(RefuseThrough(stub.AddSymbolicLink("linked.so", stub.Path()), stub.Sha256()),
	                 "linked.so is a symbolic link", "linked.so is a symbolic link");
}

TEST_CASE("The by-path source refuses a library its group can write", "[interface]") {
	SKIP_CASE_IF(FindHostSkipReason());
	TemporaryLibrary stub(ReadFile(REDSHIFT_TEST_STUB_DRIVER));
	REQUIRE(chmod(stub.Path().c_str(), 0664) == 0);
	CheckPathRefusal(RefuseThrough(stub.Path(), stub.Sha256()), "librsodbc64.so is writable by its group",
	                 "librsodbc64.so is writable by its group");
}

TEST_CASE("The by-path source refuses a library other users can write", "[interface]") {
	SKIP_CASE_IF(FindHostSkipReason());
	TemporaryLibrary stub(ReadFile(REDSHIFT_TEST_STUB_DRIVER));
	REQUIRE(chmod(stub.Path().c_str(), 0606) == 0);
	CheckPathRefusal(RefuseThrough(stub.Path(), stub.Sha256()), "librsodbc64.so is writable by other users",
	                 "librsodbc64.so is writable by other users");
}

TEST_CASE("The by-path source refuses a library in a directory other users can write", "[interface]") {
	SKIP_CASE_IF(FindHostSkipReason());
	TemporaryLibrary stub(ReadFile(REDSHIFT_TEST_STUB_DRIVER));
	REQUIRE(chmod(stub.Directory().c_str(), 0707) == 0);
	// The message names the directory by its role, as it holds no path.
	CheckPathRefusal(RefuseThrough(stub.Path(), stub.Sha256()), "its directory is writable by other users",
	                 stub.Directory() + " is writable by other users");
}

TEST_CASE("The by-path source refuses a library in a directory another user owns", "[interface]") {
	SKIP_CASE_IF(FindHostSkipReason());
	SKIP_CASE_IF(::getuid() == 0 ? "root owns every directory it creates" : "");
	SKIP_CASE_IF(FindTrustedAncestorSkipReason(FindTemporaryRoot()));
	TemporaryLibrary stub(ReadFile(REDSHIFT_TEST_STUB_DRIVER));
	// A loader that trusts another uid sees this user's directory as another user's; /tmp above it
	// is a root-owned sticky ancestor, so the library's own directory is the first refused.
	ByPathDriverLoader loader(::getuid() + 1);
	CheckPathRefusal(LoadPinned(loader, stub), "its directory is owned by another user",
	                 stub.Directory() + " is owned by another user");
}

TEST_CASE("The by-path source refuses / when another user owns it, as inside a user namespace", "[interface]") {
	SKIP_CASE_IF(FindHostSkipReason().empty() ? "/ is owned by root or this user on this host" : "");
	CheckPathRefusal(RefuseThrough("/redshift-by-path-absent/librsodbc64.so", WRONG_PIN),
	                 "a directory above it is owned by another user", "/ is owned by another user");
}

TEST_CASE("The by-path source refuses a library under a sticky directory this user owns", "[interface]") {
	SKIP_CASE_IF(FindHostSkipReason());
	TemporaryDirectory sticky(FindTemporaryRoot());
	REQUIRE(chmod(sticky.Path().c_str(), 01777) == 0);
	TemporaryLibrary stub(ReadFile(REDSHIFT_TEST_STUB_DRIVER), sticky.Path());
	CheckPathRefusal(RefuseThrough(stub.Path(), stub.Sha256()), "a directory above it is writable by its group",
	                 sticky.Path() + " is writable by its group");
}

TEST_CASE("The by-path source refuses a library reached through a linked directory", "[interface]") {
	SKIP_CASE_IF(FindHostSkipReason());
	TemporaryLibrary stub(ReadFile(REDSHIFT_TEST_STUB_DRIVER));
	const std::string linked_directory = stub.AddSymbolicLink("self", ".");
	CheckPathRefusal(RefuseThrough(linked_directory + "/librsodbc64.so", stub.Sha256()),
	                 "its directory is a symbolic link", linked_directory + " is a symbolic link");
}

TEST_CASE("The by-path source refuses a directory that is not there", "[interface]") {
	SKIP_CASE_IF(FindHostSkipReason());
	CheckPathRefusal(RefuseThrough("/redshift-by-path-absent/librsodbc64.so", WRONG_PIN),
	                 "its directory cannot be read (No such file or directory)",
	                 "/redshift-by-path-absent cannot be read (No such file or directory)");
}

TEST_CASE("The by-path source checks / as the directory of a library directly under it", "[interface]") {
	SKIP_CASE_IF(FindHostSkipReason());
	const auto result = RefuseThrough("/redshift-by-path-absent.so", WRONG_PIN);
	CheckPathRefusal(result, "redshift-by-path-absent.so cannot be opened (No such file or directory)",
	                 "redshift-by-path-absent.so cannot be opened (No such file or directory)");
	CHECK(result.error.attempts[0].directory == "/");
}

TEST_CASE("The by-path source refuses a path that leads through a regular file", "[interface]") {
	SKIP_CASE_IF(FindHostSkipReason());
	TemporaryLibrary stub(ReadFile(REDSHIFT_TEST_STUB_DRIVER));
	CheckPathRefusal(RefuseThrough(stub.Path() + "/librsodbc64.so", WRONG_PIN), "its directory is not a directory",
	                 stub.Path() + " is not a directory");
}

TEST_CASE("The by-path source refuses a library sitting in a root-owned sticky directory", "[interface]") {
	SKIP_CASE_IF(FindHostSkipReason());
	const std::string root = FindTemporaryRoot();
	SKIP_CASE_IF(FindStickyRootSkipReason(root));
	// The directory holding the library is never exempt; the TemporaryLibrary cases keep theirs
	// in a directory below this one, where this one is an exempt ancestor.
	CheckPathRefusal(RefuseThrough(root + "/redshift-by-path-in-tmp.so", WRONG_PIN),
	                 "its directory is writable by its group", root + " is writable by its group");
}

TEST_CASE("The by-path source lets a root-owned library through to the platform loader", "[interface]") {
	SKIP_CASE_IF(FindHostSkipReason());
	const std::string library = FindRootOwnedLibrary();
	SKIP_CASE_IF(FindRootOwnedLibrarySkipReason(library));
	// It passes every check and is refused only for what it is: a library with no ODBC entry
	// points. Loading it again maps nothing new, as the process already runs on it.
	const auto result = RefuseThrough(library, ComputeSha256(ReadFile(library)));
	CHECK(result.error.kind == DriverErrorKind::MISSING_ENTRY_POINT);
}

#ifdef __linux__
TEST_CASE("The by-path source reports a library it cannot read with the reason", "[interface]") {
	SKIP_CASE_IF(FindHostSkipReason());
	// A process's own memory file is a regular file it owns, and reading it from offset 0, which
	// nothing maps, fails.
	const std::string memory = "/proc/" + std::to_string(::getpid()) + "/mem";
	CheckPathRefusal(RefuseThrough(memory, WRONG_PIN), "mem cannot be read (Input/output error)",
	                 "mem cannot be read (Input/output error)");
}

TEST_CASE("The by-path source closes the descriptor it checked a library through", "[interface]") {
	SKIP_CASE_IF(FindHostSkipReason());
	TemporaryLibrary stub(ReadFile(REDSHIFT_TEST_STUB_DRIVER));
	const idx_t before = CountOpenDescriptors();
	REQUIRE(RefuseThrough(stub.Path(), WRONG_PIN).error.kind == DriverErrorKind::NO_USABLE_DIRECTORY);
	CHECK(CountOpenDescriptors() == before);
}
#endif

TEST_CASE("The by-path source checks a refused library again on the next call", "[interface]") {
	SKIP_CASE_IF(FindLoadingSkipReason());
	TemporaryLibrary stub(ReadFile(REDSHIFT_TEST_PARTIAL_STUB_DRIVER));
	ByPathDriverLoader loader;
	REQUIRE(LoadUnset(loader).error.kind == DriverErrorKind::NO_USABLE_DIRECTORY);
	REQUIRE(LoadThrough(loader, stub.Path(), WRONG_PIN).error.kind == DriverErrorKind::NO_USABLE_DIRECTORY);
	CHECK(LoadPinned(loader, stub).error.kind == DriverErrorKind::MISSING_ENTRY_POINT);
}

TEST_CASE("The by-path source names the first entry point a library does not export", "[interface]") {
	SKIP_CASE_IF(FindLoadingSkipReason());
	TemporaryLibrary stub(ReadFile(REDSHIFT_TEST_PARTIAL_STUB_DRIVER));
	ByPathDriverLoader loader;
	const auto result = LoadPinned(loader, stub);
	CHECK_FALSE(result.ok());
	CHECK(result.error.kind == DriverErrorKind::MISSING_ENTRY_POINT);
	CHECK_THAT(result.error.message, Catch::Contains("REDSHIFT_TEST_DRIVER_PATH names does not export SQLAllocHandle"));
	CHECK_THAT(result.error.message, !Catch::Contains(stub.Directory()));
	REQUIRE(result.error.attempts.size() == 1);
	CHECK(result.error.attempts[0].directory == stub.Directory());
	CHECK(GetOnlyAttemptReason(result) == "librsodbc64.so: does not export SQLAllocHandle");
}

TEST_CASE("The by-path source reports a file the platform loader refuses as LOAD_FAILED", "[interface]") {
	SKIP_CASE_IF(FindLoadingSkipReason());
	TemporaryLibrary text("not a library");
	ByPathDriverLoader loader;
	const auto result = LoadPinned(loader, text);
	CHECK_FALSE(result.ok());
	CHECK(result.error.kind == DriverErrorKind::LOAD_FAILED);
	// The loader's own text, with each path cut to its file name; the attempt keeps the path.
	CHECK_THAT(result.error.message,
	           Catch::StartsWith("the platform loader refused the library REDSHIFT_TEST_DRIVER_PATH names: "));
	CHECK_THAT(result.error.message, Catch::Contains("librsodbc64.so"));
	CHECK(result.error.message.find('/') == std::string::npos);
	CHECK_THAT(GetOnlyAttemptReason(result), Catch::Contains(text.Path()));
#ifdef __linux__
	// glibc's form: the path, then the reason; dyld's starts "dlopen(".
	CHECK_THAT(result.error.message, Catch::Contains(" names: librsodbc64.so: "));
	CHECK_THAT(GetOnlyAttemptReason(result), Catch::StartsWith("librsodbc64.so: " + text.Path() + ": "));
#endif
}

TEST_CASE("The by-path source hashes a library past its first read", "[interface]") {
	SKIP_CASE_IF(FindLoadingSkipReason());
	// Longer than the 64 KiB read, so a pin that matched only the first read would be refused here
	// before the platform loader, which then refuses the text.
	TemporaryLibrary text("not a library" + std::string(3 << 16, 'x'));
	ByPathDriverLoader loader;
	CHECK(LoadPinned(loader, text).error.kind == DriverErrorKind::LOAD_FAILED);
}

#ifdef __linux__
TEST_CASE("The by-path source binds every symbol at load, so an unresolved one fails it", "[interface]") {
	SKIP_CASE_IF(FindLoadingSkipReason());
	TemporaryLibrary stub(ReadFile(REDSHIFT_TEST_UNRESOLVED_STUB_DRIVER));
	ByPathDriverLoader loader;
	const auto result = LoadPinned(loader, stub);
	CHECK_FALSE(result.ok());
	CHECK(result.error.kind == DriverErrorKind::LOAD_FAILED);
	// The platform loader's own message, which names the symbol.
	CHECK_THAT(GetOnlyAttemptReason(result), Catch::Contains("redshift_stub_unresolved_symbol"));
	CHECK_THAT(result.error.message, Catch::Contains("redshift_stub_unresolved_symbol"));
	CHECK_THAT(result.error.message, !Catch::Contains(stub.Directory()));
}
#endif

#ifdef __linux__
TEST_CASE("The by-path source cuts a failing dependency's path in the loader's text to its file name", "[interface]") {
	SKIP_CASE_IF(FindLoadingSkipReason());
	TemporaryLibrary stub(ReadFile(REDSHIFT_TEST_DEPENDENT_STUB_DRIVER));
	const std::string dependency_path = REDSHIFT_TEST_FAILING_DEPENDENCY;
	const std::string dependency_name = dependency_path.substr(dependency_path.rfind('/') + 1);
	const std::string dependency = stub.AddFile(dependency_name, ReadFile(dependency_path));
	ByPathDriverLoader loader;
	const auto result = LoadPinned(loader, stub);
	CHECK(result.error.kind == DriverErrorKind::LOAD_FAILED);
	// glibc names the dependency, not the library, by its full path.
	CHECK_THAT(GetOnlyAttemptReason(result), Catch::Contains(dependency + ": undefined symbol"));
	CHECK_THAT(result.error.message, Catch::Contains(" names: " + dependency_name + ": undefined symbol"));
	CHECK(result.error.message.find('/') == std::string::npos);
}
#endif

TEST_CASE("The by-path source returns its loader outcome on every later call", "[interface]") {
	SKIP_CASE_IF(FindLoadingSkipReason());
	TemporaryLibrary stub(ReadFile(REDSHIFT_TEST_PARTIAL_STUB_DRIVER));
	ByPathDriverLoader loader;
	const auto first = LoadPinned(loader, stub);
	REQUIRE(first.error.kind == DriverErrorKind::MISSING_ENTRY_POINT);
	// A pin that no longer matches is not checked: the loader has run, so its outcome stands.
	const auto second = LoadThrough(loader, stub.Path(), WRONG_PIN);
	CHECK_FALSE(second.ok());
	CHECK(second.error.kind == DriverErrorKind::MISSING_ENTRY_POINT);
	CHECK(second.error.message == first.error.message);
	CHECK(IsMapped(stub.Path()));
}

TEST_CASE("After a final failure the by-path source reads neither variable", "[interface]") {
	SKIP_CASE_IF(FindLoadingSkipReason());
	TemporaryLibrary partial(ReadFile(REDSHIFT_TEST_PARTIAL_STUB_DRIVER));
	TemporaryLibrary complete(ReadFile(REDSHIFT_TEST_STUB_DRIVER));
	ByPathDriverLoader loader;
	const auto first = LoadPinned(loader, partial);
	REQUIRE(first.error.kind == DriverErrorKind::MISSING_ENTRY_POINT);
	const auto unset = LoadUnset(loader);
	CHECK(unset.error.kind == DriverErrorKind::MISSING_ENTRY_POINT);
	CHECK(unset.error.message == first.error.message);
	// Another library, which would load: the first outcome still stands, and nothing maps it.
	const auto changed = LoadPinned(loader, complete);
	CHECK_FALSE(changed.ok());
	CHECK(changed.error.kind == DriverErrorKind::MISSING_ENTRY_POINT);
	CHECK_FALSE(IsMapped(complete.Path()));
}

TEST_CASE("A LOAD_FAILED outcome is final", "[interface]") {
	SKIP_CASE_IF(FindLoadingSkipReason());
	TemporaryLibrary text("not a library");
	TemporaryLibrary complete(ReadFile(REDSHIFT_TEST_STUB_DRIVER));
	ByPathDriverLoader loader;
	REQUIRE(LoadPinned(loader, text).error.kind == DriverErrorKind::LOAD_FAILED);
	CHECK(LoadPinned(loader, complete).error.kind == DriverErrorKind::LOAD_FAILED);
	CHECK_FALSE(IsMapped(complete.Path()));
}

TEST_CASE("The by-path source loads a library that exports every entry point", "[interface]") {
	SKIP_CASE_IF(FindLoadingSkipReason());
	TemporaryLibrary stub(ReadFile(REDSHIFT_TEST_STUB_DRIVER));
	ByPathDriverLoader loader;
	CHECK(RequireLoaded(LoadPinned(loader, stub)).api.Complete());
}

TEST_CASE("The by-path source names the same LoadedDriver on every later success", "[interface]") {
	SKIP_CASE_IF(FindLoadingSkipReason());
	TemporaryLibrary stub(ReadFile(REDSHIFT_TEST_STUB_DRIVER));
	ByPathDriverLoader loader;
	const LoadedDriver &first = RequireLoaded(LoadPinned(loader, stub));
	// One LoadedDriver per loader, and ByPathDriverSource keeps one loader for the process, as
	// the driver loader does.
	CHECK(LoadPinned(loader, stub).driver == &first);
}

TEST_CASE("After a success the by-path source reads neither variable", "[interface]") {
	SKIP_CASE_IF(FindLoadingSkipReason());
	TemporaryLibrary stub(ReadFile(REDSHIFT_TEST_STUB_DRIVER));
	TemporaryLibrary other(ReadFile(REDSHIFT_TEST_STUB_DRIVER) + std::string(1, '\0'));
	ByPathDriverLoader loader;
	const LoadedDriver &first = RequireLoaded(LoadPinned(loader, stub));
	CHECK(LoadUnset(loader).driver == &first);
	// Another spelling of the path, or another library: the first success stands, and nothing maps
	// the other library.
	CHECK(LoadThrough(loader, stub.Directory() + "/./librsodbc64.so", stub.Sha256()).driver == &first);
	CHECK(LoadPinned(loader, other).driver == &first);
	CHECK_FALSE(IsMapped(other.Path()));
}

#ifdef __linux__
// Hidden, so only RunChildCase runs it: it spends its process's ByPathDriverSource on a stub.
TEST_CASE("ByPathDriverSource in a child keeps its first outcome for the process", "[.by-path-child]") {
	const LoadedDriver &first = RequireLoaded(ByPathDriverSource());
	ScopedEnvironmentVariable pin(BY_PATH_DRIVER_SHA256_VARIABLE, nullptr);
	CHECK(ByPathDriverSource().driver == &first);
	ScopedEnvironmentVariable path(BY_PATH_DRIVER_PATH_VARIABLE, "/redshift-by-path-absent/librsodbc64.so");
	CHECK(ByPathDriverSource().driver == &first);
}

TEST_CASE("ByPathDriverSource keeps its first outcome for the process, as a child shows", "[interface]") {
	SKIP_CASE_IF(FindLoadingSkipReason());
	TemporaryLibrary stub(ReadFile(REDSHIFT_TEST_STUB_DRIVER));
	CHECK(RunChildCase(stub.Path(), stub.Sha256()) == 0);
	// The control: a child that cannot load the stub fails, so a passing child is not one that
	// ran nothing.
	CHECK(RunChildCase(stub.Path(), WRONG_PIN) != 0);
}
#endif

TEST_CASE("Concurrent first calls on one loader all get the one LoadedDriver", "[interface]") {
	SKIP_CASE_IF(FindLoadingSkipReason());
	TemporaryLibrary stub(ReadFile(REDSHIFT_TEST_STUB_DRIVER));
	ScopedEnvironmentVariable path_variable(BY_PATH_DRIVER_PATH_VARIABLE, stub.Path().c_str());
	ScopedEnvironmentVariable pin_variable(BY_PATH_DRIVER_SHA256_VARIABLE, stub.Sha256().c_str());
	ByPathDriverLoader loader;
	const int thread_count = 8;
	std::vector<const LoadedDriver *> drivers(thread_count, nullptr);
	std::atomic<int> ready {0};
	std::vector<std::thread> workers;
	for (int worker_index = 0; worker_index < thread_count; worker_index++) {
		workers.emplace_back([&, worker_index]() {
			ready++;
			while (ready < thread_count) {
				std::this_thread::yield();
			}
			drivers[worker_index] = loader.Load().driver;
		});
	}
	for (auto &worker : workers) {
		worker.join();
	}
	REQUIRE(drivers[0] != nullptr);
	for (const LoadedDriver *driver : drivers) {
		CHECK(driver == drivers[0]);
	}
}

TEST_CASE("The by-path source reports the origin, the build and the file it loaded", "[interface]") {
	SKIP_CASE_IF(FindLoadingSkipReason());
	TemporaryLibrary stub(ReadFile(REDSHIFT_TEST_STUB_DRIVER));
	ByPathDriverLoader loader;
	const auto &identity = RequireLoaded(LoadPinned(loader, stub)).identity;
	CHECK(identity.origin == DriverOrigin::TEST_BY_PATH);
	CHECK(identity.version == "sha256:" + stub.Sha256().substr(0, 12));
	CHECK(identity.Describe() ==
	      "test-by-path sha256:" + stub.Sha256().substr(0, 12) + " " + duckdb::DuckDB::Platform());
	CHECK_FALSE(identity.development);
	REQUIRE(identity.files.size() == 1);
	CHECK(identity.files[0].name == "librsodbc64.so");
	CHECK(identity.files[0].sha256 == stub.Sha256());
	CHECK(identity.directory == stub.Directory());
	CHECK(identity.set_sha256.empty());
}

TEST_CASE("The by-path source reports no refused or overridden environment", "[interface]") {
	SKIP_CASE_IF(FindLoadingSkipReason());
	TemporaryLibrary stub(ReadFile(REDSHIFT_TEST_STUB_DRIVER));
	ByPathDriverLoader loader;
	const LoadedDriver &driver = RequireLoaded(LoadPinned(loader, stub));
	CHECK(driver.refused_environment.empty());
	CHECK(driver.overridden_environment.empty());
}

TEST_CASE("The by-path source names no trust directory", "[interface]") {
	SKIP_CASE_IF(FindLoadingSkipReason());
	TemporaryLibrary stub(ReadFile(REDSHIFT_TEST_STUB_DRIVER));
	ByPathDriverLoader loader;
	CHECK(RequireLoaded(LoadPinned(loader, stub)).trust_directory.empty());
}

TEST_CASE("A library the by-path source loaded keeps its symbols to itself", "[interface]") {
	SKIP_CASE_IF(FindLoadingSkipReason());
	TemporaryLibrary stub(ReadFile(REDSHIFT_TEST_STUB_DRIVER));
	ByPathDriverLoader loader;
	RequireLoaded(LoadPinned(loader, stub));
	// RTLD_LOCAL, checked as the driver loader checks the shipped load.
	void *global = dlsym(RTLD_DEFAULT, "SQLDriverConnect");
	Dl_info where {};
	INFO("a global lookup finds SQLDriverConnect in "
	     << (global && dladdr(global, &where) && where.dli_fname ? where.dli_fname : "no image"));
	CHECK(global == nullptr);
}

TEST_CASE("PlatformDlopenFlags gives this build the driver loader's flags", "[interface]") {
#ifdef REDSHIFT_BY_PATH_DEEPBIND
	CHECK(PlatformDlopenFlags() == (RTLD_NOW | RTLD_LOCAL | RTLD_DEEPBIND));
#else
	// A sanitized build, or a platform without RTLD_DEEPBIND such as macOS.
	CHECK(PlatformDlopenFlags() == (RTLD_NOW | RTLD_LOCAL));
#endif
#if defined(__linux__) && !defined(REDSHIFT_BY_PATH_SANITIZER_BUILD) && !defined(REDSHIFT_BY_PATH_DEEPBIND)
	FAIL("an unsanitized Linux build loads with RTLD_DEEPBIND (the driver loader's flags)");
#endif
}

#ifdef __linux__
TEST_CASE("A library the by-path source loaded binds its own symbols first only under RTLD_DEEPBIND", "[interface]") {
	SKIP_CASE_IF(FindLoadingSkipReason());
	// The interposer defines the stub's probe too and joins the global scope, which an ordinary
	// load searches before the library's own definitions and an RTLD_DEEPBIND load searches after.
	// It stays loaded, because the stub's binding may point into it.
	REQUIRE(dlopen(REDSHIFT_TEST_INTERPOSER, RTLD_NOW | RTLD_GLOBAL) != nullptr);
	TemporaryLibrary stub(ReadFile(REDSHIFT_TEST_STUB_DRIVER));
	ByPathDriverLoader loader;
	RequireLoaded(LoadPinned(loader, stub));
	void *library = dlopen(stub.Path().c_str(), RTLD_NOW | RTLD_NOLOAD);
	REQUIRE(library != nullptr);
	const auto bound_probe = reinterpret_cast<int (*)()>(dlsym(library, "redshift_stub_bound_probe"));
	dlclose(library);
	REQUIRE(bound_probe != nullptr);
#ifdef REDSHIFT_BY_PATH_DEEPBIND
	CHECK(bound_probe() == 1);
#else
	CHECK(bound_probe() == 2);
#endif
}
#endif

TEST_CASE("FileContains finds a needle that spans two of its reads", "[interface]") {
	TemporaryDirectory directory(FindTemporaryRoot());
	const std::string path = directory.Register("artefact");
	WriteFile(path, std::string((1 << 20) - 3, 'x') + "ByPathDriverSource" + std::string(16, 'x'));
	CHECK(FileContains(path, "ByPathDriverSource"));
	CHECK_FALSE(FileContains(path, "OpenRedshiftConnection"));
}

TEST_CASE("The loadable extension keeps the function names the next case looks for", "[interface]") {
	CHECK(FileContains(REDSHIFT_LOADABLE_EXTENSION, "OpenRedshiftConnection"));
}

TEST_CASE("The loadable extension contains nothing from the by-path source", "[interface]") {
	// An unstripped artefact keeps the function's name; any artefact that links the source keeps
	// the variable name the source reads.
	CHECK_FALSE(FileContains(REDSHIFT_LOADABLE_EXTENSION, "ByPathDriverSource"));
	CHECK_FALSE(FileContains(REDSHIFT_LOADABLE_EXTENSION, BY_PATH_DRIVER_PATH_VARIABLE));
}

#ifdef __linux__
TEST_CASE("The client suite, which links the by-path source, contains what the extension case looks for",
          "[interface]") {
	CHECK(FileContains("/proc/self/exe", "ByPathDriverSource"));
	CHECK(FileContains("/proc/self/exe", BY_PATH_DRIVER_PATH_VARIABLE));
}
#endif

TEST_CASE("The by-path source loads the developer's own driver once per process", "[interface]") {
	SKIP_CASE_IF(FindDeveloperDriverSkipReason());
	SKIP_CASE_IF(FindHostSkipReason());
	// The only case that loads through this process's ByPathDriverSource, whose outcome is then
	// final; every other case loads through a loader of its own or in a child.
	const LoadedDriver &first = RequireLoaded(ByPathDriverSource());
	CHECK(first.api.Complete());
	CHECK(first.identity.origin == DriverOrigin::TEST_BY_PATH);
	ScopedEnvironmentVariable path(BY_PATH_DRIVER_PATH_VARIABLE, nullptr);
	ScopedEnvironmentVariable pin(BY_PATH_DRIVER_SHA256_VARIABLE, nullptr);
	CHECK(ByPathDriverSource().driver == &first);
}
