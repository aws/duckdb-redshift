//===----------------------------------------------------------------------===//
// Installing, re-checking and repairing the private driver set.
// Common POSIX code.
//===----------------------------------------------------------------------===//

#include "driver_loader_core.hpp"
#include "sha256.hpp"

#include <cerrno>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <ctime>
#include <mutex>
#include <map>
#include <set>

#include <dirent.h>
#include <fcntl.h>
#include <sys/file.h>
#include <sys/resource.h>
#include <sys/stat.h>
#include <unistd.h>

namespace duckdb {
namespace redshift_driver {
namespace detail {

std::vector<DriverFile> DriverSet::Manifest() const {
	std::vector<DriverFile> manifest;
	manifest.reserve(files.size());
	for (const auto &file : files) {
		manifest.push_back({file.name, file.sha256});
	}
	return manifest;
}

namespace {

const char *const SET = "the driver set";
const char *const SET_FILE = "a file of the driver set";
const char *const STAGING = "the staging directory";
// Step 3's read-only base: its driver root cannot be written, and holds no valid set.
const char *const READ_ONLY = "the driver root holds no valid copy of this driver version and cannot be written";

// A finding about the set: `state`, and for anything but VALID the path and the rule.
SetState Found(SetState state, const std::string &path, std::string rule, std::string &where, std::string &reason) {
	where = path;
	reason = std::move(rule);
	return state;
}

// Step 2's owner and mode rule for the set directory or one of its files, on what fstat of its
// descriptor says, or fstatat of its name when it could not be opened.
bool PassesRule(const struct stat &st, uid_t user, const std::string &what, std::string &rule) {
	if (st.st_uid != user && st.st_uid != 0) {
		rule = what + " is owned by another user";
	} else if ((st.st_mode & (S_IWGRP | S_IWOTH)) != 0) {
		// Sticky or not: unlike an ancestor, the set's own entries are what is loaded.
		rule = what + " is writable by another user";
	} else {
		return true;
	}
	return false;
}

// The ACL half (macOS): no entry that lets another account change the set's directory or
// files, read through the descriptor whose owner, mode and hash are checked. False with `rule` and `state`
// otherwise: an entry that grants a write right is evidence the set is INVALID; an ACL that cannot be read is
// not, and leaves it UNCHECKED.
bool PassesAclRule(AclRuleHook acl_rule, const std::string &path, int fd, uid_t user, const std::string &what,
                   std::string &rule, SetState &state) {
	const auto acl = acl_rule(path, fd, user, false);
	if (acl.finding == AclFinding::NONE) {
		return true;
	}
	rule = what + " " + acl.rule;
	state = acl.finding == AclFinding::UNREADABLE ? SetState::UNCHECKED : SetState::INVALID;
	return false;
}

// An entry of the set that could not be opened, judged by what fstatat says of it without following
// a link: a link, the wrong type (`regular`: a file, else a directory), owner or mode is still
// positive evidence; otherwise UNCHECKED with `unchecked`.
SetState Judge(int dir_fd, const std::string &name, bool regular, uid_t user, const std::string &what,
               std::string unchecked, const std::string &path, std::string &where, std::string &reason) {
	struct stat st;
	if (::fstatat(dir_fd, name.c_str(), &st, AT_SYMLINK_NOFOLLOW) == 0) {
		std::string rule;
		if (S_ISLNK(st.st_mode)) {
			rule = what + " is a symbolic link";
		} else if (regular ? !S_ISREG(st.st_mode) : !S_ISDIR(st.st_mode)) {
			rule = what + (regular ? " is not a regular file" : " is not a directory");
		}
		if (!rule.empty() || !PassesRule(st, user, what, rule)) {
			return Found(SetState::INVALID, path, rule, where, reason);
		}
	}
	return Found(SetState::UNCHECKED, path, std::move(unchecked), where, reason);
}

// The names in the directory `fd` refers to, other than "." and "..". Leaves `fd` open.
bool ListNames(int fd, std::set<std::string> &names, int &error) {
	const int listing = ::fcntl(fd, F_DUPFD_CLOEXEC, 0);
	DIR *handle = listing < 0 ? nullptr : ::fdopendir(listing);
	if (handle == nullptr) {
		error = errno;
		if (listing >= 0) {
			::close(listing);
		}
		return false;
	}
	errno = 0;
	while (const auto *entry = ::readdir(handle)) {
		const std::string name = entry->d_name;
		if (name != "." && name != "..") {
			names.insert(name);
		}
		errno = 0;
	}
	error = errno;
	::closedir(handle);
	return error == 0;
}

bool WriteAll(int fd, const unsigned char *data, size_t size) {
	while (size > 0) {
		const ssize_t written = ::write(fd, data, size);
		if (written < 0) {
			if (errno == EINTR) {
				continue;
			}
			return false;
		}
		if (written == 0) {
			// No progress and no error, as a FUSE daemon may answer: retrying would spin forever.
			errno = EIO;
			return false;
		}
		data += written;
		size -= static_cast<size_t>(written);
	}
	return true;
}

// The staging directory this call made, removed with what it wrote as soon as the install fails,
// through its descriptor. Only a directory still at its name with the same inode is removed, except
// when it could not be opened at all (inode_ is 0): then it is the empty directory mkdtemp just made,
// removed by name. rmdir removes only an empty one; anything left is the stale sweep's.
class Staging {
public:
	Staging(int root_fd, std::string root) : root_fd_(root_fd), root_(std::move(root)) {
	}
	~Staging() {
		Remove();
	}
	Staging(const Staging &) = delete;
	Staging &operator=(const Staging &) = delete;

