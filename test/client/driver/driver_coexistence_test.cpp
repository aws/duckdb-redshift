// Coexistence on Linux: the embedded driver next to a customer-installed copy that
// unixODBC loads, and the refusal of an allocator that replaces the C library's (the suite's own, jemalloc,
// tcmalloc). Offline: only the closed-port smoke, against a local port nothing listens on. Also the helpers
// those cases rest on: HostLibrary's rules for a library that is not found; ClosedLocalPort with no socket to
// be had; the smoke's stop when a handle cannot be set up, and its pass rule; and the probe's name for each
// error kind.
#include "allocator_replacement.hpp"
#include "driver_info.h"
#include "embedded_test_helpers.hpp"
#include "error_kind_name.hpp"
#include "loader_payload.hpp"
#include "temp_dir.hpp"

#include "catch.hpp"

#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <string>
#include <utility>
#include <vector>

#include <dlfcn.h>
#include <fcntl.h>
#include <sys/resource.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>

using namespace duckdb::redshift_driver;
using redshift_driver_test::ClosedPort;
using redshift_driver_test::ReadWholeFile;
using redshift_driver_test::TempDir;
using redshift_driver_test::Unwrapped;

namespace {

// unixODBC's entry points, from a library the process loaded as an application linked to it would.
struct DriverManager {
#define REDSHIFT_DM_MEMBER(name) decltype(&::SQL##name) name = nullptr;
	REDSHIFT_ODBC_ENTRY_POINTS(REDSHIFT_DM_MEMBER)
#undef REDSHIFT_DM_MEMBER
	bool Load(void *handle) {
#define REDSHIFT_DM_RESOLVE(name) name = reinterpret_cast<decltype(name)>(::dlsym(handle, "SQL" #name));
		REDSHIFT_ODBC_ENTRY_POINTS(REDSHIFT_DM_RESOLVE)
#undef REDSHIFT_DM_RESOLVE
		return AllocHandle && SetEnvAttr && DriverConnect && GetDiagRec && FreeHandle;
	}
	bool Smoke(const std::string &attributes, int port) const {
		SQLHENV env = nullptr;
		std::string answer;
		return AllocHandle(SQL_HANDLE_ENV, SQL_NULL_HANDLE, &env) == SQL_SUCCESS &&
		       redshift_driver_test::SmokeOn(*this, env, port, answer, attributes);
	}
};

// The fetched driver the build embedded, as the first file of the set.
std::string FetchedDriverPath() {
	std::string path;
#define REDSHIFT_FIRST_PATH(stem, file_path)                                                                           \
	if (path.empty()) {                                                                                                \
		path = file_path;                                                                                              \
	}
	REDSHIFT_DRIVER_EMBED_FILES(REDSHIFT_FIRST_PATH)
#undef REDSHIFT_FIRST_PATH
	return path;
}

bool EmbeddedSmoke(const LoadedDriver &driver, int port) {
	std::string answer;
	return redshift_driver_test::ClosedPortSmoke(driver, port, answer);
}

} // namespace

