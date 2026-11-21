// ScopedEnvironmentVariable, the guard the interface tests set and unset variables through. Each
// case uses a name no other test reads, and leaves it unset. On Windows the cases set, unset and
// read the variable in the process's environment block, which the interface reads.

#include "scoped_environment_variable.hpp"

#include "catch.hpp"

#include <cstdlib>
#include <string>

using namespace duckdb::redshift_driver::testing;

namespace {

const char *const NAME = "REDSHIFT_INTERFACE_TEST_SCOPED_VARIABLE";
const char *const UNSET = "<unset>";

//! Sets the variable to `value`, or unsets it when `value` is nullptr: on Windows in both the block
//! and this binary's C runtime copy, as the guard writes them.
void SetVariable(const char *value) {
#if defined(_WIN32)
	const auto name = WideText(NAME);
	_wputenv_s(name.c_str(), value ? WideText(value).c_str() : L"");
	REQUIRE(SetEnvironmentVariableW(name.c_str(), value ? WideText(value).c_str() : nullptr) != 0);
#else
	REQUIRE((value ? setenv(NAME, value, 1) : unsetenv(NAME)) == 0);
#endif
}

//! The variable's value, or UNSET: on Windows as the environment block holds it, which ASCII values
//! keep as they were.
std::string ReadVariable() {
#if defined(_WIN32)
	std::wstring value;
	return ReadEnvironmentBlock(WideText(NAME), value) ? std::string(value.begin(), value.end()) : UNSET;
#else
	const char *value = std::getenv(NAME);
	return value != nullptr ? value : UNSET;
#endif
}

#if defined(_WIN32)
//! GrowingRead's state: the value it writes, the calls it has served, and the calls, counted from 1,
//! before which it makes the value one character longer.
std::wstring grown_value;
int read_calls = 0;
int first_growth = 0;
int last_growth = 0;

//! GetEnvironmentVariableW, after growing the value in the block on the calls it grows, as another
//! module that writes the variable between two of ReadEnvironmentBlock's calls.
DWORD WINAPI GrowingRead(LPCWSTR name, LPWSTR buffer, DWORD size) {
	read_calls++;
	if (read_calls >= first_growth && read_calls <= last_growth) {
		grown_value.push_back(L'x');
		SetEnvironmentVariableW(name, grown_value.c_str());
	}
	return GetEnvironmentVariableW(name, buffer, size);
}

//! Sets the variable to "ab" and reads it through GrowingRead, which grows it on calls `first` to
//! `last`.
std::wstring ReadGrowing(int first, int last) {
	SetVariable("ab");
	grown_value = L"ab";
	read_calls = 0;
	first_growth = first;
	last_growth = last;
	std::wstring value;
	REQUIRE(ReadEnvironmentBlock(WideText(NAME), value, GrowingRead));
	SetVariable(nullptr);
	return value;
}
#endif

} // namespace

TEST_CASE("ScopedEnvironmentVariable restores the value the variable had before it", "[interface]") {
	SetVariable("before");
	{
		ScopedEnvironmentVariable variable(NAME, "during");
		CHECK(ReadVariable() == "during");
	}
	CHECK(ReadVariable() == "before");
	SetVariable(nullptr);
}

TEST_CASE("ScopedEnvironmentVariable restores an empty value the variable had before it", "[interface]") {
	SetVariable("");
	{
		ScopedEnvironmentVariable variable(NAME, "during");
		CHECK(ReadVariable() == "during");
	}
	CHECK(ReadVariable() == "");
	SetVariable(nullptr);
}

TEST_CASE("ScopedEnvironmentVariable leaves a variable unset that was unset before it", "[interface]") {
	SetVariable(nullptr);
	{
		ScopedEnvironmentVariable variable(NAME, "during");
		CHECK(ReadVariable() == "during");
	}
	CHECK(ReadVariable() == UNSET);
}

