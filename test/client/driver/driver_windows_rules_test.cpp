// The Windows loader's rules that need no Windows call, run on every platform's driver suite.

#include "catch.hpp"
#include "driver_windows_rules.hpp"

#include <string>
#include <vector>

using namespace duckdb::redshift_driver::windows_rules;

namespace {

const std::string USER = "S-1-5-21-1-2-3-1001";
const std::string USERS = "S-1-5-32-545";

SecurityFacts OwnedBy(const std::string &owner, std::vector<AccessEntry> entries = {}) {
	SecurityFacts facts;
	facts.owner = owner;
	facts.entries = std::move(entries);
	return facts;
}

AccessEntry Allow(const std::string &sid, uint32_t mask) {
	AccessEntry entry;
	entry.sid = sid;
	entry.mask = mask;
	return entry;
}

} // namespace

TEST_CASE("Windows path rules accept a canonical path, a lower-case drive and a drive root", "[windows_rules]") {
	for (const auto *path : {L"C:\\a\\b", L"c:\\Users\\runner", L"C:\\", L"C:\\console", L"C:\\com10", L"C:\\coma",
	                         L"C:\\com-", L"C:\\lpt", L"C:\\a~1b"}) {
		INFO(Narrow(path));
		CHECK(PathFormRule(path).empty());
	}
}

TEST_CASE("Windows path rules refuse a path that is not ASCII", "[windows_rules]") {
	CHECK(PathFormRule(L"C:\\caf\u00e9") == "is not ASCII, which the driver cannot use");
}

TEST_CASE("Windows path rules refuse a character no file name can hold", "[windows_rules]") {
	for (const auto *path : {L"C:\\a\tb", L"C:\\a<b", L"C:\\a>b", L"C:\\a\"b", L"C:\\a|b", L"C:\\a?b", L"C:\\a*b"}) {
		INFO(Narrow(path));
		CHECK(PathFormRule(path) == "holds a character a Windows file name cannot hold");
	}
}

TEST_CASE("Windows path rules refuse a forward slash", "[windows_rules]") {
	CHECK(PathFormRule(L"C:/a") == "holds a forward slash");
}

TEST_CASE("Windows path rules refuse a device or Win32-namespace path", "[windows_rules]") {
	CHECK(PathFormRule(L"\\\\?\\C:\\a") == "is a device or Win32-namespace path");
	CHECK(PathFormRule(L"\\\\.\\C:\\a") == "is a device or Win32-namespace path");
}

TEST_CASE("Windows path rules refuse a UNC path", "[windows_rules]") {
	CHECK(PathFormRule(L"\\\\server\\share") == "is a UNC path");
}

TEST_CASE("Windows path rules refuse a path not rooted at a drive letter", "[windows_rules]") {
	for (const auto *path : {L"", L"\\foo", L"1:\\a", L"@:\\a", L"{:\\a", L"C", L"Cx\\a"}) {
		INFO(Narrow(path));
		CHECK(PathFormRule(path) == "is not rooted at a drive letter");
	}
}

TEST_CASE("Windows path rules refuse a drive-relative path", "[windows_rules]") {
	CHECK(PathFormRule(L"C:foo") == "is relative to a drive's current folder");
	CHECK(PathFormRule(L"C:") == "is relative to a drive's current folder");
}

TEST_CASE("Windows path rules refuse an alternate data stream", "[windows_rules]") {
	CHECK(PathFormRule(L"C:\\a:b") == "names an alternate data stream");
}

TEST_CASE("Windows path rules refuse a trailing separator", "[windows_rules]") {
	CHECK(PathFormRule(L"C:\\a\\") == "ends with a separator");
}

TEST_CASE("Windows path rules refuse an empty component", "[windows_rules]") {
	CHECK(PathFormRule(L"C:\\a\\\\b") == "has an empty component");
}

TEST_CASE("Windows path rules refuse a dot component", "[windows_rules]") {
	CHECK(PathFormRule(L"C:\\a\\.\\b") == "has a . or .. component");
	CHECK(PathFormRule(L"C:\\a\\..") == "has a . or .. component");
}