	// mkdtemp, then mode 0700 through an O_PATH descriptor, as the driver root's: mkdtemp's 0700 is
	// narrowed by the umask, which may leave the owner no read permission to open it with.
	bool Create(std::string &where, std::string &reason) {
		auto path = root_ + "/" + STAGING_PREFIX + "XXXXXX";
		where = root_;
		if (::mkdtemp(&path[0]) == nullptr) {
			const int error = errno;
			// A base the user cannot write may still hold a valid set; this one
			// does not, so the next base is tried.
			reason = error == EACCES || error == EPERM || error == EROFS
			             ? READ_ONLY
			             : std::string(STAGING) + " cannot be created: " + ErrnoText(error);
			return false;
		}
		path_ = path;
		name_ = path.substr(root_.size() + 1);
		owned_ = true;
		where = path_;
#ifdef O_PATH
		const UniqueFd opened(::openat(root_fd_, name_.c_str(), O_PATH | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC));
#else
		const UniqueFd opened(::openat(root_fd_, name_.c_str(), O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC));
#endif
		struct stat st;
		if (opened.get() < 0 || ::fstat(opened.get(), &st) != 0) {
			const int error = errno;
			reason = std::string(STAGING) + " cannot be opened after it was created: " + ErrnoText(error);
			return false;
		}
		device_ = st.st_dev;
		inode_ = st.st_ino;
		if (!SetPrivateMode(path_, opened.get(), st, reason)) {
			reason = std::string(STAGING) + " " + reason;
			return false;
		}
		// Readable now, for fsync, which an O_PATH descriptor cannot do. Only the user and root can
		// write the root; whatever is renamed into place is checked again afterwards.
		fd_ = ::openat(root_fd_, name_.c_str(), O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC);
		if (fd_ < 0) {
			const int error = errno;
			reason = std::string(STAGING) + " cannot be opened after it was created: " + ErrnoText(error);
			return false;
		}
		return true;
	}

	// Each file O_CREAT | O_EXCL | O_NOFOLLOW relative to the staging directory, so nothing at its
	// name is followed or reused, then mode 0500 through the descriptor whatever the umask, and
	// flushed; then the directory itself.
	bool Write(const DriverSet &set, std::string &where, std::string &reason) {
		const std::string what = SET_FILE;
		names_.reserve(set.files.size());
		for (const auto &file : set.files) {
			names_.push_back(file.name);
		}
		for (const auto &file : set.files) {
			where = path_ + "/" + file.name;
			const UniqueFd out(
			    ::openat(fd_, file.name.c_str(), O_WRONLY | O_CREAT | O_EXCL | O_NOFOLLOW | O_CLOEXEC, 0500));
			if (out.get() < 0) {
				const int error = errno;
				reason = what + " cannot be created: " + ErrnoText(error);
				return false;
			}
			if (::fchmod(out.get(), 0500) != 0) {
				const int error = errno;
				reason = what + " cannot be given its mode: " + ErrnoText(error);
				return false;
			}
			if (!WriteAll(out.get(), file.data, file.size)) {
				const int error = errno;
				reason = what + " cannot be written: " + ErrnoText(error);
				return false;
			}
			if (::fsync(out.get()) != 0) {
				const int error = errno;
				reason = what + " cannot be flushed: " + ErrnoText(error);
				return false;
			}
		}
		where = path_;
		if (::fsync(fd_) != 0) {
			const int error = errno;
			reason = std::string(STAGING) + " cannot be flushed: " + ErrnoText(error);
			return false;
		}
		return true;
	}

