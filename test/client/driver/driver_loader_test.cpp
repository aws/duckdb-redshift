// The run-time sequence and the Linux loader: DriverLoader on test-input libraries in temporary base
// directories, and LoadEmbeddedDriver with the real driver, each of the latter in a child process so
// that its one-per-process state starts fresh.
#include "driver_info.h"
#include "driver_loader_core.hpp"
#include "embedded_test_helpers.hpp"
#include "loader_payload.hpp"
#include "sha256.hpp"
#include "temp_dir.hpp"

#include "catch.hpp"

#include <algorithm>
#include <cerrno>
#include <climits>
#include <atomic>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <functional>
#include <new>
#include <iterator>
#include <sstream>
#include <string>
#include <thread>
#include <vector>

#include <dirent.h>
#include <dlfcn.h>
#include <fcntl.h>
#include <sys/resource.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>
#if defined(__linux__)
#include <link.h>
#include <sys/statfs.h>
#else
#include <mach-o/dyld.h>
#endif

using namespace duckdb::redshift_driver;
using namespace duckdb::redshift_driver::detail;
using redshift_driver_test::ClosedPort;
using redshift_driver_test::DescriptorLimit;
using redshift_driver_test::DescriptorLimitInUse;
using redshift_driver_test::InChild;
using redshift_driver_test::LoadFrom;
using redshift_driver_test::PlatformLoaderCalls;
using redshift_driver_test::PrepareDriverSet;
using redshift_driver_test::ReadWholeFile;
using redshift_driver_test::ScopedEnv;
using redshift_driver_test::TempDir;
using redshift_driver_test::TestPayload;

namespace {

int ConstructorRuns(const std::string &log) {
	std::ifstream in(log);
	return static_cast<int>(std::count(std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>(), '\n'));
}

// The types: an error is final exactly when the platform loader was called.
void CheckStage(const DriverLoadResult &result, int calls_before) {
	REQUIRE_FALSE(result.ok());
	CHECK(IsFinalKind(result.error.kind) == (PlatformLoaderCalls() > calls_before));
}

// No message of any outcome holds a path, a credential or an environment value.
void CheckNoPath(const DriverError &error, const std::string &directory) {
	CHECK(error.message.find(directory) == std::string::npos);
	CHECK(error.message.find("/proc/") == std::string::npos);
	CHECK(error.message.find("/tmp") == std::string::npos);
	CHECK(error.message.find("redshift-driver-test") == std::string::npos);
}

} // namespace

TEST_CASE("A complete driver loads once, and every later call returns the same LoadedDriver", "[driver][loader]") {
	TempDir tmp;
	ScopedEnv log("REDSHIFT_TEST_CONSTRUCTOR_LOG", (tmp / "constructor.log").c_str());
	TestPayload payload(redshift_driver_test::TestInputPath(REDSHIFT_TEST_DRIVER_COMPLETE));
	DriverLoader loader(payload.config);
	const int calls = PlatformLoaderCalls();
	auto first = loader.Load(LoadFrom(tmp.path()));
	REQUIRE(first.ok());
	CHECK(first.driver->api.Complete());
	CHECK(first.driver->identity.version == "9.9.9.9");
	CHECK(first.driver->identity.set_sha256 == payload.config.set.set_sha256);
	CHECK(first.driver->identity.directory == payload.SetDirectory(tmp.path()));
	REQUIRE(first.driver->identity.files.size() == 2);
	CHECK(first.driver->identity.files[0].name == "librsodbc64.so");
	CHECK(first.driver->identity.files[0].sha256 == payload.config.set.files[0].sha256);
	CHECK(first.driver->trust_directory.empty());
	CHECK(first.driver->refused_environment == std::vector<std::string> {"REDSHIFT_TEST_REFUSED"});
	REQUIRE(first.driver->overridden_environment.size() == 1);
	CHECK(first.driver->overridden_environment[0].keyword == "Keyword");
	CHECK(PlatformLoaderCalls() == calls + 1);
	CHECK(ConstructorRuns(tmp / "constructor.log") == 1);

	// Even with different inputs, the stored outcome is returned, and nothing is loaded again.
	auto second = loader.Load(LoadFrom("/nonexistent"));
	CHECK(second.driver == first.driver);
	CHECK(PlatformLoaderCalls() == calls + 1);
	CHECK(ConstructorRuns(tmp / "constructor.log") == 1);
}

#if defined(__linux__)
TEST_CASE("The driver is loaded as /proc/thread-self/fd/<n>, hashed through that descriptor, and its flags are the "
          "loader's",
          "[driver][loader]") {
	TempDir tmp;
	TestPayload payload(redshift_driver_test::TestInputPath(REDSHIFT_TEST_DRIVER_COMPLETE));
	{
		DriverLoader loader(payload.config);
		auto inputs = LoadFrom(tmp.path());
		// The loader takes the driver's DT_NEEDED names from its config, whatever the inputs say.
		inputs.search.needed = {"libredshift_not_there.so.1"};
		auto result = loader.Load(inputs);
		REQUIRE(result.ok());
		Dl_info where;
		REQUIRE(::dladdr(reinterpret_cast<void *>(result.driver->api.AllocHandle), &where) != 0);
		if (::access("/proc/thread-self/fd", F_OK) == 0) {
			CHECK(std::string(where.dli_fname).find("/proc/thread-self/fd/") == 0);
		}
	}
	// Where the descriptor has no name (no /proc), the checked path is loaded.
	TempDir other;
	DriverLoader by_path(payload.config);
	auto inputs = LoadFrom(other.path());
	inputs.hooks.descriptor_path = [](int) {
		return std::string();
	};
	auto result = by_path.Load(inputs);
	REQUIRE(result.ok());
	Dl_info where;
	REQUIRE(::dladdr(reinterpret_cast<void *>(result.driver->api.AllocHandle), &where) != 0);
	CHECK(std::string(where.dli_fname) == payload.SetDirectory(other.path()) + "/librsodbc64.so");
	// In every build, a sanitizer build included.
	CHECK(PlatformDlopenFlags() == (RTLD_NOW | RTLD_LOCAL | RTLD_DEEPBIND));
}
#endif

#if defined(__APPLE__)
TEST_CASE("The driver is loaded by its checked path, with the loader's flags", "[driver][loader]") {
	TempDir tmp;
	TestPayload payload(redshift_driver_test::TestInputPath(REDSHIFT_TEST_DRIVER_COMPLETE));
	DriverLoader loader(payload.config);
	auto result = loader.Load(LoadFrom(tmp.path()));
	REQUIRE(result.ok());
	Dl_info where;
	REQUIRE(::dladdr(reinterpret_cast<void *>(result.driver->api.AllocHandle), &where) != 0);
	CHECK(std::string(where.dli_fname) == payload.SetDirectory(tmp.path()) + "/librsodbc64.so");
	CHECK(DescriptorPath(0).empty());
	CHECK(PlatformDlopenFlags() == (RTLD_NOW | RTLD_LOCAL)); // macOS has no RTLD_DEEPBIND
}
#endif

#if defined(__linux__)
TEST_CASE("A name an object is already loaded under is not reused: the descriptor moves to another number",
          "[driver][loader]") {
	TempDir tmp;
	TestPayload payload(redshift_driver_test::TestInputPath(REDSHIFT_TEST_DRIVER_COMPLETE));
	static std::vector<std::string> asked;
	asked.clear();
	DriverLoader loader(payload.config);
	auto inputs = LoadFrom(tmp.path());
	// The first two names are taken, as if earlier objects were loaded under them.
	inputs.hooks.name_in_use = [](const std::string &name) {
		asked.push_back(name);
		return asked.size() <= 2;
	};
	auto result = loader.Load(inputs);
	REQUIRE(result.ok());
	REQUIRE(asked.size() == 3);
	CHECK(asked[0] != asked[1]);
	CHECK(asked[1] != asked[2]);
	Dl_info where;
	REQUIRE(::dladdr(reinterpret_cast<void *>(result.driver->api.AllocHandle), &where) != 0);
	CHECK(std::string(where.dli_fname) == asked[2]);

	// Every name taken: nothing is loaded, and the base fails (it is not a final outcome).
	TempDir all_taken;
	DriverLoader refused(payload.config);
	auto taken = LoadFrom(all_taken.path());
	taken.hooks.name_in_use = [](const std::string &) {
		return true;
	};
	const int calls = PlatformLoaderCalls();
	auto none = refused.Load(taken);
	REQUIRE_FALSE(none.ok());
	CHECK(none.error.kind == DriverErrorKind::NO_USABLE_DIRECTORY);
	CHECK(none.error.attempts.at(0).reason ==
	      "the extension's directory: no unused name was found for the driver file's descriptor");
	CHECK(PlatformLoaderCalls() == calls);
	CheckNoPath(none.error, all_taken.path());
}
#endif

