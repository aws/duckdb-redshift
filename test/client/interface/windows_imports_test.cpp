// Windows with MSVC only. The loadable extension reaches the driver only through the function
// table, so it imports no ODBC driver manager, which would bring in the system's DSN and registry
// configuration. dumpbin is the one CMake found at configure time beside the compiler that built
// the extension; without it the case is skipped, or fails with RS_REQUIRE_DUMPBIN=1.

#include "scoped_environment_variable.hpp"

#include "catch.hpp"

#include <algorithm>
#include <cctype>
#include <cstdio>
#include <cstdlib>
#include <string>

using namespace duckdb::redshift_driver::testing;

namespace {

//! Runs `command` through cmd.exe and returns its exit status, with its output, stderr included, in
//! `output`. The whole line is quoted once more, so cmd.exe keeps the quotes around each path.
int Run(const std::string &command, std::string &output) {
	output.clear();
	FILE *pipe = _popen(("\"" + command + " 2>&1\"").c_str(), "r");
	if (pipe == nullptr) {
		return -1;
	}
	char buffer[4096];
	while (fgets(buffer, sizeof(buffer), pipe) != nullptr) {
		output += buffer;
	}
	return _pclose(pipe);
}

//! The DLL whose import table the case reads: RS_IMPORTS_DLL when it is set, so a build that embeds
//! the driver can check its own loadable extension, else the loadable extension this build made.
std::string ImportsDll() {
	const char *dll = std::getenv("RS_IMPORTS_DLL");
	if (dll != nullptr && *dll != '\0') {
		return dll;
	}
	return REDSHIFT_LOADABLE_EXTENSION;
}

//! Whether `path` names a file this process can open for reading.
bool Readable(const std::string &path) {
	FILE *file = std::fopen(path.c_str(), "rb");
	if (file == nullptr) {
		return false;
	}
	std::fclose(file);
	return true;
}

} // namespace

TEST_CASE("The loadable extension imports no ODBC driver manager", "[interface]") {
	// Checked before dumpbin, so a DLL path that names no file fails rather than skips.
	const auto dll = ImportsDll();
	INFO("DLL: " << dll);
	REQUIRE(Readable(dll));
	const std::string dumpbin = REDSHIFT_DUMPBIN;
	if (dumpbin.empty()) {
		const char *required = std::getenv("RS_REQUIRE_DUMPBIN");
		if (required != nullptr && std::string(required) == "1") {
			FAIL("RS_REQUIRE_DUMPBIN=1, but CMake found no dumpbin beside the MSVC compiler");
		}
		SKIP_TEST("skipped: CMake found no dumpbin beside the MSVC compiler");
		return;
	}
	std::string output;
	const int status = Run("\"" + dumpbin + "\" /nologo /imports \"" + dll + "\"", output);
	INFO(output);
	REQUIRE(status == 0);
	// Each imported DLL is named in the case the linker recorded, so it is compared in lower case.
	std::transform(output.begin(), output.end(), output.begin(),
	               [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
	// The control: the import table was read, and it names kernel32.dll, which the C runtime the
	// extension links statically imports.
	REQUIRE(output.find("kernel32.dll") != std::string::npos);
	CHECK(output.find("odbc32.dll") == std::string::npos);
	CHECK(output.find("odbccp32.dll") == std::string::npos);
}

TEST_CASE("RS_IMPORTS_DLL picks the DLL, and a path naming no file fails the case", "[interface]") {
	{
		ScopedEnvironmentVariable unset("RS_IMPORTS_DLL", nullptr);
		CHECK(ImportsDll() == REDSHIFT_LOADABLE_EXTENSION);
	}
	const std::string missing = std::string(REDSHIFT_LOADABLE_EXTENSION) + ".no-such-file";
	ScopedEnvironmentVariable variable("RS_IMPORTS_DLL", missing.c_str());
	CHECK(ImportsDll() == missing);
	// The first case requires this before dumpbin, so it fails there rather than skips.
	CHECK_FALSE(Readable(ImportsDll()));
}
