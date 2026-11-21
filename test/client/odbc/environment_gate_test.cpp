// The driver's environment and logging on the embedded driver, each case in a new process whose
// environment is set before it starts: every refused variable, set in turn, refuses the connect
// before any socket opens, and one the driver reads at load before the driver is mapped; logging
// turned on through each INI file the driver reads writes no log and shows no canary.
// The controls show that the trap stops a socket, a socket pair and a call
// of another ABI, that the map check sees a mapped file, and that the child runner reports a failed
// setup (126), a failed exec (127), a failed case and a timeout. Linux only (child_process.hpp); the
// driver's cases skip in a build without it (the INI case also without the test endpoints), and with
// REDSHIFT_REQUIRE_DRIVER=1 they fail instead.

// catch.hpp comes first, as in this directory's other tests.
#include "catch.hpp"

#include "canary_output.hpp"
#include "child_process.hpp"
#include "driver_policy.hpp"
#include "fake_driver_fixture.hpp"
#include "live_target.hpp"
#include "thrown.hpp"
#include "trust_store_test_support.hpp"

#include <algorithm>
#include <climits>
#include <cstdint>
#include <cstdlib>
#include <dirent.h>
#include <fstream>
#include <iostream>
#include <string>
#include <sys/mman.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <thread>
#include <vector>

using redshift_test::CHILD_MODE;
using redshift_test::DriverCaseSkippedFor;
using redshift_test::RunSuiteChild;
using redshift_trust_test::TemporaryDirectory;