TEST_CASE("A name for the descriptor that leads to another file is not used: the checked path is loaded",
          "[driver][loader]") {
	TempDir tmp;
	TestPayload payload(redshift_driver_test::TestInputPath(REDSHIFT_TEST_DRIVER_COMPLETE));
	// A different library (one that lacks an entry point), as the file a descriptor name leads to. Were it
	// loaded in place of the checked file, the load would end in MISSING_ENTRY_POINT.
	static std::string elsewhere;
	elsewhere = redshift_driver_test::TestInputPath(REDSHIFT_TEST_DRIVER_MISSING_ENTRY);
	DriverLoader loader(payload.config);
	auto inputs = LoadFrom(tmp.path());
	inputs.hooks.descriptor_path = [](int) {
		return elsewhere;
	};
	auto result = loader.Load(inputs);
	REQUIRE(result.ok());
	Dl_info where;
	REQUIRE(::dladdr(reinterpret_cast<void *>(result.driver->api.AllocHandle), &where) != 0);
	CHECK(std::string(where.dli_fname) == payload.SetDirectory(tmp.path()) + "/librsodbc64.so");
}

TEST_CASE("A loaded file that is not the checked one is refused after the load, and the outcome is final",
          "[driver][loader]") {
	// The check after the load (LoadedFileIsChecked) finds another file only where dyld took one from a DYLD_
	// variable it read at start-up, so a stand-in says the file differs, on every platform. It is asked about an
	// address in the loaded file, with the checked file's identity.
	CHECK((LoadHooks().loaded_file_is_checked == &LoadedFileIsChecked));
	TempDir tmp;
	TestPayload payload(redshift_driver_test::TestInputPath(REDSHIFT_TEST_DRIVER_COMPLETE));
	static const void *asked_address;
	static struct stat asked_checked;
	asked_address = nullptr;
	DriverLoader loader(payload.config);
	auto inputs = LoadFrom(tmp.path());
	inputs.hooks.loaded_file_is_checked = [](const void *address, const struct stat &checked) {
		asked_address = address;
		asked_checked = checked;
		return false;
	};
	const int calls = PlatformLoaderCalls();
	auto result = loader.Load(inputs);
	REQUIRE_FALSE(result.ok());
	CHECK(result.error.kind == DriverErrorKind::LOAD_FAILED);
	CHECK(result.error.message ==
	      "the Redshift ODBC driver could not be loaded (the extension's directory): the platform loader loaded a file "
	      "other than the one that was checked (start the program without DYLD_ variables set to connect)");
	REQUIRE_FALSE(result.error.attempts.empty());
	CHECK(result.error.attempts.back().directory == payload.SetDirectory(tmp.path()));
	CHECK(result.error.attempts.back().reason ==
	      "the loaded file is not the checked one (no absolute name, or another inode)");
	CheckNoPath(result.error, tmp.path());
	CHECK(PlatformLoaderCalls() == calls + 1);
	const auto driver_file = payload.SetDirectory(tmp.path()) + "/" + payload.config.set.files[0].name;
	struct stat set_file;
	REQUIRE(::stat(driver_file.c_str(), &set_file) == 0);
	CHECK(asked_checked.st_dev == set_file.st_dev);
	CHECK(asked_checked.st_ino == set_file.st_ino);
	// The address is in the file that loaded, which the platform's own check takes as the checked one.
	Dl_info where;
	CHECK(::dladdr(asked_address, &where) != 0);
	CHECK(LoadedFileIsChecked(asked_address, set_file));
	// Final: the same answer on the next call, which loads nothing.
	CHECK(loader.Load(LoadFrom(tmp.path())).error.message == result.error.message);
	CHECK(PlatformLoaderCalls() == calls + 1);
}

#if defined(__linux__)
TEST_CASE("A file put in the driver's place after it was hashed is not what loads: the descriptor kept is the "
          "hashed file's",
          "[driver][loader]") {
	TempDir tmp;
	TestPayload payload(redshift_driver_test::TestInputPath(REDSHIFT_TEST_DRIVER_COMPLETE));
	static std::string set_file, replacement;
	set_file = payload.SetDirectory(tmp.path()) + "/librsodbc64.so";
	replacement = tmp / "replacement";
	std::ofstream(replacement) << "not the driver";
	DriverLoader loader(payload.config);
	auto inputs = LoadFrom(tmp.path());
	// Between the hash of the first file and the end of the check, the name is made to lead to another file.
	inputs.hooks.after_hash = [](const std::string &path) {
		if (path == set_file) {
			(void)::rename(replacement.c_str(), path.c_str());
		}
	};
	auto result = loader.Load(inputs);
	REQUIRE(result.ok());
	CHECK(ReadWholeFile(set_file) == "not the driver");
	Dl_info where;
	REQUIRE(::dladdr(reinterpret_cast<void *>(result.driver->api.AllocHandle), &where) != 0);
	if (::access("/proc/thread-self/fd", F_OK) == 0) {
		CHECK(std::string(where.dli_fname).find("/proc/thread-self/fd/") == 0);
	}
}
#endif

#if defined(__linux__)
TEST_CASE("A descriptor that cannot be duplicated is reported as that, with the error", "[driver][loader]") {
	TempDir tmp;
	TestPayload payload(redshift_driver_test::TestInputPath(REDSHIFT_TEST_DRIVER_COMPLETE));
	DriverLoader loader(payload.config);
	auto inputs = LoadFrom(tmp.path());
	// Every name is taken, so the loader looks for a free number; the limit is lowered so that none above
	// the descriptor's own can be had (F_DUPFD fails with EINVAL for a minimum at or above the limit).
	inputs.hooks.name_in_use = [](const std::string &) {
		return true;
	};
	const DescriptorLimit limit;
	inputs.hooks.descriptor_path = [](int fd) {
		DescriptorLimitInUse()->Lower(static_cast<rlim_t>(fd) + 1);
		return DescriptorPath(fd);
	};
	const int calls = PlatformLoaderCalls();
	auto result = loader.Load(inputs);
	limit.Restore();
	REQUIRE_FALSE(result.ok());
	CHECK(result.error.kind == DriverErrorKind::NO_USABLE_DIRECTORY);
	REQUIRE_FALSE(result.error.attempts.empty());
	const auto reason = result.error.attempts[0].reason;
	INFO(reason);
	CHECK(reason.find("the driver file's descriptor cannot be duplicated to look for an unused name: ") !=
	      std::string::npos);
	CHECK(reason.find("no unused name") == std::string::npos);
	CHECK(PlatformLoaderCalls() == calls);
}
#endif

TEST_CASE("A driver missing an entry point gives MISSING_ENTRY_POINT once, and loads no second copy",
          "[driver][loader]") {
	TempDir tmp;
	REQUIRE(::mkdir((tmp / "second").c_str(), 0700) == 0);
	ScopedEnv log("REDSHIFT_TEST_CONSTRUCTOR_LOG", (tmp / "constructor.log").c_str());
	TestPayload payload(redshift_driver_test::TestInputPath(REDSHIFT_TEST_DRIVER_MISSING_ENTRY));
	DriverLoader loader(payload.config);
	const int calls = PlatformLoaderCalls();
	auto first = loader.Load(LoadFrom(tmp.path(), tmp / "second"));
	REQUIRE_FALSE(first.ok());
	CHECK(first.error.kind == DriverErrorKind::MISSING_ENTRY_POINT);
	CHECK(first.error.message == "the Redshift ODBC driver does not export SQLGetDiagField");
	CheckStage(first, calls);
	auto second = loader.Load(LoadFrom(tmp.path(), tmp / "second"));
	CHECK(second.error.kind == DriverErrorKind::MISSING_ENTRY_POINT);
	CHECK(second.error.message == first.error.message);
	CHECK(PlatformLoaderCalls() == calls + 1);
	CHECK(ConstructorRuns(tmp / "constructor.log") == 1);
	// The handle was kept: the library is still loaded, under the name it was loaded by.
	CHECK(first.error.attempts.at(0).directory == payload.SetDirectory(tmp.path()));
	// The second base was never prepared.
	CHECK(::access((tmp / "second/.redshift-driver").c_str(), F_OK) != 0);
}