TEST_CASE("A customer-installed copy that unixODBC loads and the embedded driver both work",
          "[driver][coexistence][real]") {
	if (!redshift_driver_test::HostHasDriverPrerequisites()) {
		return;
	}
	const auto unixodbc = redshift_driver_test::HostLibrary("REDSHIFT_TEST_UNIXODBC", "libodbc.so.2");
	if (unixodbc.empty()) {
		return;
	}
	TempDir tmp;
	// The customer's copy: the same locked driver, installed elsewhere and registered with unixODBC.
	REQUIRE(::mkdir((tmp / "customer").c_str(), 0700) == 0);
	{ std::ofstream(tmp / "customer/librsodbc64.so", std::ios::binary) << ReadWholeFile(FetchedDriverPath()); }
	{
		std::ofstream(tmp / "odbcinst.ini") << "[Customer Redshift]\nDriver=" << tmp / "customer/librsodbc64.so"
		                                    << "\n";
	}
	REQUIRE(::mkdir((tmp / "ours").c_str(), 0700) == 0);
	const int port = ClosedPort();
	const int status = redshift_driver_test::InChild(tmp / "ours", [&] {
		::setenv("ODBCSYSINI", tmp.path().c_str(), 1);
		::setenv("ODBCINSTINI", "odbcinst.ini", 1);
		void *dm_handle = ::dlopen(unixodbc.c_str(), RTLD_NOW | RTLD_GLOBAL);
		DriverManager dm;
		if (dm_handle == nullptr || !dm.Load(dm_handle)) {
			return 10;
		}
		const std::string customer = "Driver={Customer Redshift};";
		if (!dm.Smoke(customer, port)) {
			return 11;
		}
		// Ours, loaded while unixODBC holds the customer's copy, is a separate copy from its own directory.
		auto ours = LoadEmbeddedDriver();
		if (!ours.ok() || !EmbeddedSmoke(*ours.driver, port)) {
			return 12;
		}
		if (ours.driver->identity.directory.find(tmp / "ours/") != 0) {
			return 13;
		}
		// Both keep working, and the process still resolves unixODBC's entry points.
		return dm.Smoke(customer, port) && EmbeddedSmoke(*ours.driver, port) &&
		               ::dlsym(RTLD_DEFAULT, "SQLDriverConnect") == reinterpret_cast<void *>(dm.DriverConnect)
		           ? 0
		           : 14;
	});
	CHECK(status == 0);
}

TEST_CASE("allocator child", "[.allocator-child]") {
	// Run under a preloaded allocator: what the load says, one line per field.
	const auto result = LoadEmbeddedDriver();
	if (result.ok()) {
		std::printf("loaded\n");
	} else {
		std::printf("kind %s\nmessage %s\n", redshift_driver_test::ErrorKindName(result.error.kind),
		            result.error.message.c_str());
		for (const auto &attempt : result.error.attempts) {
			std::printf("attempt %s: %s\n", attempt.directory.c_str(), attempt.reason.c_str());
		}
	}
	std::fflush(nullptr);
	// A load that succeeds is the failure here: under such an allocator the loader must refuse.
	::_exit(result.ok() ? 1 : 0);
}

namespace {

// Runs the allocator child with `allocator` in LD_PRELOAD. Under RTLD_DEEPBIND the driver's own free is
// glibc's, while memory glibc allocates for it comes from the allocator: the driver's first call frees a strdup
// copy of its own directory, and the process aborts. So the load must be refused, before a base is chosen
// (nothing is written), by the allocator's file name, with its path only in the attempt.
void RequireAllocatorRefused(const std::string &allocator) {
	TempDir tmp;
	const int status = redshift_driver_test::RerunHidden("[.allocator-child]", allocator, tmp.path());
	const auto output = ReadWholeFile(tmp / "child-output.txt");
	INFO(output);
	REQUIRE(WIFEXITED(status));
	CHECK(WEXITSTATUS(status) == 0);
	const auto name = allocator.substr(allocator.rfind('/') + 1);
	CHECK(output.find("kind REFUSED_ENVIRONMENT\n") != std::string::npos);
	const std::string message = "message " + name +
	                            " has replaced the C library's malloc in this process, and the Redshift ODBC driver "
	                            "uses the C library's own allocator, so memory can pass between the two and crash the "
	                            "process. To connect, start the program without " +
	                            name +
	                            " in LD_PRELOAD; a program that links or defines its own malloc cannot use the "
	                            "Redshift driver yet.\n";
	CHECK(output.find(message) != std::string::npos);
	CHECK(output.find("attempt " + allocator +
	                  ": replaces the C library's malloc, free, calloc or realloc for the C library's own calls\n") !=
	      std::string::npos);
	struct stat st;
	CHECK(::lstat((tmp / ".redshift-driver").c_str(), &st) != 0);
}

// Runs `program` with `preload` in LD_PRELOAD (none when empty) and its standard output in `out`; the wait status.
int RunProgram(const std::string &program, const std::string &preload, const std::string &out) {
	std::fflush(nullptr);
	const pid_t child = ::fork();
	REQUIRE(child >= 0);
	if (child == 0) {
		if (preload.empty()) {
			::unsetenv("LD_PRELOAD");
		} else {
			::setenv("LD_PRELOAD", preload.c_str(), 1);
		}
		const int fd = ::open(out.c_str(), O_WRONLY | O_CREAT | O_TRUNC, 0600);
		if (fd >= 0) {
			::dup2(fd, 1);
		}
		::execl(program.c_str(), program.c_str(), static_cast<char *>(nullptr));
		::_exit(127);
	}
	int status = 0;
	REQUIRE(::waitpid(child, &status, 0) == child);
	return status;
}

} // namespace