namespace {

//! The hidden case a child runs. Its CHILD_MODE: `socket` opens one under the trap, `socketpair` opens a
//! pair under it, `no-socket` opens none under it, `x32-call` and `i386-call` make a call of another ABI
//! under it, `hang` sleeps past any timeout a case gives, `fail` fails, `driver-skip` skips as a driver case
//! does, with a reason, `driver-run` has none and says it ran, `connect` connects through the embedded
//! driver and reports what it saw, and `trapped-connect` does so under the trap.
const char *const CHILD_TAG = "[.environment-gate-child]";
//! Long enough for a driver load and a refused connect on a loaded host; a hang fails the case.
const auto CHILD_TIMEOUT = std::chrono::seconds(120);

//! One row of policy/refused-environment.txt: a name and the earliest phase the driver reads it in.
struct RefusedRow {
	std::string name;
	std::string phase;
};

//! Every refused variable, from the policy list the driver's own list is generated from.
std::vector<RefusedRow> RefusedRows() {
	std::vector<RefusedRow> refused;
	for (const auto &row : redshift_test::PolicyRows("refused-environment.txt")) {
		REQUIRE(row.size() == 2);
		refused.push_back({row[0], row[1]});
	}
	return refused;
}

//! The location of each Linux `ini` row of policy/config-files.txt: the files the driver reads its
//! logging settings from. A location holding a backslash is a Windows row, which this case leaves out.
std::vector<std::string> IniLocations() {
	std::vector<std::string> locations;
	for (const auto &row : redshift_test::PolicyRows("config-files.txt")) {
		if (row.size() > 1 && row[1] == "ini" && row[0].find('\\') == std::string::npos) {
			locations.push_back(row[0]);
		}
	}
	return locations;
}

//! A child's whole environment: `mode`, its own home and driver directory, the canary keys for the
//! default chain and, in the test-endpoint build, both endpoints at closed ports on this host, so a
//! connect that gets past the client fails in the driver without leaving the host.
std::vector<std::string> ChildEnvironment(const std::string &mode, const std::string &home,
                                          const std::string &driver_directory) {
	std::vector<std::string> environment {std::string(CHILD_MODE) + "=" + mode,
	                                      "PATH=/usr/bin:/bin",
	                                      "HOME=" + home,
	                                      "REDSHIFT_DRIVER_DIR=" + driver_directory,
	                                      std::string("AWS_ACCESS_KEY_ID=") + redshift_test::CANARY_KEY_ID,
	                                      std::string("AWS_SECRET_ACCESS_KEY=") + redshift_test::CANARY_SECRET,
	                                      std::string("AWS_SESSION_TOKEN=") + redshift_test::CANARY_TOKEN,
	                                      "AWS_REGION=us-east-1",
	                                      "AWS_EC2_METADATA_DISABLED=true"};
	if (duckdb::BuildHasTestEndpoints()) {
		environment.push_back(std::string(duckdb::TEST_HOST_VARIABLE) + "=127.0.0.1");
		environment.push_back(std::string(duckdb::TEST_URL_VARIABLE) + "=https://127.0.0.1:9");
	}
	return environment;
}

//! Why the INI case cannot run here, or empty when it can: it needs the embedded driver and, so that its
//! connect stays on this host, the test-endpoint build.
std::string IniCaseSkipReason(const std::string &driver_reason, bool has_test_endpoints) {
	if (!driver_reason.empty()) {
		return driver_reason;
	}
	return has_test_endpoints ? std::string() : "skipped: only the test-endpoint build keeps the connect on this host";
}

//! The message a connect refused for `row`'s variable starts with: the loader's, before the load, for a
//! name read at load, else the client's own.
std::string RefusalOf(const RefusedRow &row) {
	const auto refusal = row.name + " is set; the Redshift driver would read it";
	return row.phase == "load" ? "the Redshift driver could not be loaded: " + refusal : refusal;
}

//! The one driver set, a directory, under `driver_directory`, which a connect wrote; the loader keeps
//! its lock file beside the sets.
std::string DriverSet(const std::string &driver_directory) {
	const auto root = driver_directory + "/.redshift-driver";
	std::vector<std::string> sets;
	for (const auto &name : redshift_test::NamesIn(root)) {
		struct stat info;
		if (lstat((root + "/" + name).c_str(), &info) == 0 && S_ISDIR(info.st_mode)) {
			sets.push_back(root + "/" + name);
		}
	}
	REQUIRE(sets.size() == 1);
	return sets[0];
}

//! Writes `content` to `path`, in a directory the loader may have left read-only to its owner.
void Plant(const std::string &path, const std::string &content) {
	const auto directory = path.substr(0, path.rfind('/'));
	struct stat info;
	REQUIRE(stat(directory.c_str(), &info) == 0);
	REQUIRE(chmod(directory.c_str(), info.st_mode | S_IWUSR) == 0);
	std::ofstream file(path.c_str());
	file << content;
	file.close();
	REQUIRE(chmod(directory.c_str(), info.st_mode & 07777) == 0);
	// An unwritten file would let the cases that expect no log pass without checking anything.
	REQUIRE(file.good());
}

//! The INI text that turns `location`'s logging on, with the log in `log_directory`: the driver's
//! own file takes its settings under [Driver], an odbc.ini under [ODBC].
std::string LoggingOn(const std::string &location, const std::string &log_directory) {
	const auto section = location.find("amazon.redshiftodbc.ini") != std::string::npos ? "[Driver]" : "[ODBC]";
	return std::string(section) + "\nLogLevel=6\nLogPath=" + log_directory + "\n";
}

//! A connect through the embedded driver with the canary credentials, then what it threw, its
//! extra_info, what it logged and whether a file under REDSHIFT_DRIVER_DIR is mapped, on standard
//! output for the parent to read.
void ConnectAndReport() {
	duckdb::SetDriverSourceForTesting(&duckdb::redshift_driver::LoadEmbeddedDriver);
	auto sink = duckdb::make_shared_ptr<redshift_test::CapturingLogSink>();
	auto config = redshift_test::UsEast1Config();
	config.aws_secret = "canaries";
	const auto credentials = redshift_test::CanaryCredentials();
	const auto error =
	    redshift_test::CaptureError([&]() { duckdb::OpenRedshiftConnection(config, &credentials, sink); });
	std::cout << "thrown: " << error.RawMessage() << "\n";
	for (const auto &entry : error.ExtraInfo()) {
		std::cout << "extra_info: " << entry.first << "=" << entry.second << "\n";
	}
	for (const auto &line : sink->Lines()) {
		std::cout << "log: " << line << "\n";
	}
	std::cout << "mapped: " << redshift_test::MapsFileUnder(std::getenv("REDSHIFT_DRIVER_DIR")) << "\n";
	char cwd[PATH_MAX];
	std::cout << "cwd: " << (getcwd(cwd, sizeof(cwd)) ? cwd : "unknown") << "\n";
	duckdb::ResetProcessStateForTesting();
}

//! Makes getpid as another ABI does, `x32` with the x32 bit set in its number or `i386` through int
//! 0x80, where getpid is call 20, and prints what it returned. Only an x86_64 build has either ABI.
void ForeignGetpid(const std::string &abi) {
#if defined(__x86_64__)
	int64_t result = 20;
	if (abi == "x32") {
		result = syscall(redshift_test::TRAP_X32_CALL | SYS_getpid);
	} else {
		__asm__ volatile("int $0x80" : "+a"(result) : : "r8", "r9", "r10", "r11", "memory");
	}
	std::cout << abi << " call returned " << result << "\n";
#else
	FAIL("an " + abi + " call needs an x86_64 build");
#endif
}

} // namespace

