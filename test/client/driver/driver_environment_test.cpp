// The load-phase environment refusal, each environment handle allocated with ODBCINI
// pointed at /dev/null and the proof that REDSHIFT_DRIVER_DIR
// cannot bring in another library.
#include "driver_info.h"
#include "driver_loader_core.hpp"
#include "embedded_test_helpers.hpp"
#include "loader_payload.hpp"
#include "sha256.hpp"
#include "temp_dir.hpp"

#include "catch.hpp"

#include <algorithm>
#include <chrono>
#include <cstdlib>
#include <fstream>
#include <future>
#include <sstream>
#include <string>
#include <vector>

#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>

using namespace duckdb::redshift_driver;
using namespace duckdb::redshift_driver::detail;
using redshift_driver_test::Expect;
using redshift_driver_test::InBoundedChild;
using redshift_driver_test::LoadFrom;
using redshift_driver_test::PlatformLoaderCalls;
using redshift_driver_test::PrepareDriverSet;
using redshift_driver_test::ReadWholeFile;
using redshift_driver_test::ScopedEnv;
using redshift_driver_test::TempDir;
using redshift_driver_test::TestPayload;
using redshift_driver_test::WAIT_LIMIT;

namespace {

// A loader for the complete test-input driver, with a policy like the checked-in one but with one
// name moved to the handle phase and one to connect.
struct FixturePolicy : TestPayload {
	FixturePolicy() : TestPayload(redshift_driver_test::TestInputPath(REDSHIFT_TEST_DRIVER_COMPLETE)) {
		config.refused_environment = {"REDSHIFT_TEST_LOAD_A", "REDSHIFT_TEST_HANDLE", "REDSHIFT_TEST_CONNECT",
		                              "REDSHIFT_TEST_LOAD_B"};
		config.load_refused_environment = {"REDSHIFT_TEST_LOAD_A", "REDSHIFT_TEST_LOAD_B"};
	}
};

// The client's check on reaching the driver, with the name in place of <NAME>; the client's own
// tests assert the same literal for its check.
std::string ClientSentence(const std::string &name) {
	return name + " is set; the Redshift driver would read it, and it can change the driver's connection or security "
	              "settings. Unset it to connect.";
}

// The policy file's names for one phase, in file order.
std::vector<std::string> PolicyNamesInPhase(const std::string &phase) {
	std::ifstream in(std::string(REDSHIFT_TEST_POLICY_DIR) + "/refused-environment.txt");
	REQUIRE(in.good());
	std::vector<std::string> names;
	std::string line;
	while (std::getline(in, line)) {
		if (line.empty() || line[0] == '#') {
			continue;
		}
		std::istringstream words(line);
		std::string name, row_phase;
		words >> name >> row_phase;
		if (row_phase == phase) {
			names.push_back(name);
		}
	}
	return names;
}

bool Written(const std::string &base) {
	return ::access((base + "/.redshift-driver").c_str(), F_OK) == 0;
}

} // namespace

TEST_CASE("A set load-phase name is refused before the platform loader call, naming it and not its value",
          "[driver][environment]") {
	TempDir tmp;
	FixturePolicy policy;
	DriverLoader loader(policy.config);
	const int calls = PlatformLoaderCalls();
	for (const char *value : {"planted-secret-value", ""}) {
		INFO("value: '" << value << "'");
		ScopedEnv set("REDSHIFT_TEST_LOAD_B", value);
		auto result = loader.Load(LoadFrom(tmp.path()));
		REQUIRE_FALSE(result.ok());
		CHECK(result.error.kind == DriverErrorKind::REFUSED_ENVIRONMENT);
		CHECK(result.error.message == ClientSentence("REDSHIFT_TEST_LOAD_B"));
		CHECK(result.error.message.find("planted-secret-value") == std::string::npos);
		CHECK(PlatformLoaderCalls() == calls);
		// Checked before the bases are chosen: nothing was made, written or hashed.
		CHECK_FALSE(Written(tmp.path()));
		CHECK_FALSE(IsFinalKind(result.error.kind));
	}
	// Not stored: once it is unset, the next call loads.
	auto loaded = loader.Load(LoadFrom(tmp.path()));
	CHECK(loaded.ok());
	CHECK(PlatformLoaderCalls() == calls + 1);
}

