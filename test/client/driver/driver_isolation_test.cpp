// Symbol isolation on Linux: the embedded driver's symbols stay out
// of the process's global scope, the host keeps its own OpenSSL, and with another ODBC library preloaded
// the driver's own calls stay inside it. The preload cases run this executable again with LD_PRELOAD and
// LD_DEBUG=bindings, so the dynamic linker reports every binding.
#include "driver_info.h"
#include "embedded_test_helpers.hpp"
#include "loader_payload.hpp"
#include "temp_dir.hpp"

#include "catch.hpp"

#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <map>
#include <set>
#include <sstream>
#include <string>
#include <vector>

#include <dirent.h>
#include <dlfcn.h>
#include <sys/wait.h>
#include <unistd.h>

using namespace duckdb::redshift_driver;
using redshift_driver_test::ClosedPort;
using redshift_driver_test::InChild;
using redshift_driver_test::ReadFileIfThere;
using redshift_driver_test::ReadWholeFile;
using redshift_driver_test::TempDir;

namespace {

// The file the dynamic linker names the object that holds `address` by, or empty.
std::string ObjectOf(const void *address) {
	Dl_info info;
	return ::dladdr(address, &info) != 0 && info.dli_fname != nullptr ? info.dli_fname : std::string();
}

} // namespace

TEST_CASE("After loading, the driver's symbols are not in the global scope", "[driver][isolation][real]") {
	if (!redshift_driver_test::HostHasDriverPrerequisites()) {
		return;
	}
	TempDir tmp;
	const int status = InChild(tmp.path(), [&] {
		if (::dlsym(RTLD_DEFAULT, "SQLDriverConnect") != nullptr) {
			return 9; // something in this test process already exports it
		}
		auto result = LoadEmbeddedDriver();
		if (!result.ok() || result.driver->api.DriverConnect == nullptr) {
			return 10;
		}
		return ::dlsym(RTLD_DEFAULT, "SQLDriverConnect") == nullptr ? 0 : 11;
	});
	CHECK(status == 0);
}

namespace {

// The host's OpenSSL, `library` loaded globally after the driver, as a host program or another extension
// would, is the one the process resolves, never the driver's bundled copy: a symbol the driver put in the
// global scope would come first. The 2.3.0.0 driver exports no OpenSSL symbol, so today this passes with or
// without RTLD_LOCAL; the SQLDriverConnect case above is the one that guards RTLD_LOCAL now. A later driver
// that exports one fails verification's exports rule (verify-release.sh) first, and this is the check behind
// that rule if such exports are ever accepted. A host without `library` is a case it cannot run (HostCannotRun).
void CheckHostKeepsItsOpenSsl(const std::string &library) {
	TempDir tmp;
	const int status = InChild(tmp.path(), [&] {
		auto result = LoadEmbeddedDriver();
		if (!result.ok()) {
			return 10;
		}
		void *host = ::dlopen(library.c_str(), RTLD_NOW | RTLD_GLOBAL);
		if (host == nullptr) {
			return 77;
		}
		void *resolved = ::dlsym(RTLD_DEFAULT, "SSL_new");
		if (resolved == nullptr || resolved != ::dlsym(host, "SSL_new")) {
			return 12;
		}
		const auto file = ObjectOf(resolved);
		const bool own = !file.empty() &&
		                 file != ObjectOf(reinterpret_cast<void *>(result.driver->api.DriverConnect)) &&
		                 file.find(result.driver->identity.directory) == std::string::npos;
		return own ? 0 : 13;
	});
	if (status == 77) {
		redshift_driver_test::HostCannotRun("the host has no " + library);
		return;
	}
	CHECK(status == 0);
}

// A name no host has, for the hidden row below.
const char *const NO_SUCH_OPENSSL = "libredshift-no-such-ssl.so.3";

} // namespace

TEST_CASE("Loading the driver does not change which OpenSSL the host resolves", "[driver][isolation][real]") {
	if (!redshift_driver_test::HostHasDriverPrerequisites()) {
		return;
	}
	CheckHostKeepsItsOpenSsl("libssl.so.3");
}

TEST_CASE("missing OpenSSL child", "[.missing-openssl-child]") {
	CheckHostKeepsItsOpenSsl(NO_SUCH_OPENSSL);
}