TEST_CASE("An allocator that replaces the C library's is refused before the load, by its file name",
          "[driver][coexistence]") {
	// The test's own allocator, built here, so the rule is checked on every host.
	RequireAllocatorRefused(redshift_driver_test::TestInputPath(REDSHIFT_TEST_ALLOCATOR_INTERPOSER));
}

TEST_CASE("An allocator that replaces only calloc, or only realloc, is refused too", "[driver][coexistence]") {
	// malloc and free stay the C library's, so each of the two names is what is found. The refusal's message
	// says the library replaced malloc for a replacement of any of the four, as the attempt names all four.
	RequireAllocatorRefused(redshift_driver_test::TestInputPath(REDSHIFT_TEST_CALLOC_INTERPOSER));
	RequireAllocatorRefused(redshift_driver_test::TestInputPath(REDSHIFT_TEST_REALLOC_INTERPOSER));
}

TEST_CASE("With jemalloc preloaded the load is refused, not crashed", "[driver][coexistence][real]") {
	// jemalloc uses static TLS, so it cannot be dlopen'ed into this process: it is found on disk.
	const auto jemalloc = redshift_driver_test::HostLibrary("REDSHIFT_TEST_JEMALLOC", "libjemalloc.so.2");
	if (jemalloc.empty()) {
		return;
	}
	RequireAllocatorRefused(jemalloc);
}

TEST_CASE("With tcmalloc preloaded the load is refused, not crashed", "[driver][coexistence][real]") {
	// Where REDSHIFT_TEST_TCMALLOC names it or the host has it; it is seldom installed, so a run that
	// requires host libraries does not require this one.
	const char *named = std::getenv("REDSHIFT_TEST_TCMALLOC");
	std::string tcmalloc = named != nullptr ? named : "";
	for (const auto &dir : duckdb::redshift_driver::detail::SystemLibraryDirectories()) {
		if (tcmalloc.empty() && ::access((dir + "/libtcmalloc.so.4").c_str(), R_OK) == 0) {
			tcmalloc = dir + "/libtcmalloc.so.4";
		}
	}
	if (tcmalloc.empty()) {
		WARN("libtcmalloc.so.4 is not installed and REDSHIFT_TEST_TCMALLOC is not set; this row was not run");
		return;
	}
	RequireAllocatorRefused(tcmalloc);
}

TEST_CASE("The allocator refusal names the object's file and never its path, or says another allocator",
          "[driver][coexistence]") {
	detail::AllocatorReplacement named;
	named.replaced = true;
	named.file = "/opt/example/lib/libexample_alloc.so.1";
	auto error = detail::AllocatorReplacementError(named);
	CHECK(error.kind == DriverErrorKind::REFUSED_ENVIRONMENT);
	CHECK(error.message.find("libexample_alloc.so.1 has replaced the C library's malloc") == 0);
	CHECK(error.message.find("To connect, start the program without libexample_alloc.so.1 in LD_PRELOAD; a program "
	                         "that links or defines its own malloc cannot use the Redshift driver yet.") !=
	      std::string::npos);
	CHECK(error.message.find('/') == std::string::npos);
	REQUIRE(error.attempts.size() == 1);
	CHECK(error.attempts[0].directory == named.file);
	// No loaded object holds the address: nothing to name.
	detail::AllocatorReplacement unnamed;
	unnamed.replaced = true;
	error = detail::AllocatorReplacementError(unnamed);
	CHECK(error.message.find("another allocator has replaced the C library's malloc") == 0);
	CHECK(error.message.find("To connect, start the program without another allocator in LD_PRELOAD;") !=
	      std::string::npos);
}