TEST_CASE("Of two set load-phase names, the first in the list is named", "[driver][environment]") {
	TempDir tmp;
	FixturePolicy policy;
	DriverLoader loader(policy.config);
	ScopedEnv b("REDSHIFT_TEST_LOAD_B", "x");
	ScopedEnv a("REDSHIFT_TEST_LOAD_A", "x");
	CHECK(loader.Load(LoadFrom(tmp.path())).error.message == ClientSentence("REDSHIFT_TEST_LOAD_A"));
}

TEST_CASE("The refusal is just before the loader call as well, after the set has been checked",
          "[driver][environment]") {
	TempDir tmp;
	FixturePolicy policy;
	DriverError error;
	std::string set_dir;
	REQUIRE(
	    PrepareDriverSet(redshift_driver_test::Inputs(tmp.path(), ""), policy.config.set, ::geteuid(), set_dir, error));
	LoadOptions options;
	options.load_refused_environment = policy.config.load_refused_environment;
	redshift_driver_test::WithoutHostSearch(options.search);
	options.hooks.on_platform_loader_call = redshift_driver_test::CountLoaderCall;
	LoadedDriver out;
	std::string where, reason;
	const int calls = PlatformLoaderCalls();
	{
		ScopedEnv set("REDSHIFT_TEST_LOAD_A", "x");
		CHECK(LoadDriverFileSet(set_dir, policy.config.set.Manifest(), options, out, error, where, reason) ==
		      LoadResult::REFUSED_ENVIRONMENT);
		CHECK(error.kind == DriverErrorKind::REFUSED_ENVIRONMENT);
		CHECK(error.message == ClientSentence("REDSHIFT_TEST_LOAD_A"));
		CHECK(PlatformLoaderCalls() == calls);
	}
#if defined(__linux__)
	// The search list too.
	options.search.list = [](std::vector<std::string> &directories) {
		directories.push_back("relative");
		return true;
	};
	CHECK(LoadDriverFileSet(set_dir, policy.config.set.Manifest(), options, out, error, where, reason) ==
	      LoadResult::REFUSED_ENVIRONMENT);
	CHECK(error.kind == DriverErrorKind::REFUSED_ENVIRONMENT);
	CHECK(error.message.find("the platform loader's search list holds an empty or relative directory") == 0);
	CHECK(PlatformLoaderCalls() == calls);
	options.search.list = redshift_driver_test::NoSearchDirectories;
#endif
	CHECK(LoadDriverFileSet(set_dir, policy.config.set.Manifest(), options, out, error, where, reason) ==
	      LoadResult::LOADED);
	CHECK(PlatformLoaderCalls() == calls + 1);
}

#if defined(__linux__)
namespace {
int g_search_calls = 0;
// A search list that is fine the first time it is read and relative afterwards, as if the process's
// state changed between the check before the bases and the check just before the load.
bool ChangingSearchList(std::vector<std::string> &directories) {
	if (g_search_calls++ > 0) {
		directories.push_back("relative");
	}
	return true;
}
} // namespace

TEST_CASE("A refusal found just before the load ends the call: no other base would be any better",
          "[driver][environment]") {
	TempDir tmp;
	REQUIRE(::mkdir((tmp / "second").c_str(), 0700) == 0);
	FixturePolicy policy;
	DriverLoader loader(policy.config);
	auto inputs = LoadFrom(tmp.path(), tmp / "second");
	inputs.search.list = ChangingSearchList;
	g_search_calls = 0;
	const int calls = PlatformLoaderCalls();
	auto result = loader.Load(inputs);
	REQUIRE_FALSE(result.ok());
	CHECK(result.error.kind == DriverErrorKind::REFUSED_ENVIRONMENT);
	CHECK(g_search_calls == 2);
	CHECK(PlatformLoaderCalls() == calls);
	// The second base was not touched, and the refusal is not stored.
	CHECK_FALSE(Written(tmp / "second"));
	g_search_calls = 0;
	inputs.search.list = redshift_driver_test::NoSearchDirectories;
	CHECK(loader.Load(inputs).ok());
}
#endif

