// The variables the driver reads that the extension leaves set, each set to a hostile value in a new
// process before it connects to each live target whose variables are set, the workgroup and the
// provisioned cluster: the connection still reaches the same target and returns the same values, text
// and UTF-8 alike, and no hostile value shows.
// The `claim:` rows' claims are checked too: a file they name is never opened, the trace directory
// stays empty, and no program starts. Linux only (child_process.hpp); live, so the cases skip
// without the live variables or the embedded driver. The controls show the open watch and the
// process trap can fail.

// catch.hpp comes first, as in this directory's other tests.
#include "catch.hpp"

#include "canary_output.hpp"
#include "child_process.hpp"
#include "driver_policy.hpp"
#include "duckdb/main/database.hpp"
#include "extension_database.hpp"
#include "fake_driver_fixture.hpp"
#include "live_target.hpp"
#include "live_workgroup.hpp"
#include "odbc/connection_settings.hpp"
#include "redshift_extension.hpp"
#include "trust_store_test_support.hpp"

#include <algorithm>
#include <cstdlib>
#include <iostream>
#include <map>
#include <set>
#include <spawn.h>
#include <sstream>
#include <string>
#include <sys/stat.h>
#include <thread>
#include <vector>

using redshift_test::CHILD_MODE;
using redshift_test::ChildRun;
using redshift_test::RunSuiteChild;
using redshift_test::SkippedFor;
using redshift_trust_test::TemporaryDirectory;