TEST_CASE("A child the environment gate's cases run", "[.environment-gate-child]") {
	const std::string mode = std::getenv(CHILD_MODE) ? std::getenv(CHILD_MODE) : "";
	if (mode == "socket" || mode == "socketpair" || mode == "no-socket" || mode == "trapped-connect" ||
	    mode == "x32-call" || mode == "i386-call") {
		redshift_test::TrapSockets();
	}
	if (mode == "socket") {
		const int opened = socket(AF_INET, SOCK_STREAM, 0);
		std::cout << "socket returned " << opened << "\n";
	} else if (mode == "socketpair") {
		int pair[2];
		std::cout << "socketpair returned " << socketpair(AF_UNIX, SOCK_STREAM, 0, pair) << "\n";
	} else if (mode == "no-socket") {
		std::cout << "no socket opened\n";
	} else if (mode == "x32-call" || mode == "i386-call") {
		ForeignGetpid(mode.substr(0, mode.find('-')));
	} else if (mode == "fail") {
		FAIL("the child fails, as asked");
	} else if (mode == "hang") {
		std::this_thread::sleep_for(std::chrono::seconds(30));
	} else if (mode == "driver-skip" || mode == "driver-run") {
		if (!DriverCaseSkippedFor(mode == "driver-skip" ? "skipped: the child's reason" : "")) {
			std::cout << "the driver case ran\n";
		}
	} else {
		REQUIRE((mode == "connect" || mode == "trapped-connect"));
		ConnectAndReport();
	}
}

TEST_CASE("A child that opens a socket under the trap is killed before the call returns", "[odbc]") {
	TemporaryDirectory directory;
	const auto run = RunSuiteChild(CHILD_TAG, ChildEnvironment("socket", directory.Directory("home"), ""),
	                               directory.Directory("cwd"), CHILD_TIMEOUT);

	CHECK(run.Trapped());
	CHECK(run.output.find("socket returned") == std::string::npos);
}

TEST_CASE("A child that opens a socket pair under the trap is killed before the call returns", "[odbc]") {
	TemporaryDirectory directory;
	const auto run = RunSuiteChild(CHILD_TAG, ChildEnvironment("socketpair", directory.Directory("home"), ""),
	                               directory.Directory("cwd"), CHILD_TIMEOUT);

	CHECK(run.Trapped());
	CHECK(run.output.find("socketpair returned") == std::string::npos);
}

TEST_CASE("A child that opens no socket under the trap exits normally", "[odbc]") {
	TemporaryDirectory directory;
	const auto run = RunSuiteChild(CHILD_TAG, ChildEnvironment("no-socket", directory.Directory("home"), ""),
	                               directory.Directory("cwd"), CHILD_TIMEOUT);

	CHECK(run.exit_code == 0);
	CHECK_FALSE(run.Trapped());
	CHECK_THAT(run.output, Catch::Contains("no socket opened"));
}

#if defined(__x86_64__)
TEST_CASE("A child that makes an x32 call under the trap is killed before the call returns", "[odbc]") {
	TemporaryDirectory directory;
	const auto run = RunSuiteChild(CHILD_TAG, ChildEnvironment("x32-call", directory.Directory("home"), ""),
	                               directory.Directory("cwd"), CHILD_TIMEOUT);

	CHECK(run.Trapped());
	CHECK(run.output.find("call returned") == std::string::npos);
}