TEST_CASE("Windows path rules refuse a component that ends with a dot or a space", "[windows_rules]") {
	CHECK(PathFormRule(L"C:\\a.") == "has a component that ends with a dot or a space");
	CHECK(PathFormRule(L"C:\\a \\b") == "has a component that ends with a dot or a space");
}

TEST_CASE("Windows path rules refuse a device-name component, ignoring case and extension", "[windows_rules]") {
	for (const auto *path : {L"C:\\con", L"C:\\a\\Aux.x", L"C:\\PRN", L"C:\\nul.txt", L"C:\\com0", L"C:\\COM9",
	                         L"C:\\lpt0", L"C:\\LPT9.log", L"C:\\conin$", L"C:\\CONOUT$", L"C:\\con.a.b"}) {
		INFO(Narrow(path));
		CHECK(PathFormRule(path) == "has a component that is a device name");
	}
}

TEST_CASE("Narrow marks characters outside ASCII", "[windows_rules]") {
	CHECK(Narrow(L"C:\\a") == "C:\\a");
	CHECK(Narrow(L"C:\\\u00e9") == "C:\\?");
	CHECK(Widen("C:\\a") == L"C:\\a");
}

TEST_CASE("The longest path under a base is a set file's in the set directory", "[windows_rules]") {
	// <base>\.redshift-driver\<64 hex>\<name>: 4 + 1 + 16 + 1 + 64 + 1 + 10.
	CHECK(LongestPathUnder("C:\\a", {"NOTICE", "rsodbc.dll"}) == 97);
	CHECK(LongestPathUnder("C:\\a", {"rsodbc.dll", "NOTICE"}) == 97);
	// A base 166 characters long reaches 259 with that set, the longest path accepted.
	CHECK(LongestPathUnder("C:\\" + std::string(163, 'x'), {"rsodbc.dll"}) == MAX_PATH_CHARACTERS);
	// A drive root already ends in its separator: 3 + 16 + 1 + 64 + 1 + 10.
	CHECK(LongestPathUnder("D:\\", {"rsodbc.dll"}) == 95);
}

TEST_CASE("A name joins a folder with one separator, also below a drive root", "[windows_rules]") {
	CHECK(JoinPath("D:\\", ".redshift-driver") == "D:\\.redshift-driver");
	CHECK(JoinPath("D:\\base", "trust") == "D:\\base\\trust");
	// An empty folder, as an unknown per-user base gives, has no last character to look at.
	CHECK(JoinPath("", "x") == "\\x");
}

TEST_CASE("A system file's path is empty when the system folder is unknown", "[windows_rules]") {
	CHECK(SystemFilePath("C:\\Windows\\System32", "a.dll") == "C:\\Windows\\System32\\a.dll");
	// Never "\\a.dll", a file at the current drive's root.
	CHECK(SystemFilePath("", "a.dll").empty());
}

TEST_CASE("Windows paths compare ignoring ASCII case", "[windows_rules]") {
	CHECK(SamePath("c:\\Users\\A", "C:\\users\\a"));
	CHECK_FALSE(SamePath("C:\\a", "C:\\ab"));
	CHECK_FALSE(SamePath("C:\\a", "C:\\b"));
}

TEST_CASE("The drive rule accepts only a fixed, removable or RAM drive", "[windows_rules]") {
	for (const auto type : {DRIVE_KIND_FIXED, DRIVE_KIND_REMOVABLE, DRIVE_KIND_RAMDISK}) {
		INFO(type);
		CHECK(DriveTypeRule(type).empty());
	}
	CHECK(DriveTypeRule(DRIVE_KIND_REMOTE) == "the folder is on a network volume");
	// A type that cannot be determined is no evidence the drive is local, so it is refused too.
	for (const auto type : {DRIVE_KIND_UNKNOWN, DRIVE_KIND_NO_ROOT_DIR, DRIVE_KIND_CDROM, 7u}) {
		INFO(type);
		CHECK(DriveTypeRule(type) == "the folder is not on a local fixed, removable or RAM drive");
	}
}

