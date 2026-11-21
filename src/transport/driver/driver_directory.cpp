//===----------------------------------------------------------------------===//
// Base directories for the private driver copy and the checks on them.
// Common POSIX code; the platform files provide the per-user cache path, the no-exec
// test, the mapped-file lookup, the ACL rule and the volume rule.
//===----------------------------------------------------------------------===//

#include "driver_loader_core.hpp"
#include "sha256.hpp"

#include <cerrno>
#include <cinttypes>
#include <climits>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <vector>

#include <dlfcn.h>
#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>

namespace duckdb {
namespace redshift_driver {
namespace detail {

std::string ErrnoText(int error_number) {
	return ErrnoMessage(error_number);
}

AclCheck AclReadFailure(int error) {
	if (error == ENOENT || error == ENOTSUP || error == EOPNOTSUPP) {
		return AclCheck();
	}
	return {AclFinding::UNREADABLE, std::string(ACL_UNREADABLE) + ": " + ErrnoText(error)};
}

bool NoAclToClear(int error) {
	return error == ENOTSUP || error == EOPNOTSUPP;
}

bool AclQualifierGrantsToAnother(AclQualifier who, uid_t id, uid_t user) {
	return who != AclQualifier::USER || (id != user && id != 0);
}

std::string Parent(const std::string &path) {
	const auto slash = path.find_last_of('/');
	if (slash == std::string::npos || slash == 0) {
		return "/";
	}
	return path.substr(0, slash);
}

namespace {

// realpath, or empty with `reason` set.
std::string Resolve(const std::string &path, std::string &reason) {
	char resolved[PATH_MAX];
	if (::realpath(path.c_str(), resolved) == nullptr) {
		const int error = errno;
		reason = "cannot be resolved: " + ErrnoText(error);
		return std::string();
	}
	return resolved;
}

// True when nothing but empty and "." components follows position `start` of `path`.
bool IsLastComponent(const std::string &path, size_t start) {
	while (start < path.size()) {
		auto end = path.find('/', start);
		if (end == std::string::npos) {
			end = path.size();
		}
		const auto component = path.substr(start, end - start);
		if (!component.empty() && component != ".") {
			return false;
		}
		start = end + 1;
	}
	return true;
}

// The override's rules: absolute, an existing directory, no symbolic link and no '..' in any
// component. The path is used as given, so it must not need resolving; `normalized` drops empty and
// "." components. On a refusal `reason` names the variable and no path, and `where` is the
// component it is about.
bool CheckOverride(const std::string &value, std::string &normalized, std::string &where, std::string &reason) {
	const std::string variable = DRIVER_DIR_VARIABLE;
	if (value.empty() || value[0] != '/') {
		reason = variable + " is not an absolute path";
		return false;
	}
	where = value;
	if (value.find('$') != std::string::npos) {
		reason = variable + " contains a '$', which the platform loader would rewrite";
		return false;
	}
	std::string prefix;
	size_t start = 1;
	while (start <= value.size()) {
		auto end = value.find('/', start);
		if (end == std::string::npos) {
			end = value.size();
		}
		const auto component = value.substr(start, end - start);
		start = end + 1;
		if (component.empty() || component == ".") {
			continue;
		}
		if (component == "..") {
			reason = variable + " contains '..'";
			return false;
		}
		prefix += "/" + component;
		where = prefix;
		// The component is the value itself, or one of its parents.
		const std::string subject = IsLastComponent(value, start) ? variable : variable + ": a parent directory";
		struct stat st;
		if (::lstat(prefix.c_str(), &st) != 0) {
			const int error = errno;
			reason =
			    error == ENOENT ? variable + " does not exist" : subject + " cannot be examined: " + ErrnoText(error);
			return false;
		}
		if (S_ISLNK(st.st_mode)) {
			reason = variable + " contains a symbolic link; set it to the path the link resolves to";
			return false;
		}
		if (!S_ISDIR(st.st_mode)) {
			reason = subject + " is not a directory";
			return false;
		}
	}
	normalized = prefix.empty() ? "/" : prefix;
	return true;
}

// The address LoadedFromDirectory looks up (MappedFilePath, else dladdr): any function defined in
// this file.
void Anchor() {
}

} // namespace

bool SetPrivateMode(const std::string &dir, int fd, const struct stat &st, std::string &reason) {
#ifdef O_PATH
	// fchmod refuses an O_PATH descriptor, but its /proc/thread-self/fd entry names the same inode.
	// Not /proc/self, the main thread's: a thread with its own descriptor table (CLONE_FILES) may
	// have another file at that number there.
	const auto by_descriptor = "/proc/thread-self/fd/" + std::to_string(fd);
	if (::chmod(by_descriptor.c_str(), 0700) == 0) {
		return true;
	}
	if (errno != ENOENT) {
		const int error = errno;
		reason = "cannot be made private: " + ErrnoText(error);
		return false;
	}
	// No /proc (or a kernel before 3.17): a descriptor fchmod accepts, checked to be the same
	// directory. This one needs the owner's read permission, so under a umask that removed it the
	// directory is refused.
	const UniqueFd second(::open(dir.c_str(), O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC));
	const int readable = second.get();
	if (readable < 0) {
		const int error = errno;
		reason = "cannot be opened after it was created: " + ErrnoText(error);
		return false;
	}
	struct stat again;
	bool made = false;
	if (::fstat(readable, &again) != 0 || again.st_dev != st.st_dev || again.st_ino != st.st_ino) {
		reason = "changed after it was created";
	} else if (::fchmod(readable, 0700) != 0) {
		const int error = errno;
		reason = "cannot be made private: " + ErrnoText(error);
	} else {
		made = true;
	}
	return made;
#else
	if (::fchmod(fd, 0700) != 0) {
		const int error = errno;
		reason = "cannot be made private: " + ErrnoText(error);
		return false;
	}
	return true;
#endif
}

namespace {

bool MakeDirectoriesBelow(const std::string &top, const std::string &dir, std::string &where, std::string &reason) {
	const std::string subject = dir == top ? "the directory " : "a parent directory ";
	where = dir;
	// lstat: what is made lies below a walked, resolved directory, so a link here leads to no
	// directory (stat found none) or was put there after the walk; it is refused, not followed.
	struct stat st;
	if (::lstat(dir.c_str(), &st) == 0) {
		if (S_ISDIR(st.st_mode)) {
			return true;
		}
		reason = subject + (S_ISLNK(st.st_mode) ? "is a symbolic link that leads to no directory"
		                                        : "exists and is not a directory");
		return false;
	}
	// ENOTDIR: a parent is not a directory, which the parent's own check names.
	const int error = errno;
	if (error != ENOENT && error != ENOTDIR) {
		reason = subject + "cannot be examined: " + ErrnoText(error);
		return false;
	}
	const auto parent = Parent(dir);
	if (parent != dir && !MakeDirectoriesBelow(top, parent, where, reason)) {
		return false;
	}
	where = dir;
	if (!MakePrivateDirectory(dir, reason)) {
		reason = subject + reason;
		return false;
	}
	return true;
}

} // namespace

bool MakeDirectories(const std::string &dir, std::string &where, std::string &reason) {
	return MakeDirectoriesBelow(dir, dir, where, reason);
}

bool MakePrivateDirectory(const std::string &dir, std::string &reason, bool (*clear_acl)(int fd)) {
	if (::mkdir(dir.c_str(), 0700) != 0) {
		const int error = errno;
		// Whatever the error: on NFS, mkdir of a name that exists in a directory the user cannot
		// write fails with EACCES or EROFS rather than EEXIST. An existing directory is left to the
		// checks that follow; a link or a file is refused here.
		struct stat st;
		if (::lstat(dir.c_str(), &st) == 0) {
			if (S_ISLNK(st.st_mode)) {
				reason = "is a symbolic link";
				return false;
			}
			if (!S_ISDIR(st.st_mode)) {
				reason = "is not a directory";
				return false;
			}
			return true;
		}
		reason = "cannot be created: " + ErrnoText(error);
		return false;
	}
	// The umask applies to mkdir; the mode must not depend on it. The mode is set through a
	// descriptor, not the name: whoever can write the parent could put a symbolic link there after
	// the mkdir, and chmod would follow it to a file of the user's. The descriptor is O_PATH where
	// there is one, because the umask may have left the owner no read permission to open it with.
#ifdef O_PATH
	const UniqueFd opened(::open(dir.c_str(), O_PATH | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC));
#else
	const UniqueFd opened(::open(dir.c_str(), O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC));
#endif
	const int fd = opened.get();
	if (fd < 0) {
		const int error = errno;
		reason = "cannot be opened after it was created: " + ErrnoText(error);
		return false;
	}
	struct stat st;
	bool made = false;
	if (::fstat(fd, &st) != 0) {
		const int error = errno;
		reason = "cannot be examined: " + ErrnoText(error);
	} else if (st.st_uid != ::geteuid()) {
		// Another directory renamed into its place.
		reason = "is not owned by the user after it was created";
	} else {
		made = SetPrivateMode(dir, fd, st, reason);
		// Entries inherited from the parent, which the mode does not show (macOS).
		if (made && !clear_acl(fd)) {
			const int error = errno;
			reason = "cannot be made private: its access control list cannot be cleared: " + ErrnoText(error);
			made = false;
		}
		// Not left behind at its name, where every later call would find it and refuse it: the
		// directory this call made, if it is still the one there (rmdir removes only an empty one).
		struct stat now;
		if (!made && ::lstat(dir.c_str(), &now) == 0 && now.st_dev == st.st_dev && now.st_ino == st.st_ino) {
			::rmdir(dir.c_str());
		}
	}
	return made;
}

std::string LoadedFromDirectory() {
	const auto address = reinterpret_cast<const void *>(&Anchor);
	// The platform hook names the mapped file as the system recorded it. dladdr is only the
	// fallback, and only for an absolute name: it gives the name passed to dlopen, which may be
	// relative to whatever the current directory was, and for the main program argv[0], which
	// whoever started the process chose.
	// Only where the system has no record of the mapping (no /proc, and always on macOS): a file removed since it was
	// loaded, or named by an escaped path, may have had its old name replaced, by whoever can write
	// its directory, with a link to another directory.
	bool mapped = false;
	auto file = MappedFilePath(address, &mapped);
	Dl_info info;
	if (file.empty() && !mapped && ::dladdr(address, &info) != 0 && info.dli_fname != nullptr &&
	    info.dli_fname[0] == '/') {
		std::string ignored;
		file = Resolve(info.dli_fname, ignored);
	}
	return file.empty() ? std::string() : Parent(file);
}

BaseInputs CurrentBaseInputs() {
	BaseInputs inputs;
	const char *value = std::getenv(DRIVER_DIR_VARIABLE);
	inputs.override_set = value != nullptr;
	inputs.override_value = value ? value : "";
	inputs.extension_dir = LoadedFromDirectory();
	inputs.per_user_base = PerUserCacheBase();
	return inputs;
}

const char *BaseRole(BaseKind kind) {
	switch (kind) {
	case BaseKind::OVERRIDE:
		return DRIVER_DIR_VARIABLE;
	case BaseKind::EXTENSION:
		return "the extension's directory";
	case BaseKind::CACHE:
		return "the per-user cache";
	}
	return "";
}

std::vector<BaseCandidate> BaseCandidates(const BaseInputs &inputs) {
	std::vector<BaseCandidate> candidates;
	auto consider = [&](BaseKind kind, const std::string &dir) {
		candidates.push_back(
		    {kind, {dir, inputs.is_noexec(dir) ? std::string(BaseRole(kind)) + " is mounted noexec" : ""}});
	};
	if (inputs.override_set) {
		std::string dir, where, reason;
		if (!CheckOverride(inputs.override_value, dir, where, reason)) {
			candidates.push_back({BaseKind::OVERRIDE, {where, reason}});
			return candidates;
		}
		consider(BaseKind::OVERRIDE, dir);
		return candidates;
	}
	if (inputs.extension_dir.empty()) {
		candidates.push_back({BaseKind::EXTENSION, {"", "the extension's own directory could not be determined"}});
	} else {
		consider(BaseKind::EXTENSION, inputs.extension_dir);
	}
	if (inputs.per_user_base.empty()) {
		// The rule is the platform's, and so is its wording.
		candidates.push_back(
		    {BaseKind::CACHE, {"", std::string("the per-user cache cannot be found: ") + PerUserCacheRule()}});
		return candidates;
	}
	// Created and resolved only when it is tried (ResolveCache).
	candidates.push_back({BaseKind::CACHE, {inputs.per_user_base, ""}, true});
	return candidates;
}

namespace {

// A directory of the user's that a concurrent first run is still making lacks an owner permission
// until that run sets 0700, a moment after its mkdir. A permission refusal at, or just below, a
// directory the user owns is therefore looked at again briefly before it counts; by the time it is
// looked at, the mode may already be right, so the refusal, not the mode now, decides.
bool OwnedDirectory(const std::string &dir, uid_t user) {
	struct stat st;
	return !dir.empty() && ::lstat(dir.c_str(), &st) == 0 && S_ISDIR(st.st_mode) && st.st_uid == user;
}

bool MayBeHalfMade(const DriverAttempt &attempt, uid_t user) {
	return attempt.reason.find(ErrnoText(EACCES)) != std::string::npos &&
	       (OwnedDirectory(attempt.directory, user) || OwnedDirectory(Parent(attempt.directory), user));
}

} // namespace

// The path as given, resolved one component at a time as the kernel would: every directory on it
// before the last is as step 2 requires (a root-owned sticky one allowed), and every symbolic link
// on it, or in a link's target, is the user's or root's, so a link another user planted cannot
// decide where the cache goes. The last directory is walked as the base later.
// `is_target`: `path` is the directory itself, not a parent of it, for the reason's wording.
bool CheckGivenPath(const std::string &path, bool is_target, uid_t user, std::string &resolved, std::string &where,
                    std::string &reason) {
	// Components still to resolve, the next one last.
	std::vector<std::string> pending;
	const auto push = [&pending](const std::string &target) {
		std::vector<std::string> parts;
		size_t start = 0;
		while (start <= target.size()) {
			auto end = target.find('/', start);
			if (end == std::string::npos) {
				end = target.size();
			}
			const auto part = target.substr(start, end - start);
			if (!part.empty() && part != ".") {
				parts.push_back(part);
			}
			start = end + 1;
		}
		pending.insert(pending.end(), parts.rbegin(), parts.rend());
	};
	push(path);
	std::string prefix; // resolved so far, a real directory; empty for "/"
	int links = 0;
	while (!pending.empty()) {
		const auto component = pending.back();
		pending.pop_back();
		if (component == "..") {
			prefix = prefix.substr(0, prefix.rfind('/') == std::string::npos ? 0 : prefix.rfind('/'));
			continue;
		}
		const std::string current = prefix + "/" + component;
		where = current;
		struct stat st;
		if (::lstat(current.c_str(), &st) != 0) {
			const int error = errno;
			reason = (pending.empty() && is_target ? "the directory" : "a parent directory") +
			         std::string(" cannot be examined: ") + ErrnoText(error);
			return false;
		}
		if (S_ISLNK(st.st_mode)) {
			if (st.st_uid != user && st.st_uid != 0) {
				reason = pending.empty() && is_target ? "the directory is a symbolic link another user owns"
				                                      : "a parent directory is a symbolic link another user owns";
				return false;
			}
			std::vector<char> target(static_cast<size_t>(PATH_MAX) + 1);
			const auto length = ::readlink(current.c_str(), target.data(), target.size());
			if (length < 0 || static_cast<size_t>(length) >= target.size() || ++links > SYMLINK_LIMIT) {
				reason = pending.empty() && is_target ? "the directory's symbolic link cannot be followed"
				                                      : "a parent directory's symbolic link cannot be followed";
				return false;
			}
			const std::string text(target.data(), static_cast<size_t>(length));
			if (!text.empty() && text[0] == '/') {
				prefix.clear();
			}
			push(text);
			continue;
		}
		if (!S_ISDIR(st.st_mode)) {
			reason = pending.empty() && is_target ? "the directory is not a directory"
			                                      : "a parent directory is not a directory";
			return false;
		}
		// The last directory too: another user's, under a sticky parent, could be swapped for a link
		// after this walk and before it is resolved.
		if (st.st_uid != user && st.st_uid != 0) {
			reason = pending.empty() && is_target ? "the directory is owned by another user"
			                                      : "a parent directory is owned by another user";
			return false;
		}
		if (!pending.empty()) {
			if ((st.st_mode & (S_IWGRP | S_IWOTH)) != 0 && !(st.st_uid == 0 && (st.st_mode & S_ISVTX) != 0)) {
				reason = "a parent directory is writable by another user";
				return false;
			}
		}
		prefix = current;
	}
	resolved = prefix.empty() ? "/" : prefix;
	return true;
}

namespace {

CacheBase ResolveCacheOnce(const BaseInputs &inputs, uid_t user) {
	const std::string role = BaseRole(BaseKind::CACHE);
	const auto &path = inputs.per_user_base;
	if (path.find('$') != std::string::npos) {
		return {false, {path, role + "'s path contains a '$', which the platform loader would rewrite"}};
	}
	// A '..' would let the kernel resolve the part not yet made through a directory never walked.
	for (size_t at = path.find("/.."); at != std::string::npos; at = path.find("/..", at + 1)) {
		if (at + 3 == path.size() || path[at + 3] == '/') {
			return {false, {path, role + "'s path contains '..'"}};
		}
	}
	// The deepest part of the path that exists is walked as given and as resolved before anything is
	// created under it, so nothing is made in, or decided by, a directory or link another user could
	// change, nor, for a root process whose HOME is a user's, in a directory that user owns.
	std::string existing = path;
	struct stat st;
	while (::stat(existing.c_str(), &st) != 0) {
		const int error = errno;
		if (error != ENOENT && error != ENOTDIR) {
			return {false, {Parent(existing), role + ": a parent directory cannot be examined: " + ErrnoText(error)}};
		}
		const auto parent = Parent(existing);
		if (parent == existing) {
			break;
		}
		existing = parent;
	}
	std::string reason, where;
	std::string walked;
	if (!CheckGivenPath(existing, existing == path, user, walked, where, reason)) {
		return {false, {where, role + ": " + reason}};
	}
	auto target = path;
	if (existing != path) {
		const auto real = Resolve(existing, reason);
		if (real.empty()) {
			return {false, {existing, role + ": a parent directory " + reason}};
		}
		if (!CheckAncestry(real, AncestryEnd::PARENT, user, where, reason)) {
			return {false, {where, role + ": " + reason}};
		}
		auto rest = path.substr(existing.size());
		if (rest.empty() || rest[0] != '/') {
			rest = "/" + rest;
		}
		target = real == "/" ? std::string() : real;
		for (size_t start = 1; start <= rest.size();) {
			auto end = rest.find('/', start);
			if (end == std::string::npos) {
				end = rest.size();
			}
			const auto part = rest.substr(start, end - start);
			if (!part.empty() && part != ".") {
				target += "/" + part;
			}
			start = end + 1;
		}
		if (!MakeDirectories(target, where, reason)) {
			return {false, {where, role + ": " + reason}};
		}
	}
	const auto cache = Resolve(target, reason);
	if (cache.empty()) {
		return {false, {target, role + " " + reason}};
	}
	// What was made is a resolved directory plus link-free components, and what existed is what the
	// walk resolved it to; anything else moved under it.
	if (cache != (existing == path ? walked : target)) {
		return {false, {target, role + ": a parent directory changed while it was created"}};
	}
	// The extension may itself live in the cache; one base is tried, or skipped, once.
	if (cache == inputs.extension_dir) {
		return {true, {cache, ""}};
	}
	// An existing cache is accepted as found, so one an earlier run left without the owner's read
	// or search permission (an umask applied before its mode was set) is refused here.
	// access, not faccessat with AT_EACCESS: glibc implements that with faccessat2, which a seccomp
	// profile older than it refuses with EPERM. The process is not set-id, so the two agree.
	if (::access(cache.c_str(), R_OK | X_OK) != 0) {
		const int error = errno;
		return {false, {cache, role + " cannot be read and searched: " + ErrnoText(error)}};
	}
	if (inputs.is_noexec(cache)) {
		return {false, {cache, role + " is mounted noexec"}};
	}
	return {false, {cache, ""}};
}

} // namespace

CacheBase ResolveCache(const BaseInputs &inputs, uid_t user) {
	for (int tries = 0;; tries++) {
		auto cache = ResolveCacheOnce(inputs, user);
		if (cache.skip || cache.attempt.reason.empty() || tries >= CACHE_MODE_RETRIES ||
		    !MayBeHalfMade(cache.attempt, user)) {
			return cache;
		}
		::usleep(CACHE_MODE_RETRY_MICROSECONDS);
	}
}

std::vector<std::string> ChooseBaseDirectories(const BaseInputs &inputs, std::vector<DriverAttempt> &attempts) {
	std::vector<std::string> bases;
	for (auto &candidate : BaseCandidates(inputs)) {
		if (candidate.pending) {
			const auto cache = ResolveCache(inputs, ::geteuid());
			if (cache.skip) {
				continue;
			}
			candidate.attempt = cache.attempt;
		}
		if (candidate.attempt.reason.empty()) {
			bases.push_back(candidate.attempt.directory);
		} else {
			attempts.push_back(candidate.attempt);
		}
	}
	return bases;
}

// Inside a user namespace the kernel shows every owner the namespace does not map as the overflow
// user. A caller that is that user itself, mapped to it or not mapped at all, cannot tell its own
// directories from anyone else's, so the owner check means nothing there.
// The initial namespace maps every id to itself, and there the overflow user is a real one.
bool OwnerIsAmbiguous(uid_t user) {
#ifdef __linux__
	uint64_t overflow = 65534;
	if (FILE *in = std::fopen("/proc/sys/kernel/overflowuid", "re")) {
		if (std::fscanf(in, "%" SCNu64, &overflow) != 1) {
			overflow = 65534;
		}
		std::fclose(in);
	}
	if (user != overflow) {
		return false;
	}
	FILE *map = std::fopen("/proc/self/uid_map", "re");
	if (map == nullptr) {
		return true;
	}
	uint64_t inside = 1, outside = 1, count = 0, more = 0;
	const bool initial = std::fscanf(map, "%" SCNu64 " %" SCNu64 " %" SCNu64, &inside, &outside, &count) == 3 &&
	                     inside == 0 && outside == 0 && count == UINT32_MAX && std::fscanf(map, "%" SCNu64, &more) != 1;
	std::fclose(map);
	return !initial;
#else
	(void)user;
	return false;
#endif
}

std::string OverflowUserReason(uid_t user) {
	return "the user (uid " + std::to_string(user) +
	       ") is a user namespace's overflow user, as which every unmapped owner appears, so no directory can be "
	       "told to be its own";
}

bool CheckAncestry(const std::string &dir, AncestryEnd end, uid_t user, std::string &where, std::string &reason,
                   AclRuleHook acl_rule) {
	const bool dir_is_base = end == AncestryEnd::BASE;
	const std::string self = end == AncestryEnd::BASE          ? "the directory"
	                         : end == AncestryEnd::DRIVER_ROOT ? "the driver root"
	                                                           : "a parent directory";
	where = dir;
	if (dir.empty() || dir[0] != '/') {
		reason = self + " is not an absolute path";
		return false;
	}
	// glibc's dlopen rewrites $ORIGIN, $LIB and $PLATFORM even in an absolute name, so a path with a
	// '$' could load another file than the one checked.
	if (dir.find('$') != std::string::npos) {
		reason = self + "'s path contains a '$', which the platform loader would rewrite";
		return false;
	}
	if (OwnerIsAmbiguous(user)) {
		reason = OverflowUserReason(user);
		return false;
	}
	// "/" first, then each prefix down to dir itself.
	std::vector<std::string> paths {"/"};
	for (size_t at = dir.find('/', 1); at != std::string::npos; at = dir.find('/', at + 1)) {
		paths.push_back(dir.substr(0, at));
	}
	if (dir != "/") {
		paths.push_back(dir);
	}
	for (size_t i = 0; i < paths.size(); i++) {
		const auto &path = paths[i];
		const bool is_dir_itself = i + 1 == paths.size();
		const std::string what = is_dir_itself ? self : "a parent directory";
		where = path;
		struct stat st;
		if (::lstat(path.c_str(), &st) != 0) {
			const int error = errno;
			reason = what + " cannot be examined: " + ErrnoText(error);
			return false;
		}
		if (S_ISLNK(st.st_mode)) {
			reason = what + " is a symbolic link";
			return false;
		}
		if (!S_ISDIR(st.st_mode)) {
			reason = what + " is not a directory";
			return false;
		}
		if (st.st_uid != user && st.st_uid != 0) {
			reason = what + " is owned by another user";
			return false;
		}
		const auto acl = acl_rule(path, -1, user, false);
		if (acl.finding != AclFinding::NONE) {
			reason = what + " " + acl.rule;
			return false;
		}
		if ((st.st_mode & (S_IWGRP | S_IWOTH)) == 0) {
			continue;
		}
		// A root-owned sticky directory such as /tmp: others may add entries but cannot replace
		// ours, so it may lie on the path, but a base there would be shared with every user.
		const bool shared_sticky = st.st_uid == 0 && (st.st_mode & S_ISVTX) != 0;
		if (!shared_sticky) {
			reason = what + " is writable by another user";
			return false;
		}
		if (is_dir_itself && dir_is_base) {
			reason = "the directory is shared (sticky, writable by others) and cannot be the base";
			return false;
		}
	}
	return true;
}

bool DriverRootMayBeReadable(uid_t owner) {
	return owner == 0;
}

bool PrepareDriverRoot(const std::string &base, uid_t user, std::string &root, std::string &where, std::string &reason,
                       std::string (*volume_rule)(const std::string &dir), AclRuleHook acl_rule,
                       bool *inherited_acl_not_cleared) {
	if (inherited_acl_not_cleared != nullptr) {
		*inherited_acl_not_cleared = false;
	}
	if (!CheckAncestry(base, AncestryEnd::BASE, user, where, reason, acl_rule)) {
		return false;
	}
	// Before anything is made there: a volume where the owners and modes the walk checked mean nothing (macOS).
	const auto volume = volume_rule(base);
	if (!volume.empty()) {
		where = base;
		reason = volume;
		return false;
	}
	root = (base == "/" ? std::string() : base) + "/" + DRIVER_ROOT_NAME;
	where = root;
	if (!MakePrivateDirectory(root, reason)) {
		reason = "the driver root " + reason;
		return false;
	}
	// A volume mounted on the driver root counts too: the checks below would see its owners and modes as the asker's.
	const auto root_volume = volume_rule(root);
	if (!root_volume.empty()) {
		reason = root_volume;
		return false;
	}
	if (!CheckAncestry(root, AncestryEnd::DRIVER_ROOT, user, where, reason, acl_rule)) {
		return false;
	}
	struct stat st;
	if (::lstat(root.c_str(), &st) != 0) {
		const int error = errno;
		reason = "the driver root cannot be examined: " + ErrnoText(error);
		return false;
	}
	// No access for group or others; a root-owned root may be readable, but no one
	// else may write it, sticky or not.
	const bool may_be_readable = DriverRootMayBeReadable(st.st_uid);
	const mode_t forbidden = may_be_readable ? (S_IWGRP | S_IWOTH) : (S_IRWXG | S_IRWXO);
	if ((st.st_mode & forbidden) != 0) {
		reason = may_be_readable ? "the driver root is writable by group or others"
		                         : "the driver root grants access to group or others (it must be owner-only, such as "
		                           "mode 0700)";
		return false;
	}
	// The same rule for an ACL: the walk above refused write rights; a root of the user's must grant
	// no access at all to anyone else. A new root inherits its base's inheritable entries (macOS), which
	// the run that made it clears a moment after its mkdir (MakePrivateDirectory).
	if (!may_be_readable) {
		const auto acl = acl_rule(root, -1, user, true);
		if (acl.finding != AclFinding::NONE) {
			reason = "the driver root " + acl.rule;
			if (inherited_acl_not_cleared != nullptr) {
				// A root of the user's: the walk refused another's, and root's is exempt.
				*inherited_acl_not_cleared = acl.inherited;
			}
			return false;
		}
	}
	// An existing root the user cannot read and search, as an earlier run under a umask that took
	// the owner's bits could leave it, is refused rather than used.
	if (::access(root.c_str(), R_OK | X_OK) != 0) {
		const int error = errno;
		reason = st.st_uid == 0 && user != 0
		             ? "the driver root belongs to root and cannot be read and searched by you; an "
		               "administrator must make it and the driver's files readable (mode 0755)"
		             : "the driver root cannot be read and searched: " + ErrnoText(error);
		return false;
	}
	return true;
}

DriverRoots::DriverRoots(const BaseInputs &inputs, uid_t user)
    : inputs_(inputs), candidates_(BaseCandidates(inputs)), user_(user) {
}

bool DriverRoots::Next(std::string &root) {
	// No base can be checked for an overflow user; it is said once, not for every base.
	if (next_ == 0 && OwnerIsAmbiguous(user_)) {
		attempts.push_back({"", OverflowUserReason(user_)});
		next_ = candidates_.size();
		return false;
	}
	while (next_ < candidates_.size()) {
		auto &candidate = candidates_[next_++];
		kind_ = candidate.kind;
		if (candidate.pending) {
			const auto cache = ResolveCache(inputs_, user_);
			if (cache.skip) {
				continue;
			}
			candidate.attempt = cache.attempt;
		}
		if (!candidate.attempt.reason.empty()) {
			attempts.push_back(candidate.attempt);
			continue;
		}
		std::string where, reason;
		bool prepared = false;
		for (int tries = 0;; tries++) {
			// A root another first run has made but not yet made private (half-made, or with the entries it
			// inherited not yet cleared) is looked at again before it counts.
			bool inherited_acl_not_cleared = false;
			prepared = PrepareDriverRoot(candidate.attempt.directory, user_, root, where, reason, inputs_.volume_rule,
			                             inputs_.acl_rule, &inherited_acl_not_cleared);
			if (prepared || tries >= CACHE_MODE_RETRIES ||
			    !(inherited_acl_not_cleared || MayBeHalfMade({where, reason}, user_))) {
				break;
			}
			::usleep(CACHE_MODE_RETRY_MICROSECONDS);
		}
		if (!prepared) {
			Fail(where, reason);
			continue;
		}
		// A file system mounted below the base, on the driver root, counts too. Any kind of file system
		// will do, local, network or FUSE, as for DuckDB's own extensions: its owners and modes are
		// checked as it reports them (network and FUSE file systems can still leave a gap there). The
		// one exception is macOS's volume that ignores ownership, which PrepareDriverRoot refuses (VolumeRule).
		if (inputs_.is_noexec(root)) {
			Fail(root, "the driver root is on a file system mounted noexec");
			continue;
		}
		return true;
	}
	return false;
}

void DriverRoots::Fail(const std::string &where, const std::string &rule) {
	attempts.push_back({where, std::string(BaseRole(kind_)) + ": " + rule});
}

const char *DriverRoots::Role() const {
	return BaseRole(kind_);
}

std::string DescribeAttempts(const std::vector<DriverAttempt> &attempts) {
	std::string text;
	for (const auto &attempt : attempts) {
		text += (text.empty() ? "" : "; ") + attempt.reason;
	}
	return text;
}

DriverError NoUsableDirectory(const std::vector<DriverAttempt> &attempts) {
	DriverError error;
	error.kind = DriverErrorKind::NO_USABLE_DIRECTORY;
	error.attempts = attempts;
	error.message = "no usable directory to install the Redshift ODBC driver in";
	if (!attempts.empty()) {
		error.message += " (" + DescribeAttempts(attempts) + ")";
	}
	const bool ambiguous =
	    attempts.size() == 1 && attempts[0].reason.find(") is a user namespace's overflow user") != std::string::npos;
	if (ambiguous) {
		// No directory would do: the process must run as a user its user namespace maps.
		error.message += ". Run it as a user that its user namespace maps";
		return error;
	}
	error.message += ". Set " + std::string(DRIVER_DIR_VARIABLE) +
	                 " to the absolute path of an existing directory on a file system that allows executing code, "
	                 "with no symbolic link in its path, where it and every directory above it are owned by you or "
	                 "root and writable by no one else";
	return error;
}

} // namespace detail
} // namespace redshift_driver
} // namespace duckdb
