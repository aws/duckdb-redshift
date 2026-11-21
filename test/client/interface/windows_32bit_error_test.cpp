// Linux and macOS. The header refuses 32-bit Windows with an #error that comes before any include,
// so this platform's compiler can show it with no Windows build and before its standard library is
// read as Windows': told _WIN32 alone it stops at the #error, and told _WIN64 too it does not.

#include "catch.hpp"

#include <cstdio>
#include <string>
#include <sys/wait.h>

namespace {

const char *const MESSAGE = "32-bit Windows is not a supported platform; build windows_amd64";

//! Checks the interface header's syntax with `defines`, as the build's compiler reads it; returns
//! the compiler's exit status, with its output, stderr included, in `output`.
int CheckHeader(const std::string &defines, std::string &output) {
	const std::string command = std::string("'" REDSHIFT_CXX_COMPILER "' -fsyntax-only ") + defines +
	                            " -I'" REDSHIFT_UNIXODBC_INCLUDE "' -x c++ '" REDSHIFT_INTERFACE_HEADER "' 2>&1";
	output.clear();
	FILE *pipe = popen(command.c_str(), "r");
	REQUIRE(pipe != nullptr);
	char buffer[4096];
	while (fgets(buffer, sizeof(buffer), pipe) != nullptr) {
		output += buffer;
	}
	const int status = pclose(pipe);
	return WIFEXITED(status) ? WEXITSTATUS(status) : -1;
}

} // namespace

TEST_CASE("The interface header stops a 32-bit Windows build at its #error", "[interface]") {
	std::string output;
	const int status = CheckHeader("-D_WIN32 -U_WIN64", output);
	INFO(output);
	CHECK(status != 0);
	REQUIRE(output.find(MESSAGE) != std::string::npos);
	// The #error comes first, before the compiler reads any header.
	CHECK(output.find(MESSAGE) < output.find("windows.h"));
}

TEST_CASE("The interface header gets past its #error for 64-bit Windows", "[interface]") {
	// The control: the same command with _WIN64 fails here too, on a Windows header this platform
	// lacks, but not at the #error. Which header fails first depends on the standard library.
	std::string output;
	const int status = CheckHeader("-D_WIN32 -D_WIN64", output);
	INFO(output);
	CHECK(status != 0);
	CHECK(output.find(MESSAGE) == std::string::npos);
}
