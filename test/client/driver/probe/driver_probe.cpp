// redshift_driver_probe: loads the embedded driver the way the extension does and reports, one line
// per check, what happened, the closed-port smoke last. Lines are `<check> <ok|fail|skip> <detail>`,
// tab-separated, so the reports of several hosts can be compared. Built with the driver suite (make
// test_driver); third_party/redshift_odbc/tools/host-probe.sh runs it.
//
//   redshift_driver_probe
//
// It takes no arguments and connects to nothing but a local port nothing listens on: the environment
// allocation (AllocateEnvironmentHandle) and that smoke are its only ODBC calls, as the
// boundary between the client and the loader allows. A live query is the extension's own LOAD and query, run
// separately.
//
// Exits 0 when no check failed, 1 when one did, 2 on a usage error.
#include "closed_port_smoke.hpp"
#include "driver_info.h"
#include "driver_loader_core.hpp"
#include "error_kind_name.hpp"

#include <cstdio>
#include <cstring>
#include <string>

#include <unistd.h>

using namespace duckdb::redshift_driver;

namespace {

int failures = 0;

void Report(const char *check, const char *outcome, const std::string &detail) {
	std::string clean = detail;
	for (auto &c : clean) {
		if (c == '\t' || c == '\n' || c == '\r') {
			c = ' ';
		}
	}
	std::printf("%s\t%s\t%s\n", check, outcome, clean.c_str());
	failures += std::strcmp(outcome, "fail") == 0 ? 1 : 0;
}

// The exit status, from the lines printed: both ends of main() take it from here, so the refused-base
// case (which requires exit 1 and a `load fail` line) covers the counter as well as the status.
int Status() {
	return failures == 0 ? 0 : 1;
}

void ReportClosedPortSmoke(const LoadedDriver &driver) {
	const int port = redshift_driver_test::ClosedLocalPort();
	std::string answer = "no local port";
	const bool own = port > 0 && redshift_driver_test::ClosedPortSmoke(driver, port, answer);
	Report("closed_port_smoke", own ? "ok" : "fail", answer);
}

} // namespace

int main(int argc, char **argv) {
	// Line-buffered, so a crash in the driver leaves every line printed so far.
	std::setvbuf(stdout, nullptr, _IOLBF, 0);
	if (argc != 1) {
		// The probe makes no live connect (those belong to the ODBC client's tests), so the usage line
		// says where a live query runs.
		std::fprintf(stderr,
		             "usage: %s (no arguments; a row's live query is the extension's own LOAD and query, run beside "
		             "this probe)\n",
		             argv[0]);
		return 2;
	}
	Report("platform", "ok", REDSHIFT_DRIVER_PLATFORM);
	// The base directories the loader considers, in order, and why any is skipped or refused. The per-user
	// cache is resolved, and so made, with the loader's own ResolveCache, as the loader does when it reaches
	// it, so its line has the directory and the outcome the loader has.
	const auto inputs = detail::CurrentBaseInputs();
	for (auto &base : detail::BaseCandidates(inputs)) {
		const std::string role = detail::BaseRole(base.kind);
		if (base.pending) {
			const auto cache = detail::ResolveCache(inputs, ::geteuid());
			if (cache.skip) {
				Report("base", "skip", role + ": " + cache.attempt.directory + ": it is the extension's directory");
				continue;
			}
			base.attempt = cache.attempt;
		}
		Report("base", base.attempt.reason.empty() ? "ok" : "skip",
		       role + ": " + base.attempt.directory + (base.attempt.reason.empty() ? "" : ": " + base.attempt.reason));
	}
	auto result = LoadEmbeddedDriver();
	if (!result.ok()) {
		// The attempts name each directory, which the message never does.
		for (const auto &attempt : result.error.attempts) {
			Report("attempt", "fail", attempt.directory + ": " + attempt.reason);
		}
		Report("load", "fail",
		       std::string(redshift_driver_test::ErrorKindName(result.error.kind)) + " " + result.error.message);
		return Status();
	}
	const auto &driver = *result.driver;
	Report("load", "ok", driver.identity.Describe());
	Report("directory", "ok", driver.identity.directory);
	ReportClosedPortSmoke(driver);
	return Status();
}
