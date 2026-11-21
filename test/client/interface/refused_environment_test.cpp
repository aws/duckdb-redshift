#include "odbc_driver.hpp"
#include "scoped_environment_variable.hpp"

#include "catch.hpp"

#include <cstdlib>
#include <string>
#include <utility>
#include <vector>

using namespace duckdb::redshift_driver;
using namespace duckdb::redshift_driver::testing;

namespace {

const char *const REFUSED_A = "REDSHIFT_INTERFACE_TEST_REFUSED_A";
const char *const REFUSED_B = "REDSHIFT_INTERFACE_TEST_REFUSED_B";
const char *const REFUSED_C = "REDSHIFT_INTERFACE_TEST_REFUSED_C";

LoadedDriver Refusing(std::vector<std::string> names) {
	LoadedDriver driver;
	driver.refused_environment = std::move(names);
	return driver;
}

//! The variable found, as a string, so a check that fails prints the name instead of a pointer.
std::string FoundName(const LoadedDriver &driver) {
	const char *name = FindRefusedEnvironmentVariable(driver);
	return name == nullptr ? "(none)" : name;
}

} // namespace

TEST_CASE("FindRefusedEnvironmentVariable finds nothing when no refused variable is set", "[interface]") {
	ScopedEnvironmentVariable unset_a(REFUSED_A, nullptr);
	CHECK(FindRefusedEnvironmentVariable(Refusing({REFUSED_A})) == nullptr);
}

TEST_CASE("FindRefusedEnvironmentVariable returns the name of a set refused variable", "[interface]") {
	ScopedEnvironmentVariable set_a(REFUSED_A, "a-value");
	CHECK(FoundName(Refusing({REFUSED_A})) == REFUSED_A);
}

TEST_CASE("FindRefusedEnvironmentVariable counts an empty value as set", "[interface]") {
	ScopedEnvironmentVariable empty_a(REFUSED_A, "");
	CHECK(FoundName(Refusing({REFUSED_A})) == REFUSED_A);
}

TEST_CASE("FindRefusedEnvironmentVariable returns the first set variable in list order", "[interface]") {
	ScopedEnvironmentVariable unset_a(REFUSED_A, nullptr);
	ScopedEnvironmentVariable set_b(REFUSED_B, "b-value");
	ScopedEnvironmentVariable set_c(REFUSED_C, "c-value");
	// C is listed before B, so a search in name or environment order would answer B.
	CHECK(FoundName(Refusing({REFUSED_A, REFUSED_C, REFUSED_B})) == REFUSED_C);
}

#if defined(_WIN32)
namespace {

//! An export of ucrtbase.dll, the shared Universal C runtime whose copy of the environment the driver
//! reads, as `Function`, or nullptr. The DLL is loaded from System32, where the driver's import of it
//! resolves, and never freed, so the check finds it loaded.
template <typename Function>
Function SharedRuntimeExport(const char *name) {
	const HMODULE ucrt = LoadLibraryExW(L"ucrtbase.dll", nullptr, LOAD_LIBRARY_SEARCH_SYSTEM32);
	return ucrt == nullptr ? nullptr : reinterpret_cast<Function>(GetProcAddress(ucrt, name));
}

using SharedPutenv = int(__cdecl *)(const char *);
using SharedGetenv = char *(__cdecl *)(const char *);

//! Removes REFUSED_A from the shared runtime's copy and the block through the runtime's own _putenv
//! when the scope ends, so a case that fails leaves nothing in the copy for the cases after it.
struct RemovedFromSharedRuntimeAtExit {
	SharedPutenv put;
	~RemovedFromSharedRuntimeAtExit() {
		put((std::string(REFUSED_A) + "=").c_str());
	}
};

//! For a scope, the shared runtime's narrow (`__p__environ`) or wide (`__p__wenviron`) table is
//! `table`, an array the case owns, or none for nullptr; the runtime's own is put back after. Only
//! reads reach the runtime's environment meanwhile.
template <typename Character>
class SharedTableAs {
public:
	SharedTableAs(const char *table_export, Character **table) {
		using Slot = Character ***(__cdecl *)();
		const auto slot_of = SharedRuntimeExport<Slot>(table_export);
		REQUIRE(slot_of != nullptr);
		slot = slot_of();
		REQUIRE(slot != nullptr);
		saved = *slot;
		*slot = table;
	}
	~SharedTableAs() {
		*slot = saved;
	}
	SharedTableAs(const SharedTableAs &) = delete;
	SharedTableAs &operator=(const SharedTableAs &) = delete;

private:
	Character ***slot = nullptr;
	Character **saved = nullptr;
};

} // namespace

TEST_CASE("On Windows FindRefusedEnvironmentVariable finds a name set only in the environment block", "[interface]") {
	// Set as a host runtime other than the extension's sets it after LOAD, which getenv does not see.
	ScopedEnvironmentVariable unset_a(REFUSED_A, nullptr);
	REQUIRE(SetEnvironmentVariableW(WideText(REFUSED_A).c_str(), L"a-value") != 0);
	REQUIRE(std::getenv(REFUSED_A) == nullptr);
	CHECK(FoundName(Refusing({REFUSED_A})) == REFUSED_A);
}

TEST_CASE("On Windows FindRefusedEnvironmentVariable passes a name removed from the block", "[interface]") {
	ScopedEnvironmentVariable unset_a(REFUSED_A, nullptr);
	REQUIRE(SetEnvironmentVariableW(WideText(REFUSED_A).c_str(), L"a-value") != 0);
	REQUIRE(SetEnvironmentVariableW(WideText(REFUSED_A).c_str(), nullptr) != 0);
	CHECK(FindRefusedEnvironmentVariable(Refusing({REFUSED_A})) == nullptr);
}