#if defined(__linux__)
TEST_CASE("A library the driver needs that neither the library cache nor a system directory has is "
          "MISSING_PREREQUISITE, before anything is written, and final",
          "[driver][loader]") {
	TempDir tmp;
	REQUIRE(::mkdir((tmp / "second").c_str(), 0700) == 0);
	TestPayload payload(redshift_driver_test::TestInputPath(REDSHIFT_TEST_DRIVER_NEEDS_ABSENT));
	payload.config.prerequisites = {"libredshift_absent_test.so.1"};
	payload.config.prerequisite_hint = "it comes from the test's package";
	DriverLoader loader(payload.config);
	const int calls = PlatformLoaderCalls();
	// A library cache that does not exist, so nothing names the library, which nothing has loaded.
	const auto no_cache = tmp / "no-ld.so.cache";
	auto inputs = LoadFrom(tmp.path(), tmp / "second");
	inputs.search.cache_path = no_cache.c_str();
	inputs.search.system_directories = {tmp / "second"}; // no copy there either
	auto result = loader.Load(inputs);
	REQUIRE_FALSE(result.ok());
	CHECK(result.error.kind == DriverErrorKind::MISSING_PREREQUISITE);
	CHECK(result.error.message == "the Redshift ODBC driver needs libredshift_absent_test.so.1, which this host does "
	                              "not have; it comes from the test's package; install it (or, if "
	                              "it is installed, have an administrator run ldconfig), then restart the program and "
	                              "connect again");
	// Found before the bases are chosen: no loader call, nothing written in either base.
	CHECK(PlatformLoaderCalls() == calls);
	CHECK(::access((tmp / ".redshift-driver").c_str(), F_OK) != 0);
	CHECK(::access((tmp / "second/.redshift-driver").c_str(), F_OK) != 0);
	REQUIRE(result.error.attempts.size() == 1);
	CHECK(result.error.attempts[0].directory == no_cache);
	CHECK(result.error.attempts[0].reason ==
	      "neither the library cache nor a system directory has a file for libredshift_absent_test.so.1");
	// Final, as the message says: the same answer, with nothing looked at again.
	CHECK(IsFinalKind(result.error.kind));
	CHECK(loader.Load(inputs).error.message == result.error.message);
	CHECK(PlatformLoaderCalls() == calls);
	CHECK(::access((tmp / ".redshift-driver").c_str(), F_OK) != 0);

	// A library that is not a listed prerequisite: the same kind and remedy, without the package hint, which is
	// for the listed ones; no cache at all is the same as one that names nothing.
	TempDir other;
	TestPayload unlisted(redshift_driver_test::TestInputPath(REDSHIFT_TEST_DRIVER_NEEDS_ABSENT));
	unlisted.config.prerequisites = {"libother.so.1"};
	unlisted.config.prerequisite_hint = "it comes from the test's package";
	DriverLoader plain(unlisted.config);
	auto missing = plain.Load(LoadFrom(other.path()));
	CHECK(missing.error.kind == DriverErrorKind::MISSING_PREREQUISITE);
	CHECK(missing.error.message == "the Redshift ODBC driver needs libredshift_absent_test.so.1, which this host does "
	                               "not have; install it (or, if it is installed, have an "
	                               "administrator run ldconfig), then restart the program and connect again");
	CHECK(missing.error.attempts.empty()); // no cache path to name
	CheckNoPath(missing.error, other.path());
}
#endif

TEST_CASE("A file the platform loader refuses gives LOAD_FAILED after exactly one loader call", "[driver][loader]") {
	TempDir tmp;
	REQUIRE(::mkdir((tmp / "second").c_str(), 0700) == 0);
	TestPayload payload("this is not a shared library", true);
	DriverLoader loader(payload.config);
	const int calls = PlatformLoaderCalls();
	auto first = loader.Load(LoadFrom(tmp.path(), tmp / "second"));
	REQUIRE_FALSE(first.ok());
	CHECK(first.error.kind == DriverErrorKind::LOAD_FAILED);
	CHECK(first.error.message == "the Redshift ODBC driver could not be loaded (the extension's directory): the "
	                             "platform loader refused the driver: a library it needs is missing, unreadable or "
	                             "incompatible");
	CheckNoPath(first.error, tmp.path());
	CheckStage(first, calls);
	// The full text and the directory are in the attempts, for tools the process owner runs.
	REQUIRE(first.error.attempts.size() == 1);
	CHECK(first.error.attempts[0].directory == payload.SetDirectory(tmp.path()));
	CHECK_FALSE(first.error.attempts[0].reason.empty());
	CHECK(PlatformLoaderCalls() == calls + 1);
	auto second = loader.Load(LoadFrom(tmp.path(), tmp / "second"));
	CHECK(second.error.kind == DriverErrorKind::LOAD_FAILED);
	CHECK(second.error.message == first.error.message);
	CHECK(PlatformLoaderCalls() == calls + 1);
	CHECK(::access((tmp / "second/.redshift-driver").c_str(), F_OK) != 0);
}

TEST_CASE("A final failure after an earlier base was refused names that base, by its role and its rule",
          "[driver][loader]") {
	TempDir tmp;
	REQUIRE(::mkdir((tmp / "second").c_str(), 0700) == 0);
	TestPayload payload("this is not a shared library", true);
	DriverLoader loader(payload.config);
	static std::string first_prefix;
	first_prefix = tmp.path() + "/.redshift-driver/";
	auto inputs = LoadFrom(tmp.path(), tmp / "second");
	// The first base is on a noexec file system at the load check; the second is fine and then the platform
	// loader refuses the file, so the base tried before is named after the final message.
	inputs.hooks.no_exec_state = [](const std::string &dir) {
		return dir.compare(0, first_prefix.size(), first_prefix) == 0 ? 1 : 0;
	};
	auto result = loader.Load(inputs);
	REQUIRE_FALSE(result.ok());
	CHECK(result.error.kind == DriverErrorKind::LOAD_FAILED);
	CHECK(result.error.message ==
	      "the Redshift ODBC driver could not be loaded (the per-user cache): the platform loader refused the driver: "
	      "a library it needs is missing, unreadable or incompatible (tried before: the extension's directory: the "
	      "driver set is on a file system mounted noexec)");
	CheckNoPath(result.error, tmp.path());
	// Final: the same answer on the next call, with no third try.
	CHECK(loader.Load(inputs).error.message == result.error.message);
}

TEST_CASE("A base whose volume the platform's rule refuses is refused before anything is made there, and the next "
          "base is used",
          "[driver][loader]") {
	// The platform's volume rule (VolumeRule: macOS refuses a volume that ignores ownership; Linux has none) is a
	// BaseInputs hook, so the common code's call is tested on every platform with a rule that refuses one base.
	CHECK((CurrentBaseInputs().volume_rule == &VolumeRule));
	CHECK((redshift_driver_test::Inputs("", "").volume_rule == &VolumeRule));
	// The ACL hook too (macOS also has AclRule with AclEntryReads, hence the cast to the four-argument one).
	CHECK((CurrentBaseInputs().acl_rule == static_cast<AclRuleHook>(&AclRule)));
	TempDir tmp;
	if (!redshift_driver_test::PrivateAncestry(tmp)) {
		return;
	}
	REQUIRE(::mkdir((tmp / "ext").c_str(), 0700) == 0);
	REQUIRE(::mkdir((tmp / "cache").c_str(), 0700) == 0);
	static std::string refused;
	refused = tmp / "ext";
	auto inputs = LoadFrom(tmp / "ext", tmp / "cache");
	inputs.bases.volume_rule = [](const std::string &dir) {
		return dir == refused ? std::string("the driver root is on a volume this test refuses") : std::string();
	};
	DriverRoots roots(inputs.bases, ::geteuid());
	std::string root;
	REQUIRE(roots.Next(root));
	CHECK(root == tmp / "cache/.redshift-driver");
	REQUIRE(roots.attempts.size() == 1);
	CHECK(roots.attempts[0].directory == tmp / "ext");
	CHECK(roots.attempts[0].reason == "the extension's directory: the driver root is on a volume this test refuses");
	CHECK(::access((tmp / "ext/.redshift-driver").c_str(), F_OK) != 0);
	// Through the loader: the driver goes to the next base, and the refused one is left as it was.
	TestPayload payload(redshift_driver_test::TestInputPath(REDSHIFT_TEST_DRIVER_COMPLETE));
	DriverLoader loader(payload.config);
	auto result = loader.Load(inputs);
	REQUIRE(result.ok());
	CHECK(result.driver->identity.directory.find(tmp / "cache/.redshift-driver/") == 0);
	CHECK(::access((tmp / "ext/.redshift-driver").c_str(), F_OK) != 0);
}