TEST_CASE("The C library's own allocator is not a replacement, nor is a program's canonical PLT entry, but a "
          "program that defines malloc is",
          "[driver][coexistence]") {
	// This process: no allocator preloaded, so every slot is the C library's own.
	const auto here = detail::FindAllocatorReplacement();
	CHECK_FALSE(here.replaced);
	// A non-PIE program that takes malloc's and free's addresses: the C library's GLOB_DAT slots for them point
	// at the program's PLT entries (python-build-standalone's Python is such a program), which define nothing.
	TempDir tmp;
	const auto program = redshift_driver_test::TestInputPath(REDSHIFT_TEST_NON_PIE_HOST);
	const auto out = tmp / "out.txt";
	int status = RunProgram(program, "", out);
	auto output = ReadWholeFile(out);
	INFO(output);
	REQUIRE(WIFEXITED(status));
	REQUIRE(output.find("canonical yes\n") != std::string::npos);
	CHECK(output.find("replacement none\n") != std::string::npos);
	// The same program with an allocator preloaded: its PLT entries do not hide the replacement, which only the
	// C library's JUMP_SLOT slots show here.
	const auto allocator = redshift_driver_test::TestInputPath(REDSHIFT_TEST_ALLOCATOR_INTERPOSER);
	status = RunProgram(program, allocator, out);
	output = ReadWholeFile(out);
	INFO(output);
	REQUIRE(WIFEXITED(status));
	CHECK(output.find("replacement " + allocator + "\n") != std::string::npos);
	// A program that defines malloc itself is a replacement, named by the program's own file.
	const auto own_malloc = redshift_driver_test::TestInputPath(REDSHIFT_TEST_OWN_MALLOC_HOST);
	status = RunProgram(own_malloc, "", out);
	output = ReadWholeFile(out);
	INFO(output);
	REQUIRE(WIFEXITED(status));
	CHECK(output.find("replacement " + own_malloc + "\n") != std::string::npos);
}

TEST_CASE("host library child", "[.host-library-child]") {
	// No variable or host has this library, so HostLibrary decides by REDSHIFT_TEST_REQUIRE_HOST_LIBRARIES.
	const auto path =
	    redshift_driver_test::HostLibrary("REDSHIFT_TEST_UNINSTALLED_LIBRARY", "libredshift-not-installed.so.0");
	std::printf("host library: [%s]\n", path.c_str());
}

TEST_CASE("A host library that is not found warns and stops the row, or fails it when the run requires host "
          "libraries; an empty variable counts as unset",
          "[driver][coexistence]") {
	struct Row {
		const char *variable; // REDSHIFT_TEST_UNINSTALLED_LIBRARY's value; nullptr leaves it unset
		const char *required; // REDSHIFT_TEST_REQUIRE_HOST_LIBRARIES's value
		bool fails;           // the row's case fails
		bool warns;           // the row warns and HostLibrary returns nothing
		const char *says;     // in the child's output
	};
	const char *warned = "REDSHIFT_TEST_UNINSTALLED_LIBRARY is not set; this row was not run";
	const char *failed = "REDSHIFT_TEST_REQUIRE_HOST_LIBRARIES=1 requires this row to run";
	// The unset rows must not inherit a caller's value.
	const redshift_driver_test::ScopedEnv unset("REDSHIFT_TEST_UNINSTALLED_LIBRARY", nullptr);
	for (const Row &row : {Row {nullptr, "0", false, true, warned}, Row {nullptr, "1", true, false, failed},
	                       Row {"", "0", false, true, warned}, Row {"", "1", true, false, failed},
	                       Row {"/no/such/libnamed.so", "1", false, false, "host library: [/no/such/libnamed.so]"}}) {
		TempDir tmp;
		std::vector<std::pair<std::string, std::string>> extra {{"REDSHIFT_TEST_REQUIRE_HOST_LIBRARIES", row.required}};
		if (row.variable != nullptr) {
			extra.emplace_back("REDSHIFT_TEST_UNINSTALLED_LIBRARY", row.variable);
		}
		const int status = redshift_driver_test::RerunHidden("[.host-library-child]", "", tmp.path(), extra);
		const auto output = ReadWholeFile(tmp / "child-output.txt");
		INFO("variable " << (row.variable == nullptr ? "unset" : "'" + std::string(row.variable) + "'") << ", required "
		                 << row.required << ": " << output);
		REQUIRE(WIFEXITED(status));
		CHECK(WEXITSTATUS(status) == (row.fails ? 1 : 0));
		CHECK(Unwrapped(output).find(Unwrapped(row.says)) != std::string::npos);
		CHECK((output.find("host library: []\n") != std::string::npos) == row.warns);
	}
}

