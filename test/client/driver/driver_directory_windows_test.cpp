// The Windows directory checks against real folders under the long temporary folder, run by the
// Windows driver suite. Owner and system-folder cases are synthetic in driver_windows_rules_test.cpp.

#include "windows_temp_dir.hpp"

#include <algorithm>
#include <cstdlib>
#include <fstream>
#include <string>
#include <vector>

using namespace duckdb::redshift_driver::windows_driver;
using duckdb::redshift_driver::windows_rules::Narrow;
using duckdb::redshift_driver::windows_rules::SamePath;
using duckdb::redshift_driver::windows_rules::Widen;
using redshift_driver_test::BackupPrivilegeEnabled;
using redshift_driver_test::MakeDirectoryLink;
using redshift_driver_test::MakeJunction;
using redshift_driver_test::ReplaceAccessList;
using redshift_driver_test::SetAccessList;
using redshift_driver_test::WindowsTempDir;

namespace {

const std::vector<std::string> SET_NAMES = {"rsodbc.dll", "NOTICE"};

unsigned RemoteDrive(const wchar_t *) {
	return DRIVE_REMOTE;
}

unsigned UnknownDrive(const wchar_t *) {
	return DRIVE_UNKNOWN;
}

BaseInputs OverrideInputs(const std::string &dir) {
	BaseInputs inputs;
	inputs.override_set = true;
	inputs.override_value = Widen(dir);
	return inputs;
}

std::string BaseRefusal(const std::string &base, const BaseInputs &inputs = BaseInputs()) {
	std::string where, reason;
	return CheckBase(base, false, inputs, CurrentUserSid(), where, reason) ? "" : reason;
}

// HandleRule for the file at `path`, opened as the loader opens a set file.
std::string FileRule(const std::string &path) {
	FileHandle file(CreateFileW(Widen(path).c_str(), GENERIC_READ | READ_CONTROL, FILE_SHARE_READ, nullptr,
	                            OPEN_EXISTING, FILE_FLAG_OPEN_REPARSE_POINT, nullptr));
	REQUIRE(file.valid());
	return HandleRule(file.get(), path, false, CurrentUserSid(), nullptr, nullptr);
}

// Gives the folder or file at `path` one extended attribute, as WSL keeps its file metadata, through
// ntdll's NtSetEaFile, which no Win32 call wraps.
void SetExtendedAttribute(const std::string &path) {
	struct IoStatus {
		void *status;
		ULONG_PTR information;
	} io {};
	using SetEaFile = LONG(NTAPI *)(HANDLE, IoStatus *, void *, ULONG);
	const auto set_ea = reinterpret_cast<SetEaFile>(GetProcAddress(GetModuleHandleW(L"ntdll.dll"), "NtSetEaFile"));
	REQUIRE(set_ea != nullptr);
	// One FILE_FULL_EA_INFORMATION: no next entry or flags, the name's and value's lengths, then the
	// name with its NUL and the value.
	const std::string name = "REDSHIFT", value = "x";
	std::vector<unsigned char> entry(8 + name.size() + 1 + value.size());
	entry[5] = static_cast<unsigned char>(name.size());
	entry[6] = static_cast<unsigned char>(value.size());
	std::copy(name.begin(), name.end(), entry.begin() + 8);
	std::copy(value.begin(), value.end(), entry.begin() + 8 + name.size() + 1);
	FileHandle target(CreateFileW(Widen(path).c_str(), FILE_WRITE_EA, FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr,
	                              OPEN_EXISTING, FILE_FLAG_BACKUP_SEMANTICS, nullptr));
	REQUIRE(target.valid());
	REQUIRE(set_ea(target.get(), &io, entry.data(), static_cast<ULONG>(entry.size())) == 0);
}

} // namespace

TEST_CASE("A test folder under the long temporary folder passes the base check", "[windows_driver]") {
	WindowsTempDir temp;
	CHECK(BaseRefusal(temp.path()).empty());
}