TEST_CASE("A driver root whose own volume the platform's rule refuses (one mounted there) is refused, and the next "
          "base is used",
          "[driver][loader]") {
	// The owner and mode checks on the root would see such a volume's owners as the asker's, so the rule is asked
	// for the root as well as for its base.
	TempDir tmp;
	if (!redshift_driver_test::PrivateAncestry(tmp)) {
		return;
	}
	REQUIRE(::mkdir((tmp / "ext").c_str(), 0700) == 0);
	REQUIRE(::mkdir((tmp / "cache").c_str(), 0700) == 0);
	static std::string refused;
	refused = tmp / "ext/.redshift-driver";
	auto inputs = LoadFrom(tmp / "ext", tmp / "cache");
	inputs.bases.volume_rule = [](const std::string &dir) {
		return dir == refused ? std::string("the driver root is on a volume this test refuses") : std::string();
	};
	DriverRoots roots(inputs.bases, ::geteuid());
	std::string root;
	REQUIRE(roots.Next(root));
	CHECK(root == tmp / "cache/.redshift-driver");
	REQUIRE(roots.attempts.size() == 1);
	CHECK(roots.attempts[0].directory == refused);
	CHECK(roots.attempts[0].reason == "the extension's directory: the driver root is on a volume this test refuses");
}

TEST_CASE("A failure before the loader call is not stored: once the directory is fixed, the next call loads",
          "[driver][loader]") {
	TempDir tmp;
	if (!redshift_driver_test::PrivateAncestry(tmp)) {
		return;
	}
	REQUIRE(::mkdir((tmp / "ext").c_str(), 0700) == 0);
	REQUIRE(::chmod((tmp / "ext").c_str(), 0770) == 0);
	TestPayload payload(redshift_driver_test::TestInputPath(REDSHIFT_TEST_DRIVER_COMPLETE));
	DriverLoader loader(payload.config);
	const int calls = PlatformLoaderCalls();
	auto refused = loader.Load(LoadFrom(tmp / "ext"));
	REQUIRE_FALSE(refused.ok());
	CHECK(refused.error.kind == DriverErrorKind::NO_USABLE_DIRECTORY);
	CHECK(refused.error.message.find("REDSHIFT_DRIVER_DIR") != std::string::npos);
	REQUIRE(refused.error.attempts.size() == 2);
	CHECK(refused.error.attempts[0].directory == tmp / "ext");
	CheckStage(refused, calls);
	CHECK(PlatformLoaderCalls() == calls);

	// REDSHIFT_DRIVER_DIR set to a safe directory takes effect at the next call.
	REQUIRE(::mkdir((tmp / "admin").c_str(), 0700) == 0);
	auto inputs = LoadFrom(tmp / "ext");
	inputs.bases.override_set = true;
	inputs.bases.override_value = tmp / "admin";
	auto loaded = loader.Load(inputs);
	REQUIRE(loaded.ok());
	CHECK(loaded.driver->identity.directory.find(tmp / "admin/.redshift-driver/") == 0);
	CHECK(PlatformLoaderCalls() == calls + 1);

	// Or the directory itself fixed.
	TempDir other;
	REQUIRE(::mkdir((other / "ext").c_str(), 0700) == 0);
	REQUIRE(::chmod((other / "ext").c_str(), 0770) == 0);
	DriverLoader second(payload.config);
	CHECK_FALSE(second.Load(LoadFrom(other / "ext")).ok());
	REQUIRE(::chmod((other / "ext").c_str(), 0700) == 0);
	CHECK(second.Load(LoadFrom(other / "ext")).ok());
	CHECK(PlatformLoaderCalls() == calls + 2);
}

TEST_CASE("A base that cannot be used as it is moves on at once; a set that keeps changing, after three tries",
          "[driver][loader]") {
	TempDir tmp;
	REQUIRE(::mkdir((tmp / "second").c_str(), 0700) == 0);
	TestPayload payload(redshift_driver_test::TestInputPath(REDSHIFT_TEST_DRIVER_COMPLETE));
	static std::vector<std::string> checked;
	checked.clear();
	static std::string refused_prefix;
	refused_prefix = tmp.path() + "/.redshift-driver/";
	// The first base's file system reports noexec at the load check, whatever the base check saw: a fact
	// about the base, which the same set would meet again, so one try.
	{
		DriverLoader loader(payload.config);
		auto inputs = LoadFrom(tmp.path(), tmp / "second");
		inputs.hooks.no_exec_state = [](const std::string &dir) {
			checked.push_back(dir);
			return dir.compare(0, refused_prefix.size(), refused_prefix) == 0 ? 1 : 0;
		};
		const int calls = PlatformLoaderCalls();
		auto result = loader.Load(inputs);
		REQUIRE(result.ok());
		CHECK(result.driver->identity.directory == payload.SetDirectory(tmp / "second"));
		CHECK(PlatformLoaderCalls() == calls + 1);
		CHECK(std::count(checked.begin(), checked.end(), payload.SetDirectory(tmp.path())) == 1);
		CHECK(std::count(checked.begin(), checked.end(), payload.SetDirectory(tmp / "second")) == 1);
	}
	// A set that is changed under the loader at every check (something else rewrites a byte of it, as a
	// racing process could) is worth another try each time: three, then the next base.
	{
		TempDir fresh;
		REQUIRE(::mkdir((fresh / "second").c_str(), 0700) == 0);
		checked.clear();
		refused_prefix = fresh.path() + "/.redshift-driver/";
		DriverLoader loader(payload.config);
		auto inputs = LoadFrom(fresh.path(), fresh / "second");
		inputs.hooks.no_exec_state = [](const std::string &dir) {
			checked.push_back(dir);
			if (dir.compare(0, refused_prefix.size(), refused_prefix) == 0) {
				const auto notice = dir + "/NOTICE";
				::chmod(notice.c_str(), 0700);
				std::ofstream rewrite(notice, std::ios::binary | std::ios::trunc);
				rewrite << "changed under the loader";
				rewrite.close();
				::chmod(notice.c_str(), 0500);
			}
			return 0;
		};
		const int calls = PlatformLoaderCalls();
		auto result = loader.Load(inputs);
		REQUIRE(result.ok());
		CHECK(result.driver->identity.directory == payload.SetDirectory(fresh / "second"));
		CHECK(PlatformLoaderCalls() == calls + 1);
		CHECK(std::count(checked.begin(), checked.end(), payload.SetDirectory(fresh.path())) == INSTALL_TRIES);
		CHECK(std::count(checked.begin(), checked.end(), payload.SetDirectory(fresh / "second")) == 1);
	}

	// A noexec state that cannot be told is a failure of the base, not "exec allowed".
	TempDir other;
	DriverLoader unknown(payload.config);
	auto unknown_inputs = LoadFrom(other.path());
	unknown_inputs.hooks.no_exec_state = [](const std::string &) {
		return -1;
	};
	const int calls = PlatformLoaderCalls();
	auto none = unknown.Load(unknown_inputs);
	REQUIRE_FALSE(none.ok());
	CHECK(none.error.kind == DriverErrorKind::NO_USABLE_DIRECTORY);
	CHECK(none.error.attempts.at(0).reason ==
	      "the extension's directory: the driver set's file system cannot be examined for the noexec option");
	CheckStage(none, calls);
}

TEST_CASE("An error inside the loader is retried before the platform loader call and final after it",
          "[driver][loader]") {
	TestPayload payload(redshift_driver_test::TestInputPath(REDSHIFT_TEST_DRIVER_COMPLETE));
	// Before the call: out of memory, say, while the inputs are read. Not stored.
	{
		TempDir tmp;
		DriverLoader loader(payload.config);
		int reads = 0;
		const int calls = PlatformLoaderCalls();
		const std::function<LoadInputs()> inputs = [&]() -> LoadInputs {
			if (reads++ == 0) {
				throw std::bad_alloc();
			}
			return LoadFrom(tmp.path());
		};
		auto failed = loader.Load(inputs);
		REQUIRE_FALSE(failed.ok());
		CHECK(failed.error.kind == DriverErrorKind::NO_USABLE_DIRECTORY);
		CHECK(failed.error.message ==
		      "the Redshift ODBC driver could not be prepared (out of memory or another internal error); try again");
		CHECK_FALSE(IsFinalKind(failed.error.kind));
		CHECK(PlatformLoaderCalls() == calls);
		CHECK(loader.Load(inputs).ok());
		CHECK(PlatformLoaderCalls() == calls + 1);
	}
	// From the call on: the driver's code may have run, so the outcome is final and the same on every call.
	// The hook runs once the call is recorded, just before the first dlopen.
	{
		TempDir tmp;
		DriverLoader loader(payload.config);
		auto inputs = LoadFrom(tmp.path());
		inputs.hooks.on_platform_loader_call = []() {
			redshift_driver_test::CountLoaderCall();
			throw std::bad_alloc();
		};
		const int calls = PlatformLoaderCalls();
		auto failed = loader.Load(inputs);
		REQUIRE_FALSE(failed.ok());
		CHECK(failed.error.kind == DriverErrorKind::LOAD_FAILED);
		CHECK(failed.error.message == "the Redshift ODBC driver could not be prepared (out of memory or another "
		                              "internal error); restart the program and connect again");
		CHECK(IsFinalKind(failed.error.kind));
		CHECK(PlatformLoaderCalls() == calls + 1);
		auto again = loader.Load(LoadFrom(tmp.path()));
		CHECK(again.error.kind == DriverErrorKind::LOAD_FAILED);
		CHECK(again.error.message == failed.error.message);
		CHECK(PlatformLoaderCalls() == calls + 1);
	}
}