TEST_CASE("Names read only at handle allocation or connect do not stop the load", "[driver][environment]") {
	TempDir tmp;
	FixturePolicy policy;
	DriverLoader loader(policy.config);
	ScopedEnv handle("REDSHIFT_TEST_HANDLE", "x");
	ScopedEnv connect("REDSHIFT_TEST_CONNECT", "x");
	auto result = loader.Load(LoadFrom(tmp.path()));
	REQUIRE(result.ok());
	// The connect still refuses them before it allocates a handle: they are in the full list.
	CHECK(FindRefusedEnvironmentVariable(*result.driver) == std::string("REDSHIFT_TEST_HANDLE"));
}

TEST_CASE("FindSetVariable reads whether a name is set, an empty value included, and nothing else",
          "[driver][environment]") {
	CHECK(FindSetVariable({}) == nullptr);
	CHECK(FindSetVariable({"REDSHIFT_TEST_NEVER_SET_ZZ"}) == nullptr);
	ScopedEnv empty("REDSHIFT_TEST_SET_EMPTY", "");
	CHECK(std::string(FindSetVariable({"REDSHIFT_TEST_NEVER_SET_ZZ", "REDSHIFT_TEST_SET_EMPTY"})) ==
	      "REDSHIFT_TEST_SET_EMPTY");
}

TEST_CASE("The generated load-phase names are the policy file's load rows", "[driver][environment]") {
	const std::vector<std::string> generated {REDSHIFT_DRIVER_LOAD_REFUSED_ENVIRONMENT};
	CHECK(generated == PolicyNamesInPhase("load"));
	CHECK_FALSE(generated.empty());
	// The full list is a superset, and nothing is refused twice.
	const std::vector<std::string> all {REDSHIFT_DRIVER_REFUSED_ENVIRONMENT};
	for (const auto &name : generated) {
		CHECK(std::find(all.begin(), all.end(), name) != all.end());
	}
}

TEST_CASE("The shipped configuration loads with ODBCINI set, which it leaves as it was",
          "[driver][environment][odbcini][real]") {
	// ODBCINI is refused at the first environment handle, not at the load (a handle-phase row of the policy): the
	// load must go on, so the client's own check, after it, is the one that refuses it.
	if (!redshift_driver_test::HostHasDriverPrerequisites()) {
		return;
	}
	const auto load_names = PolicyNamesInPhase("load");
	REQUIRE(std::find(load_names.begin(), load_names.end(), "ODBCINI") == load_names.end());
	TempDir tmp;
	const int status = redshift_driver_test::InChild(tmp.path(), [&] {
		const std::string named = tmp / "user-odbc.ini";
		::setenv("ODBCINI", named.c_str(), 1);
		auto result = LoadEmbeddedDriver();
		const char *after = std::getenv("ODBCINI");
		if (!result.ok()) {
			std::fprintf(stderr, "load: %s\n", result.error.message.c_str());
			return 1;
		}
		const char *refused = FindRefusedEnvironmentVariable(*result.driver);
		return after != nullptr && named == after && refused != nullptr && std::string(refused) == "ODBCINI" ? 0 : 2;
	});
	CHECK(status == 0);
}

TEST_CASE("The shipped configuration refuses a load-phase name from the checked-in policy, in the client's words",
          "[driver][environment][real]") {
	TempDir tmp;
	const auto names = PolicyNamesInPhase("load");
	REQUIRE_FALSE(names.empty());
	const auto &name = names.front();
	INFO(name);
	std::fflush(nullptr);
	const pid_t child = ::fork();
	REQUIRE(child >= 0);
	if (child == 0) {
		redshift_driver_test::ClearRefusedEnvironment();
		::unsetenv("LD_LIBRARY_PATH");
		::setenv("REDSHIFT_DRIVER_DIR", tmp.path().c_str(), 1);
		::setenv(name.c_str(), "planted-secret-value", 1);
		const int calls = PlatformLoaderCalls();
		auto result = LoadEmbeddedDriver();
		::_exit(!result.ok() && result.error.kind == DriverErrorKind::REFUSED_ENVIRONMENT &&
		                result.error.message == ClientSentence(name) && PlatformLoaderCalls() == calls &&
		                !Written(tmp.path())
		            ? 0
		            : 1);
	}
	int status = 0;
	REQUIRE(::waitpid(child, &status, 0) == child);
	CHECK((WIFEXITED(status) && WEXITSTATUS(status) == 0));
}