TEST_CASE("A base given by its 8.3 short name is refused with the short-name rule", "[windows_driver]") {
	WindowsTempDir temp;
	const auto dir = temp.Make("redshift-long-folder-name");
	wchar_t short_form[MAX_PATH];
	const DWORD length = GetShortPathNameW(Widen(dir).c_str(), short_form, MAX_PATH);
	REQUIRE(length > 0);
	const auto short_dir = Narrow(std::wstring(short_form, length));
	// A volume whose 8.3 name creation is off (NtfsDisable8dot3NameCreation, as some Windows containers
	// have it; fsutil 8dot3name turns it on) gives the long name back, so the fixture cannot be made
	// there. The case then warns, saying why, and skips, unless RS_REQUIRE_SHORT_NAMES is 1, which
	// fails it instead: a host that turns 8.3 name creation on sets that, so there the rule is checked
	// or the run fails.
	if (short_dir == dir) {
		const char *required = std::getenv("RS_REQUIRE_SHORT_NAMES");
		if (required != nullptr && std::string(required) == "1") {
			FAIL("RS_REQUIRE_SHORT_NAMES=1, but this volume makes no 8.3 short name for a long "
			     "folder name, so the fixture cannot be made");
		}
		// Catch counts a skip but prints no reason for it, so the warning is what the log shows.
		WARN("this volume makes no 8.3 short names (8.3 name creation is off), so a folder with one "
		     "cannot be made and the short-name rule was not tested; set RS_REQUIRE_SHORT_NAMES=1 to "
		     "fail instead");
		SKIP_TEST("this volume makes no 8.3 short names; set RS_REQUIRE_SHORT_NAMES=1 to fail instead");
		return;
	}
	CHECK(BaseRefusal(short_dir) == "the path has an 8.3 short name that differs from its long form");
}

TEST_CASE("A base on a network volume is refused with its own rule", "[windows_driver]") {
	WindowsTempDir temp;
	BaseInputs inputs;
	inputs.drive_type = RemoteDrive;
	CHECK(BaseRefusal(temp.path(), inputs) == "the folder is on a network volume");
	// A base and parent that do not exist: any open before the drive check would refuse it first.
	CHECK(BaseRefusal(temp.path() + "\\missing\\base", inputs) == "the folder is on a network volume");
}

TEST_CASE("A base on a drive whose type cannot be determined is refused as not local", "[windows_driver]") {
	WindowsTempDir temp;
	BaseInputs inputs;
	inputs.drive_type = UnknownDrive;
	CHECK(BaseRefusal(temp.path(), inputs) == "the folder is not on a local fixed, removable or RAM drive");
}

// An OWNER RIGHTS deny entry takes READ_CONTROL from the owner too, so the loader's open fails;
// with SeBackupPrivilege enabled a backup-semantics open reads past it.
TEST_CASE("A base the account cannot open is refused and marked unreadable", "[windows_driver]") {
	if (BackupPrivilegeEnabled()) {
		WARN("SeBackupPrivilege is enabled, so no deny entry can stop the open; run as a standard account");
		return;
	}
	WindowsTempDir temp;
	const auto dir = temp.Make("base");
	SetAccessList(dir, "D:P(D;;RC;;;OW)(A;OICI;FA;;;<user>)");
	CHECK(BaseRefusal(dir) == "the folder cannot be opened: Windows error 5");
	bool unreadable = false;
	CHECK(OwnDirectoryRule(dir, CurrentUserSid(), &unreadable) == "cannot be opened: Windows error 5");
	CHECK(unreadable);
	ReplaceAccessList(dir, "D:P(A;OICI;FA;;;<user>)");
}