TEST_CASE("Corrupt bytes give EMBEDDED_BYTES_CORRUPT before any load, and the next call tries again",
          "[driver][loader]") {
	TempDir tmp;
	TestPayload payload(redshift_driver_test::TestInputPath(REDSHIFT_TEST_DRIVER_COMPLETE));
	payload.config.set.files[1].sha256 = std::string(64, '0');
	DriverLoader loader(payload.config);
	const int calls = PlatformLoaderCalls();
	auto result = loader.Load(LoadFrom(tmp.path()));
	CHECK(result.error.kind == DriverErrorKind::EMBEDDED_BYTES_CORRUPT);
	CHECK(PlatformLoaderCalls() == calls);
	CheckStage(result, calls);
	CheckNoPath(result.error, tmp.path());
	// No set was written.
	std::string where, reason;
	CHECK(CheckInstalledSet(payload.SetDirectory(tmp.path()), payload.config.set.Manifest(), ::geteuid(), where,
	                        reason) != SetState::VALID);
	CHECK(loader.Load(LoadFrom(tmp.path())).error.kind == DriverErrorKind::EMBEDDED_BYTES_CORRUPT);
}

TEST_CASE("Eight threads loading at once get the same LoadedDriver from one loader call", "[driver][loader]") {
	TempDir tmp;
	TestPayload payload(redshift_driver_test::TestInputPath(REDSHIFT_TEST_DRIVER_COMPLETE));
	DriverLoader loader(payload.config);
	const int calls = PlatformLoaderCalls();
	std::vector<const LoadedDriver *> drivers(8, nullptr);
	std::vector<std::thread> threads;
	for (size_t i = 0; i < drivers.size(); i++) {
		threads.emplace_back([&, i] { drivers[i] = loader.Load(LoadFrom(tmp.path())).driver; });
	}
	for (auto &thread : threads) {
		thread.join();
	}
	REQUIRE(drivers[0] != nullptr);
	for (auto *driver : drivers) {
		CHECK(driver == drivers[0]);
	}
	CHECK(PlatformLoaderCalls() == calls + 1);
}

TEST_CASE("LoadDriverFileSet re-walks and re-hashes the set just before loading", "[driver][loader]") {
	TempDir tmp;
	TestPayload payload(redshift_driver_test::TestInputPath(REDSHIFT_TEST_DRIVER_COMPLETE));
	DriverError error;
	std::string set_dir;
	REQUIRE(PrepareDriverSet(redshift_driver_test::Inputs(tmp.path(), ""), payload.config.set, ::geteuid(), set_dir,
	                         error));
	const auto manifest = payload.config.set.Manifest();
	LoadOptions options;
	redshift_driver_test::WithoutHostSearch(options.search);
	options.hooks.on_platform_loader_call = redshift_driver_test::CountLoaderCall;
	std::string where, reason;
	LoadedDriver out;
	const int calls = PlatformLoaderCalls();
	// A hash that does not match what is there.
	auto wrong = manifest;
	wrong[0].sha256 = std::string(64, 'f');
	CHECK(LoadDriverFileSet(set_dir, wrong, options, out, error, where, reason) == LoadResult::CHANGED);
	CHECK(reason == "a file of the driver set does not match the embedded driver");
	CHECK(where == set_dir + "/librsodbc64.so");
	CHECK(PlatformLoaderCalls() == calls);
	CHECK_FALSE(out.api.Complete());
	// A byte changed after the install, which the fast path had checked: in the driver, then in the notice.
	for (const auto &name : {std::string("librsodbc64.so"), std::string("NOTICE")}) {
		INFO(name);
		const auto path = set_dir + "/" + name;
		REQUIRE(::chmod(path.c_str(), 0700) == 0);
		auto bytes = ReadWholeFile(path);
		bytes[bytes.size() / 2] ^= 1;
		{
			std::ofstream rewrite(path, std::ios::binary | std::ios::trunc);
			rewrite << bytes;
		}
		REQUIRE(::chmod(path.c_str(), 0500) == 0);
		CHECK(LoadDriverFileSet(set_dir, manifest, options, out, error, where, reason) == LoadResult::CHANGED);
		CHECK(reason == "a file of the driver set does not match the embedded driver");
		CHECK(PlatformLoaderCalls() == calls);
		// The install repairs it, and the notice is damaged next.
		if (name == "NOTICE") {
			break;
		}
		REQUIRE(PrepareDriverSet(redshift_driver_test::Inputs(tmp.path(), ""), payload.config.set, ::geteuid(), set_dir,
		                         error));
	}
	// A file that is writable by others, or a name that is not the manifest's.
	TempDir other;
	REQUIRE(PrepareDriverSet(redshift_driver_test::Inputs(other.path(), ""), payload.config.set, ::geteuid(), set_dir,
	                         error));
	REQUIRE(::chmod((set_dir + "/NOTICE").c_str(), 0666) == 0);
	CHECK(LoadDriverFileSet(set_dir, manifest, options, out, error, where, reason) == LoadResult::CHANGED);
	CHECK(reason == "a file of the driver set is writable by another user");
	CHECK(PlatformLoaderCalls() == calls);
	// A set that is gone is retryable too: the install puts it back, so the next try can load it. In a real
	// run the no-exec test of the set directory fails first, since the directory is not there to examine, so
	// this arm is reached only past that test; the hook stands in for a directory that exists at the test and
	// is gone at the check, which is the race it is for.
	REQUIRE(::chmod((set_dir + "/NOTICE").c_str(), 0500) == 0);
	const auto moved = set_dir + ".away";
	REQUIRE(::rename(set_dir.c_str(), moved.c_str()) == 0);
	auto gone = options;
	gone.hooks.no_exec_state = [](const std::string &) {
		return 0;
	};
	CHECK(LoadDriverFileSet(set_dir, manifest, gone, out, error, where, reason) == LoadResult::CHANGED);
	CHECK(reason == "the driver set does not exist");
	CHECK(where == set_dir);
	CHECK(PlatformLoaderCalls() == calls);
	// Without the hook the same set ends the base at the no-exec test, before any state is judged.
	CHECK(LoadDriverFileSet(set_dir, manifest, options, out, error, where, reason) == LoadResult::BASE_FAILED);
	CHECK(reason == "the driver set's file system cannot be examined for the noexec option");
	REQUIRE(::rename(moved.c_str(), set_dir.c_str()) == 0);
	// A set that cannot be checked, since the driver's descriptor cannot be kept (the descriptor limit lowered
	// to 3 after its hash, so F_DUPFD_CLOEXEC fails), ends the base instead: the same set would meet the same
	// fact again, so a retry would repair nothing (step 6, and the install reads UNCHECKED the same way).
	// Nothing is loaded.
	const DescriptorLimit limit;
	options.hooks.after_hash = [](const std::string &) {
		DescriptorLimitInUse()->Lower(3);
	};
	const auto unchecked = LoadDriverFileSet(set_dir, manifest, options, out, error, where, reason);
	limit.Restore();
	CHECK(unchecked == LoadResult::BASE_FAILED);
	CHECK(reason == "a file of the driver set cannot be kept open: Invalid argument");
	CHECK(where == set_dir + "/librsodbc64.so");
	CHECK(PlatformLoaderCalls() == calls);
	options.hooks.after_hash = nullptr;
	// A set the walk above it refuses is UNCHECKED as well, and ends the base for the same reason: here the
	// driver root is writable by another user, which no retry of this set can change. The states that a
	// concurrent repair can mend stay retryable, so the two cases above and this one separate CHANGED from
	// BASE_FAILED: mapping every non-VALID state to one of them fails one of the three.
	REQUIRE(::chmod(Parent(set_dir).c_str(), 0777) == 0);
	CHECK(LoadDriverFileSet(set_dir, manifest, options, out, error, where, reason) == LoadResult::BASE_FAILED);
	CHECK(reason == "the driver root is writable by another user");
	CHECK(where == Parent(set_dir));
	CHECK(PlatformLoaderCalls() == calls);
	REQUIRE(::chmod(Parent(set_dir).c_str(), 0700) == 0);
}

