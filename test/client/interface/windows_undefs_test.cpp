// The interface header removes the Win32 macros DuckDB removes, from a copy of DuckDB's list, since
// it includes no DuckDB header. Read as text on every platform, so a DuckDB move that changes the
// list fails here on Linux too, not only in the first Windows build after it.

#include "catch.hpp"

#include <algorithm>
#include <fstream>
#include <regex>
#include <string>
#include <vector>

namespace {

const std::regex UNDEF_LINE(R"(^\s*#\s*undef\s+(\w+)\s*$)");

//! The lines of `path`; fails the case when the file cannot be read.
std::vector<std::string> ReadLines(const char *path) {
	std::ifstream in(path);
	INFO("reading " << path);
	REQUIRE(in.is_open());
	std::vector<std::string> lines;
	std::string line;
	while (std::getline(in, line)) {
		if (!line.empty() && line.back() == '\r') {
			line.pop_back();
		}
		lines.push_back(line);
	}
	return lines;
}

//! The names DuckDB's windows_undefs.hpp removes, sorted.
std::vector<std::string> DuckDBNames() {
	std::vector<std::string> names;
	std::smatch match;
	for (const auto &line : ReadLines(REDSHIFT_DUCKDB_WINDOWS_UNDEFS)) {
		if (std::regex_match(line, match, UNDEF_LINE)) {
			names.push_back(match[1]);
		}
	}
	std::sort(names.begin(), names.end());
	return names;
}

//! The names in the header's Windows block, the `#if defined(_WIN32)` right after `#undef BOOL`,
//! sorted; a line in the block that is neither a comment nor an #undef fails the case.
std::vector<std::string> HeaderNames() {
	const auto lines = ReadLines(REDSHIFT_INTERFACE_HEADER);
	auto line = std::find(lines.begin(), lines.end(), "#undef BOOL");
	REQUIRE(line != lines.end());
	REQUIRE(++line != lines.end());
	REQUIRE(*line == "#if defined(_WIN32)");
	std::vector<std::string> names;
	std::smatch match;
	for (++line; line != lines.end() && *line != "#endif"; ++line) {
		if (std::regex_match(*line, match, UNDEF_LINE)) {
			names.push_back(match[1]);
		} else {
			INFO("the header's Windows block holds " << *line);
			REQUIRE(line->rfind("//", 0) == 0);
		}
	}
	REQUIRE(line != lines.end());
	std::sort(names.begin(), names.end());
	return names;
}

} // namespace

TEST_CASE("The interface header removes exactly the Win32 macros DuckDB removes", "[interface]") {
	const auto duckdb_names = DuckDBNames();
	// The control: DuckDB's list was read, and holds the macros that would break std::min and max.
	REQUIRE(std::count(duckdb_names.begin(), duckdb_names.end(), "min") == 1);
	REQUIRE(std::count(duckdb_names.begin(), duckdb_names.end(), "max") == 1);
	CHECK(HeaderNames() == duckdb_names);
}