TEST_CASE("The owner rule accepts the user, SYSTEM and Administrators", "[windows_rules]") {
	for (const auto &owner : {USER, std::string(SYSTEM_SID), std::string(ADMINISTRATORS_SID)}) {
		CHECK(SecurityRule(OwnedBy(owner), PathPlace::OWN, USER).empty());
		CHECK(SecurityRule(OwnedBy(owner), PathPlace::ANCESTOR, USER).empty());
	}
}

TEST_CASE("The owner rule accepts TrustedInstaller only above the base", "[windows_rules]") {
	CHECK(SecurityRule(OwnedBy(TRUSTED_INSTALLER_SID), PathPlace::ANCESTOR, USER).empty());
	CHECK(SecurityRule(OwnedBy(TRUSTED_INSTALLER_SID), PathPlace::OWN, USER) ==
	      "is owned by an account other than you, SYSTEM or Administrators");
}

TEST_CASE("The owner rule refuses another standard user, above the base too", "[windows_rules]") {
	CHECK(SecurityRule(OwnedBy("S-1-5-21-1-2-3-1002"), PathPlace::ANCESTOR, USER) ==
	      "is owned by an account other than you, SYSTEM or Administrators");
}

TEST_CASE("The access rule refuses a NULL DACL", "[windows_rules]") {
	auto facts = OwnedBy(USER);
	facts.has_dacl = false;
	CHECK(SecurityRule(facts, PathPlace::ANCESTOR, USER) ==
	      "has a NULL access control list, which lets every account change it");
}

TEST_CASE("The access rule refuses an entry of a type it does not read", "[windows_rules]") {
	auto facts = OwnedBy(USER);
	facts.unknown_entry = true;
	CHECK(SecurityRule(facts, PathPlace::ANCESTOR, USER) ==
	      "has an access control entry of a type the loader does not read");
}

TEST_CASE("The access rule refuses each write right to another account at the base", "[windows_rules]") {
	for (uint32_t right : {RIGHT_WRITE_DATA, RIGHT_APPEND_DATA, RIGHT_DELETE, RIGHT_DELETE_CHILD, RIGHT_WRITE_DAC,
	                       RIGHT_WRITE_OWNER, RIGHT_GENERIC_ALL, RIGHT_GENERIC_WRITE}) {
		INFO(right);
		CHECK(SecurityRule(OwnedBy(USER, {Allow(USERS, right)}), PathPlace::OWN, USER) ==
		      "lets another account change it through its access control list");
	}
}

TEST_CASE("The ancestor rule refuses the rights that could replace the next folder", "[windows_rules]") {
	for (uint32_t right : {RIGHT_DELETE, RIGHT_DELETE_CHILD, RIGHT_WRITE_DAC, RIGHT_WRITE_OWNER, RIGHT_GENERIC_ALL}) {
		INFO(right);
		CHECK(SecurityRule(OwnedBy(SYSTEM_SID, {Allow(USERS, right)}), PathPlace::ANCESTOR, USER) ==
		      "lets another account replace what is below it through its access control list");
	}
}

TEST_CASE("The ancestor rule accepts C:\\'s add-file and add-folder rights for Authenticated Users",
          "[windows_rules]") {
	const auto facts = OwnedBy(TRUSTED_INSTALLER_SID, {Allow("S-1-5-11", RIGHT_APPEND_DATA | RIGHT_WRITE_DATA)});
	CHECK(SecurityRule(facts, PathPlace::ANCESTOR, USER).empty());
	CHECK(SecurityRule(OwnedBy(USER, facts.entries), PathPlace::OWN, USER) ==
	      "lets another account change it through its access control list");
}

TEST_CASE("The ancestor rule accepts generic write, which maps to adding files or folders", "[windows_rules]") {
	CHECK(SecurityRule(OwnedBy(USER, {Allow(USERS, RIGHT_GENERIC_WRITE)}), PathPlace::ANCESTOR, USER).empty());
}