// On a kernel built without IA32 emulation, `int 0x80` faults before the trap sees it, so this case
// fails there rather than skipping.
TEST_CASE("A child that makes a 32-bit call under the trap is killed before the call returns", "[odbc]") {
	TemporaryDirectory directory;
	const auto run = RunSuiteChild(CHILD_TAG, ChildEnvironment("i386-call", directory.Directory("home"), ""),
	                               directory.Directory("cwd"), CHILD_TIMEOUT);

	CHECK(run.Trapped());
	CHECK(run.output.find("call returned") == std::string::npos);
}
#endif

// StartSuiteChild's child exits 126, before the suite starts, when its own setup fails.
TEST_CASE("A child that cannot enter its directory exits 126 before any case runs", "[odbc]") {
	TemporaryDirectory directory;
	const auto output_path = directory.Path("child.out");
	const auto child =
	    redshift_test::StartSuiteChild(CHILD_TAG, ChildEnvironment("no-socket", directory.Directory("home"), ""),
	                                   directory.Path("missing"), output_path);
	int status = 0;
	REQUIRE(waitpid(child, &status, 0) == child);

	CHECK(WIFEXITED(status));
	CHECK(WEXITSTATUS(status) == 126);
	std::ifstream output(output_path.c_str());
	CHECK(std::string(std::istreambuf_iterator<char>(output), std::istreambuf_iterator<char>()).empty());
}

// StartSuiteChild's child exits 127 when execve fails: one environment string longer than the kernel
// takes (MAX_ARG_STRLEN, 32 pages, at most 2 MiB) fails it with E2BIG.
TEST_CASE("A child whose suite the kernel cannot start exits 127 before any case runs", "[odbc]") {
	TemporaryDirectory directory;
	auto environment = ChildEnvironment("no-socket", directory.Directory("home"), "");
	environment.push_back("REDSHIFT_TEST_PADDING=" + std::string(4 * 1024 * 1024, 'x'));
	const auto run = RunSuiteChild(CHILD_TAG, environment, directory.Directory("cwd"), CHILD_TIMEOUT);

	CHECK(run.exit_code == 127);
	CHECK(run.output.empty());
}

TEST_CASE("A child whose case fails exits with a code other than 0", "[odbc]") {
	TemporaryDirectory directory;
	const auto run = RunSuiteChild(CHILD_TAG, ChildEnvironment("fail", directory.Directory("home"), ""),
	                               directory.Directory("cwd"), CHILD_TIMEOUT);

	CHECK(run.exit_code > 0);
	CHECK_THAT(run.output, Catch::Contains("the child fails, as asked"));
}

namespace {

//! A child that runs `spec` with REDSHIFT_REQUIRE_DRIVER set to `value`, or unset for an empty `value`, in
//! directories of its own under `directory`.
redshift_test::ChildRun RunRequiringDriver(TemporaryDirectory &directory, const std::string &spec,
                                           const std::string &mode, const std::string &value) {
	static int runs = 0;
	const auto name = std::to_string(++runs);
	auto environment = ChildEnvironment(mode, directory.Directory("home-" + name), "");
	if (!value.empty()) {
		environment.push_back("REDSHIFT_REQUIRE_DRIVER=" + value);
	}
	return RunSuiteChild(spec, environment, directory.Directory("cwd-" + name), CHILD_TIMEOUT);
}

} // namespace

TEST_CASE("A driver case's skip becomes a failure only where REDSHIFT_REQUIRE_DRIVER=1", "[odbc]") {
	TemporaryDirectory directory;
	const auto failed = RunRequiringDriver(directory, CHILD_TAG, "driver-skip", "1");
	INFO(failed.output);
	CHECK(failed.exit_code > 0);
	// Catch wraps its report at 80 columns, so only the message's start is sure to be on one line.
	CHECK_THAT(failed.output, Catch::Contains("REDSHIFT_REQUIRE_DRIVER=1 requires"));
	for (const auto &value : {"", "0"}) {
		const auto skipped = RunRequiringDriver(directory, CHILD_TAG, "driver-skip", value);
		INFO(value << ": " << skipped.output);
		CHECK(skipped.exit_code == 0);
		CHECK_THAT(skipped.output, Catch::Contains("All tests were skipped"));
	}
	// A case with no reason to skip runs, the variable set or not.
	for (const auto &value : {"", "1"}) {
		const auto ran = RunRequiringDriver(directory, CHILD_TAG, "driver-run", value);
		INFO(value << ": " << ran.output);
		CHECK(ran.exit_code == 0);
		CHECK_THAT(ran.output, Catch::Contains("the driver case ran"));
	}
}

