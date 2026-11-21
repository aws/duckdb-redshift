//===----------------------------------------------------------------------===//
//                         DuckDB - Redshift
//
// src/transport/driver/driver_windows_rules.hpp
//
// The Windows loader's decisions that need no Windows call: path forms and lengths, owners and
// access lists, the trust file's path, the registry trace values, file versions and the post-load
// module check. The Windows files gather the facts and apply these; Linux's driver suite tests them.
//===----------------------------------------------------------------------===//

#pragma once

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace duckdb {
namespace redshift_driver {
namespace windows_rules {

// Names inside a base: the driver root, its lock, the staging and set-aside prefixes and their
// random part, and the trust directory beside the root.
constexpr const char *DRIVER_ROOT_NAME = ".redshift-driver";
constexpr const char *LOCK_NAME = ".lock";
constexpr const char *STAGING_PREFIX = ".staging-";
constexpr const char *INVALID_PREFIX = ".invalid-";
constexpr size_t RANDOM_NAME_LENGTH = 16;
constexpr const char *TRUST_DIRECTORY_NAME = "trust";
constexpr size_t SET_HASH_LENGTH = 64;
// The trust file is <sha256 of its PEM>.pem, as the ODBC client names it.
constexpr size_t TRUST_FILE_NAME_LENGTH = 64 + 4;

// The driver reads its own path into a MAX_PATH buffer, and the trust file's
// path must fit the client's 255-byte limit.
constexpr size_t MAX_PATH_CHARACTERS = 259;
constexpr size_t MAX_TRUST_PATH_BYTES = 255;

// What is wrong with `path` as a canonical Windows path, or empty. Checks the
// text alone; the 8.3 and network-volume rules need the file system and are the caller's.
std::string PathFormRule(const std::wstring &path);
// `path` as narrow text; only for a path PathFormRule accepted, which is ASCII.
std::string Narrow(const std::wstring &path);
std::wstring Widen(const std::string &path);
// `dir`\`name`, with no second separator after a drive root such as D:\.
std::string JoinPath(const std::string &dir, const std::string &name);
// `name` in the system folder `system_dir`, or empty when that folder is unknown, so that no
// caller reads a file of that name at the current drive's root instead.
std::string SystemFilePath(const std::string &system_dir, const std::string &name);
// The longest path the loader would use under `base` for a set of `file_names` (set files,
// staging and set-aside copies, the lock and the trust file).
size_t LongestPathUnder(const std::string &base, const std::vector<std::string> &file_names);
// Whether two paths name the same file as Windows compares them: ignoring ASCII case.
bool SamePath(const std::string &a, const std::string &b);

// GetDriveTypeW's results as winbase.h numbers them, named apart from its macros.
constexpr unsigned DRIVE_KIND_UNKNOWN = 0;
constexpr unsigned DRIVE_KIND_NO_ROOT_DIR = 1;
constexpr unsigned DRIVE_KIND_REMOVABLE = 2;
constexpr unsigned DRIVE_KIND_FIXED = 3;
constexpr unsigned DRIVE_KIND_REMOTE = 4;
constexpr unsigned DRIVE_KIND_CDROM = 5;
constexpr unsigned DRIVE_KIND_RAMDISK = 6;
// What breaks the local-drive rule for a base whose drive root GetDriveTypeW reports
// as `type`, or empty. Only a type known to be local passes, so an undetermined one is refused.
std::string DriveTypeRule(unsigned type);

// File attributes as winnt.h numbers them, named apart from its macros. A folder or file with any of
// LINK_OR_PLACEHOLDER is refused: a reparse point (a junction, link or mount
// point), or a cloud-files placeholder, such as a folder under OneDrive that may be fetched on open.
// RECALL_ON_OPEN's number is also winnt.h's FILE_ATTRIBUTE_EA, but on NTFS an extended attribute does
// not set it in GetFileInformationByHandle's attributes, so a folder or file holding one passes.
constexpr uint32_t ATTRIBUTE_DIRECTORY = 0x00000010;
constexpr uint32_t ATTRIBUTE_REPARSE_POINT = 0x00000400;
constexpr uint32_t ATTRIBUTE_OFFLINE = 0x00001000;
constexpr uint32_t ATTRIBUTE_RECALL_ON_OPEN = 0x00040000;
constexpr uint32_t ATTRIBUTE_PINNED = 0x00080000;
constexpr uint32_t ATTRIBUTE_UNPINNED = 0x00100000;
constexpr uint32_t ATTRIBUTE_RECALL_ON_DATA_ACCESS = 0x00400000;
constexpr uint32_t LINK_OR_PLACEHOLDER = ATTRIBUTE_REPARSE_POINT | ATTRIBUTE_OFFLINE | ATTRIBUTE_RECALL_ON_OPEN |
                                         ATTRIBUTE_PINNED | ATTRIBUTE_UNPINNED | ATTRIBUTE_RECALL_ON_DATA_ACCESS;
// What breaks the attribute rule for a folder (`directory`) or file with these attributes, or empty.
std::string AttributeRule(uint32_t attributes, bool directory);

// Access rights as winnt.h numbers them, named apart from its macros.
constexpr uint32_t RIGHT_WRITE_DATA = 0x00000002;   // FILE_WRITE_DATA, FILE_ADD_FILE
constexpr uint32_t RIGHT_APPEND_DATA = 0x00000004;  // FILE_APPEND_DATA, FILE_ADD_SUBDIRECTORY
constexpr uint32_t RIGHT_DELETE_CHILD = 0x00000040; // FILE_DELETE_CHILD
constexpr uint32_t RIGHT_DELETE = 0x00010000;
constexpr uint32_t RIGHT_WRITE_DAC = 0x00040000;
constexpr uint32_t RIGHT_WRITE_OWNER = 0x00080000;
constexpr uint32_t RIGHT_GENERIC_ALL = 0x10000000;
constexpr uint32_t RIGHT_GENERIC_WRITE = 0x40000000;

constexpr const char *SYSTEM_SID = "S-1-5-18";
constexpr const char *ADMINISTRATORS_SID = "S-1-5-32-544";
constexpr const char *TRUSTED_INSTALLER_SID = "S-1-5-80-956008885-3418522649-1831038044-1853292631-2271478464";
constexpr const char *CREATOR_OWNER_SID = "S-1-3-0";

// One entry of a discretionary access list, read from a security descriptor.
struct AccessEntry {
	std::string sid;
	bool allow = true;
	bool inherit_only = false; // applies to children only, not to the object itself
	uint32_t mask = 0;
};
struct SecurityFacts {
	std::string owner;
	bool has_dacl = true;       // false for a NULL DACL
	bool unknown_entry = false; // an entry of a type other than plain allow and deny
	std::vector<AccessEntry> entries;
};
// Where an object is on the path: above the base, or the base and anything below it.
enum class PathPlace { ANCESTOR, OWN };
// What breaks the access rule for `facts` at `place` for the account `user`, or empty.
std::string SecurityRule(const SecurityFacts &facts, PathPlace place, const std::string &user);

// What stops `dir` holding the trust file, or empty.
std::string TrustDirectoryRule(const std::string &dir);

// A base refused: `directory` for a debug log, `reason` its role and rule, never a path.
struct Attempt {
	std::string directory;
	std::string reason;
};
// NO_USABLE_DIRECTORY's message: the reasons, then the remedy that names REDSHIFT_DRIVER_DIR.
std::string NoUsableDirectoryMessage(const std::vector<Attempt> &attempts);

// A file's identity: its volume serial number and file index.
struct FileId {
	uint32_t volume = 0;
	uint64_t index = 0;
	bool operator==(const FileId &other) const {
		return volume == other.volume && index == other.index;
	}
};
// A module of the process, and its file's identity when that could be read.
struct LoadedModule {
	std::string path;
	bool identified = false;
	FileId id;
};
// Every handle `list(data, capacity, needed)` reports, starting at `handles`' size and growing it to
// `needed` until the list fits. False, with `handles` empty, when a call fails: a list that could not
// be read must not pass as a process with no such module.
template <class Handle, class List>
bool ListModuleHandles(const List &list, std::vector<Handle> &handles) {
	if (handles.empty()) {
		handles.resize(1);
	}
	for (;;) {
		size_t needed = 0;
		if (!list(handles.data(), handles.size(), needed)) {
			handles.clear();
			return false;
		}
		const bool fits = needed <= handles.size();
		handles.resize(needed);
		if (fits) {
			return true;
		}
	}
}
// A set file as it was checked before the load.
struct CheckedFile {
	std::string path;
	FileId id;
};
// The file name at the end of `path`.
std::string BaseName(const std::string &path);
// What breaks the loaded-module rule after the load, or empty: each checked file's name must be the name of
// exactly one module, and that module must be the checked file. Other names are not counted; a
// module whose path cannot be read fails it, as it may have any name.
std::string PostLoadRule(const std::vector<LoadedModule> &modules, const std::vector<CheckedFile> &files);

// The run-time load the driver makes by bare name, which binds to System32's copy only when that
// copy is loaded first.
constexpr const char *DBGHELP = "DbgHelp.dll";
constexpr const char *DBGHELP_MESSAGE =
    "this program has loaded a DbgHelp.dll from outside the Windows system folder, which the Redshift ODBC driver "
    "would use; connect from a program that does not load its own DbgHelp.dll";
// Whether a module named `name` is loaded from anywhere but `system_path`, before the load.
bool LoadedFromElsewhere(const std::vector<LoadedModule> &modules, const std::string &name,
                         const std::string &system_path);
// What breaks "exactly one module named `name`, the one at `system_path`" after the load, or empty.
std::string SystemCopyRule(const std::vector<LoadedModule> &modules, const std::string &name,
                           const std::string &system_path);

// A trace value read as `type` and raw `bytes` counts as off only when it is
// REG_SZ holding exactly the empty string or 0, as UTF-16 with or without its terminating NULs.
constexpr uint32_t REGISTRY_STRING = 1; // REG_SZ
bool TraceValueIsOff(uint32_t type, const std::string &bytes);
constexpr const char *REGISTRY_TRACE_MESSAGE =
    "the installed Amazon Redshift ODBC driver's registry settings turn driver logging on, which the Redshift "
    "extension's driver would also read. An administrator must turn that logging off to connect";

// "a.b.c.d" as VS_FIXEDFILEINFO packs it, each part at most 65535; false, leaving `version`, otherwise.
bool ParseFileVersion(const std::string &text, uint64_t &version);
std::string FileVersionText(uint64_t version);
// What reading the runtime copy found, and MISSING_PREREQUISITE's sentence for it. `own_copy` is a
// copy the program loaded from outside System32, which the redistributable does not replace.
enum class RuntimeState { MISSING, UNREADABLE, READ };
std::string RuntimeMessage(const std::string &dll, const std::string &minimum, bool own_copy, RuntimeState state,
                           uint64_t version);

// The rule of a base whose invalid set cannot be set aside.
constexpr const char *SET_IN_USE_RULE =
    "the driver files are in use by another process and cannot be repaired; close that program and connect again";

} // namespace windows_rules
} // namespace redshift_driver
} // namespace duckdb