TEST_CASE("A parent the account cannot open refuses the base below it", "[windows_driver]") {
	if (BackupPrivilegeEnabled()) {
		WARN("SeBackupPrivilege is enabled, so no deny entry can stop the open; run as a standard account");
		return;
	}
	WindowsTempDir temp;
	const auto parent = temp.Make("parent");
	const auto base = temp.Make("parent\\base");
	SetAccessList(parent, "D:P(D;;RC;;;OW)(A;OICI;FA;;;<user>)");
	CHECK(BaseRefusal(base) == "a parent folder cannot be opened: Windows error 5");
	ReplaceAccessList(parent, "D:P(A;OICI;FA;;;<user>)");
}

TEST_CASE("A junction as the base is refused", "[windows_driver]") {
	WindowsTempDir temp;
	const auto target = temp.Make("target");
	MakeJunction(temp.path() + "\\link", target);
	CHECK(BaseRefusal(temp.path() + "\\link") ==
	      "the folder is a junction, link, mount point or cloud-files placeholder");
}

TEST_CASE("A directory symbolic link as the base is refused", "[windows_driver]") {
	WindowsTempDir temp;
	MakeDirectoryLink(temp.path() + "\\link", temp.Make("target"));
	CHECK(BaseRefusal(temp.path() + "\\link") ==
	      "the folder is a junction, link, mount point or cloud-files placeholder");
}

// The link rule refuses 0x00040000, RECALL_ON_OPEN, which is also winnt.h's FILE_ATTRIBUTE_EA. Here
// an extended attribute leaves that bit clear, so a folder or file holding one is judged as if plain.
TEST_CASE("A folder and a file with an extended attribute are not refused as links", "[windows_driver]") {
	WindowsTempDir temp;
	const auto folder = temp.Make("with-ea");
	SetExtendedAttribute(folder);
	CHECK(BaseRefusal(folder).empty());
	const auto plain = temp.path() + "\\plain", marked = temp.path() + "\\with-ea.dll";
	std::ofstream(plain) << "a set file";
	std::ofstream(marked) << "a set file";
	SetExtendedAttribute(marked);
	CHECK(FileRule(plain).empty());
	CHECK(FileRule(marked).empty());
}

TEST_CASE("A REDSHIFT_DRIVER_DIR that does not exist is refused as a folder that cannot be opened",
          "[windows_driver]") {
	WindowsTempDir temp;
	DriverRoots roots(OverrideInputs(temp.path() + "\\missing"), CurrentUserSid(), SET_NAMES);
	std::string root;
	CHECK_FALSE(roots.Next(root));
	REQUIRE(roots.attempts.size() == 1);
	CHECK(roots.attempts[0].reason.rfind("REDSHIFT_DRIVER_DIR: the folder cannot be opened: Windows error ", 0) == 0);
	CHECK(GetFileAttributesW(Widen(temp.path() + "\\missing").c_str()) == INVALID_FILE_ATTRIBUTES);
}

TEST_CASE("A base that grants Users add-file is refused", "[windows_driver]") {
	WindowsTempDir temp;
	const auto dir = temp.Make("base");
	SetAccessList(dir, "D:P(A;OICI;FA;;;<user>)(A;;0x2;;;BU)");
	CHECK(BaseRefusal(dir) == "the folder lets another account change it through its access control list");
}

TEST_CASE("A parent that grants Users delete-child refuses the base below it", "[windows_driver]") {
	WindowsTempDir temp;
	const auto parent = temp.Make("parent");
	const auto base = temp.Make("parent\\base");
	SetAccessList(parent, "D:P(A;OICI;FA;;;<user>)(A;;0x40;;;BU)");
	CHECK(BaseRefusal(base) ==
	      "a parent folder lets another account replace what is below it through its access control list");
}

TEST_CASE("A grandparent that grants Users delete-child refuses the base two folders below it", "[windows_driver]") {
	WindowsTempDir temp;
	const auto grandparent = temp.Make("grandparent");
	temp.Make("grandparent\\parent");
	const auto base = temp.Make("grandparent\\parent\\base");
	SetAccessList(grandparent, "D:P(A;OICI;FA;;;<user>)(A;;0x40;;;BU)");
	std::string where, reason;
	CHECK_FALSE(CheckBase(base, false, BaseInputs(), CurrentUserSid(), where, reason));
	CHECK(reason == "a parent folder lets another account replace what is below it through its access control list");
	CHECK(where == grandparent);
}

