//===----------------------------------------------------------------------===//
// The macOS platform hooks of the driver loader.
//===----------------------------------------------------------------------===//

#include "driver_loader_core.hpp"

#include <algorithm>
#include <cerrno>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <iterator>

#include <dlfcn.h>
#include <membership.h>
#include <sys/acl.h>
#include <sys/mount.h>
#include <sys/param.h>
#include <sys/stat.h>
#include <unistd.h>

namespace duckdb {
namespace redshift_driver {
namespace detail {

// No RTLD_DEEPBIND on macOS. The two-level namespace binds each of the driver's ordinary imports to the
// library it names. Weak definitions (C++ inline and template instantiations) can coalesce across images;
// the driver's macOS tests check what that reaches (third_party/redshift_odbc/README.md, weak-binds).
int PlatformDlopenFlags() {
	return RTLD_NOW | RTLD_LOCAL;
}

std::vector<std::string> PlatformLoaderVariables() {
	// dyld tries a dlopen path's file name in each DYLD_LIBRARY_PATH directory, and the path plus
	// DYLD_IMAGE_SUFFIX, before the path itself; DYLD_FRAMEWORK_PATH does the same for the frameworks the
	// driver links; the versioned paths put a library with the same install name and a newer version in
	// place of the one named; DYLD_FORCE_FLAT_NAMESPACE binds every image's imports to the first image that
	// defines the name. The fallback paths apply only to a path that is missing, which the checked file is
	// not. DYLD_INSERT_LIBRARIES is not refused: an inserted library is code the process started with, which
	// can interpose anything whatever the loader does. dyld reads these at start-up (a
	// hardened host without the allow-dyld-environment-variables entitlement ignores them), so one the host unset
	// later still applies: the file loaded is also checked afterwards (LoadedFileIsChecked).
	return {"DYLD_LIBRARY_PATH",           "DYLD_FRAMEWORK_PATH",           "DYLD_IMAGE_SUFFIX",
	        "DYLD_VERSIONED_LIBRARY_PATH", "DYLD_VERSIONED_FRAMEWORK_PATH", "DYLD_FORCE_FLAT_NAMESPACE"};
}

const std::string *MissingPrerequisite(const char *dlerror_text, const std::vector<std::string> &prerequisites) {
	// dyld: "dlopen(<file>, 0x0006): Library not loaded: <install name>". policy/host-prerequisites.txt lists
	// none for macOS (the driver links only system libraries), so this answers only a test's list.
	const std::string text = dlerror_text != nullptr ? dlerror_text : "";
	const std::string marker = "Library not loaded: ";
	const auto at = text.find(marker);
	if (at == std::string::npos) {
		return nullptr;
	}
	auto name = text.substr(at + marker.size());
	name = name.substr(0, name.find_first_of("\r\n"));
	name = name.substr(name.find_last_of('/') + 1);
	for (const auto &prerequisite : prerequisites) {
		if (name == prerequisite) {
			return &prerequisite;
		}
	}
	return nullptr;
}

int NoExecState(const std::string &dir) {
	struct statfs st;
	if (::statfs(dir.c_str(), &st) != 0) {
		return -1;
	}
	return (st.f_flags & MNT_NOEXEC) != 0 ? 1 : 0;
}

bool IsNoExecMount(const std::string &dir) {
	return NoExecState(dir) == 1;
}

std::string VolumeRuleFromFlags(uint32_t statfs_flags) {
	// Every owner and mode the walk checks there is the asker's, so they say nothing.
	return (statfs_flags & MNT_IGNORE_OWNERSHIP) != 0
	           ? "the driver root is on a volume that ignores ownership, where every local user can change it (turn "
	             "off \"Ignore ownership on this volume\" in its Get Info window, or set REDSHIFT_DRIVER_DIR on "
	             "another volume)"
	           : std::string();
}

std::string VolumeRule(const std::string &dir) {
	struct statfs st;
	if (::statfs(dir.c_str(), &st) != 0) {
		const int error = errno;
		return "the driver root's volume cannot be examined: " + ErrnoText(error);
	}
	return VolumeRuleFromFlags(st.f_flags);
}

std::string DescriptorPath(int) {
	// Not tried: how dyld treats a /dev/fd name (what dladdr reports for it, library validation, its reuse of
	// an image by name) is unverified, so the checked path is loaded and the mapping checked afterwards.
	return std::string();
}

bool LoadedFileIsChecked(const void *address, const struct stat &checked) {
	// dyld records the path of the file it mapped for each image, after any DYLD_ search path it applied. The
	// checked path is absolute, so an image dyld names no absolute file for is not the checked one: refused.
	Dl_info info;
	struct stat st;
	return ::dladdr(address, &info) != 0 && info.dli_fname != nullptr && info.dli_fname[0] == '/' &&
	       ::stat(info.dli_fname, &st) == 0 && st.st_dev == checked.st_dev && st.st_ino == checked.st_ino;
}

std::string MappedFilePath(const void *, bool *mapped) {
	// dladdr's name, which LoadedFromDirectory takes when it is absolute: dyld's record of the path it
	// mapped the extension from, absolute when DuckDB gave it an absolute path (else the per-user cache is used).
	if (mapped != nullptr) {
		*mapped = false;
	}
	return std::string();
}

const char *PerUserCacheRule() {
	// Both conditions that leave none, since the rule reads nothing to say which one held.
	return "HOME is not an absolute path, or the user is root, whose HOME may be another user's (sudo keeps the "
	       "caller's)";
}

std::string PerUserCacheBaseFor(uid_t user) {
	// ~/Library/Caches, where macOS keeps per-user caches out of backups and where an app sandbox's HOME
	// points into its container. XDG_CACHE_HOME is a Linux convention and is not
	// read, nor is the password database. macOS's sudo keeps the caller's HOME, so root has no per-user
	// cache, and a sudo run never creates a root-owned cache in a user's home.
	const char *home = std::getenv("HOME");
	if (user != 0 && home != nullptr && home[0] == '/') {
		return std::string(home) + "/Library/Caches/duckdb-redshift";
	}
	return std::string();
}

std::string PerUserCacheBase() {
	return PerUserCacheBaseFor(::geteuid());
}

namespace {

// The rights: add_file and write_data are one bit, as are add_subdirectory and append_data.
constexpr acl_perm_t WRITE_RIGHTS[] = {ACL_WRITE_DATA, ACL_APPEND_DATA,    ACL_DELETE_CHILD,
                                       ACL_DELETE,     ACL_WRITE_SECURITY, ACL_CHANGE_OWNER};
// Every right an entry can grant, for the owner-only driver root.
constexpr acl_perm_t ALL_RIGHTS[] = {
    ACL_READ_DATA,          ACL_WRITE_DATA,          ACL_EXECUTE,         ACL_DELETE,
    ACL_APPEND_DATA,        ACL_DELETE_CHILD,        ACL_READ_ATTRIBUTES, ACL_WRITE_ATTRIBUTES,
    ACL_READ_EXTATTRIBUTES, ACL_WRITE_EXTATTRIBUTES, ACL_READ_SECURITY,   ACL_WRITE_SECURITY,
    ACL_CHANGE_OWNER};

} // namespace

// A group may hold anyone, wheel included, so only the user and root by user ID are trusted; a UUID that does not
// resolve is not (AclQualifierGrantsToAnother).
bool GrantsToAnother(acl_entry_t entry, uid_t user, bool any_access, const AclEntryReads &reads) {
	acl_tag_t tag;
	if (reads.tag_type(entry, &tag) != 0) {
		return true;
	}
	if (tag != ACL_EXTENDED_ALLOW) {
		return false; // a deny entry only takes rights away
	}
	acl_permset_t permissions;
	if (reads.permset(entry, &permissions) != 0) {
		return true;
	}
	const auto *first = any_access ? std::begin(ALL_RIGHTS) : std::begin(WRITE_RIGHTS);
	const auto *last = any_access ? std::end(ALL_RIGHTS) : std::end(WRITE_RIGHTS);
	if (std::none_of(first, last, [&](acl_perm_t right) { return ::acl_get_perm_np(permissions, right) != 0; })) {
		return false;
	}
	auto *who = static_cast<guid_t *>(reads.qualifier(entry));
	if (who == nullptr) {
		return true;
	}
	auto account = AclQualifier::UNRESOLVED;
	id_t id = 0;
	int type = -1;
	if (::mbr_uuid_to_id(who->g_guid, &id, &type) == 0) {
		account = type == ID_TYPE_UID ? AclQualifier::USER : AclQualifier::GROUP;
	}
	::acl_free(who);
	return AclQualifierGrantsToAnother(account, id, user);
}

bool ClearAcl(int fd) {
	acl_t empty = ::acl_init(0);
	if (empty == nullptr) {
		return false;
	}
	const bool cleared = ::acl_set_fd_np(fd, empty, ACL_TYPE_EXTENDED) == 0 || NoAclToClear(errno);
	const int error = errno;
	::acl_free(empty);
	errno = error; // for the caller's reason
	return cleared;
}

AclCheck AclRule(const std::string &path, int fd, uid_t user, bool any_access) {
	return AclRule(path, fd, user, any_access, AclEntryReads());
}

AclCheck AclRule(const std::string &path, int fd, uid_t user, bool any_access, const AclEntryReads &reads) {
	// Extended ACLs do not show in the mode bits. Read without following a link: the walk refused links,
	// and a set file is read through the descriptor its owner, mode and hash come from. An inherit-only
	// entry counts too: what the loader creates below it would inherit it.
	acl_t acl = fd >= 0 ? ::acl_get_fd_np(fd, ACL_TYPE_EXTENDED) : ::acl_get_link_np(path.c_str(), ACL_TYPE_EXTENDED);
	if (acl == nullptr) {
		// None, or one that cannot be read (EACCES: an entry denies this user readsecurity), which refuses.
		return AclReadFailure(errno);
	}
	AclCheck check;
	bool inherited = true;
	acl_entry_t entry;
	for (int which = ACL_FIRST_ENTRY; ::acl_get_entry(acl, which, &entry) == 0; which = ACL_NEXT_ENTRY) {
		if (!GrantsToAnother(entry, user, any_access, reads)) {
			continue;
		}
		check.finding = AclFinding::BROKEN;
		check.rule = any_access ? "grants access to another user or group through its access control list (it "
		                          "must be owner-only)"
		                        : "is writable by another user or group through its access control list";
		// Whether the directory got the entry from its parent when it was made; one set on the directory itself,
		// or whose flags cannot be read, decides it, so no later entry is looked at.
		acl_flagset_t flags;
		if (reads.flagset(entry, &flags) != 0 || reads.flag(flags, ACL_ENTRY_INHERITED) != 1) {
			inherited = false;
			break;
		}
	}
	check.inherited = check.finding == AclFinding::BROKEN && inherited;
	::acl_free(acl);
	return check;
}

} // namespace detail
} // namespace redshift_driver
} // namespace duckdb
