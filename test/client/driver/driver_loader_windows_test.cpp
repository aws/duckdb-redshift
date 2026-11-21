// The Windows load against the test fakes, run by the Windows driver suite. Each load
// copies the fake under names of its own, since a DLL is never unloaded and the post-load check
// counts every module of a set name in the process.

#include "windows_temp_dir.hpp"

#include "driver_loader_windows.hpp"
#include "sha256.hpp"

#include <psapi.h>

#include <algorithm>
#include <atomic>
#include <cstdint>
#include <fstream>
#include <iterator>
#include <new>
#include <thread>

using namespace duckdb::redshift_driver;
using namespace duckdb::redshift_driver::windows_driver;
using duckdb::redshift_driver::windows_rules::Widen;
using redshift_driver_test::RandomName;
using redshift_driver_test::WindowsTempDir;

namespace {

// The flags the loader uses, for the tests' own loads of a fake.
constexpr DWORD SEARCH_PRIVATE = LOAD_LIBRARY_SEARCH_DLL_LOAD_DIR | LOAD_LIBRARY_SEARCH_SYSTEM32;

std::vector<unsigned char> FileBytes(std::string path) {
	std::replace(path.begin(), path.end(), '/', '\\');
	std::ifstream in(path, std::ios::binary);
	REQUIRE(in);
	return std::vector<unsigned char>(std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>());
}

// A fresh 16-letter name, as the rename's generated names are.
std::string LetterName() {
	std::string name;
	while (name.size() < 16) {
		for (char c : RandomName()) {
			name.push_back(static_cast<char>('a' + (c - '0')));
		}
	}
	return name.substr(0, 16);
}

// A set of the fake driver and a library copy under fresh names, with a notice, as the lock lists one.
struct TestSet {
	std::vector<std::vector<unsigned char>> bytes;
	LoaderConfig config;
};

TestSet MakeSet(const char *driver = REDSHIFT_TEST_DRIVER_COMPLETE, bool with_library = true) {
	TestSet set;
	set.bytes = {FileBytes(driver), {'n', 'o', 't', 'i', 'c', 'e'}};
	std::vector<std::string> names = {LetterName() + ".dll", "NOTICE"};
	if (with_library) {
		set.bytes.insert(set.bytes.begin() + 1, FileBytes(REDSHIFT_TEST_DRIVER_COMPLETE));
		names.insert(names.begin() + 1, LetterName() + ".dll");
	}
	for (size_t i = 0; i < names.size(); i++) {
		set.config.files.push_back({names[i], detail::Sha256Hex(set.bytes[i].data(), set.bytes[i].size()),
		                            set.bytes[i].data(), set.bytes[i].size()});
	}
	set.config.set_sha256 = detail::Sha256Hex(names[0].data(), names[0].size());
	set.config.refused_environment = {"REDSHIFT_TEST_REFUSED_NAME"};
	return set;
}

LoadInputs InputsFor(const std::string &base) {
	LoadInputs inputs;
	inputs.bases.override_set = true;
	inputs.bases.override_value = Widen(base);
	inputs.bases.per_user_base = Widen(base + "\\cache");
	inputs.user = CurrentUserSid();
	return inputs;
}

std::atomic<int> g_loader_calls {0};
void CountLoaderCall(const std::vector<std::string> &) {
	g_loader_calls++;
}

std::atomic<int> g_write_errors {0};
void TryToReplace(const std::vector<std::string> &paths) {
	for (const auto &path : paths) {
		FileHandle writer(CreateFileW(Widen(path).c_str(), GENERIC_WRITE,
		                              FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr, OPEN_EXISTING, 0,
		                              nullptr));
		g_write_errors += !writer.valid() && GetLastError() == ERROR_SHARING_VIOLATION ? 1 : 0;
		const auto other = path + ".other";
		std::ofstream(other, std::ios::binary) << "planted";
		g_write_errors += !MoveFileExW(Widen(other).c_str(), Widen(path).c_str(), MOVEFILE_REPLACE_EXISTING) ? 1 : 0;
	}
}

std::string InstallInto(const std::string &root, const LoaderConfig &config, InstallResult expected,
                        int deadline = 30000) {
	std::string set_dir, where, reason;
	CHECK(InstallSet(root, config.set_sha256, config.files, CurrentUserSid(), deadline, set_dir, where, reason) ==
	      expected);
	return reason;
}

// HKEY_LOCAL_MACHINE redirected to a test-owned key of the user's own hive.
class RedirectedMachineHive {
public:
	RedirectedMachineHive() : path_(L"Software\\redshift-driver-test-" + Widen(RandomName())) {
		REQUIRE(RegCreateKeyExW(HKEY_CURRENT_USER, path_.c_str(), 0, nullptr, 0, KEY_ALL_ACCESS, nullptr, &root_,
		                        nullptr) == ERROR_SUCCESS);
		REQUIRE(RegOverridePredefKey(HKEY_LOCAL_MACHINE, root_) == ERROR_SUCCESS);
	}
	~RedirectedMachineHive() {
		RegOverridePredefKey(HKEY_LOCAL_MACHINE, nullptr);
		RegCloseKey(root_);
		RegDeleteTreeW(HKEY_CURRENT_USER, path_.c_str());
	}
	HKEY DriverKey() {
		HKEY key = nullptr;
		REQUIRE(RegCreateKeyExW(root_, L"SOFTWARE\\Amazon\\Amazon Redshift ODBC Driver (x64)\\Driver", 0, nullptr, 0,
		                        KEY_ALL_ACCESS, nullptr, &key, nullptr) == ERROR_SUCCESS);
		return key;
	}

private:
	std::wstring path_;
	HKEY root_ = nullptr;
};

void SetString(HKEY key, const wchar_t *name, const std::wstring &value) {
	REQUIRE(RegSetValueExW(key, name, 0, REG_SZ, reinterpret_cast<const BYTE *>(value.c_str()),
	                       static_cast<DWORD>((value.size() + 1) * sizeof(wchar_t))) == ERROR_SUCCESS);
}

// Gives file `i` of `set` other bytes, with their SHA-256, as a corrupted or foreign file.
void ReplaceBytes(TestSet &set, size_t i, std::vector<unsigned char> bytes) {
	set.bytes[i] = std::move(bytes);
	auto &file = set.config.files[i];
	file.sha256 = detail::Sha256Hex(set.bytes[i].data(), set.bytes[i].size());
	file.data = set.bytes[i].data();
	file.size = set.bytes[i].size();
}

std::vector<std::string> g_loaded_paths;
void RecordPaths(const std::vector<std::string> &paths) {
	g_loaded_paths = paths;
}

std::string SystemDbgHelpPath() {
	wchar_t system[MAX_PATH];
	const UINT length = GetSystemDirectoryW(system, MAX_PATH);
	REQUIRE((length > 0 && length < MAX_PATH));
	return windows_rules::Narrow(std::wstring(system, length)) + "\\" + windows_rules::DBGHELP;
}

// The paths of the process's modules named DbgHelp.dll.
std::vector<std::string> DbgHelpModules() {
	std::vector<HMODULE> handles(4096);
	DWORD needed = 0;
	REQUIRE(K32EnumProcessModules(GetCurrentProcess(), handles.data(),
	                              static_cast<DWORD>(handles.size() * sizeof(HMODULE)), &needed));
	REQUIRE(needed <= handles.size() * sizeof(HMODULE));
	std::vector<std::string> found;
	for (size_t i = 0; i < needed / sizeof(HMODULE); i++) {
		wchar_t name[MAX_PATH];
		const DWORD length = GetModuleFileNameW(handles[i], name, MAX_PATH);
		const auto path = windows_rules::Narrow(std::wstring(name, length < MAX_PATH ? length : 0));
		if (windows_rules::SamePath(windows_rules::BaseName(path), windows_rules::DBGHELP)) {
			found.push_back(path);
		}
	}
	return found;
}

// A copy of System32's DbgHelp.dll in `dir`, loaded by full path, as a host could load its own;
// null when it cannot be, so a hook can call it without throwing.
HMODULE LoadPlantedDbgHelp(const std::string &dir) {
	wchar_t system[MAX_PATH];
	const UINT length = GetSystemDirectoryW(system, MAX_PATH);
	const auto planted = Widen(dir + "\\" + windows_rules::DBGHELP);
	const auto source = std::wstring(system, length < MAX_PATH ? length : 0) + L"\\" + Widen(windows_rules::DBGHELP);
	if (!CopyFileW(source.c_str(), planted.c_str(), TRUE)) {
		return nullptr;
	}
	return LoadLibraryExW(planted.c_str(), nullptr, SEARCH_PRIVATE);
}

std::string g_planted_dir;
HMODULE g_planted = nullptr;
void PlantDbgHelp(const std::vector<std::string> &) {
	g_planted = LoadPlantedDbgHelp(g_planted_dir);
}

bool TraceIsOff() {
	DriverError error;
	const bool off = CheckRegistryTrace(error);
	CHECK((off || error.kind == DriverErrorKind::REFUSED_ENVIRONMENT));
	return off;
}

} // namespace