TEST_CASE("A folder that inherits a Users add-file entry, with no protected list, is refused", "[windows_driver]") {
	WindowsTempDir temp;
	const auto parent = temp.Make("parent");
	SetAccessList(parent, "D:P(A;OICI;FA;;;<user>)(A;OICI;0x2;;;BU)");
	// Made with the default access list, so it inherits both of the parent's entries.
	const auto base = parent + "\\base";
	REQUIRE(CreateDirectoryW(Widen(base).c_str(), nullptr));
	CHECK(BaseRefusal(base) == "the folder lets another account change it through its access control list");
}

TEST_CASE("Each folder the loader makes has a protected list that grants only the user", "[windows_driver]") {
	WindowsTempDir temp;
	redshift_driver_test::CheckOwnerOnlyList(temp.Make("made"));
	// The per-user cache, its driver root and its trust directory, each made by the loader.
	BaseInputs inputs;
	inputs.extension_failure = "its file cannot be found: Windows error 126";
	inputs.per_user_base = Widen(temp.path() + "\\cache");
	DriverRoots roots(inputs, CurrentUserSid(), SET_NAMES);
	std::string root;
	REQUIRE(roots.Next(root));
	REQUIRE(root == temp.path() + "\\cache\\.redshift-driver");
	const auto trust = ChooseTrustDirectory(temp.path() + "\\cache", inputs, CurrentUserSid());
	REQUIRE(trust == temp.path() + "\\cache\\trust");
	for (const auto &dir : {temp.path() + "\\cache", root, trust}) {
		redshift_driver_test::CheckOwnerOnlyList(dir);
	}
}

TEST_CASE("REDSHIFT_DRIVER_DIR is the only base, and its driver root is made there", "[windows_driver]") {
	WindowsTempDir temp;
	auto inputs = OverrideInputs(temp.path());
	inputs.per_user_base = Widen(temp.path() + "\\cache");
	DriverRoots roots(inputs, CurrentUserSid(), SET_NAMES);
	std::string root;
	REQUIRE(roots.Next(root));
	CHECK(root == temp.path() + "\\.redshift-driver");
	CHECK(BaseRefusal(root).empty());
	CHECK_FALSE(roots.Next(root));
}

TEST_CASE("A REDSHIFT_DRIVER_DIR with a forward slash is refused with no fallback", "[windows_driver]") {
	WindowsTempDir temp;
	auto inputs = OverrideInputs("C:/redshift");
	inputs.per_user_base = Widen(temp.path() + "\\cache");
	DriverRoots roots(inputs, CurrentUserSid(), SET_NAMES);
	std::string root;
	CHECK_FALSE(roots.Next(root));
	REQUIRE(roots.attempts.size() == 1);
	CHECK(roots.attempts[0].reason == "REDSHIFT_DRIVER_DIR: the path holds a forward slash");
}

TEST_CASE("A REDSHIFT_DRIVER_DIR that cannot be read is refused", "[windows_driver]") {
	auto inputs = OverrideInputs("");
	inputs.override_failure = "it cannot be read: Windows error 8";
	DriverRoots roots(inputs, CurrentUserSid(), SET_NAMES);
	std::string root;
	CHECK_FALSE(roots.Next(root));
	REQUIRE(roots.attempts.size() == 1);
	CHECK(roots.attempts[0].reason == "REDSHIFT_DRIVER_DIR: it cannot be read: Windows error 8");
}