TEST_CASE("ScopedEnvironmentVariable unsets the variable for its scope when given no value", "[interface]") {
	SetVariable("before");
	{
		ScopedEnvironmentVariable variable(NAME, nullptr);
		CHECK(ReadVariable() == UNSET);
	}
	CHECK(ReadVariable() == "before");
	SetVariable(nullptr);
}

TEST_CASE("ScopedEnvironmentVariable sets an empty value for its scope", "[interface]") {
	SetVariable(nullptr);
	{
		ScopedEnvironmentVariable variable(NAME, "");
		CHECK(ReadVariable() == "");
	}
	CHECK(ReadVariable() == UNSET);
}

TEST_CASE("After an empty scope then an unset scope, the variable stays unset", "[interface]") {
	SetVariable(nullptr);
	{ ScopedEnvironmentVariable empty(NAME, ""); }
	{
		ScopedEnvironmentVariable unset(NAME, nullptr);
		CHECK(ReadVariable() == UNSET);
	}
	CHECK(ReadVariable() == UNSET);
}

#if defined(_WIN32)
TEST_CASE("ScopedEnvironmentVariable refuses a name or value that is not UTF-8", "[interface]") {
	SetVariable(nullptr);
	const std::string refusal = "ScopedEnvironmentVariable takes UTF-8 names and values";
	CHECK_THROWS_WITH(ScopedEnvironmentVariable("REDSHIFT_INTERFACE_TEST_\xff", nullptr), refusal);
	CHECK_THROWS_WITH(ScopedEnvironmentVariable(NAME, "a\xff"), refusal);
	CHECK(ReadVariable() == UNSET);
}

TEST_CASE("ScopedEnvironmentVariable restores a value only the environment block held", "[interface]") {
	// SetEnvironmentVariableW alone, as a host runtime other than this binary's sets a variable.
	SetVariable(nullptr);
	REQUIRE(SetEnvironmentVariableW(WideText(NAME).c_str(), L"block-only") != 0);
	REQUIRE(std::getenv(NAME) == nullptr);
	{
		ScopedEnvironmentVariable variable(NAME, "during");
		CHECK(ReadVariable() == "during");
	}
	CHECK(ReadVariable() == "block-only");
	SetVariable(nullptr);
}

TEST_CASE("Under ScopedEnvironmentVariable, getenv sees a value but no empty or unset one", "[interface]") {
	SetVariable("before");
	{
		ScopedEnvironmentVariable valued(NAME, "during");
		CHECK(std::string(std::getenv(NAME) ? std::getenv(NAME) : UNSET) == "during");
	}
	{
		ScopedEnvironmentVariable empty(NAME, "");
		CHECK(std::getenv(NAME) == nullptr);
	}
	{
		ScopedEnvironmentVariable unset(NAME, nullptr);
		CHECK(std::getenv(NAME) == nullptr);
	}
	CHECK(std::string(std::getenv(NAME) ? std::getenv(NAME) : UNSET) == "before");
	SetVariable(nullptr);
}

TEST_CASE("ReadEnvironmentBlock reads an empty value as set after a stale ERROR_ENVVAR_NOT_FOUND", "[interface]") {
	ScopedEnvironmentVariable empty(NAME, "");
	const auto name = WideText(NAME);
	std::wstring value = L"stale";
	SetLastError(ERROR_ENVVAR_NOT_FOUND);
	const bool set = ReadEnvironmentBlock(name, value);
	CHECK(set);
	CHECK(value.empty());
}

TEST_CASE("ReadEnvironmentBlock reads whole a value that grew once between its size query and its read",
          "[interface]") {
	CHECK(ReadGrowing(2, 2) == L"abx");
	CHECK(read_calls == 3);
}

TEST_CASE("ReadEnvironmentBlock gives one U+001A for a value that grows before each of its reads", "[interface]") {
	CHECK(ReadGrowing(2, 1000) == std::wstring(1, L'\x1a'));
	CHECK(read_calls == 4);
}

TEST_CASE("WideText gives empty text for text that is not UTF-8", "[interface]") {
	CHECK(WideText("ab") == L"ab");
	CHECK(WideText("a\xff").empty());
}
#endif