	// Renamed to `name` in the root with a plain rename, which every file system has. A directory
	// is never renamed over one that holds anything (EEXIST or ENOTEMPTY), nor over a file or a link
	// (ENOTDIR), so a set another process finished is never replaced; only an empty directory at
	// `name`, which is no set, would be. Sets `error` on failure.
	bool RenameTo(const std::string &name, int &error) {
		if (::renameat(root_fd_, name_.c_str(), root_fd_, name.c_str()) == 0) {
			Release();
			return true;
		}
		error = errno;
		return false;
	}

	// The staging directory's random part, and its identity, as created.
	std::string Suffix() const {
		return name_.substr(std::string(STAGING_PREFIX).size());
	}
	bool Is(const struct stat &st) const {
		return st.st_dev == device_ && st.st_ino == inode_;
	}

	void Remove() {
		// Only while the directory this call made is still at its staging name: a rename the file system
		// reported as failed may have happened all the same (an NFS client that retransmits it gets
		// ENOENT), and then fd_ is the installed set, which must not be emptied.
		struct stat st;
		if (owned_ && (inode_ == 0 || (::fstatat(root_fd_, name_.c_str(), &st, AT_SYMLINK_NOFOLLOW) == 0 && Is(st)))) {
			if (fd_ >= 0) {
				// Every name the set has, whoever put it there: the directory is this call's own.
				for (const auto &name : names_) {
					::unlinkat(fd_, name.c_str(), 0);
				}
			}
			::unlinkat(root_fd_, name_.c_str(), AT_REMOVEDIR);
		}
		Release();
	}

private:
	// Nothing left to remove: renamed into place, or removed.
	void Release() {
		if (fd_ >= 0) {
			::close(fd_);
		}
		fd_ = -1;
		owned_ = false;
		names_.clear();
	}

	int root_fd_;
	std::string root_;
	std::string path_;
	std::string name_;
	bool owned_ = false; // name_ is this call's staging directory, still to be removed
	int fd_ = -1;
	dev_t device_ = 0;
	ino_t inode_ = 0;
	std::vector<std::string> names_;
};

const char *const LOCK_NOT_OURS = "the driver root's lock file is not a regular file the user owns";
// The reason a root is not written again by this process, followed by the rule the written set broke.
const char *const FAILED_HERE = "the file system did not keep the driver set this process wrote here";

bool HasPrefix(const std::string &name, const char *prefix) {
	return name.compare(0, std::char_traits<char>::length(prefix), prefix) == 0;
}

// `<root>/.lock`, held with flock for one install or repair, and released with LOCK_UN before it is
// closed.
class RootLock {
public:
	RootLock() = default;
	~RootLock() {
		if (fd_ >= 0) {
			if (locked_) {
				::flock(fd_, LOCK_UN);
			}
			::close(fd_);
		}
	}
	RootLock(const RootLock &) = delete;
	RootLock &operator=(const RootLock &) = delete;