TEST_CASE("REDSHIFT_DRIVER_DIR holding another library under the driver's name is replaced, never loaded",
          "[driver][environment]") {
	// The planted library is the complete test-input driver; its constructor would log if it ran. The
	// real driver is what the shipped loader writes and loads, so here a planted file in the set's
	// place, with the right name and the wrong bytes, is set aside for a copy of the payload.
	TempDir tmp;
	if (!redshift_driver_test::PrivateAncestry(tmp)) {
		return;
	}
	ScopedEnv log("REDSHIFT_TEST_CONSTRUCTOR_LOG", (tmp / "planted.log").c_str());
	TestPayload payload("not the driver", true);
	DriverError error;
	std::string set_dir;
	REQUIRE(PrepareDriverSet(redshift_driver_test::Inputs(tmp.path(), ""), payload.config.set, ::geteuid(), set_dir,
	                         error));
	REQUIRE(::chmod((set_dir + "/librsodbc64.so").c_str(), 0700) == 0);
	{
		std::ofstream planted(set_dir + "/librsodbc64.so", std::ios::binary | std::ios::trunc);
		planted << ReadWholeFile(redshift_driver_test::TestInputPath(REDSHIFT_TEST_DRIVER_COMPLETE));
	}
	REQUIRE(::chmod((set_dir + "/librsodbc64.so").c_str(), 0500) == 0);
	// By hash it is not the set's file, before anything loads it.
	LoadOptions options;
	redshift_driver_test::WithoutHostSearch(options.search);
	options.hooks.on_platform_loader_call = redshift_driver_test::CountLoaderCall;
	LoadedDriver out;
	std::string where, reason;
	const int calls = PlatformLoaderCalls();
	CHECK(LoadDriverFileSet(set_dir, payload.config.set.Manifest(), options, out, error, where, reason) ==
	      LoadResult::CHANGED);
	CHECK(reason == "a file of the driver set does not match the embedded driver");
	CHECK(PlatformLoaderCalls() == calls);
	// The loader sets the planted file aside and writes the payload's own bytes (not a library, so the
	// load is refused): the planted library's constructor never ran.
	DriverLoader loader(payload.config);
	auto result = loader.Load(redshift_driver_test::LoadFrom(tmp.path()));
	REQUIRE_FALSE(result.ok());
	CHECK(result.error.kind == DriverErrorKind::LOAD_FAILED);
	CHECK(ReadWholeFile(set_dir + "/librsodbc64.so") == "not the driver");
	CHECK(::access((tmp / "planted.log").c_str(), F_OK) != 0);
}

// ---- The environment handle -----------------------------------------------------

namespace {

// What the fake allocation saw: every call, with its arguments and what ODBCINI was then.
struct Allocation {
	SQLSMALLINT type;
	SQLHANDLE input;
	bool odbcini_set;
	std::string odbcini;
};
std::vector<Allocation> g_allocations;
SQLRETURN g_allocation_result = SQL_SUCCESS;
void (*g_during_allocation)() = nullptr;
int g_handle_storage = 0;

SQLRETURN FakeAllocHandle(SQLSMALLINT type, SQLHANDLE input, SQLHANDLE *output) {
	const char *value = std::getenv("ODBCINI");
	g_allocations.push_back({type, input, value != nullptr, value != nullptr ? value : ""});
	if (g_during_allocation != nullptr) {
		g_during_allocation();
	}
	*output = &g_handle_storage;
	return g_allocation_result;
}

// An API with nothing in it but the fake allocation: anything else the wrapper called would crash.
OdbcApi AllocationOnly() {
	g_allocations.clear();
	g_allocation_result = SQL_SUCCESS;
	g_during_allocation = nullptr;
	OdbcApi api;
	api.AllocHandle = FakeAllocHandle;
	return api;
}

} // namespace