TEST_CASE("With no REDSHIFT_DRIVER_DIR, the extension's folder is the base", "[windows_driver]") {
	WindowsTempDir temp;
	BaseInputs inputs;
	inputs.extension_file = Widen(temp.Make("base") + "\\redshift.duckdb_extension");
	inputs.per_user_base = Widen(temp.path() + "\\cache");
	DriverRoots roots(inputs, CurrentUserSid(), SET_NAMES);
	std::string root;
	REQUIRE(roots.Next(root));
	CHECK(std::string(roots.Role()) == "the extension's directory");
	CHECK(root == temp.path() + "\\base\\.redshift-driver");
}

TEST_CASE("An extension path over 259 characters refuses its base and the cache is used", "[windows_driver]") {
	WindowsTempDir temp;
	BaseInputs inputs;
	inputs.extension_file = Widen("C:\\" + std::string(300, 'x') + "\\redshift.duckdb_extension");
	inputs.per_user_base = Widen(temp.path() + "\\cache");
	DriverRoots roots(inputs, CurrentUserSid(), SET_NAMES);
	std::string root;
	REQUIRE(roots.Next(root));
	CHECK(std::string(roots.Role()) == "the per-user cache");
	CHECK(root == temp.path() + "\\cache\\.redshift-driver");
	REQUIRE(roots.attempts.size() == 1);
	CHECK(roots.attempts[0].reason == "the extension's directory: the extension's path is longer than 259 characters");
}

TEST_CASE("A base whose longest path would pass 259 characters is refused for its length", "[windows_driver]") {
	WindowsTempDir temp;
	// 93 more characters for <base>\.redshift-driver\<64 hex>\rsodbc.dll makes 259 at a 166-character base.
	const size_t pad = 166 - temp.path().size() - 1;
	REQUIRE(temp.path().size() + 1 < 166);
	const auto fits = temp.Make(std::string(pad, 'a'));
	const auto over = temp.Make(std::string(pad + 1, 'b'));
	DriverRoots fitting(OverrideInputs(fits), CurrentUserSid(), {"rsodbc.dll"});
	std::string root;
	CHECK(fitting.Next(root));
	DriverRoots too_long(OverrideInputs(over), CurrentUserSid(), {"rsodbc.dll"});
	CHECK_FALSE(too_long.Next(root));
	REQUIRE(too_long.attempts.size() == 1);
	CHECK(too_long.attempts[0].reason ==
	      "REDSHIFT_DRIVER_DIR: a path the driver would use there is longer than 259 characters");
}

TEST_CASE("With no known folder and the extension refused, NO_USABLE_DIRECTORY names roles alone", "[windows_driver]") {
	BaseInputs inputs;
	inputs.extension_failure = "its file cannot be found: Windows error 126";
	DriverRoots roots(inputs, CurrentUserSid(), SET_NAMES);
	std::string root;
	CHECK_FALSE(roots.Next(root));
	REQUIRE(roots.attempts.size() == 2);
	CHECK(roots.attempts[1].reason == "the per-user cache: there is no known folder for local application data");
	const auto message = duckdb::redshift_driver::windows_rules::NoUsableDirectoryMessage(roots.attempts);
	CHECK(message.find("Set REDSHIFT_DRIVER_DIR") != std::string::npos);
	CHECK(message.find(":\\") == std::string::npos);
}

TEST_CASE("CurrentBaseInputs reads REDSHIFT_DRIVER_DIR from the environment block on each call", "[windows_driver]") {
	REQUIRE(SetEnvironmentVariableW(L"REDSHIFT_DRIVER_DIR", L"C:\\set-after-load"));
	auto inputs = CurrentBaseInputs();
	CHECK(inputs.override_set);
	CHECK(inputs.override_value == L"C:\\set-after-load");
	REQUIRE(SetEnvironmentVariableW(L"REDSHIFT_DRIVER_DIR", L""));
	inputs = CurrentBaseInputs();
	CHECK(inputs.override_set);
	CHECK(inputs.override_value.empty());
	REQUIRE(SetEnvironmentVariableW(L"REDSHIFT_DRIVER_DIR", nullptr));
	inputs = CurrentBaseInputs();
	CHECK_FALSE(inputs.override_set);
	CHECK(inputs.extension_failure.empty());
	// The suite links the loader statically, so the file that holds CurrentBaseInputs is the program.
	std::vector<wchar_t> program(32768);
	const DWORD length = GetModuleFileNameW(nullptr, program.data(), 32768);
	REQUIRE(length > 0);
	REQUIRE(length < 32768);
	CHECK(SamePath(Narrow(inputs.extension_file), Narrow(std::wstring(program.data(), length))));
	const std::wstring cache_name = L"\\duckdb-redshift";
	REQUIRE(inputs.per_user_base.size() > cache_name.size());
	CHECK(inputs.per_user_base.compare(inputs.per_user_base.size() - cache_name.size(), cache_name.size(),
	                                   cache_name) == 0);
}