TEST_CASE("The access rule accepts full control for the user, SYSTEM and Administrators", "[windows_rules]") {
	const auto facts = OwnedBy(USER, {Allow(USER, RIGHT_GENERIC_ALL), Allow(SYSTEM_SID, RIGHT_GENERIC_ALL),
	                                  Allow(ADMINISTRATORS_SID, RIGHT_GENERIC_ALL)});
	CHECK(SecurityRule(facts, PathPlace::OWN, USER).empty());
}

TEST_CASE("The access rule accepts TrustedInstaller's full control only above the base", "[windows_rules]") {
	// A stand-in for C:\Program Files with its stock list, then the same grant on the base.
	const auto facts = OwnedBy(TRUSTED_INSTALLER_SID, {Allow(TRUSTED_INSTALLER_SID, RIGHT_GENERIC_ALL)});
	CHECK(SecurityRule(facts, PathPlace::ANCESTOR, USER).empty());
	CHECK(SecurityRule(OwnedBy(USER, facts.entries), PathPlace::OWN, USER) ==
	      "lets another account change it through its access control list");
}

TEST_CASE("The access rule takes no deny entry as a grant", "[windows_rules]") {
	auto deny = Allow(USERS, RIGHT_GENERIC_ALL);
	deny.allow = false;
	CHECK(SecurityRule(OwnedBy(USER, {deny}), PathPlace::OWN, USER).empty());
}

TEST_CASE("The access rule lets no deny entry offset a grant", "[windows_rules]") {
	auto deny = Allow(USERS, RIGHT_WRITE_DAC);
	deny.allow = false;
	CHECK(SecurityRule(OwnedBy(USER, {deny, Allow(USERS, RIGHT_WRITE_DAC)}), PathPlace::OWN, USER) ==
	      "lets another account change it through its access control list");
}

TEST_CASE("The access rule skips an inherit-only entry, which grants nothing on the folder", "[windows_rules]") {
	auto entry = Allow(USERS, RIGHT_GENERIC_ALL);
	entry.inherit_only = true;
	CHECK(SecurityRule(OwnedBy(USER, {entry}), PathPlace::OWN, USER).empty());
}

TEST_CASE("The access rule reads a CREATOR OWNER entry as the owner's", "[windows_rules]") {
	CHECK(SecurityRule(OwnedBy(USER, {Allow(CREATOR_OWNER_SID, RIGHT_GENERIC_ALL)}), PathPlace::OWN, USER).empty());
}

TEST_CASE("An entry whose SID could not be read is never the user's", "[windows_rules]") {
	// The account's SID is empty when its token cannot be read, as is an entry's that fails to convert.
	CHECK(SecurityRule(OwnedBy(SYSTEM_SID, {Allow("", RIGHT_WRITE_DAC)}), PathPlace::OWN, "") ==
	      "lets another account change it through its access control list");
	CHECK(SecurityRule(OwnedBy(""), PathPlace::OWN, "") ==
	      "is owned by an account other than you, SYSTEM or Administrators");
}

TEST_CASE("A trust folder another account owns is refused, though its list grants only that account",
          "[windows_rules]") {
	const std::string other = "S-1-5-21-1-2-3-1002";
	CHECK(SecurityRule(OwnedBy(other, {Allow(other, RIGHT_GENERIC_ALL)}), PathPlace::OWN, USER) ==
	      "is owned by an account other than you, SYSTEM or Administrators");
}