namespace {

//! The hidden case a child runs. Its CHILD_MODE: `spawn` starts a program under the process trap, `fork`
//! makes the fork system call under it, `thread` starts a thread under it, `probe` runs PROBE_SQL on the
//! live target and reports what it saw, and `trapped-probe` does so under the process trap.
const char *const CHILD_TAG = "[.known-environment-child]";
//! A connect to a target that is awake, a statement and a teardown, on a loaded host.
const auto CHILD_TIMEOUT = std::chrono::seconds(240);
//! What the target is and how its values come back as text: the database, user and application name
//! the session has, a float, a decimal, a date, a timestamp and multibyte and 4-byte UTF-8.
const char *const PROBE_SQL = "SELECT current_database() AS db, current_user AS usr, "
                              "current_setting('application_name') AS app, 1.5::float8 AS f, "
                              "12345.678::numeric(10,3) AS n, DATE '2024-02-29' AS d, "
                              "TIMESTAMP '2024-02-29 13:45:01.25' AS ts, 'é€😀'::varchar AS t";
//! What the parent passes on to a live child: the live variables, the test endpoints, and what the
//! default chain finds the parent's credentials through. Never on a command line, and never scanned
//! for, which would put them on one.
const std::vector<std::string> PASSED_ON {"REDSHIFT_TEST_LIVE_REGION",
                                          "REDSHIFT_TEST_LIVE_WORKGROUP",
                                          "REDSHIFT_TEST_LIVE_ACCOUNT_ID",
                                          redshift_test::LIVE_CLUSTER_VARIABLE,
                                          duckdb::TEST_HOST_VARIABLE,
                                          duckdb::TEST_URL_VARIABLE,
                                          "AWS_ACCESS_KEY_ID",
                                          "AWS_SECRET_ACCESS_KEY",
                                          "AWS_SESSION_TOKEN",
                                          "AWS_REGION",
                                          "AWS_PROFILE",
                                          "AWS_CONTAINER_CREDENTIALS_FULL_URI",
                                          "AWS_CONTAINER_CREDENTIALS_RELATIVE_URI",
                                          "AWS_CONTAINER_AUTHORIZATION_TOKEN",
                                          "AWS_CONTAINER_AUTHORIZATION_TOKEN_FILE",
                                          "AWS_WEB_IDENTITY_TOKEN_FILE",
                                          "AWS_ROLE_ARN"};

//! The hostile values each keyword row of policy/known-environment.txt is set to, one child each.
const std::map<std::string, std::vector<std::string>> KEYWORD_HOSTILE {{"PGDATABASE", {"canary-database"}},
                                                                       {"PGSSLMODE", {"disable"}},
                                                                       {"PGAPPNAME", {"canary-app"}},
                                                                       {"PGCONNECT_TIMEOUT", {"1"}},
                                                                       {"PGCOMPRESSION", {"off", "canary-algorithm"}},
                                                                       {"PGUSER", {"canary-user"}},
                                                                       {"PGPASSWORD", {"canary-password"}}};

//! The hostile values that are canaries, which no output may show.
const std::vector<std::string> HOSTILE_CANARIES {"canary-database", "canary-app",  "canary-user",  "canary-password",
                                                 "canary-passfile", "canary-peer", "canary-netrc", "canary-algorithm"};

//! A live target the probes reach: what messages call it, and the live variables that name it. A child
//! attaches to the cluster when it has REDSHIFT_TEST_LIVE_CLUSTER, so only the cluster's children get it.
struct LiveTarget {
	std::string noun;
	std::vector<std::string> variables;
};

//! The live targets whose variables are all set, the workgroup first.
std::vector<LiveTarget> LiveTargets() {
	std::vector<LiveTarget> targets;
	for (const auto &target : {LiveTarget {"workgroup", redshift_test::LIVE_VARIABLES},
	                           LiveTarget {"cluster", redshift_test::LIVE_CLUSTER_VARIABLES}}) {
		if (redshift_test::UnsetVariables(target.variables).empty()) {
			targets.push_back(target);
		}
	}
	return targets;
}

//! Why the live cases cannot run here, or empty when they can: no target has all its variables set
//! (the workgroup's reason is given), or DriverSkipReason.
std::string LiveTargetsSkipReason() {
	return LiveTargets().empty() ? redshift_test::LiveSkipReason() : redshift_test::DriverSkipReason();
}

//! A row of policy/known-environment.txt: a name, and the keyword that overrides it or `-`, and for
//! a `-` row whether its reason is a claim about what the driver does.
struct KnownRow {
	std::string name;
	std::string keyword;
	bool claim;
};

std::vector<KnownRow> KnownRows() {
	std::vector<KnownRow> rows;
	for (const auto &row : redshift_test::PolicyRows("known-environment.txt")) {
		REQUIRE(row.size() >= 2);
		rows.push_back({row[0], row[1], row.size() > 2 && row[2] == "claim:"});
	}
	return rows;
}

//! The NAME=value entries of PASSED_ON that are set here, the test endpoints only where the build
//! takes them, as any other build refuses them, and only to the workgroup's children, and the
//! cluster's variable only to the cluster's; and the parent's AWS files, as a child has its own home.
std::vector<std::string> PassedOn(const LiveTarget &target) {
	const char *home = std::getenv("HOME");
	const std::string aws = std::string(home ? home : "") + "/.aws/";
	const char *config = std::getenv("AWS_CONFIG_FILE");
	const char *credentials = std::getenv("AWS_SHARED_CREDENTIALS_FILE");
	std::vector<std::string> environment {"AWS_CONFIG_FILE=" + (config ? std::string(config) : aws + "config"),
	                                      "AWS_SHARED_CREDENTIALS_FILE=" +
	                                          (credentials ? std::string(credentials) : aws + "credentials")};
	for (const auto &name : PASSED_ON) {
		const bool endpoint = name == duckdb::TEST_HOST_VARIABLE || name == duckdb::TEST_URL_VARIABLE;
		// The cluster's children get no test endpoint, which would carry their control-plane calls to the
		// workgroup's test URL, and only they get the cluster's variable.
		const bool cluster = target.noun == "cluster";
		if (cluster ? endpoint : name == redshift_test::LIVE_CLUSTER_VARIABLE) {
			continue;
		}
		if (const char *value = std::getenv(name.c_str())) {
			if (!endpoint || duckdb::BuildHasTestEndpoints()) {
				environment.push_back(name + "=" + value);
			}
		}
	}
	return environment;
}

//! `sql` as a SQL string literal's body.
std::string Quoted(const std::string &sql) {
	std::string quoted;
	for (const char c : sql) {
		quoted += c == '\'' ? std::string("''") : std::string(1, c);
	}
	return quoted;
}

//! PROBE_SQL through redshift_query on the live target, the cluster when REDSHIFT_TEST_LIVE_CLUSTER is
//! set, at debug logging, then its result and every log line, on standard output for the parent to read.
void ProbeAndReport() {
	duckdb::SetDriverSourceForTesting(&duckdb::redshift_driver::LoadEmbeddedDriver);
	{
		redshift_test::ExtensionDatabase database;
		database.Run("CALL enable_logging(level = 'debug')");
		const char *region = std::getenv("REDSHIFT_TEST_LIVE_REGION");
		const char *cluster = std::getenv(redshift_test::LIVE_CLUSTER_VARIABLE);
		database.Run(cluster ? redshift_test::AttachRsCluster(region, cluster)
		                     : redshift_test::AttachRs(region, std::getenv("REDSHIFT_TEST_LIVE_WORKGROUP"),
		                                               std::getenv("REDSHIFT_TEST_LIVE_ACCOUNT_ID")));
		auto result = database.con.Query("SELECT * FROM redshift_query('rs', '" + Quoted(PROBE_SQL) + "')");
		std::cout << "probe-begin\n"
		          << (result->HasError() ? "error " + result->GetError() : result->ToString()) << "\nprobe-end\n";
		for (const auto &line : redshift_test::FirstColumn(*database.Run("SELECT message FROM duckdb_logs"))) {
			std::cout << "log: " << line << "\n";
		}
	}
	duckdb::ResetProcessStateForTesting();
}

//! The filters added between the counts `before` and `after`, or "unknown" when the kernel gave none.
std::string FiltersAdded(int before, int after) {
	return before < 0 ? "unknown" : std::to_string(after - before);
}

//! What a probe child reported between its markers, or empty when it reported nothing.
std::string ProbeOf(const ChildRun &run) {
	const auto begin = run.output.find("probe-begin\n");
	const auto end = run.output.find("\nprobe-end\n");
	return begin == std::string::npos || end == std::string::npos ? std::string()
	                                                              : run.output.substr(begin, end - begin);
}

//! A probe child of `target` in a new directory under `directory` named for `label`, with `setting`
//! (NAME=value) when not empty, under the process trap when `trap_processes`; checked to have exited
//! normally and shown no canary.
ChildRun LiveProbe(const LiveTarget &target, TemporaryDirectory &directory, const std::string &label,
                   const std::string &setting, bool trap_processes) {
	INFO(target.noun + " " + label);
	auto environment = PassedOn(target);
	environment.push_back(std::string(CHILD_MODE) + (trap_processes ? "=trapped-probe" : "=probe"));
	environment.push_back("PATH=/usr/bin:/bin");
	environment.push_back("HOME=" + directory.Directory("home-" + label));
	if (!setting.empty()) {
		environment.push_back(setting);
	}
	const auto cwd = directory.Directory("case-" + label);
	const auto run = RunSuiteChild(CHILD_TAG, environment, cwd, CHILD_TIMEOUT);
	// Scanned first: the child ran on the parent's real credentials, which no failure may print.
	const auto scan = redshift_test::ScanOutput(cwd + "/child.out", HOSTILE_CANARIES);
	REQUIRE(scan.report.empty());
	CHECK(scan.exit_code == 0);
	INFO(run.output);
	CHECK(run.exit_code == 0);
	return run;
}

//! `content` in a new file `name` under `directory`, readable by its owner alone, as the driver's
//! libpq wants a password file before it reads one.
std::string PrivateFile(TemporaryDirectory &directory, const std::string &name, const std::string &content) {
	const auto path = directory.File(name, content);
	REQUIRE(chmod(path.c_str(), 0600) == 0);
	return path;
}

//! The probe a child without a hostile variable reports, checked to have connected and read the
//! values back, so every comparison with it compares results rather than errors. The session's
//! application name is the whole one the connection sent, with DuckDB's version and platform.
std::string ConnectedBaseline(const LiveTarget &target, TemporaryDirectory &directory) {
	const auto baseline = ProbeOf(LiveProbe(target, directory, "baseline", "", false));
	const auto sent = duckdb::ApplicationName(duckdb::RedshiftExtension().Version(), duckdb::DuckDB::LibraryVersion(),
	                                          duckdb::DuckDB::Platform());
	REQUIRE_THAT(sent, Catch::Contains(std::string(" duckdb/") + duckdb::DuckDB::LibraryVersion() + " " +
	                                   duckdb::DuckDB::Platform()));
	REQUIRE_THAT(baseline, Catch::Contains("\ndev\t") && Catch::Contains("\t" + sent + "\t") &&
	                           Catch::Contains("é€😀") && !Catch::Contains("error "));
	return baseline;
}

} // namespace

