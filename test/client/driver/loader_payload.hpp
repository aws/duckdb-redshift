// A test payload for the run-time sequence: a test-input library named like the real driver, with
// one notice beside it, and the inputs that keep a loader test from depending on the host's
// platform loader search list. Shared by the loader, environment and search-list tests.
#pragma once

#include "driver_loader_core.hpp"
#include "loader_test.hpp"
#include "sha256.hpp"
#include "temp_dir.hpp"

#include "catch.hpp"

#include <fstream>
#include <iterator>
#include <string>
#include <vector>

#include <climits>
#include <cstdlib>

#include <dlfcn.h>
#include <fcntl.h>
#include <unistd.h>
#if defined(__APPLE__)
#include <mach-o/dyld.h>
#endif

namespace redshift_driver_test {

// This executable's own path, resolved.
inline std::string ExecutablePath() {
#if defined(__APPLE__)
	char given[PATH_MAX];
	uint32_t size = sizeof(given);
	REQUIRE(::_NSGetExecutablePath(given, &size) == 0);
	char buffer[PATH_MAX];
	REQUIRE(::realpath(given, buffer) != nullptr);
	std::string exe(buffer);
#else
	char buffer[4096];
	const auto length = ::readlink("/proc/self/exe", buffer, sizeof(buffer) - 1);
	REQUIRE(length > 0);
	std::string exe(buffer, static_cast<size_t>(length));
#endif
	return exe;
}

// A test input, named relative to the directory of this executable, where the build puts them.
inline std::string TestInputPath(const std::string &relative) {
	const auto exe = ExecutablePath();
	return exe.substr(0, exe.rfind('/')) + "/" + relative;
}

// Whether the host has the library the embedded driver needs beyond a base system; a
// case that loads the real driver warns and stops where it does not, as a minimal container does.
inline bool HostHasDriverPrerequisites() {
#if defined(__APPLE__)
	return true; // the macOS driver links only system libraries (policy/host-prerequisites.txt has no row)
#else
	void *handle = ::dlopen("libkeyutils.so.1", RTLD_LAZY);
	if (handle == nullptr) {
		// REDSHIFT_TEST_REQUIRE_HOST_LIBRARIES does not cover this library, so a caller that must load the real
		// driver may match this text in the output: keep it stable.
		WARN("this host has no libkeyutils.so.1, which the embedded driver needs; the real driver is not loaded here");
		return false;
	}
	::dlclose(handle);
	return true;
#endif
}

// The bytes of the file at `path`, or "" when it cannot be opened (a log a child may never have written).
inline std::string ReadFileIfThere(const std::string &path) {
	std::ifstream in(path, std::ios::binary);
	return std::string(std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>());
}

// The bytes of the file at `path`, which must be there: one that cannot be opened fails the case.
inline std::string ReadWholeFile(const std::string &path) {
	REQUIRE(std::ifstream(path, std::ios::binary).good());
	return ReadFileIfThere(path);
}

// A loader configuration for one test-input library and one notice.
struct TestPayload {
	std::string driver;
	std::string notice = "a notice";
	duckdb::redshift_driver::detail::LoaderConfig config;

	explicit TestPayload(const std::string &library_path) : driver(ReadWholeFile(library_path)) {
		Build();
	}
	// Bytes that are not a library.
	TestPayload(const std::string &bytes, bool) : driver(bytes) {
		Build();
	}
	void Build() {
		using namespace duckdb::redshift_driver::detail;
		config.set.files.clear();
		std::string manifest;
		for (auto *bytes : {&driver, &notice}) {
			SetFile file;
			file.name = bytes == &driver ? "librsodbc64.so" : "NOTICE";
			file.data = reinterpret_cast<const unsigned char *>(bytes->data());
			file.size = bytes->size();
			file.sha256 = Sha256Hex(bytes->data(), bytes->size());
			manifest += file.sha256 + "  " + std::to_string(file.size) + "  " + file.name + "\n";
			config.set.files.push_back(file);
		}
		config.set.set_sha256 = Sha256Hex(manifest.data(), manifest.size());
		config.identity.version = "9.9.9.9";
		config.identity.platform = "test_platform";
		config.identity.set_sha256 = config.set.set_sha256;
		config.refused_environment = {"REDSHIFT_TEST_REFUSED"};
		config.overridden_environment = {{"REDSHIFT_TEST_KNOWN", "Keyword"}};
#if defined(__linux__)
		config.needed = ElfNeededLibraries(config.set.files[0].data, config.set.files[0].size);
#endif
	}
	std::string SetDirectory(const std::string &root_base) const {
		return root_base + "/.redshift-driver/" + config.set.set_sha256;
	}
};

// A search list with nothing in it, so a test that loads does not depend on the host's.
inline bool NoSearchDirectories(std::vector<std::string> &) {
	return true;
}

// The search inputs with no search list and no library cache, so a test does not depend on the host's (Linux;
// macOS has neither to check).
inline void WithoutHostSearch(duckdb::redshift_driver::detail::SearchInputs &search) {
#if defined(__linux__)
	search.list = NoSearchDirectories;
	search.cache_path = nullptr;
#else
	(void)search;
#endif
}

// The inputs of a load in a test: base directories, no search list and no library cache. The test-input
// drivers need only libc.so.6, which is loaded; the one that also needs libredshift_absent_test.so.1 is
// refused for it as missing, unless a case gives a cache that names it.
inline duckdb::redshift_driver::detail::LoadInputs LoadFrom(const std::string &first,
                                                            const std::string &second = std::string()) {
	duckdb::redshift_driver::detail::LoadInputs inputs;
	inputs.bases = Inputs(first, second);
	WithoutHostSearch(inputs.search);
	inputs.hooks.on_platform_loader_call = CountLoaderCall;
	return inputs;
}

} // namespace redshift_driver_test