TEST_CASE("On Windows FindRefusedEnvironmentVariable finds a name the shared C runtime keeps after the block lost it",
          "[interface]") {
	// The driver's getenv reads ucrtbase.dll's copy, which this binary's static runtime does not
	// share, so the name goes in through that DLL's own _putenv, as a host built on it puts one.
	ScopedEnvironmentVariable unset_a(REFUSED_A, nullptr);
	const auto put = SharedRuntimeExport<SharedPutenv>("_putenv");
	const auto get = SharedRuntimeExport<SharedGetenv>("getenv");
	REQUIRE(put != nullptr);
	REQUIRE(get != nullptr);
	RemovedFromSharedRuntimeAtExit removed {put};
	REQUIRE(put((std::string(REFUSED_A) + "=a-value").c_str()) == 0);            // the copy and the block
	REQUIRE(SetEnvironmentVariableW(WideText(REFUSED_A).c_str(), nullptr) != 0); // the block alone
	std::wstring value;
	REQUIRE_FALSE(ReadEnvironmentBlock(WideText(REFUSED_A), value));
	REQUIRE(get(REFUSED_A) != nullptr);
	CHECK(FoundName(Refusing({REFUSED_A})) == REFUSED_A);
}

TEST_CASE("On Windows FindRefusedEnvironmentVariable passes a name removed from the shared C runtime and the block",
          "[interface]") {
	ScopedEnvironmentVariable unset_a(REFUSED_A, nullptr);
	const auto put = SharedRuntimeExport<SharedPutenv>("_putenv");
	const auto get = SharedRuntimeExport<SharedGetenv>("getenv");
	REQUIRE(put != nullptr);
	REQUIRE(get != nullptr);
	RemovedFromSharedRuntimeAtExit removed {put};
	REQUIRE(put((std::string(REFUSED_A) + "=a-value").c_str()) == 0);
	REQUIRE(put((std::string(REFUSED_A) + "=").c_str()) == 0); // removes it from both
	REQUIRE(get(REFUSED_A) == nullptr);
	CHECK(FindRefusedEnvironmentVariable(Refusing({REFUSED_A})) == nullptr);
}

TEST_CASE("On Windows FindRefusedEnvironmentVariable finds, in list order, a name only the shared C runtime's narrow "
          "table holds",
          "[interface]") {
	ScopedEnvironmentVariable unset_a(REFUSED_A, nullptr);
	ScopedEnvironmentVariable set_b(REFUSED_B, "b-value"); // in the block, listed after A
	std::string entry = std::string(REFUSED_A) + "=a-value";
	char *narrow_entries[] = {&entry[0], nullptr};
	wchar_t *no_entries[] = {nullptr};
	SharedTableAs<char> narrow("__p__environ", narrow_entries);
	SharedTableAs<wchar_t> wide("__p__wenviron", no_entries);
	CHECK(FoundName(Refusing({REFUSED_A, REFUSED_B})) == REFUSED_A);
}

TEST_CASE("On Windows FindRefusedEnvironmentVariable finds a name only the shared C runtime's wide table holds",
          "[interface]") {
	// The driver's _wdupenv_s reads the wide table.
	ScopedEnvironmentVariable unset_a(REFUSED_A, nullptr);
	std::wstring entry = WideText(REFUSED_A) + L"=a-value";
	char *no_entries[] = {nullptr};
	wchar_t *wide_entries[] = {&entry[0], nullptr};
	SharedTableAs<char> narrow("__p__environ", no_entries);
	SharedTableAs<wchar_t> wide("__p__wenviron", wide_entries);
	CHECK(FoundName(Refusing({REFUSED_A})) == REFUSED_A);
}

TEST_CASE("On Windows FindRefusedEnvironmentVariable passes a name the block lost when no wide table exists",
          "[interface]") {
	// Asking _wgetenv for a missing wide table makes it from the block. Made while B is in the block
	// alone, it would keep B after the block loses it, so the second check would refuse it.
	ScopedEnvironmentVariable unset_a(REFUSED_A, nullptr);
	ScopedEnvironmentVariable unset_b(REFUSED_B, nullptr);
	char *no_entries[] = {nullptr};
	SharedTableAs<char> narrow("__p__environ", no_entries);
	SharedTableAs<wchar_t> wide("__p__wenviron", nullptr);
	REQUIRE(SetEnvironmentVariableW(WideText(REFUSED_B).c_str(), L"b-value") != 0);
	CHECK(FoundName(Refusing({REFUSED_A, REFUSED_B})) == REFUSED_B);
	REQUIRE(SetEnvironmentVariableW(WideText(REFUSED_B).c_str(), nullptr) != 0);
	CHECK(FindRefusedEnvironmentVariable(Refusing({REFUSED_B})) == nullptr);
}

TEST_CASE("On Windows FindRefusedEnvironmentVariable counts a name that is not ASCII as set", "[interface]") {
	// The name is never looked up, so nothing needs to be set for it to be returned.
	const std::string name = std::string(REFUSED_A) + "_\xc3\xa9";
	ScopedEnvironmentVariable unset(name.c_str(), nullptr);
	CHECK(FoundName(Refusing({name})) == name);
}

TEST_CASE("On Windows FindRefusedEnvironmentVariable refuses an empty value after a stale ERROR_ENVVAR_NOT_FOUND",
          "[interface]") {
	ScopedEnvironmentVariable empty_a(REFUSED_A, "");
	const auto driver = Refusing({REFUSED_A});
	SetLastError(ERROR_ENVVAR_NOT_FOUND);
	const char *found = FindRefusedEnvironmentVariable(driver);
	CHECK(std::string(found == nullptr ? "(none)" : found) == REFUSED_A);
}
#endif