// The probe's load line names the error's kind (probe/driver_probe.cpp): each kind has its own name, so a
// missing or wrong arm fails here.
TEST_CASE("Each DriverErrorKind has its own name in the probe's report", "[driver][coexistence]") {
	using duckdb::redshift_driver::DriverErrorKind;
	const std::vector<std::pair<DriverErrorKind, std::string>> names = {
	    {DriverErrorKind::NOT_BUILT, "NOT_BUILT"},
	    {DriverErrorKind::EMBEDDED_BYTES_CORRUPT, "EMBEDDED_BYTES_CORRUPT"},
	    {DriverErrorKind::NO_USABLE_DIRECTORY, "NO_USABLE_DIRECTORY"},
	    {DriverErrorKind::MISSING_PREREQUISITE, "MISSING_PREREQUISITE"},
	    {DriverErrorKind::LOAD_FAILED, "LOAD_FAILED"},
	    {DriverErrorKind::MISSING_ENTRY_POINT, "MISSING_ENTRY_POINT"},
	    {DriverErrorKind::REFUSED_ENVIRONMENT, "REFUSED_ENVIRONMENT"}};
	for (const auto &kind : names) {
		CHECK(redshift_driver_test::ErrorKindName(kind.first) == kind.second);
	}
	// The table is every kind today; a kind added later without a name is a compile error in error_kind_name.hpp.
	// A value past REFUSED_ENVIRONMENT, which no enumerator has, is UNKNOWN.
	CHECK(static_cast<size_t>(DriverErrorKind::REFUSED_ENVIRONMENT) + 1 == names.size());
	CHECK(std::string(redshift_driver_test::ErrorKindName(static_cast<DriverErrorKind>(names.size()))) == "UNKNOWN");
}

TEST_CASE("With no socket to be had, ClosedLocalPort is -1", "[driver][coexistence]") {
	TempDir tmp;
	const int status = redshift_driver_test::InChild(tmp.path(), [] {
		const rlimit no_descriptors {0, 0}; // socket() fails with EMFILE
		if (::setrlimit(RLIMIT_NOFILE, &no_descriptors) != 0) {
			return 2;
		}
		return redshift_driver_test::ClosedLocalPort() == -1 ? 0 : 1;
	});
	CHECK(status == 0);
}

namespace {

// An ODBC API whose environment attribute, or else connection handle, cannot be set or allocated. It records
// each call.
struct FailingAllocation {
	bool attribute_fails = false;
	mutable std::vector<std::string> calls;
	SQLRETURN SetEnvAttr(SQLHENV, SQLINTEGER, SQLPOINTER, SQLINTEGER) const {
		calls.emplace_back("SetEnvAttr");
		return attribute_fails ? SQL_ERROR : SQL_SUCCESS;
	}
	SQLRETURN AllocHandle(SQLSMALLINT type, SQLHANDLE, SQLHANDLE *) const {
		calls.emplace_back(type == SQL_HANDLE_DBC ? "AllocHandle DBC" : "AllocHandle other");
		return SQL_ERROR;
	}
	SQLRETURN FreeHandle(SQLSMALLINT type, SQLHANDLE handle) const {
		calls.emplace_back(type == SQL_HANDLE_ENV && handle == Env() ? "FreeHandle ENV" : "FreeHandle other");
		return SQL_SUCCESS;
	}
	template <class... ARGS>
	SQLRETURN DriverConnect(ARGS...) const {
		calls.emplace_back("DriverConnect");
		return SQL_ERROR;
	}
	template <class... ARGS>
	SQLRETURN GetDiagRec(ARGS...) const {
		calls.emplace_back("GetDiagRec");
		return SQL_NO_DATA;
	}
	static SQLHENV Env() {
		static int env;
		return &env;
	}
};

} // namespace