TEST_CASE("A child the known-environment cases run", "[.known-environment-child]") {
	const std::string mode = std::getenv(CHILD_MODE) ? std::getenv(CHILD_MODE) : "";
	if (mode == "spawn" || mode == "fork" || mode == "thread") {
		redshift_test::TrapNewProcesses();
	}
	if (mode == "spawn") {
		pid_t started = 0;
		char *const arguments[] = {const_cast<char *>("true"), nullptr};
		char *const no_environment[] = {nullptr};
		const int error = posix_spawn(&started, "/bin/true", nullptr, nullptr, arguments, no_environment);
		std::cout << "posix_spawn returned " << error << "\n";
	} else if (mode == "fork") {
#if defined(SYS_fork)
		// The system call itself, which the C library's fork no longer makes.
		std::cout << "fork returned " << syscall(SYS_fork) << "\n";
#endif
	} else if (mode == "thread") {
		std::thread([] { std::cout << "thread ran\n"; }).join();
	} else {
		REQUIRE((mode == "probe" || mode == "trapped-probe"));
		// Counted before the trap and after, as a container's runtime can install a filter of its own.
		const auto before = redshift_test::SeccompFilters();
		if (mode == "trapped-probe") {
			redshift_test::TrapNewProcesses();
		}
		ProbeAndReport();
		std::cout << "filters added: " << FiltersAdded(before, redshift_test::SeccompFilters()) << "\n";
	}
}