TEST_CASE("LoadDriverFileSet ends the base on a set that lists no file, before anything is examined",
          "[driver][loader]") {
	// The driver is the list's first file, so an empty list has nothing to load; without the guard the check
	// would go on to judge the directory against no names. Here a complete set is on disk, whose files an empty
	// list does not name, so a check that ran would call the set changed instead.
	TempDir tmp;
	TestPayload payload(redshift_driver_test::TestInputPath(REDSHIFT_TEST_DRIVER_COMPLETE));
	DriverError error;
	std::string set_dir;
	REQUIRE(PrepareDriverSet(redshift_driver_test::Inputs(tmp.path(), ""), payload.config.set, ::geteuid(), set_dir,
	                         error));
	LoadOptions options;
	redshift_driver_test::WithoutHostSearch(options.search);
	options.hooks.on_platform_loader_call = redshift_driver_test::CountLoaderCall;
	std::string where = "stale", reason = "stale";
	error.message = "stale";
	LoadedDriver out;
	const int calls = PlatformLoaderCalls();
	CHECK(LoadDriverFileSet(set_dir, {}, options, out, error, where, reason) == LoadResult::BASE_FAILED);
	CHECK(reason == "the driver set lists no file");
	CHECK(where == set_dir);
	CHECK(error.message.empty());
	CHECK(PlatformLoaderCalls() == calls);
}

TEST_CASE("The types keep apart what is retryable from what is final", "[driver][loader]") {
	// Before the platform loader call: returned, and the next call runs again.
	for (const auto kind : {DriverErrorKind::EMBEDDED_BYTES_CORRUPT, DriverErrorKind::NO_USABLE_DIRECTORY,
	                        DriverErrorKind::REFUSED_ENVIRONMENT}) {
		CHECK_FALSE(IsFinalKind(kind));
	}
	// From it on: stored.
	for (const auto kind :
	     {DriverErrorKind::MISSING_PREREQUISITE, DriverErrorKind::LOAD_FAILED, DriverErrorKind::MISSING_ENTRY_POINT}) {
		CHECK(IsFinalKind(kind));
	}
	// The placeholder answers the same every time.
	CHECK(IsFinalKind(DriverErrorKind::NOT_BUILT));
}

TEST_CASE("Removing a new directory's ACL entries counts as done for ENOTSUP or EOPNOTSUPP only", "[driver][loader]") {
	// macOS's ClearAcl takes acl_set_fd_np's ENOTSUP or EOPNOTSUPP (a file system without extended ACLs, which has
	// none to clear) as cleared, through NoAclToClear. Here, since both platforms build this suite: on macOS the
	// two are different numbers (Linux's ENOTSUP is EOPNOTSUPP).
	for (const int none : {ENOTSUP, EOPNOTSUPP}) {
		INFO(none);
		CHECK(NoAclToClear(none));
	}
	for (const int error : {EPERM, EACCES, EINVAL, EIO, ENOMEM}) {
		INFO(error);
		CHECK_FALSE(NoAclToClear(error));
	}
}

#if defined(__linux__)
TEST_CASE("MissingPrerequisite finds a prerequisite in every form glibc words a refusal", "[driver][loader]") {
	const std::vector<std::string> prerequisites {"libkeyutils.so.1"};
	for (const char *form :
	     {"cannot open shared object file: No such file or directory",
	      "cannot open shared object file: Permission denied",
	      "version `KEYUTILS_1.5' not found (required by /x/librsodbc64.so)", "wrong ELF class: ELFCLASS32",
	      "file too short", "invalid ELF header", "cannot read file data: Is a directory"}) {
		INFO(form);
		CHECK(MissingPrerequisite((std::string("/x/librsodbc64.so: libkeyutils.so.1: ") + form).c_str(),
		                          prerequisites) == &prerequisites[0]);
		CHECK(MissingPrerequisite((std::string("libkeyutils.so.1: ") + form).c_str(), prerequisites) ==
		      &prerequisites[0]);
		// The name of a longer library is not the prerequisite.
		CHECK(MissingPrerequisite((std::string("/x/librsodbc64.so: xlibkeyutils.so.1: ") + form).c_str(),
		                          prerequisites) == nullptr);
	}
	CHECK(MissingPrerequisite("/x/librsodbc64.so: libssl.so.3: cannot open shared object file", prerequisites) ==
	      nullptr);
	CHECK(MissingPrerequisite("libkeyutils.so.1: undefined symbol: x", prerequisites) == nullptr);
	CHECK(MissingPrerequisite(nullptr, prerequisites) == nullptr);
	CHECK(MissingPrerequisite("anything", {}) == nullptr);
}
#endif

#if defined(__linux__)
TEST_CASE("The Linux hooks answer from the system", "[driver][loader]") {
	// A descriptor's name leads to the same file; a number nothing holds leads nowhere.
	TempDir tmp;
	std::ofstream(tmp / "file") << "x";
	const int fd = ::open((tmp / "file").c_str(), O_RDONLY | O_CLOEXEC);
	REQUIRE(fd >= 0);
	if (::access("/proc/thread-self/fd", F_OK) == 0) {
		CHECK(DescriptorPath(fd) == "/proc/thread-self/fd/" + std::to_string(fd));
		struct stat by_name, by_fd;
		REQUIRE(::stat(DescriptorPath(fd).c_str(), &by_name) == 0);
		REQUIRE(::fstat(fd, &by_fd) == 0);
		CHECK(by_name.st_ino == by_fd.st_ino);
	}
	::close(fd);
	CHECK(DescriptorPath(fd).empty());
	// An object is loaded under a path, or it is not; a bare name is never looked up, even a loaded one's.
	Dl_info libc;
	REQUIRE(::dladdr(reinterpret_cast<void *>(&::getpid), &libc) != 0);
	REQUIRE(std::strchr(libc.dli_fname, '/') != nullptr);
	CHECK(PathIsLoaded(libc.dli_fname));
	CHECK_FALSE(PathIsLoaded("libc.so.6"));
	CHECK_FALSE(PathIsLoaded("libredshift_never_loaded_zz.so.9"));
	CHECK_FALSE(PathIsLoaded("/proc/thread-self/fd/999999"));
	// The search list of this process is readable and has the system's directories.
	std::vector<std::string> list;
	REQUIRE(PlatformSearchList(list));
	CHECK_FALSE(list.empty());
	for (const auto &directory : list) {
		CHECK_FALSE(directory.empty());
	}
}
#endif

namespace {

std::vector<std::string> PolicyNames(const std::string &file, bool with_keyword_only) {
	std::ifstream in(std::string(REDSHIFT_TEST_POLICY_DIR) + "/" + file);
	REQUIRE(in.good());
	std::vector<std::string> names;
	std::string line;
	while (std::getline(in, line)) {
		if (line.empty() || line[0] == '#') {
			continue;
		}
		std::istringstream words(line);
		std::string name, second;
		words >> name >> second;
		if (with_keyword_only && second == "-") {
			continue;
		}
		names.push_back(with_keyword_only ? name + "=" + second : name);
	}
	return names;
}

} // namespace