TEST_CASE("The Windows loader loads a set from REDSHIFT_DRIVER_DIR with every entry point", "[windows_driver]") {
	WindowsTempDir temp;
	auto set = MakeSet();
	DriverLoader loader(set.config);
	const auto result = loader.Load([&] { return InputsFor(temp.path()); });
	REQUIRE(result.ok());
	CHECK(result.driver->api.Complete());
	CHECK(result.driver->identity.directory == temp.path() + "\\.redshift-driver\\" + set.config.set_sha256);
	CHECK(result.driver->trust_directory == temp.path() + "\\trust");
}

TEST_CASE("A lower-case drive letter in REDSHIFT_DRIVER_DIR loads past the final-path check", "[windows_driver]") {
	WindowsTempDir temp;
	auto base = temp.path();
	base[0] = static_cast<char>(base[0] | 0x20);
	auto set = MakeSet();
	DriverLoader loader(set.config);
	CHECK(loader.Load([&] { return InputsFor(base); }).ok());
}

TEST_CASE("A second load returns the same LoadedDriver without calling the system's loader again", "[windows_driver]") {
	WindowsTempDir temp;
	auto set = MakeSet();
	DriverLoader loader(set.config);
	g_loader_calls = 0;
	auto inputs = InputsFor(temp.path());
	inputs.before_platform_loader = CountLoaderCall;
	const auto first = loader.Load([&] { return inputs; });
	const auto second = loader.Load([&] { return inputs; });
	REQUIRE(first.ok());
	CHECK(second.driver == first.driver);
	CHECK(g_loader_calls == 1);
}