TEST_CASE("A cloud-files placeholder's attributes are refused as a link's are", "[windows_rules]") {
	const std::string link = "is a junction, link, mount point or cloud-files placeholder";
	for (uint32_t attribute : {ATTRIBUTE_REPARSE_POINT, ATTRIBUTE_OFFLINE, ATTRIBUTE_RECALL_ON_OPEN, ATTRIBUTE_PINNED,
	                           ATTRIBUTE_UNPINNED, ATTRIBUTE_RECALL_ON_DATA_ACCESS}) {
		INFO(attribute);
		CHECK(AttributeRule(ATTRIBUTE_DIRECTORY | attribute, true) == link);
		CHECK(AttributeRule(attribute, false) == link);
	}
	// A plain folder and file: read-only, hidden, system, archive and not content-indexed.
	CHECK(AttributeRule(ATTRIBUTE_DIRECTORY | 0x1 | 0x2 | 0x4 | 0x20 | 0x2000, true).empty());
	CHECK(AttributeRule(0x1 | 0x2 | 0x4 | 0x20 | 0x2000, false).empty());
}

TEST_CASE("A file where a folder must be, and a folder where a file must be, are refused", "[windows_rules]") {
	CHECK(AttributeRule(0x20, true) == "is a file, not a folder");
	CHECK(AttributeRule(ATTRIBUTE_DIRECTORY, false) == "is a folder, not a file");
}

namespace {

// A folder of the administrator tree, as icacls leaves it: owner Administrators, full
// control for Administrators, read and execute for Users, and `extra` when a step was missed.
SecurityFacts AdministratorTree(std::vector<AccessEntry> extra = {}) {
	constexpr uint32_t FULL_CONTROL = 0x001F01FF;
	constexpr uint32_t READ_AND_EXECUTE = 0x001200A9;
	auto facts = OwnedBy(ADMINISTRATORS_SID, {Allow(ADMINISTRATORS_SID, FULL_CONTROL), Allow(USERS, READ_AND_EXECUTE)});
	facts.entries.insert(facts.entries.end(), extra.begin(), extra.end());
	return facts;
}

} // namespace

TEST_CASE("The administrator tree is accepted for a standard user", "[windows_rules]") {
	CHECK(SecurityRule(AdministratorTree(), PathPlace::OWN, USER).empty());
}

TEST_CASE("The administrator tree is refused with the administrator's own entry, or a Users create-files entry",
          "[windows_rules]") {
	const auto administrator = Allow("S-1-5-21-1-2-3-500", 0x001F01FF);
	CHECK(SecurityRule(AdministratorTree({administrator}), PathPlace::OWN, USER) ==
	      "lets another account change it through its access control list");
	// C:\ProgramData's Users entry, still inherited by a base whose inheritance was not removed.
	CHECK(SecurityRule(AdministratorTree({Allow(USERS, RIGHT_WRITE_DATA | RIGHT_APPEND_DATA)}), PathPlace::OWN, USER) ==
	      "lets another account change it through its access control list");
}

TEST_CASE("The trust directory rule refuses a closing brace", "[windows_rules]") {
	CHECK(TrustDirectoryRule("C:\\a}b\\trust") == "holds a }, which the trust file's path cannot hold");
}

TEST_CASE("The trust directory rule refuses DEL, which the ODBC client's trust path check refuses", "[windows_rules]") {
	CHECK(TrustDirectoryRule("C:\\a\x7f"
	                         "b\\trust") == "holds a DEL character, which the trust file's path cannot hold");
}

TEST_CASE("The trust directory rule keeps the trust file's path within 255 bytes", "[windows_rules]") {
	// <dir>\<64 hex>.pem adds 69 bytes to the directory's path.
	CHECK(TrustDirectoryRule("C:\\" + std::string(183, 'x')).empty());
	CHECK(TrustDirectoryRule("C:\\" + std::string(184, 'x')) == "makes the trust file's path longer than 255 bytes");
}