TEST_CASE("The embedded driver loads, writes the locked bytes and answers a closed port with its own error",
          "[driver][loader][real]") {
	if (!redshift_driver_test::HostHasDriverPrerequisites()) {
		return;
	}
	TempDir tmp;
	const int port = ClosedPort();
	const int status = InChild(tmp.path(), [&] {
		auto result = LoadEmbeddedDriver();
		if (!result.ok()) {
			std::ofstream(tmp / "diagnostic.txt") << result.error.message << "\n";
			return 10;
		}
		const auto &driver = *result.driver;
		std::string expected_describe =
		    std::string("embedded ") + REDSHIFT_DRIVER_VERSION + " " + REDSHIFT_DRIVER_PLATFORM;
		if (REDSHIFT_DRIVER_DEVELOPMENT) {
			expected_describe += " (development)";
		}
		if (driver.identity.Describe() != expected_describe ||
		    driver.identity.directory != tmp / ".redshift-driver/" REDSHIFT_DRIVER_SET_SHA256) {
			return 11;
		}
		// The written bytes equal the lock's hashes, as driver_info.h records them.
#define REDSHIFT_CHECK_WRITTEN(file_name, file_size, file_sha256, stem)                                                \
	{                                                                                                                  \
		std::string hex, error;                                                                                        \
		if (!Sha256File(driver.identity.directory + "/" + file_name, hex, error) || hex != file_sha256) {              \
			return 12;                                                                                                 \
		}                                                                                                              \
	}
		REDSHIFT_DRIVER_FILES(REDSHIFT_CHECK_WRITTEN)
#undef REDSHIFT_CHECK_WRITTEN
		// The closed-port smoke: the driver's own diagnostic, from its own code.
		std::string answer;
		const bool own = redshift_driver_test::ClosedPortSmoke(driver, port, answer);
		std::ofstream(tmp / "diagnostic.txt") << answer << "\n";
		return own ? 0 : 13;
	});
	if (status != 0) {
		INFO("diagnostic: " << ReadWholeFile(tmp / "diagnostic.txt"));
		CHECK(status == 0);
	}
}

namespace {

// How the first environment handle is allocated, in ConnectInPlantedDirectory.
enum class FirstHandle { DIRECT_IN_PLANTED, DIRECT_THEN_PLANTED, WRAPPED_IN_PLANTED };

// With the real driver loaded: allocates the environment, then a connection,
// and connects to 127.0.0.1:`port`, where nothing listens, in `planted`'s directory, which holds an odbc.ini.
// The driver reads ./odbc.ini when it allocates its first environment handle: with api.AllocHandle in `planted`
// (DIRECT_IN_PLANTED), with it in an empty directory `clean` before moving there (DIRECT_THEN_PLANTED), or with
// AllocateEnvironmentHandle in `planted` (WRAPPED_IN_PLANTED), which must leave ODBCINI unset again. Returns 0, or the
// step that failed.
int ConnectInPlantedDirectory(const std::string &planted, const std::string &clean, FirstHandle how, int port) {
	auto result = LoadEmbeddedDriver();
	if (!result.ok()) {
		return 10;
	}
	auto &api = result.driver->api;
	if (::chdir((how == FirstHandle::DIRECT_THEN_PLANTED ? clean : planted).c_str()) != 0) {
		return 11;
	}
	SQLHENV env = nullptr;
	SQLHDBC dbc = nullptr;
	const SQLRETURN allocated = how == FirstHandle::WRAPPED_IN_PLANTED
	                                ? AllocateEnvironmentHandle(*result.driver, &env)
	                                : api.AllocHandle(SQL_HANDLE_ENV, SQL_NULL_HANDLE, &env);
	if (allocated != SQL_SUCCESS ||
	    api.SetEnvAttr(env, SQL_ATTR_ODBC_VERSION, reinterpret_cast<SQLPOINTER>(SQL_OV_ODBC3), 0) != SQL_SUCCESS) {
		return 12;
	}
	if (std::getenv("ODBCINI") != nullptr) {
		return 15;
	}
	if (how == FirstHandle::DIRECT_THEN_PLANTED && ::chdir(planted.c_str()) != 0) {
		return 11;
	}
	if (api.AllocHandle(SQL_HANDLE_DBC, env, &dbc) != SQL_SUCCESS) {
		return 13;
	}
	std::string connect = "Host=127.0.0.1;Port=" + std::to_string(port) +
	                      ";Database=dev;UID=nobody;PWD=not-a-password;SSLMode=require;LoginTimeout=5";
	SQLCHAR out[1024];
	SQLSMALLINT out_length = 0;
	return api.DriverConnect(dbc, nullptr, reinterpret_cast<SQLCHAR *>(&connect[0]),
	                         static_cast<SQLSMALLINT>(connect.size()), out, sizeof(out), &out_length,
	                         SQL_DRIVER_NOPROMPT) == SQL_ERROR
	           ? 0
	           : 14;
}

// How many entries a directory holds, not counting . and ..
size_t EntriesIn(const std::string &directory) {
	size_t count = 0;
	if (DIR *handle = ::opendir(directory.c_str())) {
		while (const auto *entry = ::readdir(handle)) {
			count += std::strcmp(entry->d_name, ".") != 0 && std::strcmp(entry->d_name, "..") != 0 ? 1 : 0;
		}
		::closedir(handle);
	}
	return count;
}

} // namespace

TEST_CASE("The real driver reads an odbc.ini in the current directory at its first environment handle, unless it is "
          "allocated through AllocateEnvironmentHandle",
          "[driver][loader][real]") {
	// The no-log rule on the real driver, and the premise of the end to end case that no log is written
	// (run-e2e.sh: odbc-ini): an empty log directory shows the file was not read only if the same file, read at
	// the first environment handle, makes the driver write a log there, which the first case shows.
	if (!redshift_driver_test::HostHasDriverPrerequisites()) {
		return;
	}
	const int port = ClosedPort();
	for (const auto how :
	     {FirstHandle::DIRECT_IN_PLANTED, FirstHandle::DIRECT_THEN_PLANTED, FirstHandle::WRAPPED_IN_PLANTED}) {
		INFO("case " << static_cast<int>(how));
		// Made here, in the parent, where their destructors run.
		TempDir driver_dir, planted, clean, logs;
		std::ofstream(planted / "odbc.ini") << "[ODBC]\nLogLevel=6\nLogPath=" << logs.path() << "\n";
		const int status = InChild(driver_dir.path(),
		                           [&] { return ConnectInPlantedDirectory(planted.path(), clean.path(), how, port); });
		REQUIRE(status == 0);
		if (how == FirstHandle::DIRECT_IN_PLANTED) {
			CHECK(EntriesIn(logs.path()) > 0); // the file was read: the driver wrote its log where it said
		} else {
			// Not read, neither at the allocation nor at the connect in that directory afterwards, ODBCINI unset.
			CHECK(EntriesIn(logs.path()) == 0);
		}
	}
}

TEST_CASE("The embedded driver's environment lists are the policy files'", "[driver][loader][real]") {
	if (!redshift_driver_test::HostHasDriverPrerequisites()) {
		return;
	}
	TempDir tmp;
	const auto refused = PolicyNames("refused-environment.txt", false);
	const auto known = PolicyNames("known-environment.txt", true);
	REQUIRE_FALSE(refused.empty());
	// The host prerequisites the loader names are the policy's rows for this system (none for macOS).
#if defined(__APPLE__)
	const std::string this_os = "macos";
#else
	const std::string this_os = "linux";
#endif
	std::ifstream rows(std::string(REDSHIFT_TEST_POLICY_DIR) + "/host-prerequisites.txt");
	std::vector<std::string> os_rows;
	std::string os, library;
	std::string line;
	while (std::getline(rows, line)) {
		std::istringstream words(line);
		if (!line.empty() && line[0] != '#' && (words >> os >> library) && os == this_os) {
			os_rows.push_back(library);
		}
	}
	CHECK(std::vector<std::string> {REDSHIFT_DRIVER_HOST_PREREQUISITES} == os_rows);
#if defined(__APPLE__)
	// Nor does the macOS loader find or load any library first: the driver links only system paths.
	CHECK(EmbeddedConfig().needed.empty());
#endif
	const int status = InChild(tmp.path(), [&] {
		auto result = LoadEmbeddedDriver();
		if (!result.ok()) {
			return 10;
		}
		std::vector<std::string> overridden;
		for (const auto &variable : result.driver->overridden_environment) {
			overridden.push_back(variable.name + "=" + variable.keyword);
		}
		return result.driver->refused_environment == refused && overridden == known ? 0 : 11;
	});
	CHECK(status == 0);
}

TEST_CASE("Loading the embedded driver writes nothing to standard output or standard error", "[driver][loader][real]") {
	if (!redshift_driver_test::HostHasDriverPrerequisites()) {
		return;
	}
	TempDir tmp;
	const auto output = tmp / "output.txt";
	REQUIRE(::mkdir((tmp / "dir").c_str(), 0700) == 0);
	const int status = InChild(tmp / "dir", [&] {
		const int fd = ::open(output.c_str(), O_WRONLY | O_CREAT | O_TRUNC, 0600);
		if (fd < 0 || ::dup2(fd, 1) < 0 || ::dup2(fd, 2) < 0) {
			return 10;
		}
		return LoadEmbeddedDriver().ok() ? 0 : 11;
	});
	CHECK(status == 0);
	CHECK(ReadWholeFile(output).empty());
}