TEST_CASE("Without the host's OpenSSL the OpenSSL case warns and stops, or fails when the run requires host "
          "libraries",
          "[driver][isolation][real]") {
	if (!redshift_driver_test::HostHasDriverPrerequisites()) {
		return;
	}
	for (const bool required : {false, true}) {
		TempDir tmp;
		const int status =
		    redshift_driver_test::RerunHidden("[.missing-openssl-child]", "", tmp.path(),
		                                      {{"REDSHIFT_TEST_REQUIRE_HOST_LIBRARIES", required ? "1" : "0"}});
		const auto output = ReadWholeFile(tmp / "child-output.txt");
		INFO((required ? "with" : "without") << " REDSHIFT_TEST_REQUIRE_HOST_LIBRARIES=1: " << output);
		REQUIRE(WIFEXITED(status));
		CHECK(WEXITSTATUS(status) == (required ? 1 : 0));
		const std::string says = std::string("the host has no ") + NO_SUCH_OPENSSL +
		                         (required ? ", and REDSHIFT_TEST_REQUIRE_HOST_LIBRARIES=1 requires this row to run"
		                                   : "; this row was not run");
		CHECK(redshift_driver_test::Unwrapped(output).find(redshift_driver_test::Unwrapped(says)) != std::string::npos);
	}
}

namespace {

// The hidden half of the preload cases, in the process run again: the preloaded library's
// SQLDriverConnect is the process's before and after the load, the driver answers the closed-port
// smoke, and freeing an environment that still holds a connection makes the driver call its own
// SQLDisconnect and SQLFreeConnect through its PLT, the two references that would bind to the
// preloaded library without RTLD_DEEPBIND. Writes the name the dynamic linker gives the driver.
int PreloadedChild() {
	const char *dir = std::getenv("REDSHIFT_TEST_ISOLATION_DIR");
	const char *port = std::getenv("REDSHIFT_TEST_ISOLATION_PORT");
	if (dir == nullptr || port == nullptr) {
		return 20;
	}
	void *before = ::dlsym(RTLD_DEFAULT, "SQLDriverConnect");
	if (before == nullptr) {
		return 21;
	}
	auto result = LoadEmbeddedDriver();
	if (!result.ok()) {
		std::fprintf(stderr, "load: %s\n", result.error.message.c_str());
		return 22;
	}
	const auto &driver = *result.driver;
	if (::dlsym(RTLD_DEFAULT, "SQLDriverConnect") != before ||
	    reinterpret_cast<void *>(driver.api.DriverConnect) == before) {
		return 23;
	}
	std::string answer;
	if (!redshift_driver_test::ClosedPortSmoke(driver, std::atoi(port), answer)) {
		std::fprintf(stderr, "smoke: %s\n", answer.c_str());
		return 24;
	}
	SQLHENV env = nullptr;
	SQLHDBC dbc = nullptr;
	if (AllocateEnvironmentHandle(driver, &env) != SQL_SUCCESS ||
	    driver.api.SetEnvAttr(env, SQL_ATTR_ODBC_VERSION, reinterpret_cast<SQLPOINTER>(SQL_OV_ODBC3), 0) !=
	        SQL_SUCCESS ||
	    driver.api.AllocHandle(SQL_HANDLE_DBC, env, &dbc) != SQL_SUCCESS) {
		return 25;
	}
	driver.api.FreeHandle(SQL_HANDLE_ENV, env);
	std::ofstream(std::string(dir) + "/driver-name.txt")
	    << ObjectOf(reinterpret_cast<void *>(driver.api.DriverConnect));
	return 0;
}

// Runs the hidden case with `preload` in LD_PRELOAD and the dynamic linker's binding report in
// `<dir>/ld.<pid>`. Returns the exit status, the hidden case's own code.
int RerunPreloaded(const std::string &preload, const std::string &dir) {
	const int status =
	    redshift_driver_test::RerunHidden("[.isolation-child]", preload, dir,
	                                      {{"LD_DEBUG", "bindings"},
	                                       {"LD_DEBUG_OUTPUT", dir + "/ld"},
	                                       {"REDSHIFT_TEST_ISOLATION_DIR", dir},
	                                       {"REDSHIFT_TEST_ISOLATION_PORT", std::to_string(ClosedPort())},
	                                       {"REDSHIFT_TEST_DECOY_LOG", dir + "/decoy.log"}});
	REQUIRE(WIFEXITED(status));
	return WEXITSTATUS(status);
}

// From every "binding file <from> [n] to <to> [n]: normal symbol `<name>'" line the dynamic linker
// wrote for a reference from `driver` to an SQL* function: each name and the file it bound to.
std::map<std::string, std::string> DriverSqlBindings(const std::string &dir, const std::string &driver) {
	std::map<std::string, std::string> bindings;
	DIR *handle = ::opendir(dir.c_str());
	REQUIRE(handle != nullptr);
	std::vector<std::string> logs;
	while (auto entry = ::readdir(handle)) {
		if (std::string(entry->d_name).compare(0, 3, "ld.") == 0) {
			logs.push_back(dir + "/" + entry->d_name);
		}
	}
	::closedir(handle);
	REQUIRE_FALSE(logs.empty());
	const std::string from = "binding file " + driver + " [";
	const std::string to_word = " to ";
	// The text just before a bound name, and the prefix the name must start with. The name is read from
	// just after the quote, so it keeps its SQL prefix.
	const std::string quote = "normal symbol `";
	const std::string prefix = "SQL";
	for (const auto &log : logs) {
		std::istringstream lines(ReadWholeFile(log));
		std::string line;
		while (std::getline(lines, line)) {
			const auto at = line.find(from);
			const auto to = line.find(to_word, at == std::string::npos ? 0 : at);
			const auto symbol = line.find(quote + prefix);
			if (at == std::string::npos || to == std::string::npos || symbol == std::string::npos) {
				continue;
			}
			auto target = line.substr(to + to_word.size());
			target = target.substr(0, target.find(" ["));
			const auto name = line.substr(symbol + quote.size());
			bindings[name.substr(0, name.find('\''))] = target;
		}
	}
	return bindings;
}

// Runs the hidden case with `preload` and checks that every SQL* reference of the driver bound
// inside the driver; returns the names it bound.
std::set<std::string> CheckPreloaded(const std::string &preload, TempDir &tmp) {
	const int status = RerunPreloaded(preload, tmp.path());
	INFO("child: " << ReadFileIfThere(tmp / "child-output.txt"));
	REQUIRE(status == 0);
	const auto driver = ReadFileIfThere(tmp / "driver-name.txt");
	REQUIRE_FALSE(driver.empty());
	const auto bindings = DriverSqlBindings(tmp.path(), driver);
	// The driver's references to its own SQL* functions were bound, so the check below has something
	// to check, and each bound inside the driver.
	REQUIRE_FALSE(bindings.empty());
	std::set<std::string> names;
	for (const auto &binding : bindings) {
		INFO(binding.first << " bound to " << binding.second);
		CHECK(binding.second == driver);
		names.insert(binding.first);
	}
	return names;
}

} // namespace