TEST_CASE("NO_USABLE_DIRECTORY's message ends with the REDSHIFT_DRIVER_DIR remedy", "[windows_rules]") {
	const auto message = NoUsableDirectoryMessage({{"C:\\secret", "first"}, {"D:\\secret", "second"}});
	CHECK(message.find("(first; second). Set REDSHIFT_DRIVER_DIR to ") != std::string::npos);
	CHECK(message.find("secret") == std::string::npos);
	CHECK(NoUsableDirectoryMessage({}).find("driver in. Set REDSHIFT_DRIVER_DIR") != std::string::npos);
	// The remedy names each property of a path the loader refuses, as the README's Windows limits list them.
	CHECK(NoUsableDirectoryMessage({}) ==
	      "no usable directory to install the Redshift ODBC driver in. Set REDSHIFT_DRIVER_DIR to an existing folder "
	      "on a local drive, owned by you, SYSTEM or Administrators and writable by no one else, whose path is ASCII, "
	      "in its long form (not an 8.3 short name) and short enough, with no }, no junction or link and no "
	      "component that ends in a dot or a space");
}

namespace {

LoadedModule Module(const std::string &path, uint64_t index) {
	LoadedModule module;
	module.path = path;
	module.identified = true;
	module.id.volume = 7;
	module.id.index = index;
	return module;
}

CheckedFile Checked(const std::string &path, uint64_t index) {
	CheckedFile file;
	file.path = path;
	file.id.volume = 7;
	file.id.index = index;
	return file;
}

std::string Utf16(const std::u16string &text) {
	std::string bytes;
	for (char16_t c : text) {
		bytes.push_back(static_cast<char>(c & 0xFF));
		bytes.push_back(static_cast<char>(c >> 8));
	}
	return bytes;
}

} // namespace

TEST_CASE("The post-load check passes one module per set name that is the checked file", "[windows_rules]") {
	const std::vector<LoadedModule> modules = {Module("C:\\Windows\\System32\\kernel32.dll", 1),
	                                           Module("C:\\host\\crypto.dll", 2), Module("C:\\b\\set\\ab.dll", 3)};
	CHECK(PostLoadRule(modules, {Checked("c:\\B\\set\\AB.dll", 3)}).empty());
}

TEST_CASE("The post-load check refuses a set name with no module", "[windows_rules]") {
	CHECK(PostLoadRule({Module("C:\\host\\crypto.dll", 2)}, {Checked("C:\\b\\set\\ab.dll", 3)}) ==
	      "a driver file is not among the process's modules after its load");
}

TEST_CASE("The post-load check refuses a second module of a set name from another folder", "[windows_rules]") {
	CHECK(PostLoadRule({Module("C:\\b\\set\\ab.dll", 3), Module("C:\\other\\AB.DLL", 4)},
	                   {Checked("C:\\b\\set\\ab.dll", 3)}) ==
	      "more than one module answers to the name of a driver file");
}

TEST_CASE("The post-load check refuses a module that is not the checked file", "[windows_rules]") {
	const std::string rule = "the module that answers to a driver file's name is not the file that was checked";
	CHECK(PostLoadRule({Module("C:\\other\\ab.dll", 3)}, {Checked("C:\\b\\set\\ab.dll", 3)}) == rule);
	CHECK(PostLoadRule({Module("C:\\b\\set\\ab.dll", 4)}, {Checked("C:\\b\\set\\ab.dll", 3)}) == rule);
	auto other_volume = Module("C:\\b\\set\\ab.dll", 3);
	other_volume.id.volume = 8;
	CHECK(PostLoadRule({other_volume}, {Checked("C:\\b\\set\\ab.dll", 3)}) == rule);
	auto unidentified = Module("C:\\b\\set\\ab.dll", 3);
	unidentified.identified = false;
	CHECK(PostLoadRule({unidentified}, {Checked("C:\\b\\set\\ab.dll", 3)}) == rule);
}

TEST_CASE("The post-load check refuses a module whose path cannot be read", "[windows_rules]") {
	CHECK(PostLoadRule({Module("C:\\b\\set\\ab.dll", 3), Module("", 4)}, {Checked("C:\\b\\set\\ab.dll", 3)}) ==
	      "a module's path cannot be read, or it was unloaded while the modules were listed, so it may answer "
	      "to the name of a driver file");
}