TEST_CASE("The environment handle is allocated with ODBCINI at /dev/null, and ODBCINI is unset again after",
          "[driver][environment][odbcini]") {
	const auto api = AllocationOnly();
	::unsetenv("ODBCINI");
	SQLHENV environment = nullptr;
	CHECK(AllocateWithoutOdbcIni(api, &environment) == SQL_SUCCESS);
	CHECK(environment == static_cast<SQLHENV>(&g_handle_storage));
	// One call, the allocation of an environment, with ODBCINI naming a file that holds nothing.
	REQUIRE(g_allocations.size() == 1);
	CHECK(g_allocations[0].type == SQL_HANDLE_ENV);
	CHECK(g_allocations[0].input == SQL_NULL_HANDLE);
	CHECK(g_allocations[0].odbcini_set);
	CHECK(g_allocations[0].odbcini == "/dev/null");
	// Afterwards as before, so the refused-variable check still sees a user's ODBCINI and nothing else.
	CHECK(std::getenv("ODBCINI") == nullptr);
	// The allocation's own result is what is returned, and the variable is unset whatever it was.
	for (const SQLRETURN result : {SQL_SUCCESS_WITH_INFO, SQL_ERROR, SQL_INVALID_HANDLE}) {
		g_allocation_result = result;
		CHECK(AllocateWithoutOdbcIni(api, &environment) == result);
		CHECK(std::getenv("ODBCINI") == nullptr);
	}
	CHECK(g_allocations.size() == 4);
	CHECK(std::string(ODBC_INI_VARIABLE) == "ODBCINI");
	CHECK(std::string(NO_ODBC_INI) == "/dev/null");
}

TEST_CASE("A set ODBCINI, empty or not, is left alone and nothing is allocated", "[driver][environment][odbcini]") {
	// The connect refuses a set ODBCINI before any allocation; should one be set by then all the same, the
	// allocation is refused rather than made with it, and its value is not read.
	const auto api = AllocationOnly();
	for (const char *value : {"/home/someone/odbc.ini", ""}) {
		INFO("ODBCINI='" << value << "'");
		ScopedEnv set("ODBCINI", value);
		SQLHENV environment = nullptr;
		CHECK(AllocateWithoutOdbcIni(api, &environment) == SQL_ERROR);
		CHECK(environment == nullptr);
		CHECK(g_allocations.empty());
		REQUIRE(std::getenv("ODBCINI") != nullptr);
		CHECK(std::string(std::getenv("ODBCINI")) == value);
	}
}

namespace {
int g_set_calls = 0;
} // namespace

TEST_CASE("An ODBCINI that cannot be set is refused, and nothing is allocated", "[driver][environment][odbcini]") {
	// Without it the driver would read odbc.ini in the current directory, so the allocation is not made.
	const auto api = AllocationOnly();
	::unsetenv("ODBCINI");
	g_set_calls = 0;
	SQLHENV environment = nullptr;
	// A setter that fails as setenv does, setting nothing.
	CHECK(AllocateWithoutOdbcIni(api, &environment, [] {
		      g_set_calls++;
		      return -1;
	      }) == SQL_ERROR);
	CHECK(g_set_calls == 1);
	CHECK(environment == nullptr);
	CHECK(g_allocations.empty());
	CHECK(std::getenv("ODBCINI") == nullptr);
}

namespace {

DriverLoader *g_waiting_loader = nullptr;
std::future<bool> g_waiting_load;
std::string g_waiting_base;
bool g_load_waited = false;

// During the allocation: a load on another thread must wait for it, since both hold the loader's lock.
void LoadMustWait() {
	auto load = std::async(std::launch::async, [] { return g_waiting_loader->Load(LoadFrom(g_waiting_base)).ok(); });
	g_load_waited = load.wait_for(std::chrono::milliseconds(200)) == std::future_status::timeout;
	g_waiting_load = std::move(load);
}

} // namespace