TEST_CASE("A child that starts a program under the process trap is killed before it runs", "[odbc]") {
	TemporaryDirectory directory;
	const auto run =
	    RunSuiteChild(CHILD_TAG, {std::string(CHILD_MODE) + "=spawn"}, directory.Directory("cwd"), CHILD_TIMEOUT);

	CHECK(run.Trapped());
	CHECK(run.output.find("posix_spawn returned") == std::string::npos);
}

TEST_CASE("A child that makes the fork system call under the process trap is killed before it returns", "[odbc]") {
#if defined(SYS_fork)
	TemporaryDirectory directory;
	const auto run =
	    RunSuiteChild(CHILD_TAG, {std::string(CHILD_MODE) + "=fork"}, directory.Directory("cwd"), CHILD_TIMEOUT);

	CHECK(run.Trapped());
	CHECK(run.output.find("fork returned") == std::string::npos);
#else
	SKIP_TEST("skipped: this architecture has no fork system call, only clone");
#endif
}

TEST_CASE("A child under the process trap still starts a thread", "[odbc]") {
	TemporaryDirectory directory;
	const auto run =
	    RunSuiteChild(CHILD_TAG, {std::string(CHILD_MODE) + "=thread"}, directory.Directory("cwd"), CHILD_TIMEOUT);

	CHECK(run.exit_code == 0);
	CHECK_THAT(run.output, Catch::Contains("thread ran"));
}

TEST_CASE("The open watch sees a file another process opens, and nothing for one no process opens", "[odbc]") {
	TemporaryDirectory directory;
	const auto opened = directory.File("opened", "x");
	const auto closed = directory.File("closed", "x");
	redshift_test::LetNewFilesSettle();
	redshift_test::FileOpenWatch opened_watch(opened);
	redshift_test::FileOpenWatch closed_watch(closed);
	CHECK(system(("cat '" + opened + "' > /dev/null").c_str()) == 0);

	CHECK(opened_watch.Opened());
	CHECK_FALSE(closed_watch.Opened());
}

TEST_CASE("The filter count is read from the status line, and unknown on a kernel that writes none", "[odbc]") {
	std::istringstream counted("Name:\tunittest\nSeccomp:\t2\nSeccomp_filters:\t3\nSpeculation_Store_Bypass:\tx\n");
	std::istringstream uncounted("Name:\tunittest\nSeccomp:\t2\nSpeculation_Store_Bypass:\tx\n");

	CHECK(redshift_test::SeccompFiltersIn(counted) == 3);
	CHECK(redshift_test::SeccompFiltersIn(uncounted) == -1);
	CHECK(FiltersAdded(1, 2) == "1");
	CHECK(FiltersAdded(0, 1) == "1");
	CHECK(FiltersAdded(-1, -1) == "unknown");
}