TEST_CASE("The smoke stops, and frees the environment, when a handle cannot be set up", "[driver][coexistence]") {
	for (bool attribute_fails : {true, false}) {
		FailingAllocation api;
		api.attribute_fails = attribute_fails;
		std::string answer;
		INFO((attribute_fails ? "SQL_ATTR_ODBC_VERSION refused" : "no connection handle"));
		CHECK_FALSE(redshift_driver_test::SmokeOn(api, FailingAllocation::Env(), 1, answer));
		CHECK(answer == "handle allocation failed");
		const std::vector<std::string> expected =
		    attribute_fails ? std::vector<std::string> {"SetEnvAttr", "FreeHandle ENV"}
		                    : std::vector<std::string> {"SetEnvAttr", "AllocHandle DBC", "FreeHandle ENV"};
		CHECK(api.calls == expected);
	}
}

namespace {

// An ODBC API whose connect returns `rc` with one diagnostic record, `state` and `message`, or none when
// `state` is null: what SmokeOn's pass rule is given. GetDiagRec returns `diag_rc` after filling the record.
struct ScriptedConnect {
	SQLRETURN rc;
	const char *state;
	const char *message;
	SQLRETURN diag_rc = SQL_SUCCESS;
	SQLRETURN SetEnvAttr(SQLHENV, SQLINTEGER, SQLPOINTER, SQLINTEGER) const {
		return SQL_SUCCESS;
	}
	SQLRETURN AllocHandle(SQLSMALLINT, SQLHANDLE, SQLHANDLE *out) const {
		static int handle;
		*out = &handle;
		return SQL_SUCCESS;
	}
	SQLRETURN FreeHandle(SQLSMALLINT, SQLHANDLE) const {
		return SQL_SUCCESS;
	}
	template <class... ARGS>
	SQLRETURN DriverConnect(ARGS...) const {
		return rc;
	}
	SQLRETURN GetDiagRec(SQLSMALLINT, SQLHANDLE, SQLSMALLINT record, SQLCHAR *sqlstate, SQLINTEGER *, SQLCHAR *text,
	                     SQLSMALLINT size, SQLSMALLINT *length) const {
		if (state == nullptr || record != 1) {
			return SQL_NO_DATA;
		}
		std::snprintf(reinterpret_cast<char *>(sqlstate), 6, "%s", state);
		*length = static_cast<SQLSMALLINT>(std::snprintf(reinterpret_cast<char *>(text), size, "%s", message));
		return diag_rc;
	}
	static SQLHENV Env() {
		static int env;
		return &env;
	}
};

} // namespace

TEST_CASE("The smoke passes only on the driver's own refused-connection error", "[driver][coexistence]") {
	const char *refused = "[Redshift][ODBC Driver] could not connect to server: Connection refused";
	struct Row {
		ScriptedConnect api;
		bool passes;
		const char *what;
	};
	for (const Row &row :
	     {Row {{SQL_ERROR, "HY000", refused}, true, "the driver's own refusal"},
	      Row {{SQL_SUCCESS, nullptr, nullptr}, false, "a connect that succeeds, with no diagnostic"},
	      Row {{SQL_SUCCESS_WITH_INFO, "HY000", refused},
	           false,
	           "a connect that succeeds with the refusal as a warning"},
	      Row {{SQL_ERROR, nullptr, nullptr}, false, "an error with no diagnostic"},
	      Row {{SQL_ERROR, "HY000", refused, SQL_NO_DATA}, false, "the refusal in a record GetDiagRec does not return"},
	      Row {{SQL_ERROR, "IM002",
	            "[unixODBC][Driver Manager]Data source name not found and no default driver specified"},
	           false,
	           "the driver manager's IM002: no driver was reached"},
	      Row {{SQL_ERROR, "HY000", "[Redshift][ODBC Driver] timeout expired"}, false, "the driver's timeout"},
	      Row {{SQL_ERROR, "08001", refused}, false, "the refusal under another SQLSTATE"},
	      Row {{SQL_ERROR, "HY000", "[unixODBC] Connection refused"}, false, "a refusal that is not the driver's"}}) {
		std::string answer;
		const bool passed = redshift_driver_test::SmokeOn(row.api, ScriptedConnect::Env(), 1, answer);
		INFO(row.what << ": " << answer);
		CHECK(passed == row.passes);
	}
}