TEST_CASE("preloaded child", "[.isolation-child]") {
	const int code = PreloadedChild();
	std::fflush(nullptr);
	::_exit(code);
}

TEST_CASE("With a decoy ODBC library preloaded, no driver call reaches it", "[driver][isolation][real]") {
	if (!redshift_driver_test::HostHasDriverPrerequisites()) {
		return;
	}
	const auto decoy = redshift_driver_test::TestInputPath(REDSHIFT_TEST_DRIVER_DECOY);
	TempDir tmp;
	const auto names = CheckPreloaded(decoy, tmp);
	CHECK(ReadFileIfThere(tmp / "decoy.log").empty());
	// The decoy exports every name the driver bound, so each would have reached it without RTLD_DEEPBIND.
	void *handle = ::dlopen(decoy.c_str(), RTLD_NOW | RTLD_LOCAL);
	REQUIRE(handle != nullptr);
	for (const auto &name : names) {
		INFO("the decoy (inputs/fake_driver.c, RECORD_CALLS) does not export " << name
		                                                                       << ", which the driver binds; add it");
		CHECK(::dlsym(handle, name.c_str()) != nullptr);
	}
	CHECK(names.count("SQLDisconnect") == 1);
	CHECK(names.count("SQLFreeConnect") == 1);
	// The decoy does record a call, so its empty log above means none was made.
	{
		const redshift_driver_test::ScopedEnv log("REDSHIFT_TEST_DECOY_LOG", (tmp / "control.log").c_str());
		auto *free_connect = reinterpret_cast<SQLRETURN (*)(SQLHDBC)>(::dlsym(handle, "SQLFreeConnect"));
		REQUIRE(free_connect != nullptr);
		free_connect(nullptr);
	}
	CHECK(ReadFileIfThere(tmp / "control.log") == "SQLFreeConnect\n");
	::dlclose(handle);
}

TEST_CASE("With unixODBC preloaded, the driver's calls stay inside it and unixODBC stays the process's",
          "[driver][isolation][real]") {
	if (!redshift_driver_test::HostHasDriverPrerequisites()) {
		return;
	}
	const auto unixodbc = redshift_driver_test::HostLibrary("REDSHIFT_TEST_UNIXODBC", "libodbc.so.2");
	if (unixodbc.empty()) {
		return;
	}
	TempDir tmp;
	CheckPreloaded(unixodbc, tmp);
}