	// Opening the lock is the read-only probe: a driver root the user cannot write refuses it with
	// EACCES, EPERM or EROFS. O_NONBLOCK, so a FIFO at the name fails the type check instead of
	// hanging. A holder that does not let go within the deadline, a stopped process say, makes this
	// call move on rather than wait for ever.
	bool Take(int root_fd, uid_t user, int deadline_milliseconds, std::string &reason) {
		fd_ = ::openat(root_fd, LOCK_NAME, O_RDWR | O_CREAT | O_NOFOLLOW | O_NONBLOCK | O_CLOEXEC, 0600);
		int open_error = fd_ < 0 ? errno : 0;
		if (open_error == EACCES && MadeOpenable(root_fd, user)) {
			fd_ = ::openat(root_fd, LOCK_NAME, O_RDWR | O_CREAT | O_NOFOLLOW | O_NONBLOCK | O_CLOEXEC, 0600);
			open_error = fd_ < 0 ? errno : 0;
		}
		if (fd_ < 0) {
			if (open_error == EACCES || open_error == EPERM || open_error == EROFS) {
				reason = READ_ONLY;
			} else if (open_error == ELOOP || open_error == EISDIR || open_error == ENXIO) {
				reason = LOCK_NOT_OURS; // a link, a directory, a socket
			} else {
				reason = "the driver root's lock file cannot be opened: " + ErrnoText(open_error);
			}
			return false;
		}
		struct stat st;
		if (::fstat(fd_, &st) != 0) {
			const int error = errno;
			reason = "the driver root's lock file cannot be examined: " + ErrnoText(error);
			return false;
		}
		if (!S_ISREG(st.st_mode) || st.st_uid != user) {
			reason = LOCK_NOT_OURS;
			return false;
		}
		// Through the descriptor, whatever the umask: one that made it 0400 would make every later
		// open of it fail, and the root look read-only for good.
		if ((st.st_mode & 07777) != 0600 && ::fchmod(fd_, 0600) != 0) {
			const int error = errno;
			reason = "the driver root's lock file cannot be given its mode: " + ErrnoText(error);
			return false;
		}
		const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(deadline_milliseconds);
		unsigned wait = 1000;
		while (::flock(fd_, LOCK_EX | LOCK_NB) != 0) {
			const int error = errno;
			if (error == EWOULDBLOCK && std::chrono::steady_clock::now() < deadline) {
				::usleep(wait);
				wait = wait < 50000 ? wait * 2 : 100000;
				continue;
			}
			if (error == EWOULDBLOCK || error == EINTR) {
				reason = "another process is installing the driver";
			} else if (error == ENOLCK || error == EBADF) {
				reason = "the driver root's file system does not support locking";
			} else {
				reason = "the driver root's lock cannot be taken: " + ErrnoText(error);
			}
			return false;
		}
		locked_ = true;
		return true;
	}

private:
	// A `.lock` of the user's that its mode (from an older loader under a narrow umask) makes
	// unopenable is made 0600 through an O_PATH descriptor's /proc entry; one already 0600, which a
	// concurrent first install fixed meanwhile, is opened again as it is. Anything else (a link,
	// not a regular file, another user's lock) is left as it is, and the root stays read-only.
	static bool MadeOpenable(int root_fd, uid_t user) {
#if defined(__linux__) && defined(O_PATH)
		const UniqueFd handle(::openat(root_fd, LOCK_NAME, O_PATH | O_NOFOLLOW | O_CLOEXEC));
		struct stat st;
		if (handle.get() < 0 || ::fstat(handle.get(), &st) != 0 || !S_ISREG(st.st_mode) || st.st_uid != user) {
			return false;
		}
		if ((st.st_mode & 0600) == 0600) {
			return true;
		}
		return ::chmod(("/proc/thread-self/fd/" + std::to_string(handle.get())).c_str(), 0600) == 0;
#else
		(void)root_fd;
		(void)user;
		return false;
#endif
	}

