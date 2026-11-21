//===----------------------------------------------------------------------===//
//                         DuckDB - Redshift
//
// test/client/odbc/canary_output.hpp
//
// The canary credentials and parameter a run passes where the client must never show them, the
// standard streams captured around a call, and the scan that reads captured output for the canaries
// and for the forms (scan-output.py). POSIX only.
//===----------------------------------------------------------------------===//

#pragma once

#include "catch.hpp"
#include "redshift_transport.hpp"

#include <cstdio>
#include <fcntl.h>
#include <iostream>
#include <string>
#include <sys/wait.h>
#include <unistd.h>
#include <vector>

#ifndef REDSHIFT_SOURCE_DIR
#error "REDSHIFT_SOURCE_DIR must name the checkout; test/client/odbc/sources.cmake sets it"
#endif
#ifndef REDSHIFT_PYTHON
#error "REDSHIFT_PYTHON must name the interpreter; test/client/odbc/sources.cmake sets it"
#endif

namespace redshift_test {

//! In AWS's access-key form, so the scan's access-key rule finds it as well as the canary rule.
const char *const CANARY_KEY_ID = "AKIACANARYKEYID00001";
//! 40 token characters, a secret key's length, which connect-phase redaction replaces as a run.
const char *const CANARY_SECRET = "canarySecretAccessKey0000000000000000001";
const char *const CANARY_TOKEN = "canarySessionToken/0123456789abcdefghij0123456789abcdefghij";
//! 8 bytes or more, so statement-phase redaction replaces it wherever a record echoes it.
const char *const CANARY_PARAMETER = "canary-parameter-1";

//! The canary credentials, as an AWS secret hands them to the client.
inline duckdb::RedshiftAwsCredentials CanaryCredentials() {
	return {CANARY_KEY_ID, CANARY_SECRET, CANARY_TOKEN};
}

//! Every canary, in the order scan-output.py numbers them.
inline std::vector<std::string> Canaries() {
	return {CANARY_KEY_ID, CANARY_SECRET, CANARY_TOKEN, CANARY_PARAMETER};
}

//! What this process writes to its standard output and error while this lives goes to `path`
//! instead, so a case reads what a user's terminal would show.
class CapturedStandardStreams {
public:
	explicit CapturedStandardStreams(const std::string &path) {
		Flush();
		saved_output = dup(STDOUT_FILENO);
		saved_error = dup(STDERR_FILENO);
		const int file = open(path.c_str(), O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC, 0600);
		REQUIRE((saved_output >= 0 && saved_error >= 0 && file >= 0));
		dup2(file, STDOUT_FILENO);
		dup2(file, STDERR_FILENO);
		close(file);
	}
	~CapturedStandardStreams() {
		Flush();
		dup2(saved_output, STDOUT_FILENO);
		dup2(saved_error, STDERR_FILENO);
		close(saved_output);
		close(saved_error);
	}
	CapturedStandardStreams(const CapturedStandardStreams &) = delete;
	CapturedStandardStreams &operator=(const CapturedStandardStreams &) = delete;

private:
	static void Flush() {
		std::cout.flush();
		std::cerr.flush();
		std::fflush(nullptr);
	}

	int saved_output;
	int saved_error;
};

//! scan-output.py's verdict on a file: its exit code (0 clean, 1 a finding, 2 unreadable) and its
//! report, which names each finding's line and kind, never the text found.
struct OutputScan {
	int exit_code;
	std::string report;
};

//! scan-output.py over `paths` with Canaries() and then `more_canaries`, through the python3 the build
//! was configured with and -I, so no PYTHONPATH or user site changes what runs. A canary is put on the
//! command line, so it must never be a real credential.
inline OutputScan ScanFiles(const std::vector<std::string> &paths, const std::vector<std::string> &more_canaries) {
	INFO("REDSHIFT_PYTHON is \"" << REDSHIFT_PYTHON << "\"; CMake found no python3 when it is empty");
	REQUIRE_FALSE(std::string(REDSHIFT_PYTHON).empty());
	std::string command =
	    std::string("'") + REDSHIFT_PYTHON + "' -I '" REDSHIFT_SOURCE_DIR "/test/client/odbc/scan-output.py'";
	auto canaries = Canaries();
	canaries.insert(canaries.end(), more_canaries.begin(), more_canaries.end());
	for (const auto &canary : canaries) {
		command += " --canary '" + canary + "'";
	}
	for (const auto &path : paths) {
		command += " '" + path + "'";
	}
	command += " </dev/null 2>&1";
	OutputScan scan {-1, std::string()};
	FILE *pipe = popen(command.c_str(), "r");
	REQUIRE(pipe != nullptr);
	char buffer[512];
	while (std::fgets(buffer, sizeof(buffer), pipe) != nullptr) {
		scan.report += buffer;
	}
	const int status = pclose(pipe);
	scan.exit_code = WIFEXITED(status) ? WEXITSTATUS(status) : -1;
	return scan;
}

//! ScanFiles over the one file `path`.
inline OutputScan ScanOutput(const std::string &path, const std::vector<std::string> &more_canaries = {}) {
	return ScanFiles({path}, more_canaries);
}

} // namespace redshift_test