// The three cases that need the driver, each by the start of its name, which has no comma for Catch to
// split it at.
TEST_CASE("Without the driver, each driver case fails where REDSHIFT_REQUIRE_DRIVER=1", "[odbc]") {
	if (redshift_test::DriverSkipReason().empty()) {
		SKIP_TEST("skipped: this build has the driver, so the cases run instead");
		return;
	}
	TemporaryDirectory directory;
	for (const auto &spec : {"Each refused variable stops the connect before a socket*",
	                         "Without a refused variable the connect opens a socket*",
	                         "Logging turned on through each INI file the driver reads*"}) {
		const auto failed = RunRequiringDriver(directory, spec, "unused", "1");
		INFO(spec << ": " << failed.output);
		CHECK(failed.exit_code > 0);
		CHECK_THAT(failed.output, Catch::Contains("REDSHIFT_REQUIRE_DRIVER=1 requires"));
		const auto skipped = RunRequiringDriver(directory, spec, "unused", "");
		INFO(spec << ": " << skipped.output);
		CHECK(skipped.exit_code == 0);
		CHECK_THAT(skipped.output, Catch::Contains("All tests were skipped"));
	}
}

// So REDSHIFT_REQUIRE_DRIVER=1 fails the INI case in a build with the driver but no test endpoints too.
TEST_CASE("The INI case skips for a build without the driver or without the test endpoints", "[odbc]") {
	const std::string no_driver = "skipped: this build has no embedded driver";
	CHECK(IniCaseSkipReason("", true).empty());
	CHECK(IniCaseSkipReason("", false) == "skipped: only the test-endpoint build keeps the connect on this host");
	CHECK(IniCaseSkipReason(no_driver, true) == no_driver);
	CHECK(IniCaseSkipReason(no_driver, false) == no_driver);
}

TEST_CASE("A child that outlives its timeout is killed at the timeout", "[odbc]") {
	TemporaryDirectory directory;
	const auto started = std::chrono::steady_clock::now();
	const auto run = RunSuiteChild(CHILD_TAG, ChildEnvironment("hang", directory.Directory("home"), ""),
	                               directory.Directory("cwd"), std::chrono::seconds(1));

	CHECK(run.timed_out);
	CHECK(run.signal == SIGKILL);
	// Well before the child's own 30 seconds end.
	CHECK(std::chrono::steady_clock::now() - started < std::chrono::seconds(20));
}

TEST_CASE("The map check sees a file mapped from a directory, and none once it is unmapped", "[odbc]") {
	TemporaryDirectory directory;
	const auto path = directory.File("mapped", "x");
	const int file = open(path.c_str(), O_RDONLY);
	REQUIRE(file >= 0);
	void *mapping = mmap(nullptr, 1, PROT_READ, MAP_PRIVATE, file, 0);
	close(file);
	REQUIRE(mapping != MAP_FAILED);
	const auto parent = path.substr(0, path.rfind('/'));
	CHECK(redshift_test::MapsFileUnder(parent));
	munmap(mapping, 1);
	CHECK_FALSE(redshift_test::MapsFileUnder(parent));
}

TEST_CASE("Each refused variable stops the connect before a socket, a load-phase one before the load", "[odbc]") {
	if (DriverCaseSkippedFor(redshift_test::DriverSkipReason())) {
		return;
	}
	TemporaryDirectory directory;
	const auto home = directory.Directory("home");
	// One driver directory for every child, so the set is written once; a refusal before the load
	// leaves it unmapped, which is what the check reads, not whether it exists.
	const auto driver = directory.Directory("driver");
	const auto rows = RefusedRows();
	std::vector<std::string> names;
	for (const auto &row : rows) {
		names.push_back(row.name);
	}
	// A key-log file, and an endpoint that would redirect the control-plane call: a policy edit that
	// drops either refusal fails here.
	for (const auto &name : {"SSLKEYLOGFILE", "AWS_ENDPOINT_URL"}) {
		CHECK(std::find(names.begin(), names.end(), name) != names.end());
	}
	for (const auto &row : rows) {
		INFO(row.name << " (" << row.phase << ")");
		const auto cwd = directory.Directory("case-" + row.name);
		const auto canary_path = cwd + "/canary";
		const auto value = row.name.rfind("AWS_ENDPOINT_URL", 0) == 0 ? "https://127.0.0.1:9" : canary_path;
		auto environment = ChildEnvironment("trapped-connect", home, driver);
		environment.push_back(row.name + "=" + value);
		const auto run = RunSuiteChild(CHILD_TAG, environment, cwd, CHILD_TIMEOUT);

		INFO(run.output);
		CHECK_FALSE(run.Trapped());
		CHECK(run.exit_code == 0);
		CHECK_THAT(run.output, Catch::Contains("thrown: " + RefusalOf(row)));
		CHECK_THAT(run.output, Catch::Contains(row.phase == "load" ? "mapped: 0" : "mapped: 1"));
		// Nothing was written where the variable points, as a key log would be.
		CHECK(access(canary_path.c_str(), F_OK) != 0);
	}
}