	int fd_ = -1;
	bool locked_ = false;
};

// A directory the sweep found at `name` (`st`, from fstatat): removed with its regular files and
// links, through its own descriptor, unless it is on another device than the root or holds anything
// else. It is made 0700 first when it is not (a set's directory may be 0500, or unreadable), and a
// directory left in place keeps that mode: it is garbage, and nothing reads it.
void RemoveFlat(int root_fd, const std::string &root, const std::string &name, const struct stat &st,
                const struct stat &root_st) {
	if (st.st_dev != root_st.st_dev) {
		return;
	}
#ifdef O_PATH
	const UniqueFd handle(::openat(root_fd, name.c_str(), O_PATH | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC));
#else
	const UniqueFd handle(::openat(root_fd, name.c_str(), O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC));
#endif
	struct stat at;
	if (handle.get() < 0 || ::fstat(handle.get(), &at) != 0 || at.st_dev != st.st_dev || at.st_ino != st.st_ino) {
		return;
	}
	std::string ignored;
	if ((at.st_mode & 0700) != 0700 && !SetPrivateMode(root + "/" + name, handle.get(), at, ignored)) {
		return;
	}
	const UniqueFd dir(::openat(root_fd, name.c_str(), O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC));
	struct stat opened;
	if (dir.get() < 0 || ::fstat(dir.get(), &opened) != 0 || opened.st_dev != at.st_dev || opened.st_ino != at.st_ino) {
		return;
	}
	std::set<std::string> names;
	int error = 0;
	if (!ListNames(dir.get(), names, error)) {
		return;
	}
	// The set is flat: anything but a file or a link is not what a set or a staging directory holds,
	// so the whole entry is left as it is.
	for (const auto &entry : names) {
		struct stat inner;
		if (::fstatat(dir.get(), entry.c_str(), &inner, AT_SYMLINK_NOFOLLOW) != 0 ||
		    !(S_ISREG(inner.st_mode) || S_ISLNK(inner.st_mode))) {
			return;
		}
	}
	for (const auto &entry : names) {
		::unlinkat(dir.get(), entry.c_str(), 0);
	}
	::unlinkat(root_fd, name.c_str(), AT_REMOVEDIR);
}

void Sweep(int root_fd, const std::string &root, uid_t user, time_t now) {
	struct stat root_st;
	if (::fstat(root_fd, &root_st) != 0) {
		return; // which entries are on the root's file system cannot be told
	}
	std::set<std::string> names;
	int error = 0;
	ListNames(root_fd, names, error); // a listing cut short still names entries to sweep
	for (const auto &name : names) {
		const bool staging = HasPrefix(name, STAGING_PREFIX);
		if (!staging && !HasPrefix(name, INVALID_PREFIX)) {
			continue;
		}
		struct stat st;
		if (::fstatat(root_fd, name.c_str(), &st, AT_SYMLINK_NOFOLLOW) != 0 || st.st_uid != user ||
		    (!staging && now - st.st_mtime < STALE_SECONDS)) {
			continue;
		}
		if (!S_ISDIR(st.st_mode)) {
			::unlinkat(root_fd, name.c_str(), 0); // a link is removed, never followed
			continue;
		}
		RemoveFlat(root_fd, root, name, st, root_st);
	}
}

// Moves the entry `from` in the root to a new `.invalid-` name: `suffix`, the staging directory's
// random part, with a count when an older copy has it. A plain rename onto a name that holds nothing,
// looked at first: only the user and root can write the root, and this runs under its lock. Dated now
// first, so the sweep keeps it for STALE_SECONDS from the repair, not from when it was written, even
// if this process dies between the two. False, with `error`, when it could not be moved.
bool SetAside(int root_fd, const std::string &from, const std::string &suffix, int &error) {
	::utimensat(root_fd, from.c_str(), nullptr, AT_SYMLINK_NOFOLLOW);
	for (int count = 0; count < 100; count++) {
		const auto name = INVALID_PREFIX + suffix + (count == 0 ? std::string() : "-" + std::to_string(count));
		struct stat st;
		if (::fstatat(root_fd, name.c_str(), &st, AT_SYMLINK_NOFOLLOW) == 0) {
			continue;
		}
		if (errno != ENOENT) {
			error = errno;
			return false;
		}
		if (::renameat(root_fd, from.c_str(), root_fd, name.c_str()) == 0) {
			return true;
		}
		error = errno;
		return false;
	}
	error = EEXIST;
	return false;
}

// The driver roots under which this process wrote a set that then failed its check, by device,
// inode, path and set hash, each with the rule it broke: it does not write there again. Never
// destroyed, so a host's exit cannot race a thread still installing.
std::mutex &FailedHereMutex() {
	// NOLINTNEXTLINE(cppcoreguidelines-avoid-non-const-global-variables): clang-tidy 15 flags function-local statics
	static auto &mutex = *new std::mutex();
	return mutex;
}
std::map<std::string, std::string> &FailedHere() {
	// NOLINTNEXTLINE(cppcoreguidelines-avoid-non-const-global-variables): clang-tidy 15 flags function-local statics
	static auto &keys = *new std::map<std::string, std::string>();
	return keys;
}
std::string FailedKey(int root_fd, const std::string &root, const std::string &set_sha256) {
	struct stat st;
	if (::fstat(root_fd, &st) != 0) {
		return root + "\n" + set_sha256;
	}
	return std::to_string(st.st_dev) + ":" + std::to_string(st.st_ino) + ":" + root + "\n" + set_sha256;
}
// Whether this process's set under `key` failed, and the rule it broke.
bool FailedInThisProcess(const std::string &key, std::string &rule) {
	const std::lock_guard<std::mutex> guard(FailedHereMutex());
	const auto found = FailedHere().find(key);
	if (found == FailedHere().end()) {
		return false;
	}
	rule = found->second;
	return true;
}

// Records `rule` under `key` in this process, which does not write that set under that root again.
void RememberFailed(const std::string &key, const std::string &rule) {
	const std::lock_guard<std::mutex> guard(FailedHereMutex());
	FailedHere()[key] = rule;
}

} // namespace

SetState CheckInstalledSet(const std::string &set_dir, const std::vector<DriverFile> &files, uid_t user,
                           std::string &where, std::string &reason, int *first_file_fd,
                           void (*after_hash)(const std::string &path), AclRuleHook acl_rule) {
	if (first_file_fd != nullptr) {
		*first_file_fd = -1;
	}
	// The directories above the set are the base's and the driver root's, which a repair must not
	// touch: a refusal there says nothing about the set itself.
	if (!CheckAncestry(Parent(set_dir), AncestryEnd::DRIVER_ROOT, user, where, reason, acl_rule)) {
		return SetState::UNCHECKED;
	}
	const std::string set = SET;
	const auto root = Parent(set_dir);
#ifdef O_PATH
	const UniqueFd root_fd(::openat(AT_FDCWD, root.c_str(), O_PATH | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC));
#else
	const UniqueFd root_fd(::openat(AT_FDCWD, root.c_str(), O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC));
#endif
	if (root_fd.get() < 0) {
		const int error = errno;
		return Found(SetState::UNCHECKED, root, "the driver root cannot be opened: " + ErrnoText(error), where, reason);
	}
	const auto set_name = set_dir.substr(set_dir.find_last_of('/') + 1);
	const UniqueFd dir(::openat(root_fd.get(), set_name.c_str(), O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC));
	if (dir.get() < 0) {
		const int error = errno;
		if (error == ENOENT) {
			return Found(SetState::MISSING, set_dir, set + " does not exist", where, reason);
		}
		// Linux reports a link opened O_DIRECTORY | O_NOFOLLOW as ENOTDIR.
		return Judge(root_fd.get(), set_name, false, user, set, set + " cannot be opened: " + ErrnoText(error), set_dir,
		             where, reason);
	}
	struct stat st;
	std::string rule;
	if (::fstat(dir.get(), &st) != 0) {
		const int error = errno;
		return Found(SetState::UNCHECKED, set_dir, set + " cannot be examined: " + ErrnoText(error), where, reason);
	}
	if (!PassesRule(st, user, set, rule)) {
		return Found(SetState::INVALID, set_dir, rule, where, reason);
	}
	SetState acl_state = SetState::INVALID;
	if (!PassesAclRule(acl_rule, set_dir, dir.get(), user, set, rule, acl_state)) {
		return Found(acl_state, set_dir, rule, where, reason);
	}
	std::set<std::string> names;
	int error = 0;
	if (!ListNames(dir.get(), names, error)) {
		return Found(SetState::UNCHECKED, set_dir, set + " cannot be listed: " + ErrnoText(error), where, reason);
	}
	// Exactly the manifest's names: an amazon.redshiftodbc.ini, which can turn on tracing, or a
	// root.crt the driver would trust, is never part of a valid set.
	std::set<std::string> expected;
	for (const auto &file : files) {
		expected.insert(file.name);
	}
	for (const auto &name : names) {
		if (expected.count(name) == 0) {
			return Found(SetState::INVALID, set_dir + "/" + name, set + " holds a file that is not part of it", where,
			             reason);
		}
	}
	const std::string what = SET_FILE;
	// The first file's descriptor, kept for the caller that loads it (step 5), closed unless the whole
	// set is valid.
	UniqueFd kept(-1);
	for (size_t index = 0; index < files.size(); index++) {
		const auto &file = files[index];
		const auto path = set_dir + "/" + file.name;
		// Owner, mode and hash from the one descriptor, so the bytes hashed are those of the file
		// that passed. O_NONBLOCK: a FIFO at the name fails the type check instead of hanging.
		const UniqueFd in(::openat(dir.get(), file.name.c_str(), O_RDONLY | O_NOFOLLOW | O_NONBLOCK | O_CLOEXEC));
		if (in.get() < 0) {
			const int open_error = errno;
			if (open_error == ENOENT) {
				return Found(SetState::INVALID, path, what + " is missing", where, reason);
			}
			// ELOOP for a link; ENXIO for a socket; EACCES for one the user cannot read.
			return Judge(dir.get(), file.name, true, user, what, what + " cannot be opened: " + ErrnoText(open_error),
			             path, where, reason);
		}
		if (::fstat(in.get(), &st) != 0) {
			const int stat_error = errno;
			return Found(SetState::UNCHECKED, path, what + " cannot be examined: " + ErrnoText(stat_error), where,
			             reason);
		}
		if (!S_ISREG(st.st_mode)) {
			return Found(SetState::INVALID, path, what + " is not a regular file", where, reason);
		}
		if (!PassesRule(st, user, what, rule)) {
			return Found(SetState::INVALID, path, rule, where, reason);
		}
		if (!PassesAclRule(acl_rule, path, in.get(), user, what, rule, acl_state)) {
			return Found(acl_state, path, rule, where, reason);
		}
		std::string hex, hash_error;
		if (!Sha256Fd(in.get(), hex, hash_error)) {
			return Found(SetState::UNCHECKED, path, what + " " + hash_error, where, reason);
		}
		if (hex != file.sha256) {
			return Found(SetState::INVALID, path, what + " does not match the embedded driver", where, reason);
		}
		if (after_hash != nullptr) {
			after_hash(path);
		}
		if (index == 0 && first_file_fd != nullptr) {
			const int duplicate = ::fcntl(in.get(), F_DUPFD_CLOEXEC, 3);
			const int dup_error = errno; // before the assignment, whose close() could change it
			kept = UniqueFd(duplicate);
			if (duplicate < 0) {
				return Found(SetState::UNCHECKED, path, what + " cannot be kept open: " + ErrnoText(dup_error), where,
				             reason);
			}
		}
	}
	where.clear();
	reason.clear();
	if (first_file_fd != nullptr) {
		*first_file_fd = kept.Release();
	}
	return SetState::VALID;
}

void SweepDriverRoot(const std::string &root, uid_t user, time_t now) {
	const UniqueFd root_fd(::open(root.c_str(), O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC));
	if (root_fd.get() >= 0) {
		Sweep(root_fd.get(), root, user, now);
	}
}

InstallResult InstallDriverSet(const std::string &root, const DriverSet &set, uid_t user, std::string &set_dir,
                               std::string &where, std::string &reason, int lock_deadline_milliseconds) {
	set_dir = root + "/" + set.set_sha256;
	const auto manifest = set.Manifest();
	// Step 3, the fast path: no lock, nothing written. An UNCHECKED set is no evidence; it is left as
	// it is and the base fails.
	auto state = CheckInstalledSet(set_dir, manifest, user, where, reason);
	if (state != SetState::MISSING && state != SetState::INVALID) {
		return state == SetState::VALID ? InstallResult::INSTALLED : InstallResult::FAILED;
	}
	// Step 4. A failure to repair an invalid set keeps the rule it broke in the reason.
	std::string broken = state == SetState::INVALID ? reason : std::string();
	const auto fail = [&](const std::string &at, const std::string &why) {
		where = at;
		reason = broken.empty() ? why : why + " (" + broken + ")";
		return InstallResult::FAILED;
	};
	const UniqueFd root_fd(::open(root.c_str(), O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC));
	if (root_fd.get() < 0) {
		const int error = errno;
		return fail(root, "the driver root cannot be opened: " + ErrnoText(error));
	}
	const auto failed_key = FailedKey(root_fd.get(), root, set.set_sha256);
	std::string failed_rule;
	if (FailedInThisProcess(failed_key, failed_rule)) {
		return fail(root, std::string(FAILED_HERE) + " (" + failed_rule + ")");
	}
	RootLock lock;
	std::string why;
	if (!lock.Take(root_fd.get(), user, lock_deadline_milliseconds, why)) {
		return fail(root, why);
	}
	// Under the lock: the sweep first, before this call makes a staging directory of its own.
	Sweep(root_fd.get(), root, user, ::time(nullptr));
	// Another process may have written or repaired the set while this one waited.
	state = CheckInstalledSet(set_dir, manifest, user, where, reason);
	if (state != SetState::MISSING && state != SetState::INVALID) {
		return state == SetState::VALID ? InstallResult::INSTALLED : InstallResult::FAILED;
	}
	broken = state == SetState::INVALID ? reason : std::string();
	// Only bytes that still match the build's record are written.
	for (const auto &file : set.files) {
		if (Sha256Hex(file.data, file.size) != file.sha256) {
			where.clear();
			reason = file.name + " in the extension does not match the hash recorded when it was built; reinstall the "
			                     "extension";
			return InstallResult::BYTES_CORRUPT;
		}
	}
	// A write past the limit raises SIGXFSZ, which kills the host; the base is refused instead.
	struct rlimit limit;
	if (::getrlimit(RLIMIT_FSIZE, &limit) == 0 && limit.rlim_cur != RLIM_INFINITY) {
		for (const auto &file : set.files) {
			if (file.size > limit.rlim_cur) {
				return fail(root, std::string(SET) + " cannot be written: " + SET_FILE +
				                      " is larger than the process's file size limit (RLIMIT_FSIZE)");
			}
		}
	}
	Staging staging(root_fd.get(), root);
	std::string at;
	if (!staging.Create(at, why) || !staging.Write(set, at, why)) {
		return fail(at, why);
	}
	const auto suffix = staging.Suffix();
	// Is the directory at the set's name the one this call staged? Only then is a set that fails the
	// check this call's own, however the rename that put it there reported itself.
	const auto wrote_what_is_there = [&]() {
		struct stat st;
		return ::fstatat(root_fd.get(), set.set_sha256.c_str(), &st, AT_SYMLINK_NOFOLLOW) == 0 && staging.Is(st);
	};
	// This call's own set, failing its check: set aside like any invalid set (left for the next
	// repair if it cannot be), and this base not written again by this process. Another process
	// tries once, and sees the rule.
	const auto set_aside_what_we_wrote = [&]() {
		const auto rule = reason;
		int ignored = 0;
		reason = std::string(SET) + " this process wrote does not pass the check (" + rule + ")";
		SetAside(root_fd.get(), set.set_sha256, suffix, ignored);
		RememberFailed(failed_key, rule);
		return InstallResult::FAILED;
	};
	// A missing set is renamed into place. An invalid one is first renamed aside, then the staging
	// directory renamed in, through SetAside and Staging::RenameTo, whose comment gives what a plain
	// rename cannot replace. An ENOENT from the rename aside means the invalid set is gone, whether
	// something that does not take the lock took it or this call's own rename made it and was told
	// it had failed (Staging::Remove); either way the name is free for the rename in.
	int error = 0;
	if (state == SetState::INVALID && !SetAside(root_fd.get(), set.set_sha256, suffix, error) && error != ENOENT) {
		staging.Remove();
		return fail(set_dir, std::string(SET) + " cannot be moved aside: " + ErrnoText(error));
	}
	if (!staging.RenameTo(set.set_sha256, error)) {
		staging.Remove();
		if (error != EEXIST && error != ENOTEMPTY && error != ENOTDIR && error != ENOENT) {
			return fail(set_dir, std::string(SET) + " cannot be put in place: " + ErrnoText(error));
		}
		// Something that does not take the lock put a set, or a file, there, or took the staging
		// directory away; or the rename happened although it was reported as failed (ENOENT; see
		// Staging::Remove). A valid set is used.
		state = CheckInstalledSet(set_dir, manifest, user, where, reason);
		if (state == SetState::VALID) {
			return InstallResult::INSTALLED;
		}
		if (state == SetState::UNCHECKED) {
			return InstallResult::FAILED;
		}
		// A set that fails its check and is the one this call staged came from this call's own
		// rename, which happened and reported ENOENT: it is set aside and remembered as if the
		// rename had reported success, so one failing process still leaves one set-aside copy
		// (step 4) rather than three tries ending in "kept changing".
		if (state != SetState::MISSING && wrote_what_is_there()) {
			return set_aside_what_we_wrote();
		}
		return InstallResult::RACED;
	}
	if (::fsync(root_fd.get()) != 0) {
		error = errno;
		return fail(root, "the driver root cannot be flushed: " + ErrnoText(error));
	}
	// The file system must keep what was written: its owner, its mode and its bytes.
	state = CheckInstalledSet(set_dir, manifest, user, where, reason);
	if (state == SetState::VALID) {
		return InstallResult::INSTALLED;
	}
	if (state == SetState::UNCHECKED) {
		// No evidence against the set this call wrote: it is left in place, and the base fails.
		reason = std::string(SET) + " this process wrote cannot be checked (" + reason + ")";
		return InstallResult::FAILED;
	}
	if (state == SetState::MISSING || !wrote_what_is_there()) {
		// Something that does not take the lock took this call's set away or put another there, as
		// when a rename loses: tried again, with the rule as the check found it.
		return InstallResult::RACED;
	}
	return set_aside_what_we_wrote();
}

bool PrepareWith(const BaseInputs &inputs, const DriverSet &set, uid_t user, const UseSet &use, std::string &set_dir,
                 DriverError &error) {
	DriverRoots roots(inputs, user);
	std::string root;
	while (roots.Next(root)) {
		std::string where, reason;
		// Step 6: a set something else changed meanwhile is tried again, a few times, with back-off.
		// What went wrong at the last try is what the base's attempt records.
		for (int tries = 1; tries <= INSTALL_TRIES; tries++) {
			if (tries > 1) {
				::usleep(INSTALL_RETRY_MICROSECONDS * static_cast<unsigned>(tries - 1));
			}
			const auto result =
			    InstallDriverSet(root, set, user, set_dir, where, reason, inputs.lock_deadline_milliseconds);
			if (result == InstallResult::BYTES_CORRUPT) {
				// Every base would be given the same bytes.
				error = DriverError();
				error.kind = DriverErrorKind::EMBEDDED_BYTES_CORRUPT;
				error.message = reason;
				error.attempts = roots.attempts;
				return false;
			}
			if (result == InstallResult::FAILED) {
				break;
			}
			if (result == InstallResult::RACED) {
				reason = std::string(SET) +
				         " kept changing while this process put its own in place (at the last try, " + reason + ")";
				continue;
			}
			const auto used = use(set_dir, roots, where, reason);
			if (used == SetUse::DONE) {
				return true;
			}
			if (used == SetUse::BASE_FAILED) {
				break;
			}
		}
		roots.Fail(where, reason);
	}
	set_dir.clear();
	error = NoUsableDirectory(roots.attempts);
	return false;
}

} // namespace detail
} // namespace redshift_driver
} // namespace duckdb