TEST_CASE("A DbgHelp.dll loaded from outside System32 is found before the load", "[windows_rules]") {
	const std::string system = "C:\\Windows\\system32\\DbgHelp.dll";
	CHECK_FALSE(LoadedFromElsewhere({Module("C:\\Windows\\System32\\dbghelp.dll", 1)}, DBGHELP, system));
	CHECK_FALSE(LoadedFromElsewhere({Module("C:\\app\\other.dll", 1)}, DBGHELP, system));
	CHECK(LoadedFromElsewhere({Module("C:\\Windows\\System32\\dbghelp.dll", 1), Module("C:\\app\\DBGHELP.DLL", 2)},
	                          DBGHELP, system));
}

TEST_CASE("After the load exactly one DbgHelp.dll, System32's, passes", "[windows_rules]") {
	const std::string system = "C:\\Windows\\system32\\DbgHelp.dll";
	CHECK(SystemCopyRule({Module("C:\\Windows\\System32\\dbghelp.dll", 1)}, DBGHELP, system).empty());
	CHECK(SystemCopyRule({}, DBGHELP, system) == "DbgHelp.dll is loaded 0 times, not once from System32");
	CHECK(SystemCopyRule({Module(system, 1), Module("C:\\app\\DbgHelp.dll", 2)}, DBGHELP, system) ==
	      "DbgHelp.dll is loaded 2 times, not once from System32");
	CHECK(SystemCopyRule({Module("C:\\app\\DbgHelp.dll", 2)}, DBGHELP, system) ==
	      "DbgHelp.dll is loaded from outside System32");
}

namespace {

// A module list of `count` handles numbered from 1 whose `fail_call`th call fails (0: none does).
struct FakeModuleList {
	FakeModuleList(size_t count_p, int fail_call_p) : count(count_p), fail_call(fail_call_p) {
	}
	size_t count;
	int fail_call;
	mutable int calls = 0;
	bool operator()(int *data, size_t capacity, size_t &needed) const {
		if (++calls == fail_call) {
			return false;
		}
		for (size_t i = 0; i < capacity && i < count; i++) {
			data[i] = static_cast<int>(i + 1);
		}
		needed = count;
		return true;
	}
};

} // namespace

TEST_CASE("The module list is read whole, growing its buffer as the list asks", "[windows_rules]") {
	std::vector<int> handles(4);
	CHECK(ListModuleHandles(FakeModuleList(3, 0), handles));
	CHECK(handles == std::vector<int>({1, 2, 3}));
	const FakeModuleList longer(6, 0);
	handles.assign(2, 0);
	CHECK(ListModuleHandles(longer, handles));
	CHECK(handles == std::vector<int>({1, 2, 3, 4, 5, 6}));
	CHECK(longer.calls == 2);
}

TEST_CASE("A module list that cannot be read is a failure, never an empty list", "[windows_rules]") {
	std::vector<int> handles(4);
	CHECK_FALSE(ListModuleHandles(FakeModuleList(3, 1), handles));
	CHECK(handles.empty());
	// The call after the buffer grows fails: no handle from the first call is kept.
	handles.assign(2, 0);
	CHECK_FALSE(ListModuleHandles(FakeModuleList(6, 2), handles));
	CHECK(handles.empty());
}

TEST_CASE("BaseName is the part after the last backslash", "[windows_rules]") {
	CHECK(BaseName("C:\\a\\b.dll") == "b.dll");
	CHECK(BaseName("b.dll") == "b.dll");
}

TEST_CASE("A trace value counts as off only as REG_SZ holding nothing or 0", "[windows_rules]") {
	CHECK(TraceValueIsOff(REGISTRY_STRING, ""));
	CHECK(TraceValueIsOff(REGISTRY_STRING, Utf16(std::u16string(1, u'\0'))));
	CHECK(TraceValueIsOff(REGISTRY_STRING, Utf16(u"0")));
	CHECK(TraceValueIsOff(REGISTRY_STRING, Utf16(std::u16string(u"0") + u'\0')));
	CHECK(TraceValueIsOff(REGISTRY_STRING, Utf16(std::u16string(u"0") + u'\0' + u'\0')));
}