TEST_CASE("The loader's allocation holds the lock its loads hold, so no load runs while ODBCINI is changed",
          "[driver][environment][odbcini]") {
	TempDir tmp;
	FixturePolicy policy;
	// In a bounded child: an allocation or a load that never gets the loader's lock fails this case instead of hanging
	// the suite. The WAIT_LIMIT wait below ends first, with its own message.
	CHECK(InBoundedChild(
	    [&] {
		    DriverLoader loader(policy.config);
		    auto api = AllocationOnly();
		    g_waiting_loader = &loader;
		    g_waiting_base = tmp.path();
		    g_load_waited = false;
		    g_during_allocation = LoadMustWait;
		    SQLHENV environment = nullptr;
		    bool ok = Expect(loader.AllocateEnvironment(api, &environment) == SQL_SUCCESS, "the allocation");
		    g_during_allocation = nullptr;
		    ok = Expect(g_load_waited, "the load waited for the allocation") && ok;
		    // Once the allocation is over the load goes on, and loads.
		    if (!Expect(g_waiting_load.valid() && g_waiting_load.wait_for(WAIT_LIMIT) == std::future_status::ready,
		                "the load finished within WAIT_LIMIT of the allocation")) {
			    return false;
		    }
		    ok = Expect(g_waiting_load.get(), "the load loaded") && ok;
		    ok = Expect(g_allocations.size() == 1 && g_allocations[0].odbcini == "/dev/null",
		                "one allocation, with ODBCINI at /dev/null") &&
		         ok;
		    return Expect(std::getenv("ODBCINI") == nullptr, "ODBCINI unset at the end") && ok;
	    },
	    redshift_driver_test::LOCK_CASE_BOUND_SECONDS));
}

namespace {

LoadedDriver g_second_driver;
bool g_second_waited = false;
std::future<SQLRETURN> g_second;

// During the allocation: a second one on another thread, as two connections opening at once make, must wait for
// it, so the two never set and unset ODBCINI over each other.
void SecondAllocationMustWait() {
	g_during_allocation = nullptr; // the second allocation, when it runs, starts no third
	g_second = std::async(std::launch::async, [] {
		SQLHENV environment = nullptr;
		return AllocateEnvironmentHandle(g_second_driver, &environment);
	});
	g_second_waited = g_second.wait_for(std::chrono::milliseconds(200)) == std::future_status::timeout;
}

} // namespace

TEST_CASE("AllocateEnvironmentHandle is the loader's allocation, for the driver it is given, under the loader's lock",
          "[driver][environment][odbcini]") {
	// In a bounded child, as above.
	CHECK(InBoundedChild(
	    [] {
		    LoadedDriver driver;
		    driver.api = AllocationOnly();
		    ::unsetenv("ODBCINI");
		    SQLHENV environment = nullptr;
		    bool ok = Expect(AllocateEnvironmentHandle(driver, &environment) == SQL_SUCCESS, "the first allocation");
		    ok = Expect(environment == static_cast<SQLHENV>(&g_handle_storage), "the fake's handle") && ok;
		    if (!Expect(g_allocations.size() == 1, "one allocation")) {
			    return false;
		    }
		    ok = Expect(g_allocations[0].type == SQL_HANDLE_ENV && g_allocations[0].odbcini == "/dev/null",
		                "an environment, with ODBCINI at /dev/null") &&
		         ok;
		    ok = Expect(std::getenv("ODBCINI") == nullptr, "ODBCINI unset after it") && ok;
		    // Two at once: the second waits for the first, and each runs with ODBCINI at /dev/null.
		    g_second_driver = driver;
		    g_second_waited = false;
		    g_during_allocation = SecondAllocationMustWait;
		    ok = Expect(AllocateEnvironmentHandle(driver, &environment) == SQL_SUCCESS, "the outer allocation") && ok;
		    if (!Expect(g_second.valid() && g_second.wait_for(WAIT_LIMIT) == std::future_status::ready,
		                "the second allocation finished within WAIT_LIMIT of the first")) {
			    return false;
		    }
		    ok = Expect(g_second.get() == SQL_SUCCESS, "the second allocation") && ok;
		    ok = Expect(g_second_waited, "the second allocation waited for the first") && ok;
		    ok = Expect(g_allocations.size() == 3 && g_allocations[1].odbcini == "/dev/null" &&
		                    g_allocations[2].odbcini == "/dev/null",
		                "three allocations, each with ODBCINI at /dev/null") &&
		         ok;
		    return Expect(std::getenv("ODBCINI") == nullptr, "ODBCINI unset at the end") && ok;
	    },
	    redshift_driver_test::LOCK_CASE_BOUND_SECONDS));
}