// The suite's operator new, which can fail its next allocation once, as an out-of-memory process's would
// (std::bad_alloc), so the loader's construction can fail where nothing else can make it. Armed only in a fresh
// process of one case (below); everywhere else it is plain malloc.
namespace {
std::atomic<bool> g_fail_next_allocation {false};
} // namespace

void *operator new(std::size_t size) {
	if (g_fail_next_allocation.load(std::memory_order_relaxed) && g_fail_next_allocation.exchange(false)) {
		throw std::bad_alloc();
	}
	if (void *memory = std::malloc(size == 0 ? 1 : size)) {
		return memory;
	}
	throw std::bad_alloc();
}
void operator delete(void *memory) noexcept {
	std::free(memory);
}
void operator delete(void *memory, std::size_t) noexcept {
	std::free(memory);
}

namespace {

SQLRETURN AllocatesNothing(SQLSMALLINT, SQLHANDLE, SQLHANDLE *) {
	return SQL_SUCCESS;
}

// The steps, in a process where nothing has made the loader yet: 0, or the step that failed.
int LoaderThatCannotBeMade(bool can_load) {
	g_fail_next_allocation = true;
	auto failed = LoadEmbeddedDriver();
	if (g_fail_next_allocation) {
		return 13; // the failure was not met inside the call
	}
	if (failed.ok() || failed.error.kind != DriverErrorKind::NO_USABLE_DIRECTORY ||
	    failed.error.message != "the Redshift ODBC driver could not be prepared (out of memory or another internal "
	                            "error); try again") {
		return 10;
	}
	// A loader made after all would run this stub, not crash, and the call would then succeed.
	LoadedDriver none;
	none.api.AllocHandle = AllocatesNothing;
	SQLHENV environment = nullptr;
	g_fail_next_allocation = true;
	const auto allocated = AllocateEnvironmentHandle(none, &environment);
	if (g_fail_next_allocation) {
		return 13;
	}
	if (allocated != SQL_ERROR || environment != nullptr) {
		return 11;
	}
	// Not stored: the next call makes the loader, and loads where the host has what the driver needs.
	const auto next = LoadEmbeddedDriver();
	return can_load ? (next.ok() ? 0 : 12) : (next.error.message == failed.error.message ? 12 : 0);
}

} // namespace

TEST_CASE("When the process's loader cannot be made, both entry points say so and the next call tries again",
          "[driver][loader][real]") {
	// LoadEmbeddedDriver and AllocateEnvironmentHandle catch what constructing the process's one DriverLoader throws:
	// its first allocation fails here. The loader is a function-local static that other cases make, and a fork
	// would inherit, so the steps run in a fresh exec of this executable that runs this case alone.
	if (const char *inner = std::getenv("REDSHIFT_TEST_LOADER_UNMADE")) {
		::_exit(LoaderThatCannotBeMade(std::string(inner) == "can-load"));
	}
	const bool can_load = redshift_driver_test::HostHasDriverPrerequisites();
	TempDir tmp;
	auto self = redshift_driver_test::ExecutablePath();
	char *exe = &self[0];
	std::fflush(nullptr);
	const pid_t child = ::fork();
	REQUIRE(child >= 0);
	if (child == 0) {
		redshift_driver_test::ClearRefusedEnvironment();
		::unsetenv("LD_LIBRARY_PATH");
		::setenv("REDSHIFT_DRIVER_DIR", tmp.path().c_str(), 1);
		::setenv("REDSHIFT_TEST_LOADER_UNMADE", can_load ? "can-load" : "cannot-load", 1);
		// -w NoTests: a name that matched no case (after a rename) would otherwise exit 0.
		char warn[] = "-w", no_tests[] = "NoTests", name[] = "When the process's loader cannot be made*";
		char *argv[] = {exe, warn, no_tests, name, nullptr};
		::execv(exe, argv);
		::_exit(127);
	}
	int status = 0;
	REQUIRE(::waitpid(child, &status, 0) == child);
	REQUIRE(WIFEXITED(status));
	CHECK(WEXITSTATUS(status) == 0);
}

namespace {

// How many objects are loaded as the driver: on Linux under a /proc/thread-self/fd name, through the
// descriptor the loader keeps; on macOS under the driver set's path.
int ObjectsLoadedAsDriver() {
	int count = 0;
#if defined(__APPLE__)
	const std::string set = "/.redshift-driver/" + std::string(REDSHIFT_DRIVER_SET_SHA256) + "/";
	for (uint32_t i = 0; i < ::_dyld_image_count(); i++) {
		const char *name = ::_dyld_get_image_name(i);
		count += name != nullptr && std::string(name).find(set) != std::string::npos ? 1 : 0;
	}
#else
	::dl_iterate_phdr(
	    [](struct dl_phdr_info *info, size_t, void *data) {
		    if (info->dlpi_name != nullptr && std::strncmp(info->dlpi_name, "/proc/thread-self/fd/", 21) == 0) {
			    ++*static_cast<int *>(data);
		    }
		    return 0;
	    },
	    &count);
#endif
	return count;
}

} // namespace

TEST_CASE("Eight threads calling LoadEmbeddedDriver get the same LoadedDriver", "[driver][loader][real]") {
	if (!redshift_driver_test::HostHasDriverPrerequisites()) {
		return;
	}
	TempDir tmp;
	const int status = InChild(tmp.path(), [&] {
		// The loader is the process's own and counts nothing; the driver is loaded once if one more object is
		// loaded as the driver afterwards.
		const int before = ObjectsLoadedAsDriver();
		std::vector<const LoadedDriver *> drivers(8, nullptr);
		std::vector<std::thread> threads;
		for (size_t i = 0; i < drivers.size(); i++) {
			threads.emplace_back([&, i] { drivers[i] = LoadEmbeddedDriver().driver; });
		}
		for (auto &thread : threads) {
			thread.join();
		}
		for (auto *driver : drivers) {
			if (driver == nullptr || driver != drivers[0]) {
				return 10;
			}
		}
		return ObjectsLoadedAsDriver() == before + 1 ? 0 : 11;
	});
	CHECK(status == 0);
}

TEST_CASE("Without REDSHIFT_DRIVER_DIR the embedded driver goes to the extension's directory",
          "[driver][loader][real]") {
	if (!redshift_driver_test::HostHasDriverPrerequisites()) {
		return;
	}
	// The default flow: the first base is the directory of the file the loader is in, here the test
	// executable's, which the build owns. The set written there is removed afterwards. Where the
	// directory is not the caller's to write (as root over a build directory mounted read-only), the
	// loader goes to the per-user cache instead, and the case stops.
	struct stat directory;
	if (::stat(LoadedFromDirectory().c_str(), &directory) != 0 || directory.st_uid != ::geteuid() ||
	    ::access(LoadedFromDirectory().c_str(), W_OK) != 0) {
		WARN("the executable's directory is not owned and writable by this user; not tested");
		return;
	}
	TempDir tmp;
	const auto root = LoadedFromDirectory() + "/.redshift-driver";
	const bool root_existed = ::access(root.c_str(), F_OK) == 0;
	std::fflush(nullptr);
	const pid_t child = ::fork();
	REQUIRE(child >= 0);
	if (child == 0) {
		redshift_driver_test::ClearRefusedEnvironment();
		::unsetenv("REDSHIFT_DRIVER_DIR");
		::unsetenv("LD_LIBRARY_PATH");
#if defined(__APPLE__)
		::setenv("HOME", (tmp / "home").c_str(), 1); // the cache is <HOME>/Library/Caches/duckdb-redshift
#else
		::setenv("XDG_CACHE_HOME", (tmp / "cache").c_str(), 1);
#endif
		auto result = LoadEmbeddedDriver();
		::_exit(result.ok() && result.driver->identity.directory == root + "/" REDSHIFT_DRIVER_SET_SHA256 ? 0 : 1);
	}
	int status = 0;
	REQUIRE(::waitpid(child, &status, 0) == child);
	CHECK((WIFEXITED(status) && WEXITSTATUS(status) == 0));
	// The per-user cache was not used.
#if defined(__APPLE__)
	CHECK(::access((tmp / "home/Library/Caches/duckdb-redshift/.redshift-driver").c_str(), F_OK) != 0);
#else
	CHECK(::access((tmp / "cache/duckdb-redshift/.redshift-driver").c_str(), F_OK) != 0);
#endif
	if (!root_existed) {
		const std::string remove = "chmod -R u+w '" + root + "' && rm -rf '" + root + "'";
		CHECK(std::system(remove.c_str()) == 0);
	}
}