TEST_CASE("Eight threads loading at once get the same LoadedDriver", "[windows_driver]") {
	WindowsTempDir temp;
	auto set = MakeSet();
	DriverLoader loader(set.config);
	std::vector<const LoadedDriver *> drivers(8, nullptr);
	std::vector<std::thread> threads;
	for (size_t i = 0; i < drivers.size(); i++) {
		threads.emplace_back([&, i] { drivers[i] = loader.Load([&] { return InputsFor(temp.path()); }).driver; });
	}
	for (auto &thread : threads) {
		thread.join();
	}
	REQUIRE(drivers[0] != nullptr);
	CHECK(std::all_of(drivers.begin(), drivers.end(), [&](const LoadedDriver *d) { return d == drivers[0]; }));
}

TEST_CASE("The missing-entry fake gives a final MISSING_ENTRY_POINT after one DllMain run", "[windows_driver]") {
	WindowsTempDir temp;
	const auto log = temp.path() + "\\dllmain.log";
	REQUIRE(SetEnvironmentVariableW(L"REDSHIFT_TEST_CONSTRUCTOR_LOG", Widen(log).c_str()));
	auto set = MakeSet(REDSHIFT_TEST_DRIVER_MISSING_ENTRY, false);
	DriverLoader loader(set.config);
	const auto first = loader.Load([&] { return InputsFor(temp.path()); });
	const auto second = loader.Load([&] { return InputsFor(temp.path()); });
	SetEnvironmentVariableW(L"REDSHIFT_TEST_CONSTRUCTOR_LOG", nullptr);
	CHECK(first.error.kind == DriverErrorKind::MISSING_ENTRY_POINT);
	CHECK(first.error.message == "the Redshift ODBC driver does not export SQLGetDiagField");
	CHECK(second.error.message == first.error.message);
	std::ifstream in(log);
	const std::string runs((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
	CHECK(runs == "run\n");
}

TEST_CASE("A refused name set after start is refused before any load, then loads once removed", "[windows_driver]") {
	WindowsTempDir temp;
	auto set = MakeSet();
	DriverLoader loader(set.config);
	g_loader_calls = 0;
	auto inputs = InputsFor(temp.path());
	inputs.before_platform_loader = CountLoaderCall;
	REQUIRE(SetEnvironmentVariableW(L"REDSHIFT_TEST_REFUSED_NAME", L"x"));
	const auto refused = loader.Load([&] { return inputs; });
	REQUIRE(SetEnvironmentVariableW(L"REDSHIFT_TEST_REFUSED_NAME", nullptr));
	CHECK(refused.error.kind == DriverErrorKind::REFUSED_ENVIRONMENT);
	// The client's sentence whole, as the POSIX loader and OdbcConnection word it.
	CHECK(refused.error.message == "REDSHIFT_TEST_REFUSED_NAME is set; the Redshift driver would read it, and it can "
	                               "change the driver's connection or security settings. Unset it to connect.");
	CHECK(g_loader_calls == 0);
	CHECK(loader.Load([&] { return inputs; }).ok());
}

TEST_CASE("A refused name set to an empty value is refused", "[windows_driver]") {
	WindowsTempDir temp;
	auto set = MakeSet();
	DriverLoader loader(set.config);
	REQUIRE(SetEnvironmentVariableW(L"REDSHIFT_TEST_REFUSED_NAME", L""));
	const auto refused = loader.Load([&] { return InputsFor(temp.path()); });
	SetEnvironmentVariableW(L"REDSHIFT_TEST_REFUSED_NAME", nullptr);
	CHECK(refused.error.kind == DriverErrorKind::REFUSED_ENVIRONMENT);
}

TEST_CASE("A module of a set name loaded from another folder makes the load fail for good", "[windows_driver]") {
	WindowsTempDir temp;
	auto set = MakeSet();
	const auto elsewhere = temp.Make("elsewhere") + "\\" + set.config.files[1].name;
	std::ofstream(elsewhere, std::ios::binary)
	    .write(reinterpret_cast<const char *>(set.bytes[1].data()), static_cast<std::streamsize>(set.bytes[1].size()));
	REQUIRE(LoadLibraryExW(Widen(elsewhere).c_str(), nullptr, SEARCH_PRIVATE) != nullptr);
	DriverLoader loader(set.config);
	LoadInputs inputs = InputsFor(temp.path());
	inputs.bases.override_set = false;
	inputs.bases.extension_file = Widen(temp.Make("extension") + "\\redshift.duckdb_extension");
	const auto first = loader.Load([&] { return inputs; });
	CHECK(first.error.kind == DriverErrorKind::LOAD_FAILED);
	CHECK(loader.Load([&] { return inputs; }).error.message == first.error.message);
	// No other base is tried once the system's loader was called.
	CHECK(GetFileAttributesW(Widen(temp.path() + "\\cache").c_str()) == INVALID_FILE_ATTRIBUTES);
}

TEST_CASE("A host's own module under an original name is not counted by the post-load check", "[windows_driver]") {
	WindowsTempDir temp;
	auto set = MakeSet();
	const auto host_copy = temp.Make("host") + "\\crypto.dll";
	std::ofstream(host_copy, std::ios::binary)
	    .write(reinterpret_cast<const char *>(set.bytes[1].data()), static_cast<std::streamsize>(set.bytes[1].size()));
	REQUIRE(LoadLibraryExW(Widen(host_copy).c_str(), nullptr, SEARCH_PRIVATE) != nullptr);
	DriverLoader loader(set.config);
	CHECK(loader.Load([&] { return InputsFor(temp.path()); }).ok());
}

TEST_CASE("Another open for write, or a replacing move, fails while the loader holds the set", "[windows_driver]") {
	WindowsTempDir temp;
	auto set = MakeSet();
	DriverLoader loader(set.config);
	g_write_errors = 0;
	auto inputs = InputsFor(temp.path());
	inputs.before_platform_loader = TryToReplace;
	REQUIRE(loader.Load([&] { return inputs; }).ok());
	CHECK(g_write_errors == 4); // two DLLs, each refused a writer and a move over it
}

TEST_CASE("An invalid set whose file another handle holds without delete sharing cannot be set aside",
          "[windows_driver]") {
	WindowsTempDir temp;
	auto set = MakeSet();
	const auto root = temp.Make(".redshift-driver");
	InstallInto(root, set.config, InstallResult::INSTALLED);
	const auto set_dir = root + "\\" + set.config.set_sha256;
	// As a program holds a file it has open: its folder cannot be renamed until the handle closes.
	FileHandle holder(CreateFileW(Widen(set_dir + "\\" + set.config.files[1].name).c_str(), GENERIC_READ,
	                              FILE_SHARE_READ, nullptr, OPEN_EXISTING, 0, nullptr));
	REQUIRE(holder.valid());
	std::ofstream(set_dir + "\\root.crt") << "planted";
	CHECK(InstallInto(root, set.config, InstallResult::FAILED) == windows_rules::SET_IN_USE_RULE);
}

TEST_CASE("An invalid set whose holder closes during the retries is set aside and installed again",
          "[windows_driver]") {
	WindowsTempDir temp;
	auto set = MakeSet();
	const auto root = temp.Make(".redshift-driver");
	InstallInto(root, set.config, InstallResult::INSTALLED);
	const auto set_dir = root + "\\" + set.config.set_sha256;
	FileHandle holder(CreateFileW(Widen(set_dir + "\\" + set.config.files[1].name).c_str(), GENERIC_READ,
	                              FILE_SHARE_READ, nullptr, OPEN_EXISTING, 0, nullptr));
	REQUIRE(holder.valid());
	std::ofstream(set_dir + "\\root.crt") << "planted";
	// Released half a second after the install starts, which for a set this small falls inside the
	// set-aside's retries (about a second from the first try); released before the first try, the
	// case would pass without them.
	std::thread release([&] {
		Sleep(500);
		holder = FileHandle();
	});
	InstallInto(root, set.config, InstallResult::INSTALLED);
	release.join();
	CHECK(GetFileAttributesW(Widen(set_dir + "\\root.crt").c_str()) == INVALID_FILE_ATTRIBUTES);
}

TEST_CASE("An invalid set at the first base held without delete sharing is refused, and the next base loads",
          "[windows_driver]") {
	WindowsTempDir temp;
	auto set = MakeSet();
	const auto extension = temp.Make("extension");
	const auto first_root = temp.Make("extension\\.redshift-driver");
	InstallInto(first_root, set.config, InstallResult::INSTALLED);
	const auto first_set = first_root + "\\" + set.config.set_sha256;
	FileHandle holder(CreateFileW(Widen(first_set + "\\" + set.config.files[1].name).c_str(), GENERIC_READ,
	                              FILE_SHARE_READ, nullptr, OPEN_EXISTING, 0, nullptr));
	REQUIRE(holder.valid());
	std::ofstream(first_set + "\\root.crt") << "planted";
	DriverLoader loader(set.config);
	LoadInputs inputs = InputsFor(temp.path());
	inputs.bases.override_set = false;
	inputs.bases.extension_file = Widen(extension + "\\redshift.duckdb_extension");
	const auto result = loader.Load([&] { return inputs; });
	REQUIRE(result.ok());
	CHECK(result.driver->identity.directory == temp.path() + "\\cache\\.redshift-driver\\" + set.config.set_sha256);
	CHECK(GetFileAttributesW(Widen(first_set + "\\root.crt").c_str()) != INVALID_FILE_ATTRIBUTES);
}

TEST_CASE("An invalid set whose DLL is only mapped is set aside and installed again", "[windows_driver]") {
	WindowsTempDir temp;
	auto set = MakeSet();
	const auto root = temp.Make(".redshift-driver");
	InstallInto(root, set.config, InstallResult::INSTALLED);
	const auto set_dir = root + "\\" + set.config.set_sha256;
	// Windows maps an image with delete sharing, so the mapping does not keep its folder in place.
	REQUIRE(LoadLibraryExW(Widen(set_dir + "\\" + set.config.files[1].name).c_str(), nullptr, SEARCH_PRIVATE) !=
	        nullptr);
	std::ofstream(set_dir + "\\root.crt") << "planted";
	InstallInto(root, set.config, InstallResult::INSTALLED);
	CHECK(GetFileAttributesW(Widen(set_dir + "\\root.crt").c_str()) == INVALID_FILE_ATTRIBUTES);
}

TEST_CASE("Registry trace values that count as on are refused before any load", "[windows_driver]") {
	RedirectedMachineHive hive;
	HKEY key = hive.DriverKey();
	for (const auto *value : {L"1", L" 1", L"+1", L"1x"}) {
		SetString(key, L"LogLevel", value);
		CHECK_FALSE(TraceIsOff());
	}
	RegDeleteValueW(key, L"LogLevel");
	const DWORD one = 1;
	REQUIRE(RegSetValueExW(key, L"Trace", 0, REG_DWORD, reinterpret_cast<const BYTE *>(&one), sizeof(one)) ==
	        ERROR_SUCCESS);
	CHECK_FALSE(TraceIsOff());
	RegCloseKey(key);
}

TEST_CASE("Registry trace values that count as off pass", "[windows_driver]") {
	RedirectedMachineHive hive;
	CHECK(TraceIsOff()); // no key
	HKEY key = hive.DriverKey();
	CHECK(TraceIsOff()); // neither value
	SetString(key, L"LogLevel", L"");
	SetString(key, L"Trace", L"0");
	CHECK(TraceIsOff());
	RegCloseKey(key);
}

TEST_CASE("A trace key that denies reading its values is refused", "[windows_driver]") {
	RedirectedMachineHive hive;
	HKEY key = hive.DriverKey();
	PSECURITY_DESCRIPTOR descriptor = nullptr;
	const auto sddl = "D:P(D;;KR;;;" + CurrentUserSid() + ")(A;;KA;;;" + CurrentUserSid() + ")";
	REQUIRE(ConvertStringSecurityDescriptorToSecurityDescriptorA(sddl.c_str(), SDDL_REVISION_1, &descriptor, nullptr));
	REQUIRE(RegSetKeySecurity(key, DACL_SECURITY_INFORMATION, descriptor) == ERROR_SUCCESS);
	LocalFree(descriptor);
	CHECK_FALSE(TraceIsOff());
	PSECURITY_DESCRIPTOR open = nullptr;
	REQUIRE(ConvertStringSecurityDescriptorToSecurityDescriptorA(("D:(A;;KA;;;" + CurrentUserSid() + ")").c_str(),
	                                                             SDDL_REVISION_1, &open, nullptr));
	RegSetKeySecurity(key, DACL_SECURITY_INFORMATION, open);
	LocalFree(open);
	RegCloseKey(key);
}

TEST_CASE("A runtime the program loaded from System32 that is too old gets System32's remedy", "[windows_driver]") {
	// kernel32.dll is loaded from System32, as a /MD host's VCRUNTIME140.dll is.
	DriverError error;
	CHECK_FALSE(CheckPrerequisites({{"kernel32.dll", "999.0.0.0"}}, error));
	CHECK(error.kind == DriverErrorKind::MISSING_PREREQUISITE);
	CHECK(error.message.find("would use System32's copy, which is version ") != std::string::npos);
	CHECK(error.message.find("install Microsoft's latest Visual C++ v14 x64 redistributable") != std::string::npos);
	// A minimum that cannot be read is met by no copy.
	CHECK_FALSE(CheckPrerequisites({{"kernel32.dll", "not-a-version"}}, error));
}

TEST_CASE("System32's copy of a runtime is read when the program has loaded none", "[windows_driver]") {
	REQUIRE(GetModuleHandleW(L"cabinet.dll") == nullptr);
	DriverError error;
	CHECK(CheckPrerequisites({{"cabinet.dll", "1.0.0.0"}}, error));
	CHECK_FALSE(CheckPrerequisites({{"cabinet.dll", "999.0.0.0"}}, error));
	CHECK(error.message.find("would use System32's copy") != std::string::npos);
}

TEST_CASE("A runtime the program loaded from a folder whose name is not ASCII is read by its path, and is its own",
          "[windows_driver]") {
	WindowsTempDir temp;
	// As python.org Python's runtime beside python.exe, under a profile whose user name is not ASCII.
	const auto folder = Widen(temp.path()) + L"\\J\u00fcrgen";
	REQUIRE(CreateDirectoryW(folder.c_str(), nullptr));
	const auto name = LetterName() + ".dll";
	const auto copy = folder + L"\\" + Widen(name);
	wchar_t system[MAX_PATH];
	const UINT length = GetSystemDirectoryW(system, MAX_PATH);
	REQUIRE((length > 0 && length < MAX_PATH));
	REQUIRE(CopyFileW((std::wstring(system, length) + L"\\cabinet.dll").c_str(), copy.c_str(), TRUE));
	REQUIRE(LoadLibraryExW(copy.c_str(), nullptr, SEARCH_PRIVATE) != nullptr);
	DriverError error;
	CHECK(CheckPrerequisites({{name, "1.0.0.0"}}, error));
	// A copy from outside System32, which the redistributable does not replace.
	CHECK_FALSE(CheckPrerequisites({{name, "999.0.0.0"}}, error));
	CHECK(error.kind == DriverErrorKind::MISSING_PREREQUISITE);
	CHECK(error.message.find("would use the copy loaded by the program, which is version ") != std::string::npos);
	CHECK(error.message.find("does not replace that copy; update the program") != std::string::npos);
}

TEST_CASE("A runtime missing from System32 is final for the loader", "[windows_driver]") {
	WindowsTempDir temp;
	auto set = MakeSet();
	set.config.prerequisites = {{"redshift-no-such-runtime.dll", "1.0.0.0"}};
	DriverLoader loader(set.config);
	const auto first = loader.Load([&] { return InputsFor(temp.path()); });
	CHECK(first.error.kind == DriverErrorKind::MISSING_PREREQUISITE);
	CHECK(first.error.message.find("System32's copy, which is missing") != std::string::npos);
	CHECK(loader.Load([&] { return InputsFor(temp.path()); }).error.message == first.error.message);
}

TEST_CASE("NO_USABLE_DIRECTORY is not final: a fixed REDSHIFT_DRIVER_DIR loads on the next call", "[windows_driver]") {
	WindowsTempDir temp;
	auto set = MakeSet();
	DriverLoader loader(set.config);
	const auto refused = loader.Load([&] { return InputsFor("C:/redshift"); });
	CHECK(refused.error.kind == DriverErrorKind::NO_USABLE_DIRECTORY);
	CHECK(refused.error.message.find("REDSHIFT_DRIVER_DIR: the path holds a forward slash") != std::string::npos);
	CHECK(loader.Load([&] { return InputsFor(temp.path()); }).ok());
}

TEST_CASE("A failure before the system's loader is called is not final, and the next call loads", "[windows_driver]") {
	WindowsTempDir temp;
	auto set = MakeSet();
	DriverLoader loader(set.config);
	const auto failed = loader.Load([]() -> windows_driver::LoadInputs { throw std::bad_alloc(); });
	CHECK(failed.error.kind == DriverErrorKind::NO_USABLE_DIRECTORY);
	CHECK(failed.error.message == "the Redshift ODBC driver could not be prepared (out of memory or another internal "
	                              "error); try again");
	CHECK(loader.Load([&] { return InputsFor(temp.path()); }).ok());
}

TEST_CASE("FileIdOf fails for a path it cannot open", "[windows_driver]") {
	WindowsTempDir temp;
	windows_rules::FileId id;
	CHECK_FALSE(FileIdOf(temp.path() + "\\absent.dll", id));
}

TEST_CASE("The Windows environment allocation is the driver's plain call", "[windows_driver]") {
	LoadedDriver driver;
	driver.api.AllocHandle = [](SQLSMALLINT type, SQLHANDLE, SQLHANDLE *out) -> SQLRETURN {
		*out = reinterpret_cast<SQLHANDLE>(static_cast<intptr_t>(type));
		return SQL_SUCCESS;
	};
	SQLHENV environment = nullptr;
	CHECK(AllocateEnvironment(driver, &environment) == SQL_SUCCESS);
	CHECK(environment == reinterpret_cast<SQLHENV>(static_cast<intptr_t>(SQL_HANDLE_ENV)));
}

TEST_CASE("A trace value that counts as on is refused before any load, then loads once removed", "[windows_driver]") {
	WindowsTempDir temp;
	auto set = MakeSet();
	DriverLoader loader(set.config);
	g_loader_calls = 0;
	auto inputs = InputsFor(temp.path());
	inputs.before_platform_loader = CountLoaderCall;
	RedirectedMachineHive hive;
	HKEY key = hive.DriverKey();
	SetString(key, L"LogLevel", L"1");
	const auto refused = loader.Load([&] { return inputs; });
	CHECK(refused.error.kind == DriverErrorKind::REFUSED_ENVIRONMENT);
	CHECK(refused.error.message == windows_rules::REGISTRY_TRACE_MESSAGE);
	CHECK(g_loader_calls == 0);
	REQUIRE(RegDeleteValueW(key, L"LogLevel") == ERROR_SUCCESS);
	RegCloseKey(key);
	CHECK(loader.Load([&] { return inputs; }).ok());
	CHECK(g_loader_calls == 1);
}

TEST_CASE("A driver file the system's loader refuses gives a final LOAD_FAILED", "[windows_driver]") {
	WindowsTempDir temp;
	auto set = MakeSet();
	ReplaceBytes(set, 0, {'n', 'o', 't', ' ', 'a', ' ', 'P', 'E'});
	DriverLoader loader(set.config);
	const auto first = loader.Load([&] { return InputsFor(temp.path()); });
	CHECK(first.error.kind == DriverErrorKind::LOAD_FAILED);
	CHECK(first.error.message.find("the system's loader refused a file of the driver set") != std::string::npos);
	CHECK(loader.Load([&] { return InputsFor(temp.path()); }).error.message == first.error.message);
}

TEST_CASE("Embedded bytes that differ from their recorded SHA-256 give EMBEDDED_BYTES_CORRUPT", "[windows_driver]") {
	WindowsTempDir temp;
	auto set = MakeSet();
	set.config.files[1].sha256 = std::string(64, '0');
	DriverLoader loader(set.config);
	const auto result = loader.Load([&] { return InputsFor(temp.path()); });
	CHECK(result.error.kind == DriverErrorKind::EMBEDDED_BYTES_CORRUPT);
	CHECK(result.error.message == set.config.files[1].name +
	                                  " in the extension does not match the hash recorded when it was built; reinstall "
	                                  "the extension");
}

TEST_CASE("A set that lists no file fails its base before any load", "[windows_driver]") {
	WindowsTempDir temp;
	LoaderConfig config;
	config.set_sha256 = detail::Sha256Hex("empty", 5);
	DriverLoader loader(config);
	g_loader_calls = 0;
	auto inputs = InputsFor(temp.path());
	inputs.before_platform_loader = CountLoaderCall;
	const auto result = loader.Load([&] { return inputs; });
	CHECK(result.error.kind == DriverErrorKind::NO_USABLE_DIRECTORY);
	CHECK(std::any_of(result.error.attempts.begin(), result.error.attempts.end(), [](const DriverAttempt &attempt) {
		return attempt.reason.find("the driver set lists no file") != std::string::npos;
	}));
	CHECK(g_loader_calls == 0);
}

TEST_CASE("An account that cannot be identified gives NO_USABLE_DIRECTORY", "[windows_driver]") {
	WindowsTempDir temp;
	auto set = MakeSet();
	DriverLoader loader(set.config);
	auto inputs = InputsFor(temp.path());
	inputs.user = "";
	const auto result = loader.Load([&] { return inputs; });
	CHECK(result.error.kind == DriverErrorKind::NO_USABLE_DIRECTORY);
	CHECK(result.error.message.find("the current account cannot be identified") != std::string::npos);
}

TEST_CASE("The libraries load before the driver, each by its path in the set", "[windows_driver]") {
	WindowsTempDir temp;
	auto set = MakeSet();
	DriverLoader loader(set.config);
	auto inputs = InputsFor(temp.path());
	inputs.before_platform_loader = RecordPaths;
	g_loaded_paths.clear();
	REQUIRE(loader.Load([&] { return inputs; }).ok());
	const auto set_dir = temp.path() + "\\.redshift-driver\\" + set.config.set_sha256;
	CHECK(g_loaded_paths == std::vector<std::string> {set_dir + "\\" + set.config.files[1].name,
	                                                  set_dir + "\\" + set.config.files[0].name});
}

TEST_CASE("With DbgHelp.dll in the run-time names, a copy from another folder is refused before any load",
          "[windows_driver]") {
	WindowsTempDir temp;
	auto set = MakeSet();
	set.config.system_dbghelp = true;
	const HMODULE planted = LoadPlantedDbgHelp(temp.Make("host"));
	REQUIRE(planted != nullptr);
	DriverLoader loader(set.config);
	g_loader_calls = 0;
	auto inputs = InputsFor(temp.path());
	inputs.before_platform_loader = CountLoaderCall;
	const auto refused = loader.Load([&] { return inputs; });
	CHECK(refused.error.kind == DriverErrorKind::REFUSED_ENVIRONMENT);
	CHECK(refused.error.message == windows_rules::DBGHELP_MESSAGE);
	CHECK(g_loader_calls == 0);
	REQUIRE(FreeLibrary(planted));
	CHECK(loader.Load([&] { return inputs; }).ok());
}

TEST_CASE("With DbgHelp.dll in the run-time names, System32's copy is loaded and is the only one", "[windows_driver]") {
	WindowsTempDir temp;
	auto set = MakeSet();
	set.config.system_dbghelp = true;
	DriverLoader loader(set.config);
	REQUIRE(loader.Load([&] { return InputsFor(temp.path()); }).ok());
	const auto modules = DbgHelpModules();
	REQUIRE(modules.size() == 1);
	CHECK(windows_rules::SamePath(modules[0], SystemDbgHelpPath()));
}

TEST_CASE("With DbgHelp.dll in the run-time names, a copy from another folder loaded during the load is final",
          "[windows_driver]") {
	WindowsTempDir temp;
	auto set = MakeSet();
	set.config.system_dbghelp = true;
	DriverLoader loader(set.config);
	g_planted_dir = temp.Make("during");
	g_planted = nullptr;
	auto inputs = InputsFor(temp.path());
	inputs.before_platform_loader = PlantDbgHelp;
	const auto first = loader.Load([&] { return inputs; });
	REQUIRE(g_planted != nullptr);
	CHECK(first.error.kind == DriverErrorKind::LOAD_FAILED);
	CHECK(first.error.message.find("a DbgHelp.dll other than System32's is loaded") != std::string::npos);
	CHECK(loader.Load([&] { return inputs; }).error.message == first.error.message);
	FreeLibrary(g_planted);
}

TEST_CASE("Without DbgHelp.dll in the run-time names, a copy from another folder is not refused", "[windows_driver]") {
	WindowsTempDir temp;
	auto set = MakeSet();
	const HMODULE planted = LoadPlantedDbgHelp(temp.Make("host"));
	REQUIRE(planted != nullptr);
	DriverLoader loader(set.config);
	CHECK(loader.Load([&] { return InputsFor(temp.path()); }).ok());
	FreeLibrary(planted);
}
