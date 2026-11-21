// src/transport/driver/driver_loader_core.hpp (the loader's own; never included from
// src/transport/odbc/)
// The loader's internal pieces, declared once so the common POSIX code, the platform files and
// the loader's tests share them. Includes no DuckDB header.
#pragma once

#include "odbc_driver.hpp"

#if defined(__linux__)
#include "allocator_replacement.hpp"
#endif

#include <cstddef>
#include <cstdint>
#include <functional>
#include <mutex>
#include <string>
#include <vector>

#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>
#if defined(__APPLE__)
#include <sys/acl.h>
#endif

struct stat;

namespace duckdb {
namespace redshift_driver {
namespace detail {

// The variable that names the one base directory to use instead of the defaults.
constexpr const char *DRIVER_DIR_VARIABLE = "REDSHIFT_DRIVER_DIR";

// Platform hooks, one definition per platform file (driver_loader_linux.cpp, driver_loader_macos.cpp).
// True when `dir` is on a file system mounted without exec permission (Linux: statvfs ST_NOEXEC; macOS:
// statfs MNT_NOEXEC, which macOS's statvfs does not report).
bool IsNoExecMount(const std::string &dir);
// The per-user cache base, `<cache>/duckdb-redshift`, not yet created or resolved. Linux:
// `<cache>` is $XDG_CACHE_HOME when it is absolute, else $HOME/.cache when HOME is absolute: from
// the environment only (DuckDB's home is its home_directory setting, else HOME); empty when neither
// is, and the password database is not read. macOS: `<cache>` is $HOME/Library/Caches when HOME is
// absolute and the user is not root (whose HOME under sudo is the caller's); empty otherwise.
std::string PerUserCacheBase();
#if defined(__APPLE__)
// macOS only: PerUserCacheBase for the effective user `user`, so tests can give root.
std::string PerUserCacheBaseFor(uid_t user);
#endif
// The platform's own rule for finding that base, so the shared code can say why there is none
// without naming any platform's variables. A constant: it reads nothing.
const char *PerUserCacheRule();
// The absolute path of the file mapped at `address` as the system records it (Linux:
// /proc/self/maps, resolved by the kernel), or empty (always on macOS, where dladdr's name is used). `*mapped`, when
// given, says whether the system had a mapping there at all: one it names no usable path for (removed, or a path it
// escaped) is not a reason to ask dladdr, whose name may now lead elsewhere.
std::string MappedFilePath(const void *address, bool *mapped = nullptr);
// What AclRule found: nothing against the rule; an entry that breaks it (evidence: a directory is refused, a
// set INVALID); or an ACL that cannot be read (no evidence either way: a directory is refused, a set UNCHECKED).
enum class AclFinding { NONE, BROKEN, UNREADABLE };
struct AclCheck {
	AclFinding finding = AclFinding::NONE;
	std::string rule; // without a path; empty for NONE
	// BROKEN only: every entry that breaks the rule was inherited from the parent when the directory was made (macOS:
	// ACL_ENTRY_INHERITED), as on a new driver root whose maker has not cleared them yet. Always false on Linux.
	bool inherited = false;
};
// The rule an extended ACL entry on `path` (or, when `fd` is not -1, the file it refers to) breaks for
// `user` ("is writable by another user or group through its access control list"): an entry that cannot be
// read, or an allow entry that grants another account, a group or an unresolvable UUID a right to add, write,
// delete, change the ACL or take ownership; with `any_access`, any right at all (an owner-only driver root). A deny
// entry passes (AclQualifierGrantsToAnother decides the account). macOS only: Linux checks nothing, because a POSIX ACL
// entry that grants a right raises the mode's group bits (its mask), which the mode rules refuse.
AclCheck AclRule(const std::string &path, int fd, uid_t user, bool any_access);
// The type of a hook that applies AclRule's rule, which tests replace.
using AclRuleHook = AclCheck (*)(const std::string &path, int fd, uid_t user, bool any_access);
// Removes every extended ACL entry of the directory `fd` refers to, which this process just created: on macOS a
// new directory inherits its parent's inheritable entries, which its mode does not show and 0700 does not take
// away. True on success, and always on Linux (a default ACL's entries are masked by the mode the loader sets).
bool ClearAcl(int fd);
// Whether removing the entries failed with `error` only because there are none to clear: true for ENOTSUP or
// EOPNOTSUPP (a file system without extended ACLs), false otherwise. On every platform, so Linux's tests run it too.
bool NoAclToClear(int error);
// The start of AclRule's rule for an ACL that cannot be read.
constexpr const char *ACL_UNREADABLE = "has an access control list that cannot be read";
// AclRule's answer when reading the ACL failed with `error`: NONE for ENOENT (no extended ACL) and ENOTSUP or
// EOPNOTSUPP (a file system without them); otherwise UNREADABLE, with ACL_UNREADABLE and the error. Never "no
// ACL" for EACCES, which macOS gives when an entry denies the user readsecurity (and refuses lstat there too,
// which the walk meets first): what that ACL grants cannot be seen. On every platform, so Linux's tests run it
// too.
AclCheck AclReadFailure(int error);
// What an extended ACL entry's qualifier UUID names (macOS: mbr_uuid_to_id): no account, an account that is not a
// user (a group, which may hold anyone), or a user.
enum class AclQualifier { UNRESOLVED, GROUP, USER };
// Whether an allow entry that grants a right AclRule counts breaks the rule for `user` because of whom it is for:
// true unless `who` is USER and `id`, its user ID, is `user`'s or root's. On every platform, so Linux's tests run it
// too.
bool AclQualifierGrantsToAnother(AclQualifier who, uid_t id, uid_t user);
#if defined(__APPLE__)
// macOS only: the reads GrantsToAnother and AclRule make of an entry, so a test can make each fail, which none does
// for an entry acl_get_entry gives.
struct AclEntryReads {
	int (*tag_type)(acl_entry_t entry, acl_tag_t *tag) = ::acl_get_tag_type;
	int (*permset)(acl_entry_t entry, acl_permset_t *permissions) = ::acl_get_permset;
	void *(*qualifier)(acl_entry_t entry) = ::acl_get_qualifier;
	int (*flagset)(void *entry, acl_flagset_t *flags) = ::acl_get_flagset_np;
	int (*flag)(acl_flagset_t flags, acl_flag_t flag) = ::acl_get_flag_np;
};
// macOS only: whether AclRule refuses the entry `entry` for `user`: an allow entry that grants an account other than
// `user` or root (AclQualifierGrantsToAnother) a write right, or any right with `any_access`; or an entry whose tag,
// rights or qualifier cannot be read.
bool GrantsToAnother(acl_entry_t entry, uid_t user, bool any_access, const AclEntryReads &reads = AclEntryReads());
// macOS only: AclRule with `reads` for its reads of each entry, GrantsToAnother's and the flags that say whether the
// entry was inherited.
AclCheck AclRule(const std::string &path, int fd, uid_t user, bool any_access, const AclEntryReads &reads);
#endif
// The rule the volume `dir` is on breaks for a driver root, or empty (macOS only: a volume that ignores
// ownership, MNT_IGNORE_OWNERSHIP, the default for external drives, where every file shows as the asker's and
// every local user has the owner's access, so no owner or mode the walk checks means anything; Linux: none,
// every file system is allowed). Refused for every base, REDSHIFT_DRIVER_DIR included.
std::string VolumeRule(const std::string &dir);
#if defined(__APPLE__)
// macOS only: VolumeRule's answer for a statfs entry's flags, so tests can give each case.
std::string VolumeRuleFromFlags(uint32_t statfs_flags);
#endif

// How long an install waits for a driver root's lock before it moves on to the next base,
// so a stopped holder cannot freeze every connect.
constexpr int LOCK_DEADLINE_MILLISECONDS = 30000;

// What base-directory selection and the install read, so tests can supply each part.
// CurrentBaseInputs() reads the process's own.
struct BaseInputs {
	bool override_set = false;  // REDSHIFT_DRIVER_DIR is set (an empty value counts as set)
	std::string override_value; // its value
	std::string extension_dir;  // the resolved directory of the loaded extension; empty if unknown
	std::string per_user_base;  // PerUserCacheBase(); empty if none
	bool (*is_noexec)(const std::string &dir) = IsNoExecMount;
	// The rule the volume of a base, or of its driver root, breaks, or empty (VolumeRule).
	std::string (*volume_rule)(const std::string &dir) = VolumeRule;
	// The ACL rule PrepareDriverRoot applies (AclRule); tests give their own.
	AclRuleHook acl_rule = AclRule;
	int lock_deadline_milliseconds = LOCK_DEADLINE_MILLISECONDS; // shorter in tests
};
BaseInputs CurrentBaseInputs();

// Every refusal is a DriverAttempt whose `reason` names the base by its role (REDSHIFT_DRIVER_DIR,
// "the extension's directory", "the per-user cache") and the rule it broke, and never a path, so
// a message built from reasons holds no path and no environment variable's value.
// Its `directory` is the directory the rule is about, which may be a parent of the
// base: it goes into DriverError::attempts, for a debug log, never into a message.

// The base directories to try, in order. REDSHIFT_DRIVER_DIR, when set, is the only
// candidate: it must be absolute, exist as a directory and have no symbolic link and no '..' in any
// component, and there is no fallback when it is refused. Otherwise the extension's directory, then the
// per-user cache, created with mode 0700 where missing. A base on a no-exec mount is skipped. Every
// refusal or skip is appended to `attempts`. Returned paths are absolute and, apart
// from the override (which has no link to resolve), resolved with realpath.
std::vector<std::string> ChooseBaseDirectories(const BaseInputs &inputs, std::vector<DriverAttempt> &attempts);

// The directory, inside a base directory, that holds the driver sets.
constexpr const char *DRIVER_ROOT_NAME = ".redshift-driver";

// Every directory from / down to `dir`, inclusive, is a real directory (lstat, no
// symbolic link), owned by `user` or root, and not writable by group or others, except that a
// root-owned sticky directory such as /tmp may be an ancestor, though not `dir` itself when
// `dir_is_base`. On the first failure, false with `where` the directory it is about and `reason`
// the rule, naming it as "the directory" (or "the driver root" when not `dir_is_base`) or "a
// parent directory", without a path.
// No path with a '$' is accepted: glibc's dlopen would rewrite $ORIGIN, $LIB or $PLATFORM in it.
// `acl_rule` is AclRule; a test gives one that cannot read an ACL, since Linux's AclRule finds nothing and macOS's
// walk may refuse the lstat there first.
enum class AncestryEnd { BASE, DRIVER_ROOT, PARENT };
bool CheckAncestry(const std::string &dir, AncestryEnd end, uid_t user, std::string &where, std::string &reason,
                   AclRuleHook acl_rule = AclRule);
inline bool CheckAncestry(const std::string &dir, bool dir_is_base, uid_t user, std::string &where,
                          std::string &reason) {
	return CheckAncestry(dir, dir_is_base ? AncestryEnd::BASE : AncestryEnd::DRIVER_ROOT, user, where, reason);
}

// Checks `base` and, before anything is made there, its volume (`volume_rule`, BaseInputs' hook), creates
// `<base>/.redshift-driver` with mode 0700 when missing, and checks it too, its volume first (a volume may be
// mounted there), and the mode rule (no access for group or others; a root-owned root may be readable,
// never writable). Sets `root`; on a failure `where` and `reason` as CheckAncestry's. `acl_rule` is AclRule. When
// given, `inherited_acl_not_cleared` says whether the failure is a root of the user's refused by the owner-only ACL
// rule for entries that are all inherited (AclCheck::inherited), as a concurrent first run leaves its new root until
// it clears them: DriverRoots looks at such a root again, as at a half-made one.
bool PrepareDriverRoot(const std::string &base, uid_t user, std::string &root, std::string &where, std::string &reason,
                       std::string (*volume_rule)(const std::string &dir) = VolumeRule, AclRuleHook acl_rule = AclRule,
                       bool *inherited_acl_not_cleared = nullptr);
// Whether a driver root `owner` owns may be readable by others, by its mode and its ACL
// (never writable), as an administrator's root-owned one may; a root of anyone else's must be owner-only.
bool DriverRootMayBeReadable(uid_t owner);

// Which default a base directory is, for its role in reasons.
enum class BaseKind { OVERRIDE, EXTENSION, CACHE };
const char *BaseRole(BaseKind kind);
struct BaseCandidate {
	BaseKind kind;
	DriverAttempt attempt; // an empty reason when the base may be tried
	bool pending = false;  // the per-user cache, made and resolved when it is tried (ResolveCache)
};
// The base directories in the order they are tried, each with why it was skipped or refused, if
// it was. ChooseBaseDirectories splits the same list.
std::vector<BaseCandidate> BaseCandidates(const BaseInputs &inputs);

// The per-user cache as a base: a path with '$' or '..' refused; the deepest existing part of the
// path walked as given (links only the user's or root's) and as resolved (AncestryEnd::PARENT)
// before anything is created below it; then created 0700, resolved and checked. `skip` when it is
// the extension's directory, already tried. A refusal at a directory of the user's that a
// concurrent first run is still making (its owner lacks a permission) is tried again briefly, as
// is DriverRoots' preparation of a driver root.
struct CacheBase {
	bool skip;
	DriverAttempt attempt; // an empty reason when it may be tried
};
constexpr int CACHE_MODE_RETRIES = 50;
constexpr unsigned CACHE_MODE_RETRY_MICROSECONDS = 2000;
CacheBase ResolveCache(const BaseInputs &inputs, uid_t user);

// The driver root of each usable base, one at a time and in order: a base's driver root is checked
// and created only when the caller asks for the next one, so a later base's is not touched while an
// earlier one works; the per-user cache itself is created only when it is reached. A driver root on
// a no-exec mount is refused like a base. Every skip and refusal, and any failure the caller adds,
// is in `attempts`, in the order the bases were tried.
class DriverRoots {
public:
	DriverRoots(const BaseInputs &inputs, uid_t user);
	// The next base's prepared root (PrepareDriverRoot); false when none is left. For a user
	// namespace's overflow user it is false at once, with that one attempt.
	bool Next(std::string &root);
	// Records a failure of the caller's (an install or a check) with the last base Next gave:
	// `where` the directory it is about, `rule` its rule without a path; the base's role is added.
	void Fail(const std::string &where, const std::string &rule);
	// The role of the last base Next gave ("the extension's directory"), for messages.
	const char *Role() const;
	std::vector<DriverAttempt> attempts;

private:
	BaseInputs inputs_;
	std::vector<BaseCandidate> candidates_;
	uid_t user_;
	size_t next_ = 0;
	BaseKind kind_ = BaseKind::OVERRIDE;
};

// Inside a user namespace, a caller that is the overflow user (mapped to it, or not mapped) cannot
// tell its own directories from any unmapped owner's, so no directory is safe for it.
bool OwnerIsAmbiguous(uid_t user);
std::string OverflowUserReason(uid_t user);

// "reason; reason", for messages: roles and rules, no paths.
std::string DescribeAttempts(const std::vector<DriverAttempt> &attempts);

// A load that failed inside the loader itself (out of memory, most likely), as `kind`, with the remedy
// IsFinalKind gives that kind: "try again" while no foreign code has run, which the next call does;
// "restart the program and connect again" once the platform loader was called (LOAD_FAILED), after which
// the outcome is final.
DriverError InternalError(DriverErrorKind kind);

// NO_USABLE_DIRECTORY with every attempt, pointing to REDSHIFT_DRIVER_DIR.
DriverError NoUsableDirectory(const std::vector<DriverAttempt> &attempts);

// A file of the set to install: its bytes (embedded, or a test payload) and what they must hash to.
struct SetFile {
	std::string name;
	std::string sha256;
	const unsigned char *data = nullptr;
	size_t size = 0;
};
// The set to install: the driver first, then the notices, and the set hash that names its directory.
struct DriverSet {
	std::string set_sha256;
	std::vector<SetFile> files;
	std::vector<DriverFile> Manifest() const;
};

// What an existing driver set is found to be. INVALID only on positive evidence:
// a missing or extra name, a link, a file that is not regular, the wrong owner or mode, or a hash
// that differs. UNCHECKED when a check could not run (EMFILE, ENOMEM, EIO, EACCES, ...) or a
// directory above the set breaks step 2's rule: the set is then left as it is, and the base is
// not used.
enum class SetState : uint8_t { VALID, MISSING, INVALID, UNCHECKED };

// The fast path. Every directory from / down to the driver root above
// `set_dir` passes CheckAncestry, the ancestor rule. The set's own rule: `set_dir`
// itself, opened O_DIRECTORY | O_NOFOLLOW relative to the root, is a directory owned by `user` or
// root that group and others cannot write; it holds exactly the files named in `files`; and each,
// opened O_NOFOLLOW, is a regular file owned by `user` or root, not writable by group or others,
// whose SHA-256 matches, with owner, mode and hash all read from that descriptor. An entry that
// cannot be opened is judged by what fstatat says of it (a wrong type, owner or mode is still
// evidence). Takes no lock and writes nothing. Unless VALID, `where` is the path the rule is about
// and `reason` the rule, naming the set by its role ("the driver set", "a file of the driver set").
// `first_file_fd`, when given: on VALID, a descriptor (close-on-exec) of the first file, the very one
// whose owner, mode and hash passed, so the pre-load check (step 5) loads what it hashed; -1 otherwise.
// `after_hash`, when given (tests only), is called with each file's path once its hash has matched and
// before the first file's descriptor is kept, for a test to change what the name leads to at that moment.
// `acl_rule` is AclRule, for the walk and for the set's own ACLs; a test gives one that cannot read an ACL, as
// CheckAncestry's.
SetState CheckInstalledSet(const std::string &set_dir, const std::vector<DriverFile> &files, uid_t user,
                           std::string &where, std::string &reason, int *first_file_fd = nullptr,
                           void (*after_hash)(const std::string &path) = nullptr, AclRuleHook acl_rule = AclRule);

enum class InstallResult : uint8_t {
	INSTALLED,     // `set_dir` names a valid set: found, written by this call, or written by a racing process
	BYTES_CORRUPT, // the bytes to write differ from their hash; no set written (the lock taken, the sweep run)
	FAILED,        // this root holds no valid set and none could be written; `where` and `reason` say why
	RACED          // as FAILED, because something else changed the set meanwhile; worth another try
};

// `.invalid-` entries are kept this long before the sweep removes them.
constexpr time_t STALE_SECONDS = 3600; // one hour
constexpr const char *INVALID_PREFIX = ".invalid-";
constexpr const char *STAGING_PREFIX = ".staging-";
constexpr const char *LOCK_NAME = ".lock";

// Steps 3 and 4 under one driver root. A valid set is used as it is, without the
// lock; an UNCHECKED one is left as it is and the root fails. Otherwise (missing or invalid) the
// root's `.lock` is opened (O_CREAT | O_NOFOLLOW | O_NONBLOCK, 0600; EACCES, EPERM or EROFS mean a
// read-only base; a lock of the user's that its mode makes unopenable is made 0600 first) and must
// be a regular file `user` owns, mode 0600 through its descriptor whatever the umask;
// flock(LOCK_EX | LOCK_NB) is retried with back-off until `lock_deadline_milliseconds`. Under the lock: the stale sweep
// (SweepDriverRoot); the set checked again, since another process may have written it; the bytes
// re-hashed; the root refused when RLIMIT_FSIZE is below a file's size; the files
// written into a new 0700 staging directory (mkdtemp; each file O_CREAT | O_EXCL | O_NOFOLLOW, mode
// 0500, fsync). An invalid set is then renamed aside to a unique `.invalid-` name, and the staging
// directory renamed to the set's name, both with plain rename(2) (SetAside, Staging::RenameTo), which
// never replaces a finished set. A valid set something else put there first is used; an invalid set
// something took away before it was renamed aside leaves the name free, and the new set goes in at
// once. When something that does not take the lock changed the set under this call (a rename in
// lost, or the set this call wrote taken away or replaced), the result is RACED. Any failure after
// mkdtemp removes this call's staging directory while it is still at its staging name
// (Staging::Remove). A set this call wrote that then fails the check is set aside the same way, and
// this process does not write that set under that root again (the rule it broke is remembered for the
// process, not on disk); one that cannot be checked is left in place. Sets `set_dir` to
// `<root>/<set hash>`; `reason` names no path.
InstallResult InstallDriverSet(const std::string &root, const DriverSet &set, uid_t user, std::string &set_dir,
                               std::string &where, std::string &reason,
                               int lock_deadline_milliseconds = LOCK_DEADLINE_MILLISECONDS);

// The stale sweep of the driver root `root`, run only under its lock: every `.staging-` entry `user`
// owns, whatever its age (staging happens only under the lock, so its maker died or gave up), and
// every `.invalid-` entry `user` owns whose modification time is at least STALE_SECONDS before
// `now`. Each entry is examined without following a link: a non-directory, a link included, is
// unlinked; a directory on the root's own device (st_dev) is made 0700 through its descriptor, then
// removed with its regular files and links, unless it holds anything else (a directory, say), when it
// is left in place. Never recurses below the flat set. Failures are ignored; a later install retries.
void SweepDriverRoot(const std::string &root, uid_t user, time_t now);

// How often a base whose set something else changed meanwhile is tried (step 6), and the back-off
// between tries (the second waits once this long, the third twice).
constexpr int INSTALL_TRIES = 3;
constexpr unsigned INSTALL_RETRY_MICROSECONDS = 10000;

// What a caller makes of a set that is installed (valid): it is used (DONE, which ends the search
// for good, whatever the caller recorded), it was not as installed and a repair may mend it
// (CHANGED, tried again up to INSTALL_TRIES), or this base cannot use it and the same set would meet
// the same fact (BASE_FAILED, which ends the base and moves to the next). On the last two `where` is
// the directory the rule is about and `reason` the rule, without a path.
enum class SetUse : uint8_t { DONE, CHANGED, BASE_FAILED };
using UseSet = std::function<SetUse(const std::string &set_dir, const DriverRoots &roots, std::string &where,
                                    std::string &reason)>;

// Steps 1 to 4 and 6 over DriverRoots, in order, and the one loop for them: each root is installed
// into (InstallDriverSet) up to INSTALL_TRIES times while the result is RACED or `use` says CHANGED,
// with the back-off above, and `use` is called with every set that is installed. Returns true, with
// `set_dir` the set last given to `use`, when it said DONE; otherwise false with `error` set to
// EMBEDDED_BYTES_CORRUPT (no set written, only the lock taken; no later base tried) or
// NO_USABLE_DIRECTORY (every attempt, each recorded with DriverRoots::Fail).
bool PrepareWith(const BaseInputs &inputs, const DriverSet &set, uid_t user, const UseSet &use, std::string &set_dir,
                 DriverError &error);

// The directory of the file this code was loaded from, or empty: MappedFilePath on a function in
// it, else dladdr's name for that function when absolute, resolved with realpath; then the parent.
std::string LoadedFromDirectory();

// Creates `dir` and any missing parents with mode 0700. On failure, false with `where` the
// directory that failed and `reason` naming it as "the directory" or "a parent directory".
bool MakeDirectories(const std::string &dir, std::string &where, std::string &reason);
// Creates `dir` with mode 0700 whatever the umask, or accepts it if it exists (the caller checks it).
// On failure `reason` is the rule alone ("cannot be created: ..."), for the caller to name `dir`, and a
// directory this call made is removed. A new directory's inherited ACL entries are cleared with `clear_acl`
// (ClearAcl; a test gives one that fails).
bool MakePrivateDirectory(const std::string &dir, std::string &reason, bool (*clear_acl)(int fd) = ClearAcl);
// Sets mode 0700 on the directory `dir` that `fd` (O_PATH where there is one) refers to and `st`
// describes, through the descriptor: /proc/thread-self/fd, else a second descriptor checked to be
// the same directory. On failure `reason` is the rule alone ("cannot be made private: ...").
bool SetPrivateMode(const std::string &dir, int fd, const struct stat &st, std::string &reason);

// The directory `path` is in: "/" for a name directly under it, or for a path with no '/'.
std::string Parent(const std::string &path);

// The text of an errno value, for reasons.
std::string ErrnoText(int error_number);

// ---------------------------------------------------------------------------------------------
// Loading
// ---------------------------------------------------------------------------------------------

// Platform hooks, one definition per platform file.
// The platform loader's flags: Linux RTLD_NOW | RTLD_LOCAL | RTLD_DEEPBIND, in every
// build; macOS RTLD_NOW | RTLD_LOCAL.
int PlatformDlopenFlags();
// The variables the platform loader itself reads that could make it load another file as the driver or
// under it, refused before the load like the load-phase names (macOS: the DYLD_ search paths; Linux:
// none, since glibc searches nothing for a path with a '/').
std::vector<std::string> PlatformLoaderVariables();
// The library in `prerequisites` that the platform loader's error says is missing, unreadable or of
// the wrong version, or nullptr.
const std::string *MissingPrerequisite(const char *dlerror_text, const std::vector<std::string> &prerequisites);
// 1 when `dir` is on a file system mounted noexec, 0 when not, -1 when that cannot be told (Linux:
// statvfs fails). The pre-load check counts -1 as a failure of the base, unlike IsNoExecMount.
int NoExecState(const std::string &dir);
// A name for the open descriptor `fd` that the platform loader resolves to the same file (Linux
// "/proc/thread-self/fd/<fd>"), or empty where there is none (no /proc; always on macOS).
std::string DescriptorPath(int fd);
// After the load: whether the file the platform loader mapped for the image that holds `address` is the checked
// one (`checked`'s device and inode). macOS: the file dyld names for the image (dladdr, then stat), since macOS
// loads the checked path and dyld may have resolved it elsewhere through a DYLD_ search variable it read at
// start-up; false when dyld names no absolute file. Linux: true, since the descriptor's name it loads was checked to
// lead to the hashed file just before the call, or, where /proc is missing, it loads the checked path, which
// CheckInstalledSet opened and hashed; and glibc searches nothing for a path with a '/'.
bool LoadedFileIsChecked(const void *address, const struct stat &checked);
#if defined(__linux__)
// The platform loader's search list for the main program and for the object this code is in, which
// adds a loadable extension's own run paths (Linux: dlinfo RTLD_DI_SERINFO on each), in search order
// and without repeats; false when the main program's list cannot be read, or the extension's (dlinfo or
// dladdr1 fails): a list that cannot be read is not a list that was checked. An empty element appears
// as "." there.
bool PlatformSearchList(std::vector<std::string> &directories);
// Linux only: PlatformSearchList for the object that holds `own` in place of the one this code is in.
bool PlatformSearchListOf(const void *own, std::vector<std::string> &directories);
// Linux only: the main program's search list (dlopen(nullptr) and dlinfo), appended without repeats; false
// when it cannot be read.
bool ProgramSearchList(std::vector<std::string> &directories);
// Linux only: PlatformSearchListOf with the main program's list read by `program`, so a test can make that
// half fail (glibc almost never fails it, and no input makes it).
bool PlatformSearchListFrom(bool (*program)(std::vector<std::string> &), const void *own,
                            std::vector<std::string> &directories);
// Linux only: the search list of the object that holds `address` (dladdr1 RTLD_DL_LINKMAP, so no file
// is opened), appended without repeats; false when it cannot be read. PlatformSearchList's second half.
bool ObjectSearchList(const void *address, std::vector<std::string> &directories);
// Whether an object that answers to the library `name` is already loaded in this process, decided
// from the loaded objects alone: no file is opened and no directory searched (Linux: dl_iterate_phdr,
// an object answering to the name when its DT_SONAME is the name, or its path is, which are two of the
// three ways glibc matches a bare name). The third, the name an object was first asked for, is not
// visible, so an object loaded under that name alone is reported as not loaded: the search-list check
// is then stricter, never weaker, than the platform loader.
bool LibraryIsMapped(const std::string &name);
#endif
// Whether `dlopen(name, RTLD_NOLOAD)` finds an object loaded under `name`, a path. False for a name
// with no '/', which is not looked up: dlopen would search the search list for it, opening its files.
bool PathIsLoaded(const std::string &name);
#if defined(__linux__)
// The names the driver's DT_NEEDED entries give (an ELF64 file in memory, bounds checked); empty for
// anything else. Names with a '/' or other unusual character are left out.
std::vector<std::string> ElfNeededLibraries(const unsigned char *data, size_t size);
#endif

// Why the load is refused before the driver is loaded (step 7). The list carries no mark of where an
// element came from (dlinfo gives every one flags 0), and the environment is not read to find out, so the
// cause is what the list or the cache shows, and the remedy is chosen from that.
enum class SearchCause : uint8_t {
	RELATIVE,         // an element is empty or relative: it searches the current directory
	CACHE,            // the file found for a library the driver needs (cache or system directory), or its directory
	UNREADABLE,       // dlinfo or dladdr1 failed, for the main program's list or the extension's: the list is unknown
	CACHE_UNREADABLE, // the cache exists and cannot be read or understood
	MISSING           // neither the cache nor a system directory has a library the driver needs (MISSING_PREREQUISITE)
};
struct SearchRefusal {
	SearchCause cause = SearchCause::RELATIVE;
	std::string rule;    // the rule broken, without a path ("is empty or relative", ...)
	std::string where;   // the directory or file it is about, for `attempts` only
	std::string library; // MISSING: the library's name
};

#if defined(__linux__)
// The library cache of the Linux platform loader.
constexpr const char *LIBRARY_CACHE_PATH = "/etc/ld.so.cache";
// The flags of this platform's entries in it (glibc's _DL_CACHE_DEFAULT_ID: FLAG_ELF_LIBC6 with
// FLAG_X8664_LIB64, or with FLAG_AARCH64_LIB64), which the platform loader alone takes.
#if defined(__x86_64__)
constexpr int32_t LIBRARY_CACHE_FLAGS = 0x0303;
#define REDSHIFT_LIBRARY_TRIPLET "x86_64-linux-gnu"
#elif defined(__aarch64__)
constexpr int32_t LIBRARY_CACHE_FLAGS = 0x0a03;
#define REDSHIFT_LIBRARY_TRIPLET "aarch64-linux-gnu"
#else
#error "the Linux loader is built for linux_amd64 and linux_arm64 only"
#endif

// What reading the cache came to: it does not exist, it was read, or it exists and could not be (not a
// regular file, too large, unreadable or in no format this reader knows).
enum class CacheRead : uint8_t { ABSENT, PARSED, UNREADABLE };
// `paths[i]` is the full path the cache gives for `names[i]` on this platform, or empty: the first entry
// whose key is the name, whose flags are LIBRARY_CACHE_FLAGS and that has no hardware capabilities (one with
// them is in a subdirectory for some processors only). The key must be the name exactly: ldconfig keys an entry
// by its file's soname, and a file loaded first must answer to the name by its soname, so a key glibc's own
// lookup would also take ('libkeyutils.so.01', whose digits compare as numbers) can never serve. Reads glibc's new
// format ("glibc-ld.so.cache1.1"), alone or after the old format's section. The file is opened without blocking and
// must be a regular file, so a FIFO put there cannot stop the connect.
CacheRead CachePaths(const char *cache_path, const std::vector<std::string> &names, std::vector<std::string> &paths);

// glibc's default system directories, where the platform loader looks for a library the cache does not have
// (or whose file is gone): the Debian family's multiarch pair, then the Red Hat family's lib64 pair. A host's
// glibc searches one of the pairs (Debian's /lib64 holds only the loader itself; its glibc then also searches
// /lib and /usr/lib, which are not looked in here and hold none of the driver's libraries), so for a library
// found this is the directory glibc would take it from.
inline std::vector<std::string> SystemLibraryDirectories() {
	return {"/lib/" REDSHIFT_LIBRARY_TRIPLET, "/usr/lib/" REDSHIFT_LIBRARY_TRIPLET, "/lib64", "/usr/lib64"};
}

// What the check reads, so tests can give each part.
struct SearchInputs {
	bool (*list)(std::vector<std::string> &directories) = PlatformSearchList;
	bool (*loaded)(const std::string &name) = LibraryIsMapped;
	const char *cache_path = LIBRARY_CACHE_PATH; // nullptr: no cache
	// Looked in, in order, for a library the cache has no file for.
	std::vector<std::string> system_directories = SystemLibraryDirectories();
	uid_t user = ::geteuid();
	std::vector<std::string> needed; // the driver's DT_NEEDED names; one is looked for only if not already loaded
};
#else
// macOS: dyld searches nothing for a path with a '/', and the driver's linked libraries are the absolute
// system paths verification allows (policy/macos/allowed-load-dylibs.txt), so there is no list to check
// and nothing to load first; the DYLD_ variables that would change that are refused instead
// (PlatformLoaderVariables), and the loaded file is checked afterwards (LoadedFileIsChecked).
struct SearchInputs {
	uid_t user = ::geteuid();
	std::vector<std::string> needed; // empty
};
#endif

// A library the driver needs that is loaded first, by `path`: a real path, checked.
struct NeededLibrary {
	std::string name;
	std::string path;
};

#if defined(__linux__)
// False, with `refusal`, when the platform loader's search list cannot be read or has an
// empty or relative element, which searches the current directory; or when, for a name of `needed` that no
// loaded object answers to, the library cache cannot be read (a cache that exists), neither it nor a system
// directory has a file for it (MISSING; the cache first, then `system_directories` in order, as glibc looks), or
// the file found, or its directory, is one another user could change (CACHE: the
// directory walked as given and as resolved; the file a regular file the user or root owns that no one else
// can write, after following its links, each the user's or root's, in a directory only the user and root
// can change). True with `preload` those names and the real paths of their files, in `needed`'s order, which
// the loader loads by path before the driver, so the platform loader finds them loaded and searches nothing.
// The only file opened is the cache (without blocking); the others are looked at with lstat.
bool CheckSearchList(const SearchInputs &inputs, SearchRefusal &refusal, std::vector<NeededLibrary> &preload);

// The sentence for a refusal: its cause and remedy, never a path or a value.
std::string SearchRefusalMessage(const SearchRefusal &refusal);
#endif

// Whether `directory`'s path as given walks only through directories as step 2 requires (a root-owned
// sticky one allowed) and links the user or root owns; `resolved` is where the kernel ends up. The
// last directory's own mode is not looked at. (Step 1's cache walk, also used for a library's directory.)
bool CheckGivenPath(const std::string &path, bool is_target, uid_t user, std::string &resolved, std::string &where,
                    std::string &reason);
// The kernel's own bound on symbolic links in one lookup (ELOOP).
constexpr int SYMLINK_LIMIT = 40;

// Whether `kind` is an outcome of the platform loader call or after it (final for the process) or
// of anything before (returned, and the next call runs again). Exhaustive on purpose. DriverLoader
// stores an outcome exactly when this says so. MISSING_PREREQUISITE is final also when no file is found
// for the library, before any call: the remedy is the same (install it, then restart).
bool IsFinalKind(DriverErrorKind kind);

// The hooks a load reads, which tests replace to reach the paths that need them; a shipped build keeps the
// defaults.
struct LoadHooks {
	// Whether an object is already loaded under a name: dlopen would reuse it in place of the file
	// (Linux: dlopen RTLD_NOLOAD).
	bool (*name_in_use)(const std::string &name) = PathIsLoaded;
	// A name for a descriptor the platform loader resolves to the same file; empty where there is none.
	std::string (*descriptor_path)(int fd) = DescriptorPath;
	// Whether the set's file system is mounted noexec (1, 0, or -1 when that cannot be told).
	int (*no_exec_state)(const std::string &dir) = NoExecState;
	// Called just before the platform loader is first called, for a test to count the calls.
	void (*on_platform_loader_call)() = nullptr;
	// Passed to CheckInstalledSet.
	void (*after_hash)(const std::string &path) = nullptr;
	// After the load: whether the file loaded is the checked one (LoadedFileIsChecked).
	bool (*loaded_file_is_checked)(const void *address, const struct stat &checked) = LoadedFileIsChecked;
};

// What LoadDriverFileSet needs besides the files.
struct LoadOptions {
	std::vector<std::string> prerequisites;            // policy/host-prerequisites.txt
	std::string prerequisite_hint;                     // how to get them
	std::vector<std::string> load_refused_environment; // REDSHIFT_DRIVER_LOAD_REFUSED_ENVIRONMENT
	const char *role = "the base";                     // the base by its role, for LOAD_FAILED's message
	SearchInputs search; // the search-list check's inputs, the driver's DT_NEEDED names among them
	LoadHooks hooks;
	// Set to true just before the platform loader is first called (for a needed library or the driver).
	// DriverLoader::Load keeps it in a local and reads it only to give an exception's error its kind (LOAD_FAILED after
	// the call, NO_USABLE_DIRECTORY before); whether an outcome is stored is decided by IsFinalKind on its kind.
	bool *platform_loader_called = nullptr;
};

// The ways a load ends, which the types keep apart: the driver is loaded and every
// entry point resolved (LOADED); the platform loader was called, for a library the driver needs or for the
// driver, and the outcome is final, since that library's or the driver's start-up code may have run (FINAL);
// or it was never called and no foreign code ran, for one of three reasons: the set was missing or invalid,
// which a repair may mend (CHANGED); this base cannot be used as it is, and the same set would meet the same
// fact again (BASE_FAILED, which includes a set the check could not judge at all, as the install also reads
// it); or the environment is refused, which is about no base (REFUSED_ENVIRONMENT, with `error`, whose kind
// is REFUSED_ENVIRONMENT, or MISSING_PREREQUISITE when no file is found for a library, which is stored as
// its kind says).
enum class LoadResult : uint8_t { LOADED, FINAL, CHANGED, BASE_FAILED, REFUSED_ENVIRONMENT };

// Step 5, for the set directory `dir` of `files` (the driver first): the walk from / down to
// and including the set, the no-exec test (a statvfs that fails is a failure of this base), every
// file's type, owner, mode and hash, the driver hashed through the one descriptor it is loaded
// from; the load-phase refused names, the search list and the needed libraries checked again; then each
// needed library no loaded object answers to, loaded by its checked path (RTLD_NOW | RTLD_LOCAL), and each
// must then answer to its name; then, on Linux, dlopen("/proc/thread-self/fd/<n>") after RTLD_NOLOAD showed
// the name unused; then dlsym of every entry point. Writes no file. LOADED fills `out.api` and
// `out.identity`'s files and directory. REFUSED_ENVIRONMENT sets `error` (about the environment and not this
// base). CHANGED and BASE_FAILED set `where` and `reason` (the rule, no path) and leave `error` a default one.
// FINAL sets `error` (LOAD_FAILED, MISSING_PREREQUISITE or MISSING_ENTRY_POINT; its attempts hold the directory,
// or the path of a library loaded first, and the platform loader's own text, which hold paths and must never be
// shown to SQL). The handle is never closed. Not thread-safe by itself: the caller serialises.
LoadResult LoadDriverFileSet(const std::string &dir, const std::vector<DriverFile> &files, const LoadOptions &options,
                             LoadedDriver &out, DriverError &error, std::string &where, std::string &reason);

// Step 7's first half: the first of `names` that is set in the environment (an empty value counts as
// set), or nullptr. The value is never read. FindRefusedEnvironmentVariable (odbc_driver.hpp), the client's
// check, applies the same rule to the driver's whole list; it is the interface's own, so neither calls the other.
const char *FindSetVariable(const std::vector<std::string> &names);

// The REFUSED_ENVIRONMENT error for a set load-phase variable, in the client's own refusal wording.
DriverError RefusedVariableError(const std::string &name);
// The REFUSED_ENVIRONMENT error for a set PlatformLoaderVariables() name: the system's loader reads it.
DriverError LoaderVariableError(const std::string &name);
#if defined(__linux__)
// The REFUSED_ENVIRONMENT error for an allocator that has replaced the C library's (allocator_replacement.hpp says
// why the driver cannot share a process with one). The message names the object's file name ("another allocator"
// when no loaded object holds the address), never its path, which goes in the attempt.
DriverError AllocatorReplacementError(const AllocatorReplacement &replacement);
#endif

// The driver's environment handles are allocated without an odbc.ini it could read its logging settings from
// (the driver reads one whenever its environment is allocated). AllocateWithoutOdbcIni calls
// `api.AllocHandle(SQL_HANDLE_ENV, SQL_NULL_HANDLE, environment)` with ODBCINI set to /dev/null, which holds
// nothing, and then unsets it again, as it was: a set ODBCINI is refused before every connect, so that check
// still works afterwards. The allocation is the only thing that runs while it is set.
// SQL_ERROR, and no call, when ODBCINI is already set (its value is not read) or cannot be set. Not thread-safe by
// itself: DriverLoader::AllocateEnvironment serialises it with the load. A test gives `failing_set` to stand in for
// setting it (a setter that fails); otherwise it is set here, the one place the loader sets a variable.
constexpr const char *ODBC_INI_VARIABLE = "ODBCINI";
constexpr const char *NO_ODBC_INI = "/dev/null";
SQLRETURN AllocateWithoutOdbcIni(const OdbcApi &api, SQLHENV *environment, int (*failing_set)() = nullptr);

// What the loader needs from the process, besides the driver and policy it was built with. The
// defaults read the process's own.
struct LoadInputs {
	BaseInputs bases;
	SearchInputs search; // its `needed` is the loader's config's, whatever is given here
	LoadHooks hooks;
};
LoadInputs CurrentLoadInputs();

// Everything LoadEmbeddedDriver loads, so tests can give the same sequence a test payload.
struct LoaderConfig {
	DriverSet set;
	DriverIdentity identity; // version, platform, development, set_sha256; files and directory are filled in
	std::vector<std::string> refused_environment;
	std::vector<std::string> load_refused_environment;
	std::vector<OverriddenVariable> overridden_environment;
	std::vector<std::string> prerequisites;
	std::string prerequisite_hint;
	std::vector<std::string> needed; // the driver's DT_NEEDED names
};

// What LoadEmbeddedDriver loads: the embedded set and the generated policy lists (driver_embedded.cpp,
// the one definition), so a test can give the shipped configuration to a DriverLoader of its own.
LoaderConfig EmbeddedConfig();

// The run-time sequence, whose steps the comments in these files number: 0 the environment handle
// (allocated without an odbc.ini), 1 the base directories, 2 the ancestry rule, 3 the fast path,
// 4 install or repair under the lock, 5 the pre-load check, 6 retry and fall through before the load,
// 7 the load, once, and 8 the result. Serialised by a mutex. An outcome IsFinalKind
// calls final (every one from the first platform loader call on, a success's too, and MISSING_PREREQUISITE)
// is stored and every later call returns it; otherwise each call runs again from step 1 with the inputs it
// is given. LoadEmbeddedDriver has one for the process.
class DriverLoader {
public:
	explicit DriverLoader(LoaderConfig config);
	DriverLoadResult Load(const LoadInputs &inputs);
	// Calls `inputs` only when no outcome is stored yet.
	DriverLoadResult Load(const std::function<LoadInputs()> &inputs);
	// AllocateWithoutOdbcIni under the mutex Load holds (AllocateEnvironmentHandle).
	SQLRETURN AllocateEnvironment(const OdbcApi &api, SQLHENV *environment);

private:
	DriverLoadResult Attempt(const LoadInputs &inputs, bool &platform_loader_called);
	LoadOptions Options(const LoadInputs &inputs, const char *role) const;

	LoaderConfig config_;
	std::mutex mutex_;
	bool final_ = false;  // an outcome IsFinalKind calls final is stored
	bool loaded_ = false; // and it succeeded
	DriverError final_error_;
	LoadedDriver driver_;
};

} // namespace detail
} // namespace redshift_driver
} // namespace duckdb
