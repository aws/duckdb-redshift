// Helpers for the driver suite's cases that load the embedded driver or talk to it.
#pragma once

#include "catch.hpp"
#include "closed_port_smoke.hpp"
#include "driver_info.h"
#include "driver_loader_core.hpp"

#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <utility>
#include <vector>

#include <fcntl.h>
#include <sys/resource.h>
#include <sys/wait.h>
#include <unistd.h>
#if defined(__APPLE__)
#include <mach-o/fat.h>
#include <mach-o/loader.h>
#endif

namespace redshift_driver_test {

// Unsets every refused name, and the platform loader's own variables the loader refuses (macOS's DYLD_
// search variables), so a test that loads the embedded driver does not depend on the environment it was
// started in. For child processes, which then exit.
inline void ClearRefusedEnvironment() {
	for (const char *name : {REDSHIFT_DRIVER_REFUSED_ENVIRONMENT}) {
		::unsetenv(name);
	}
	for (const auto &name : duckdb::redshift_driver::detail::PlatformLoaderVariables()) {
		::unsetenv(name.c_str());
	}
}

// In a child just forked: the environment InChild and RerunHidden give it, with REDSHIFT_DRIVER_DIR naming
// `driver_dir`, so LoadEmbeddedDriver starts from nothing and writes nowhere else.
inline void PrepareChild(const std::string &driver_dir) {
	ClearRefusedEnvironment();
	::unsetenv("LD_LIBRARY_PATH");
	::setenv("REDSHIFT_DRIVER_DIR", driver_dir.c_str(), 1);
}

// Runs `body` in a child process with REDSHIFT_DRIVER_DIR naming a fresh private directory, so
// LoadEmbeddedDriver starts from nothing and writes nowhere else. Returns the child's exit status.
template <class BODY>
int InChild(const std::string &driver_dir, BODY body) {
	std::fflush(nullptr);
	const pid_t child = ::fork();
	REQUIRE(child >= 0);
	if (child == 0) {
		PrepareChild(driver_dir);
		::_exit(body());
	}
	int status = 0;
	REQUIRE(::waitpid(child, &status, 0) == child);
	REQUIRE(WIFEXITED(status));
	return WEXITSTATUS(status);
}

// Runs this executable again on the hidden case `tag`, as InChild prepares a child, with `preload` in
// LD_PRELOAD, the variables in `extra`, and its output in `<dir>/child-output.txt`. Returns the wait status.
inline int RerunHidden(const std::string &tag, const std::string &preload, const std::string &dir,
                       const std::vector<std::pair<std::string, std::string>> &extra = {}) {
	std::fflush(nullptr);
	const pid_t child = ::fork();
	REQUIRE(child >= 0);
	if (child == 0) {
		PrepareChild(dir);
		::setenv("LD_PRELOAD", preload.c_str(), 1);
		for (const auto &variable : extra) {
			::setenv(variable.first.c_str(), variable.second.c_str(), 1);
		}
		const rlimit no_core {0, 0}; // a crash leaves no core file
		::setrlimit(RLIMIT_CORE, &no_core);
		const int fd = ::open((dir + "/child-output.txt").c_str(), O_WRONLY | O_CREAT | O_TRUNC, 0600);
		if (fd >= 0) {
			::dup2(fd, 1);
			::dup2(fd, 2);
		}
		::execl("/proc/self/exe", "redshift_driver_tests", tag.c_str(), static_cast<char *>(nullptr));
		::_exit(127);
	}
	int status = 0;
	REQUIRE(::waitpid(child, &status, 0) == child);
	return status;
}

// `text` without spaces or newlines, so a child's Catch2 message matches however its reporter wrapped it.
inline std::string Unwrapped(const std::string &text) {
	std::string out;
	for (const char c : text) {
		if (c != ' ' && c != '\n') {
			out += c;
		}
	}
	return out;
}

// A local TCP port that nothing listens on.
inline int ClosedPort() {
	const int port = ClosedLocalPort();
	REQUIRE(port > 0);
	return port;
}

// Whether this run must take every case that needs something of the host (REDSHIFT_TEST_REQUIRE_HOST_LIBRARIES=1,
// as a run on a host that installs unixODBC, jemalloc and OpenSSL 3 sets): a host library that is not found,
// or a trace that cannot be taken, then fails its case instead of warning and stopping it, and so does the
// OpenSSL case on a host without libssl.so.3.
inline bool RequireHostLibraries() {
	const char *required = std::getenv("REDSHIFT_TEST_REQUIRE_HOST_LIBRARIES");
	return required != nullptr && std::string(required) == "1";
}

// A row this host cannot run, `why` saying what it lacks: the case warns that the row was not run, and its
// caller stops it; with REDSHIFT_TEST_REQUIRE_HOST_LIBRARIES=1 the case fails instead. The one rule for every
// row that needs something of the host.
inline void HostCannotRun(const std::string &why) {
	if (RequireHostLibraries()) {
		FAIL(why << ", and REDSHIFT_TEST_REQUIRE_HOST_LIBRARIES=1 requires this row to run");
	}
	WARN(why << "; this row was not run");
}

#if defined(__linux__) // glibc's system library directories (SystemLibraryDirectories)
// The host library `variable` names, else `name` in this platform's system library directories, else
// empty, when the case warns and stops; looked up on disk, because jemalloc cannot be dlopen'ed into a
// running process to find it. A library a variable names is used as it is, so a missing one fails the case;
// an empty variable counts as unset. With REDSHIFT_TEST_REQUIRE_HOST_LIBRARIES=1, a library that is not found
// fails the case instead. (On a host without libkeyutils.so.1 the unixODBC cases stop before this; the
// allocator rows need no driver prerequisite, as the refusal comes first.)
inline std::string HostLibrary(const char *variable, const std::string &name) {
	const char *named = std::getenv(variable);
	if (named != nullptr && *named != '\0') {
		return named;
	}
	for (const auto &dir : duckdb::redshift_driver::detail::SystemLibraryDirectories()) {
		const std::string path = dir + "/" + name;
		if (::access(path.c_str(), R_OK) == 0) {
			return path;
		}
	}
	HostCannotRun(name + " is not installed and " + variable + " is not set");
	return std::string();
}

#endif

#if defined(__APPLE__)
// The big-endian 32-bit number at `at`, as a universal Mach-O file's header and slice table hold on every host.
inline uint32_t BigEndian32(const unsigned char *at) {
	return uint32_t {at[0]} << 24 | uint32_t {at[1]} << 16 | uint32_t {at[2]} << 8 | uint32_t {at[3]};
}

// The CPU types a Mach-O file has code for, from its first `size` bytes at `head`: each slice's, in order, of a
// universal file, or a thin 64-bit file's own (in its CPU's byte order, which for both macOS architectures is this
// process's, little-endian); empty for anything else, or for a slice table `size` cuts short.
inline std::vector<uint32_t> MachOCpuTypes(const unsigned char *head, std::size_t size) {
	if (size < sizeof(fat_header)) {
		return {};
	}
	const uint32_t magic = BigEndian32(head);
	if (magic == FAT_MAGIC || magic == FAT_MAGIC_64) {
		// The slice count, then one description per slice, its CPU type first.
		const std::size_t slice_size = magic == FAT_MAGIC ? sizeof(fat_arch) : sizeof(fat_arch_64);
		const uint32_t slices = BigEndian32(head + 4);
		std::vector<uint32_t> types;
		for (uint32_t i = 0; i < slices; i++) {
			const std::size_t at = sizeof(fat_header) + i * slice_size;
			if (at + sizeof(uint32_t) > size) {
				return {};
			}
			types.push_back(BigEndian32(head + at));
		}
		return types;
	}
	mach_header_64 header;
	if (size < sizeof(header)) {
		return {};
	}
	std::memcpy(&header, head, sizeof(header));
	if (header.magic != MH_MAGIC_64) {
		return {};
	}
	return {static_cast<uint32_t>(header.cputype)};
}
#endif

} // namespace redshift_driver_test