TEST_CASE("Live: a hostile variable the connection string overrides changes neither target nor values",
          "[odbc][live]") {
	if (SkippedFor(LiveTargetsSkipReason())) {
		return;
	}
	// The policy's keyword rows are the loaded driver's overridden list, which the client checks.
	std::vector<std::string> listed;
	for (const auto &row : KnownRows()) {
		if (row.keyword != "-") {
			listed.push_back(row.name);
		}
	}
	const auto load = duckdb::redshift_driver::LoadEmbeddedDriver();
	REQUIRE(load.ok());
	std::vector<std::string> loaded;
	for (const auto &variable : load.driver->overridden_environment) {
		loaded.push_back(variable.name);
	}
	std::sort(listed.begin(), listed.end());
	std::sort(loaded.begin(), loaded.end());
	CHECK(listed == loaded);

	for (const auto &target : LiveTargets()) {
		TemporaryDirectory directory;
		const auto baseline = ConnectedBaseline(target, directory);
		WARN("the probe's result on the live " << target.noun << ":\n" << baseline);
		for (const auto &name : listed) {
			// SSL_CERT_FILE is the client's own trust source, which the trust cases cover.
			if (name == "SSL_CERT_FILE") {
				continue;
			}
			INFO(name);
			const auto hostile = KEYWORD_HOSTILE.find(name);
			REQUIRE(hostile != KEYWORD_HOSTILE.end());
			for (size_t index = 0; index < hostile->second.size(); index++) {
				const auto label = name + "-" + std::to_string(index);
				CHECK(ProbeOf(LiveProbe(target, directory, label, name + "=" + hostile->second[index], false)) ==
				      baseline);
			}
		}
	}
}

TEST_CASE("Live: a hostile claim row's variable changes neither target nor values, and its claim holds",
          "[odbc][live]") {
	if (SkippedFor(LiveTargetsSkipReason())) {
		return;
	}
	for (const auto &target : LiveTargets()) {
		TemporaryDirectory directory;
		const auto baseline = ConnectedBaseline(target, directory);
		// libpq ignores a password file group or others can read, or whose fields do not match, so this one
		// matches every connection and only its owner can read it.
		const auto passfile = PrivateFile(directory, "passfile", "*:*:*:*:canary-passfile\n");
		const auto netrc = PrivateFile(directory, "netrc", "default login canary password canary-netrc\n");
		const auto trace_directory = directory.Directory("tmpdir");
		// A file beside them that no process of the case opens, so an open the host makes shows as such.
		const auto untouched = PrivateFile(directory, "untouched", "x\n");
		redshift_test::LetNewFilesSettle();
		// The claim rows the loop below has a check for, so a row added to or dropped from the policy fails here.
		const std::set<std::string> checked {"PGHOST", "PGPORT", "PGPASSFILE", "PGREQUIREPEER",
		                                     "NETRC",  "TMPDIR", "DISPLAY"};
		std::vector<std::string> claims;
		for (const auto &row : KnownRows()) {
			if (row.claim) {
				claims.push_back(row.name);
			}
		}
		CHECK(std::set<std::string>(claims.begin(), claims.end()) == checked);
		for (const auto &name : claims) {
			INFO(name);
			if (name == "PGPASSFILE" || name == "NETRC") {
				const auto &path = name == "PGPASSFILE" ? passfile : netrc;
				redshift_test::FileOpenWatch watch(path);
				redshift_test::FileOpenWatch control(untouched);
				CHECK(ProbeOf(LiveProbe(target, directory, name, name + "=" + path, false)) == baseline);
				CHECK_FALSE(control.Opened());
				CHECK_FALSE(watch.Opened());
			} else if (name == "TMPDIR") {
				CHECK(ProbeOf(LiveProbe(target, directory, name, name + "=" + trace_directory, false)) == baseline);
				CHECK(redshift_test::NamesIn(trace_directory).empty());
			} else if (name == "DISPLAY") {
				// No display serves :97; the browser sign-in plugin, its only reader, would start a browser.
				const auto run = LiveProbe(target, directory, name, "DISPLAY=:97", true);
				CHECK(ProbeOf(run) == baseline);
				// Where the kernel counts filters, the count shows the trap was in place; the offline spawn and fork
				// cases show it works. The parent runs on the same kernel outside the trap, so the 5.9+ check relies
				// on SeccompFilters() reading /proc/self/status.
				if (redshift_test::SeccompFilters() < 0) {
					WARN("this kernel writes no Seccomp_filters line (only Linux 5.9 and later do), so the trap's "
					     "filter count is not checked");
				} else {
					CHECK_THAT(run.output, Catch::Contains("filters added: 1\n"));
				}
			} else {
				const std::map<std::string, std::string> hostile {
				    {"PGHOST", "127.0.0.1"}, {"PGPORT", "1"}, {"PGREQUIREPEER", "canary-peer"}};
				REQUIRE(hostile.count(name) == 1);
				CHECK(ProbeOf(LiveProbe(target, directory, name, name + "=" + hostile.at(name), false)) == baseline);
			}
		}
	}
}