TEST_CASE("A trace value the driver's atoi could read as on is refused", "[windows_rules]") {
	for (const auto &text : {std::u16string(u"1"), std::u16string(u" 1"), std::u16string(u"+1"), std::u16string(u"1x"),
	                         std::u16string(u"00"), std::u16string(u"0") + u'\0' + u'x',
	                         std::u16string(u"\u3130")}) { // not 0, though its low byte is
		CHECK_FALSE(TraceValueIsOff(REGISTRY_STRING, Utf16(text)));
	}
}

TEST_CASE("A trace value of another type, or of an odd byte count, is refused", "[windows_rules]") {
	CHECK_FALSE(TraceValueIsOff(4, std::string(4, '\0'))); // REG_DWORD 0
	CHECK_FALSE(TraceValueIsOff(2, Utf16(u"0")));          // REG_EXPAND_SZ
	CHECK_FALSE(TraceValueIsOff(REGISTRY_STRING, Utf16(u"0") + '\0'));
}

TEST_CASE("File versions read as four parts of at most 65535", "[windows_rules]") {
	uint64_t version = 0;
	REQUIRE(ParseFileVersion("14.42.34433.0", version));
	CHECK(version == ((14ULL << 48) | (42ULL << 32) | (34433ULL << 16)));
	CHECK(FileVersionText(version) == "14.42.34433.0");
	REQUIRE(ParseFileVersion("65535.0.0.1", version));
	CHECK(FileVersionText(version) == "65535.0.0.1");
}

TEST_CASE("A file version of another form is refused, leaving the version", "[windows_rules]") {
	for (const auto *text : {"", "1.2.3", "1.2.3.4.5", "1..3.4", "1.2.3.", "1.2.3.x", "1.2.3.65536", "1.2.3.000001"}) {
		uint64_t version = 9;
		INFO(text);
		CHECK_FALSE(ParseFileVersion(text, version));
		CHECK(version == 9);
	}
}

TEST_CASE("The runtime sentence names the DLL, the copy and its version, never a path", "[windows_rules]") {
	const auto older =
	    RuntimeMessage("VCRUNTIME140.dll", "14.42.34433.0", true, RuntimeState::READ, (14ULL << 48) | (10ULL << 32));
	// The program's own copy: the redistributable does not replace it, so the remedy is the program's.
	CHECK(older == "the Redshift ODBC driver needs VCRUNTIME140.dll version 14.42.34433.0 or later; this process "
	               "would use the copy loaded by the program, which is version 14.10.0.0; installing Microsoft's "
	               "Visual C++ redistributable does not replace that copy; update the program, or the Visual C++ "
	               "runtime it ships, so that copy is 14.42.34433.0 or later, then restart the program and connect "
	               "again");
	CHECK(RuntimeMessage("VCRUNTIME140.dll", "14.44.0.0", true, RuntimeState::UNREADABLE, 0)
	          .find("the copy loaded by the program, whose version cannot be read; installing") != std::string::npos);
	CHECK(RuntimeMessage("VCRUNTIME140.dll", "14.44.0.0", false, RuntimeState::READ, (14ULL << 48) | (10ULL << 32)) ==
	      "the Redshift ODBC driver needs VCRUNTIME140.dll version 14.44.0.0 or later; this process would use "
	      "System32's copy, which is version 14.10.0.0 (a copy beside the program is used only if the program has "
	      "loaded it); install Microsoft's latest Visual C++ v14 x64 redistributable, then restart the program and "
	      "connect again");
	CHECK(RuntimeMessage("MSVCP140.dll", "1.0.0.0", false, RuntimeState::MISSING, 0)
	          .find("would use System32's copy, which is missing (") != std::string::npos);
	CHECK(RuntimeMessage("MSVCP140.dll", "1.0.0.0", false, RuntimeState::UNREADABLE, 0)
	          .find("System32's copy, whose version cannot be read (") != std::string::npos);
}