TEST_CASE("The trust directory is the base's own when the base can hold it", "[windows_driver]") {
	WindowsTempDir temp;
	BaseInputs inputs;
	inputs.per_user_base = Widen(temp.path() + "\\cache");
	const auto first = ChooseTrustDirectory(temp.path(), inputs, CurrentUserSid());
	CHECK(first == temp.path() + "\\trust");
	CHECK(ChooseTrustDirectory(temp.path(), inputs, CurrentUserSid()) == first);
}

TEST_CASE("A trust folder that is a junction moves the trust directory to the per-user cache", "[windows_driver]") {
	WindowsTempDir temp;
	MakeJunction(temp.path() + "\\trust", temp.Make("elsewhere"));
	BaseInputs inputs;
	inputs.per_user_base = Widen(temp.path() + "\\cache");
	CHECK(ChooseTrustDirectory(temp.path(), inputs, CurrentUserSid()) == temp.path() + "\\cache\\trust");
}

TEST_CASE("A trust folder that grants Users add-file moves the trust directory to the per-user cache",
          "[windows_driver]") {
	WindowsTempDir temp;
	SetAccessList(temp.Make("trust"), "D:P(A;OICI;FA;;;<user>)(A;;0x2;;;BU)");
	BaseInputs inputs;
	inputs.per_user_base = Widen(temp.path() + "\\cache");
	CHECK(ChooseTrustDirectory(temp.path(), inputs, CurrentUserSid()) == temp.path() + "\\cache\\trust");
}

TEST_CASE("A base whose path holds a brace puts the trust directory in the per-user cache", "[windows_driver]") {
	WindowsTempDir temp;
	const auto base = temp.Make("a}b");
	BaseInputs inputs;
	inputs.per_user_base = Widen(temp.path() + "\\cache");
	CHECK(ChooseTrustDirectory(base, inputs, CurrentUserSid()) == temp.path() + "\\cache\\trust");
}

TEST_CASE("No trust directory is named when neither the base nor the cache can hold one", "[windows_driver]") {
	WindowsTempDir temp;
	const auto base = temp.Make("a}b");
	CHECK(ChooseTrustDirectory(base, BaseInputs(), CurrentUserSid()).empty());
}

TEST_CASE("A base below a junction in a parent folder is refused by its final path", "[windows_driver]") {
	WindowsTempDir temp;
	temp.Make("real");
	temp.Make("real\\base");
	MakeJunction(temp.path() + "\\link", temp.path() + "\\real");
	CHECK(BaseRefusal(temp.path() + "\\link\\base") ==
	      "the folder resolves to another path, through a link or a renamed folder");
}

TEST_CASE("A driver root made beforehand as a junction refuses its base", "[windows_driver]") {
	WindowsTempDir temp;
	MakeJunction(temp.path() + "\\.redshift-driver", temp.Make("elsewhere"));
	DriverRoots roots(OverrideInputs(temp.path()), CurrentUserSid(), SET_NAMES);
	std::string root;
	CHECK_FALSE(roots.Next(root));
	REQUIRE(roots.attempts.size() == 1);
	CHECK(roots.attempts[0].reason ==
	      "REDSHIFT_DRIVER_DIR: the driver root is a junction, link, mount point or cloud-files placeholder");
}