TEST_CASE("Without a refused variable the connect opens a socket, which the trap stops", "[odbc]") {
	if (DriverCaseSkippedFor(redshift_test::DriverSkipReason())) {
		return;
	}
	TemporaryDirectory directory;
	const auto run = RunSuiteChild(
	    CHILD_TAG, ChildEnvironment("trapped-connect", directory.Directory("home"), directory.Directory("driver")),
	    directory.Directory("cwd"), CHILD_TIMEOUT);

	CHECK(run.Trapped());
	CHECK(run.output.find("thrown: ") == std::string::npos);
}

TEST_CASE("Logging turned on through each INI file the driver reads writes no log and shows no canary", "[odbc]") {
	if (DriverCaseSkippedFor(IniCaseSkipReason(redshift_test::DriverSkipReason(), duckdb::BuildHasTestEndpoints()))) {
		return;
	}
	TemporaryDirectory directory;
	const auto home = directory.Directory("home");
	const auto locations = IniLocations();
	CHECK(locations.size() == 2);
	for (const auto &location : locations) {
		INFO(location);
		const auto name = location.substr(location.rfind('/') + 1);
		// A directory of its own each, so one location's file cannot turn logging on for another's.
		const auto driver = directory.Directory("driver-" + name);
		const auto cwd = directory.Directory("case-" + name);
		const auto logs = directory.Directory("logs-" + name);
		// The driver's default trace directory (known-environment.txt), in case it ignores LogPath.
		const auto trace = directory.Directory("tmp-" + name);
		auto environment = ChildEnvironment("connect", home, driver);
		environment.push_back("TMPDIR=" + trace);
		std::string planted;
		if (location == "./odbc.ini") {
			planted = cwd + "/odbc.ini";
		} else {
			// The driver's own directory is the set's: the first connect writes the set, which is then
			// given the file, as a user could, before the connect under test.
			REQUIRE(location == "<driver-dir>/amazon.redshiftodbc.ini");
			CHECK(RunSuiteChild(CHILD_TAG, ChildEnvironment("connect", home, driver), cwd, CHILD_TIMEOUT).exit_code ==
			      0);
			planted = DriverSet(driver) + "/" + name;
		}
		Plant(planted, LoggingOn(location, logs));
		const auto run = RunSuiteChild(CHILD_TAG, environment, cwd, CHILD_TIMEOUT);

		INFO(run.output);
		CHECK(run.exit_code == 0);
		// The driver loaded and failed the connect itself, at the closed ports, from the case's directory.
		CHECK_THAT(run.output, Catch::Contains("thrown: Could not connect to Redshift at 127.0.0.1"));
		CHECK_THAT(run.output, Catch::Contains("cwd: " + cwd + "\n"));
		CHECK(redshift_test::NamesIn(logs).empty());
		CHECK(redshift_test::NamesIn(trace).empty());
		for (const auto &left : redshift_test::NamesIn(cwd)) {
			CHECK((left == "child.out" || left == "odbc.ini"));
		}
		if (location != "./odbc.ini") {
			// The loader admits only its own files to a set, so it put the planted one aside with the set.
			CHECK(access(planted.c_str(), F_OK) != 0);
		}
		const auto scan = redshift_test::ScanOutput(cwd + "/child.out");
		CHECK(scan.report.empty());
		CHECK(scan.exit_code == 0);
	}
}