TEST_CASE("A driver root made beforehand that grants Users add-file refuses its base", "[windows_driver]") {
	WindowsTempDir temp;
	SetAccessList(temp.Make(".redshift-driver"), "D:P(A;OICI;FA;;;<user>)(A;;0x2;;;BU)");
	DriverRoots roots(OverrideInputs(temp.path()), CurrentUserSid(), SET_NAMES);
	std::string root;
	CHECK_FALSE(roots.Next(root));
	REQUIRE(roots.attempts.size() == 1);
	CHECK(roots.attempts[0].reason ==
	      "REDSHIFT_DRIVER_DIR: the driver root lets another account change it through its access control list");
}

TEST_CASE("A REDSHIFT_DRIVER_DIR that names a file is refused as not a folder", "[windows_driver]") {
	WindowsTempDir temp;
	const auto file = temp.path() + "\\a-file";
	const HANDLE created =
	    CreateFileW(Widen(file).c_str(), GENERIC_WRITE, 0, nullptr, CREATE_NEW, FILE_ATTRIBUTE_NORMAL, nullptr);
	REQUIRE(created != INVALID_HANDLE_VALUE);
	CloseHandle(created);
	DriverRoots roots(OverrideInputs(file), CurrentUserSid(), SET_NAMES);
	std::string root;
	CHECK_FALSE(roots.Next(root));
	REQUIRE(roots.attempts.size() == 1);
	CHECK(roots.attempts[0].reason == "REDSHIFT_DRIVER_DIR: the folder is a file, not a folder");
}

TEST_CASE("A folder the loader makes is owned by the current account", "[windows_driver]") {
	WindowsTempDir temp;
	const auto dir = temp.Make("owned");
	PSID owner = nullptr;
	PSECURITY_DESCRIPTOR descriptor = nullptr;
	REQUIRE(GetNamedSecurityInfoA(dir.c_str(), SE_FILE_OBJECT, OWNER_SECURITY_INFORMATION, &owner, nullptr, nullptr,
	                              nullptr, &descriptor) == ERROR_SUCCESS);
	LPSTR sid = nullptr;
	REQUIRE(ConvertSidToStringSidA(owner, &sid));
	CHECK(std::string(sid) == CurrentUserSid());
	LocalFree(sid);
	LocalFree(descriptor);
}

TEST_CASE("A per-user trust file path of 255 bytes is used, and one of 256 leaves no trust directory",
          "[windows_driver]") {
	WindowsTempDir temp;
	const auto base = temp.Make("a}b");
	// <cache>\trust\<64 hex>.pem is 75 bytes longer than the cache's path.
	const auto cache_of = [&](size_t length, char fill) {
		const auto folder = temp.path() + "\\";
		REQUIRE(folder.size() < length);
		return folder + std::string(length - folder.size(), fill);
	};
	BaseInputs inputs;
	inputs.per_user_base = Widen(cache_of(180, 'c'));
	CHECK(ChooseTrustDirectory(base, inputs, CurrentUserSid()) == cache_of(180, 'c') + "\\trust");
	inputs.per_user_base = Widen(cache_of(181, 'd'));
	CHECK(ChooseTrustDirectory(base, inputs, CurrentUserSid()).empty());
}

TEST_CASE("An ASCII base keeps the trust directory when the profile's path is not ASCII", "[windows_driver]") {
	WindowsTempDir temp;
	BaseInputs inputs;
	inputs.per_user_base = Widen(temp.path()) + L"\\J\u00fcrgen\\duckdb-redshift";
	CHECK(ChooseTrustDirectory(temp.path(), inputs, CurrentUserSid()) == temp.path() + "\\trust");
	CHECK(ChooseTrustDirectory(temp.Make("a}b"), inputs, CurrentUserSid()).empty());
}
