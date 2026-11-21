// Installing, re-checking and repairing the driver set,
// on a synthetic payload in temporary directories, with forked and executed helper
// processes for the concurrent and crash cases.
#include "driver_loader_core.hpp"
#include "sha256.hpp"
#include "loader_test.hpp"
#include "temp_dir.hpp"

#include "catch.hpp"

#include <algorithm>
#include <chrono>
#include <cerrno>
#include <csignal>
#include <cstdarg>
#include <climits>
#include <cstdio>
#include <cstring>
#include <ctime>
#include <fstream>
#include <functional>
#include <iterator>
#include <string>
#include <vector>

#include <dirent.h>
#include <dlfcn.h>
#include <fcntl.h>
#include <sys/mount.h>
#include <sys/file.h>
#include <sys/resource.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/un.h>
#include <sys/time.h>
#include <sys/wait.h>
#include <unistd.h>

using duckdb::redshift_driver::DriverAttempt;
using duckdb::redshift_driver::DriverError;
using duckdb::redshift_driver::DriverErrorKind;
using namespace duckdb::redshift_driver::detail;
using redshift_driver_test::DescriptorLimit;
using redshift_driver_test::DescriptorLimitInUse;
using redshift_driver_test::DIRECTORY_ADVICE;
using redshift_driver_test::Expect;
using redshift_driver_test::Inputs;
using redshift_driver_test::MakeDir;
using redshift_driver_test::Mode;
using redshift_driver_test::NeverNoExec;
using redshift_driver_test::NO_DIRECTORY_START;
using redshift_driver_test::OTHER_USER;
using redshift_driver_test::PlatformLoaderCalls;
using redshift_driver_test::PrepareDriverSet;
using redshift_driver_test::PrivateAncestry;
using redshift_driver_test::RunAsNamespaceRoot;
using redshift_driver_test::RunAsRootOfPrivateTree;
using redshift_driver_test::TempDir;

namespace {

// The path the open descriptor `fd` names, or empty.
std::string FdPath(int fd) {
	char path[PATH_MAX];
	const auto length = ::readlink(("/proc/self/fd/" + std::to_string(fd)).c_str(), path, sizeof(path));
	return length < 0 ? std::string() : std::string(path, static_cast<size_t>(length));
}

bool StartsWith(const std::string &text, const std::string &prefix) {
	return !prefix.empty() && text.compare(0, prefix.size(), prefix) == 0;
}

// Stand-ins for the C library's read, write, fsync, openat, fdopendir, readdir, mkdtemp, flock,
// unlinkat, renameat and fstatat in this executable, to make the failures a disk, a full file system, a racing
// process or a file system without locking would cause, and to stop or kill a process at a step of an
// install. Each does nothing until a test arms it; Hooks disarms them all.
// read of the file at g_read_path fails with g_read_errno; only once with g_read_once.
std::string g_read_path;
int g_read_errno = 0;
bool g_read_once = false;
// write to a file whose path starts with g_write_prefix fails with g_write_errno, or writes at most
// g_write_most bytes, and with g_write_eintr every other call fails with EINTR; with g_write_flip
// it writes its first byte changed, as a file system that does not keep what it is given would.
std::string g_write_prefix;
int g_write_errno = 0;
size_t g_write_most = 0;
bool g_write_eintr = false;
bool g_write_flip = false;
// Apart from those, write to a file whose path starts with g_write_fail_prefix fails with ENOSPC.
std::string g_write_fail_prefix;
// With g_write_zero, write returns 0 without writing, as a FUSE daemon may; after
// WRITE_ZERO_LIMIT such calls it sets g_write_spun and fails with ENOSPC, so a loop that retries
// forever ends with a failure the test reports.
bool g_write_zero = false;
bool g_write_spun = false;
int g_write_zeros = 0;
constexpr int WRITE_ZERO_LIMIT = 1000;
// fsync records every path it flushes; the first fsync of a directory whose path starts with
// g_fsync_prefix runs g_before_fsync, once, before it, and every such fsync runs g_after_fsync, with
// its path, after it; an fsync of a path g_fsync_fails accepts fails with EIO.
std::vector<std::string> g_fsynced;
std::string g_fsync_prefix;
std::function<void()> g_before_fsync;
std::function<void(const std::string &path)> g_after_fsync;
std::function<bool(const std::string &path)> g_fsync_fails;
// fdopendir of the directory at g_fdopendir_path fails with g_fdopendir_errno.
std::string g_fdopendir_path;
int g_fdopendir_errno = 0;
// mkdtemp fails with g_mkdtemp_errno, without making anything; it is counted, and g_after_mkdtemp
// runs after one that made a directory.
int g_mkdtemp_errno = 0;
int g_mkdtemps = 0;
std::function<void(const std::string &made)> g_after_mkdtemp;
// openat with O_CREAT relative to a directory whose path starts with g_plant_prefix first puts a
// file of its own at that name, once.
std::string g_plant_prefix;
// openat(AT_FDCWD, g_openat_fail_path, ...) fails with g_openat_errno.
std::string g_openat_fail_path;
int g_openat_errno = 0;
// openat without O_PATH of the entry at g_reopen_fail_path fails with EMFILE.
std::string g_reopen_fail_path;
// Once openat with O_PATH has opened the entry at g_swap_path, that entry is renamed to g_swap_to
// and a new 0700 directory made at its name, as another process swapping it would.
std::string g_swap_path;
std::string g_swap_to;
// readdir of the directory at g_readdir_path gives one name other than . and .., then fails with EIO.
std::string g_readdir_path;
int g_readdir_given = 0;
// renameat to a name that starts with g_rename_to (any name while that is empty) fails with
// g_rename_errno; every call is counted, and one that succeeds runs g_after_rename with the name it
// renamed to.
int g_rename_errno = 0;
std::string g_rename_to;
int g_renames = 0;
std::function<void(const std::string &to)> g_after_rename;
// renameat to a name that starts with g_rename_done_to renames and then fails with ENOENT all the
// same, once, as an NFS client whose retransmitted rename the server had already done reports.
std::string g_rename_done_to;
// fstatat of a name that starts with g_fstatat_prefix fails with g_fstatat_errno; such calls are
// counted.
std::string g_fstatat_prefix;
int g_fstatat_errno = 0;
int g_fstatats = 0;
// unlinkat of an entry fails with the errno g_unlinkat_fails gives for its path, when not 0.
std::function<int(const std::string &path)> g_unlinkat_fails;
// flock fails with g_flock_errno.
int g_flock_errno = 0;
// openat runs g_after_openat_fail with the name and errno of every open that fails.
std::function<void(const std::string &name, int error)> g_after_openat_fail;

struct Hooks {
	Hooks() {
		Reset();
	}
	~Hooks() {
		Reset();
	}
	Hooks(const Hooks &) = delete;
	Hooks &operator=(const Hooks &) = delete;
	static void Reset() {
		g_read_path.clear();
		g_read_errno = 0;
		g_read_once = false;
		g_write_prefix.clear();
		g_write_errno = 0;
		g_write_most = 0;
		g_write_eintr = false;
		g_write_flip = false;
		g_write_fail_prefix.clear();
		g_write_zero = false;
		g_fsynced.clear();
		g_fsync_prefix.clear();
		g_before_fsync = nullptr;
		g_after_fsync = nullptr;
		g_fsync_fails = nullptr;
		g_fdopendir_path.clear();
		g_fdopendir_errno = 0;
		g_mkdtemp_errno = 0;
		g_mkdtemps = 0;
		g_after_mkdtemp = nullptr;
		g_plant_prefix.clear();
		g_openat_fail_path.clear();
		g_openat_errno = 0;
		g_reopen_fail_path.clear();
		g_swap_path.clear();
		g_swap_to.clear();
		g_readdir_path.clear();
		g_readdir_given = 0;
		g_write_spun = false;
		g_write_zeros = 0;
		g_rename_errno = 0;
		g_rename_to.clear();
		g_renames = 0;
		g_after_rename = nullptr;
		g_rename_done_to.clear();
		g_fstatat_prefix.clear();
		g_fstatat_errno = 0;
		g_fstatats = 0;
		g_unlinkat_fails = nullptr;
		g_flock_errno = 0;
		g_after_openat_fail = nullptr;
		redshift_driver_test::g_chmod_fail_prefix.clear();
		redshift_driver_test::g_chmod_fail_errno = 0;
	}
};

} // namespace

extern "C" ssize_t read(int fd, void *buffer, size_t count) {
	using Read = ssize_t (*)(int, void *, size_t);
	static const auto real = reinterpret_cast<Read>(::dlsym(RTLD_NEXT, "read"));
	if (g_read_errno != 0 && FdPath(fd) == g_read_path) {
		errno = g_read_errno;
		if (g_read_once) {
			g_read_errno = 0;
		}
		return -1;
	}
	return real(fd, buffer, count);
}

extern "C" ssize_t write(int fd, const void *buffer, size_t count) {
	using Write = ssize_t (*)(int, const void *, size_t);
	static const auto real = reinterpret_cast<Write>(::dlsym(RTLD_NEXT, "write"));
	static bool interrupt = false;
	if (StartsWith(FdPath(fd), g_write_fail_prefix)) {
		errno = ENOSPC;
		return -1;
	}
	if ((g_write_errno != 0 || g_write_most != 0 || g_write_eintr || g_write_flip || g_write_zero) &&
	    StartsWith(FdPath(fd), g_write_prefix)) {
		if (g_write_errno != 0) {
			errno = g_write_errno;
			return -1;
		}
		if (g_write_zero) {
			if (++g_write_zeros > WRITE_ZERO_LIMIT) {
				g_write_spun = true;
				errno = ENOSPC;
				return -1;
			}
			return 0;
		}
		interrupt = !interrupt;
		if (g_write_eintr && interrupt) {
			errno = EINTR;
			return -1;
		}
		if (g_write_most != 0) {
			count = std::min(count, g_write_most);
		}
		if (g_write_flip && count > 0) {
			std::string changed(static_cast<const char *>(buffer), count);
			changed[0] = static_cast<char>(changed[0] ^ 1);
			return real(fd, changed.data(), count);
		}
	}
	return real(fd, buffer, count);
}

extern "C" int fsync(int fd) {
	using Fsync = int (*)(int);
	static const auto real = reinterpret_cast<Fsync>(::dlsym(RTLD_NEXT, "fsync"));
	const auto path = FdPath(fd);
	g_fsynced.push_back(path);
	struct stat st;
	if (g_before_fsync && StartsWith(path, g_fsync_prefix) && ::fstat(fd, &st) == 0 && S_ISDIR(st.st_mode)) {
		const auto hook = std::move(g_before_fsync);
		g_before_fsync = nullptr;
		hook();
	}
	if (g_fsync_fails && g_fsync_fails(path)) {
		errno = EIO;
		return -1;
	}
	const int result = real(fd);
	if (g_after_fsync && StartsWith(path, g_fsync_prefix) && ::fstat(fd, &st) == 0 && S_ISDIR(st.st_mode)) {
		g_after_fsync(path);
	}
	return result;
}

extern "C" DIR *fdopendir(int fd) {
	using Fdopendir = DIR *(*)(int);
	static const auto real = reinterpret_cast<Fdopendir>(::dlsym(RTLD_NEXT, "fdopendir"));
	if (g_fdopendir_errno != 0 && FdPath(fd) == g_fdopendir_path) {
		errno = g_fdopendir_errno;
		return nullptr;
	}
	return real(fd);
}

extern "C" struct dirent *readdir(DIR *dir) {
	using Readdir = struct dirent *(*)(DIR *);
	static const auto real = reinterpret_cast<Readdir>(::dlsym(RTLD_NEXT, "readdir"));
	if (!g_readdir_path.empty() && FdPath(::dirfd(dir)) == g_readdir_path) {
		if (g_readdir_given >= 1) {
			errno = EIO;
			return nullptr;
		}
		auto *entry = real(dir);
		if (entry != nullptr && std::strcmp(entry->d_name, ".") != 0 && std::strcmp(entry->d_name, "..") != 0) {
			g_readdir_given++;
		}
		return entry;
	}
	return real(dir);
}

extern "C" char *mkdtemp(char *pattern) noexcept {
	using Mkdtemp = char *(*)(char *);
	static const auto real = reinterpret_cast<Mkdtemp>(::dlsym(RTLD_NEXT, "mkdtemp"));
	g_mkdtemps++;
	if (g_mkdtemp_errno != 0) {
		errno = g_mkdtemp_errno;
		return nullptr;
	}
	auto *made = real(pattern);
	if (made != nullptr && g_after_mkdtemp) {
		g_after_mkdtemp(made);
	}
	return made;
}

extern "C" int openat(int dir, const char *name, int flags, ...) {
	using Openat = int (*)(int, const char *, int, ...);
	static const auto real = reinterpret_cast<Openat>(::dlsym(RTLD_NEXT, "openat"));
	mode_t mode = 0;
	if ((flags & O_CREAT) != 0 || (flags & O_TMPFILE) == O_TMPFILE) {
		va_list arguments;
		va_start(arguments, flags);
		mode = static_cast<mode_t>(va_arg(arguments, int));
		va_end(arguments);
	}
	if (g_openat_errno != 0 && dir == AT_FDCWD && g_openat_fail_path == name) {
		errno = g_openat_errno;
		return -1;
	}
	if (!g_reopen_fail_path.empty() && (flags & O_PATH) == 0 && FdPath(dir) + "/" + name == g_reopen_fail_path) {
		errno = EMFILE;
		return -1;
	}
	if ((flags & O_CREAT) != 0 && StartsWith(FdPath(dir), g_plant_prefix)) {
		g_plant_prefix.clear();
		const int planted = real(dir, name, O_WRONLY | O_CREAT | O_CLOEXEC, 0600);
		if (planted >= 0) {
			::close(planted);
		}
	}
	const int result = real(dir, name, flags, mode);
	if (result >= 0 && (flags & O_PATH) != 0 && !g_swap_path.empty() && FdPath(dir) + "/" + name == g_swap_path) {
		const auto swapped = g_swap_path;
		g_swap_path.clear();
		if (::rename(swapped.c_str(), g_swap_to.c_str()) == 0 && ::mkdir(swapped.c_str(), 0700) == 0) {
			::chmod(swapped.c_str(), 0700);
		}
	}
	if (result < 0 && g_after_openat_fail) {
		const int error = errno;
		g_after_openat_fail(name, error);
		errno = error;
	}
	return result;
}

extern "C" int unlinkat(int dir, const char *name, int flags) noexcept {
	using Unlinkat = int (*)(int, const char *, int);
	static const auto real = reinterpret_cast<Unlinkat>(::dlsym(RTLD_NEXT, "unlinkat"));
	if (g_unlinkat_fails) {
		const int error = g_unlinkat_fails(FdPath(dir) + "/" + name);
		if (error != 0) {
			errno = error;
			return -1;
		}
	}
	return real(dir, name, flags);
}

extern "C" int flock(int fd, int operation) noexcept {
	using Flock = int (*)(int, int);
	static const auto real = reinterpret_cast<Flock>(::dlsym(RTLD_NEXT, "flock"));
	if (g_flock_errno != 0 && (operation & LOCK_EX) != 0) {
		errno = g_flock_errno;
		return -1;
	}
	return real(fd, operation);
}

extern "C" int renameat(int from_dir, const char *from, int to_dir, const char *to) noexcept {
	using Renameat = int (*)(int, const char *, int, const char *);
	static const auto real = reinterpret_cast<Renameat>(::dlsym(RTLD_NEXT, "renameat"));
	g_renames++;
	if (g_rename_errno != 0 && (g_rename_to.empty() || StartsWith(to, g_rename_to))) {
		errno = g_rename_errno;
		return -1;
	}
	const int result = real(from_dir, from, to_dir, to);
	if (result == 0 && g_after_rename) {
		g_after_rename(to);
	}
	if (result == 0 && StartsWith(to, g_rename_done_to)) {
		g_rename_done_to.clear();
		errno = ENOENT;
		return -1;
	}
	return result;
}

// Before glibc 2.33 libc.so exports no fstatat (it is a wrapper around __fxstatat), so there is
// nothing to stand in for or forward to; the case that arms it then says it was not tested.
#if defined(__GLIBC__) && (__GLIBC__ > 2 || (__GLIBC__ == 2 && __GLIBC_MINOR__ >= 33))
extern "C" int fstatat(int dir, const char *name, struct stat *st, int flags) noexcept {
	using Fstatat = int (*)(int, const char *, struct stat *, int);
	static const auto real = reinterpret_cast<Fstatat>(::dlsym(RTLD_NEXT, "fstatat"));
	if (g_fstatat_errno != 0 && StartsWith(name, g_fstatat_prefix)) {
		g_fstatats++;
		errno = g_fstatat_errno;
		return -1;
	}
	return real(dir, name, st, flags);
}
#endif

namespace {

// A driver and two notices, with their hashes, as driver_info.h would list them. `driver_size`
// bytes of driver, or a short one with a NUL in it.
struct Payload {
	std::vector<std::string> contents;
	DriverSet set;
	explicit Payload(size_t driver_size = 0, const std::string &licence = "the licence") {
		contents = {driver_size == 0 ? std::string("driver bytes\0with a NUL and more", 32)
		                             : std::string(driver_size, 'd'),
		            licence, "the notice"};
		const std::vector<std::string> names {"libtest.so", "LICENSE", "NOTICE"};
		std::string manifest;
		for (size_t i = 0; i < names.size(); i++) {
			SetFile file;
			file.name = names[i];
			file.sha256 = Sha256Hex(contents[i].data(), contents[i].size());
			file.size = contents[i].size();
			manifest += file.sha256 + "  " + std::to_string(file.size) + "  " + file.name + "\n";
			set.files.push_back(file);
		}
		set.set_sha256 = Sha256Hex(manifest.data(), manifest.size());
		Point();
	}
	Payload(const Payload &) = delete;
	Payload &operator=(const Payload &) = delete;
	// The set's data pointers, at `contents` as it is now.
	void Point() {
		for (size_t i = 0; i < contents.size(); i++) {
			set.files[i].data = reinterpret_cast<const unsigned char *>(contents[i].data());
		}
	}
};

BaseInputs Override(const std::string &dir) {
	BaseInputs inputs;
	inputs.override_set = true;
	inputs.override_value = dir;
	inputs.is_noexec = NeverNoExec;
	return inputs;
}

// The names in `dir`, sorted; none when it does not exist.
std::vector<std::string> Names(const std::string &dir) {
	std::vector<std::string> names;
	DIR *handle = ::opendir(dir.c_str());
	if (handle == nullptr) {
		return names;
	}
	while (const auto *entry = ::readdir(handle)) {
		const std::string name = entry->d_name;
		if (name != "." && name != "..") {
			names.push_back(name);
		}
	}
	::closedir(handle);
	std::sort(names.begin(), names.end());
	return names;
}

std::string Read(const std::string &path) {
	std::ifstream in(path, std::ios::binary);
	return std::string(std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>());
}

struct stat Stat(const std::string &path) {
	struct stat st;
	REQUIRE(::lstat(path.c_str(), &st) == 0);
	return st;
}

// What a later check compares: each file's inode and modification time, and the set directory's.
std::vector<std::string> Identity(const std::string &set_dir, const DriverSet &set) {
	std::vector<std::string> identity {std::to_string(Stat(set_dir).st_ino)};
	for (const auto &file : set.files) {
		const auto st = Stat(set_dir + "/" + file.name);
		identity.push_back(file.name + " " + std::to_string(st.st_ino) + " " + std::to_string(st.st_mtim.tv_sec) + "." +
		                   std::to_string(st.st_mtim.tv_nsec));
	}
	return identity;
}

// The set directory, or empty with `error` set.
std::string Prepare(const BaseInputs &inputs, const DriverSet &set, DriverError &error, uid_t user = ::geteuid()) {
	std::string set_dir;
	return PrepareDriverSet(inputs, set, user, set_dir, error) ? set_dir : std::string();
}

// A finding of CheckInstalledSet.
struct Finding {
	SetState state;
	std::string where;
	std::string reason;
	bool operator==(const Finding &other) const {
		return state == other.state && where == other.where && reason == other.reason;
	}
};

Finding Check(const std::string &set_dir, const DriverSet &set, uid_t user = ::geteuid()) {
	Finding finding;
	finding.state = CheckInstalledSet(set_dir, set.Manifest(), user, finding.where, finding.reason);
	return finding;
}

// Runs `body` in a child process and returns its exit status, or -1 when a signal ended it.
template <class BODY>
int InChild(BODY body) {
	std::fflush(nullptr);
	const pid_t child = ::fork();
	REQUIRE(child >= 0);
	if (child == 0) {
		::_exit(body() ? 0 : 1);
	}
	int status = 0;
	REQUIRE(::waitpid(child, &status, 0) == child);
	return WIFEXITED(status) ? WEXITSTATUS(status) : -1;
}

// Runs `body(top)` as root of the directories under `top`: as root itself in a TempDir when the
// tests run as root, else in a user namespace whose root owns a private tmpfs (top ""). Returns
// 0 when the body passed, 77 where neither is available.
template <class BODY>
int AsRoot(BODY body) {
	if (::geteuid() == 0) {
		TempDir tmp;
		REQUIRE(::chmod(tmp.path().c_str(), 0755) == 0);
		return body(tmp.path()) ? 0 : 1;
	}
	return RunAsRootOfPrivateTree([&] { return body(std::string()); });
}

// A Unix socket at `dir`/`name`, bound by its name from inside `dir`: the whole path is longer than a
// socket address holds.
void BindSocket(const std::string &dir, const std::string &name) {
	const int back = ::open(".", O_RDONLY | O_DIRECTORY | O_CLOEXEC);
	REQUIRE(back >= 0);
	REQUIRE(::chdir(dir.c_str()) == 0);
	const int sock = ::socket(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0);
	sockaddr_un address {};
	address.sun_family = AF_UNIX;
	std::snprintf(address.sun_path, sizeof(address.sun_path), "%s", name.c_str());
	const bool bound = sock >= 0 && ::bind(sock, reinterpret_cast<const sockaddr *>(&address), sizeof(address)) == 0;
	if (sock >= 0) {
		::close(sock);
	}
	REQUIRE(::fchdir(back) == 0);
	::close(back);
	REQUIRE(bound);
}

// Every name in `root` that is a staging directory.
std::vector<std::string> Staging(const std::string &root) {
	std::vector<std::string> staging;
	for (const auto &name : Names(root)) {
		if (StartsWith(name, ".staging-")) {
			staging.push_back(name);
		}
	}
	return staging;
}

// Every name in `root` that starts with `prefix`, sorted.
std::vector<std::string> Prefixed(const std::string &root, const std::string &prefix) {
	std::vector<std::string> found;
	for (const auto &name : Names(root)) {
		if (StartsWith(name, prefix)) {
			found.push_back(name);
		}
	}
	return found;
}

// Changes the first byte of the set's file at `path`, keeping its mode.
void Tamper(const std::string &path) {
	REQUIRE(::chmod(path.c_str(), 0700) == 0);
	{
		std::fstream file(path, std::ios::binary | std::ios::in | std::ios::out);
		const char first = static_cast<char>(file.get());
		file.seekp(0);
		file.put(static_cast<char>(first ^ 1));
	}
	REQUIRE(::chmod(path.c_str(), 0500) == 0);
}

// Dates `path` (a link itself, not its target) `seconds` in the past.
void Age(const std::string &path, time_t seconds) {
	struct timespec times[2];
	REQUIRE(::clock_gettime(CLOCK_REALTIME, &times[0]) == 0);
	times[0].tv_sec -= seconds;
	times[1] = times[0];
	REQUIRE(::utimensat(AT_FDCWD, path.c_str(), times, AT_SYMLINK_NOFOLLOW) == 0);
}

const std::string READ_ONLY = "the driver root holds no valid copy of this driver version and cannot be written";
const std::vector<std::string> LOCK_ONLY {".lock"};

// What a driver root holds once `set` is installed in it: the lock and the set.
std::vector<std::string> Installed(const DriverSet &set) {
	return {".lock", set.set_sha256};
}

} // namespace

namespace Catch {
template <>
struct StringMaker<Finding> {
	static std::string convert(const Finding &finding) {
		const char *states[] = {"VALID", "MISSING", "INVALID", "UNCHECKED"};
		return std::string("{") + states[static_cast<int>(finding.state)] + ", " + finding.where + ", " +
		       finding.reason + "}";
	}
};
} // namespace Catch

TEST_CASE("The first call writes exactly the driver and its notices, and another process reuses them unchanged",
          "[driver][install]") {
	TempDir tmp;
	if (!PrivateAncestry(tmp)) {
		return;
	}
	Payload payload;
	DriverError error;
	const auto set_dir = Prepare(Inputs(tmp.path(), ""), payload.set, error);
	REQUIRE(set_dir == tmp / ".redshift-driver/" + payload.set.set_sha256);
	// Only the set in the root (no staging directory), and in the set only the driver and the
	// notices: no amazon.redshiftodbc.ini, no root.crt.
	CHECK(Names(tmp / ".redshift-driver") == Installed(payload.set));
	CHECK(Names(set_dir) == std::vector<std::string> {"LICENSE", "NOTICE", "libtest.so"});
	CHECK(Mode(set_dir) == 0700);
	for (size_t i = 0; i < payload.set.files.size(); i++) {
		const auto path = set_dir + "/" + payload.set.files[i].name;
		CHECK(Mode(path) == 0500);
		CHECK(Read(path) == payload.contents[i]);
	}
	CHECK(Check(set_dir, payload.set) == Finding {SetState::VALID, "", ""});

	const auto before = Identity(set_dir, payload.set);
	CHECK(InChild([&] {
		      DriverError child_error;
		      return Prepare(Inputs(tmp.path(), ""), payload.set, child_error) == set_dir;
	      }) == 0);
	CHECK(Identity(set_dir, payload.set) == before);
	CHECK(Names(tmp / ".redshift-driver") == Installed(payload.set));
}

TEST_CASE("Installs under a umask of 0277 or 0477 give a 0700 set of 0500 files", "[driver][install]") {
	for (const mode_t mask : {0277, 0477}) {
		INFO("umask " << std::oct << mask);
		TempDir tmp;
		if (!PrivateAncestry(tmp)) {
			return;
		}
		Payload payload;
		DriverError error;
		const mode_t old_mask = ::umask(mask);
		const auto set_dir = Prepare(Override(tmp.path()), payload.set, error);
		::umask(old_mask);
		INFO(DescribeAttempts(error.attempts));
		REQUIRE(!set_dir.empty());
		CHECK(Mode(set_dir) == 0700);
		for (const auto &file : payload.set.files) {
			CHECK(Mode(set_dir + "/" + file.name) == 0500);
		}
		CHECK(Names(tmp / ".redshift-driver") == Installed(payload.set));
	}
}

TEST_CASE("Each file and the set directory are flushed before the set is put in place, and the root after",
          "[driver][install]") {
	TempDir tmp;
	if (!PrivateAncestry(tmp)) {
		return;
	}
	Hooks hooks;
	Payload payload;
	DriverError error;
	const auto root = tmp / ".redshift-driver";
	const auto set_dir = Prepare(Override(tmp.path()), payload.set, error);
	const auto flushed = g_fsynced;
	REQUIRE(!set_dir.empty());
	// The three files in the staging directory, then the directory, then the root.
	REQUIRE(flushed.size() == 5);
	const auto staging = flushed[3];
	CHECK(StartsWith(staging, root + "/.staging-"));
	for (size_t i = 0; i < 3; i++) {
		CHECK(flushed[i] == staging + "/" + payload.set.files[i].name);
	}
	CHECK(flushed[4] == root);
}

TEST_CASE("Short writes and EINTR are written through", "[driver][install]") {
	TempDir tmp;
	if (!PrivateAncestry(tmp)) {
		return;
	}
	Hooks hooks;
	Payload payload;
	g_write_prefix = tmp / ".redshift-driver/.staging-";
	g_write_most = 5;
	g_write_eintr = true;
	DriverError error;
	const auto set_dir = Prepare(Override(tmp.path()), payload.set, error);
	Hooks::Reset();
	INFO(DescribeAttempts(error.attempts));
	REQUIRE(!set_dir.empty());
	for (size_t i = 0; i < payload.set.files.size(); i++) {
		CHECK(Read(set_dir + "/" + payload.set.files[i].name) == payload.contents[i]);
	}
}

TEST_CASE("A set this process wrote that then fails the check is not used", "[driver][install]") {
	TempDir tmp;
	if (!PrivateAncestry(tmp)) {
		return;
	}
	Hooks hooks;
	Payload payload;
	g_write_prefix = tmp / ".redshift-driver/.staging-";
	g_write_flip = true;
	DriverError error;
	CHECK(Prepare(Override(tmp.path()), payload.set, error).empty());
	Hooks::Reset();
	REQUIRE(error.attempts.size() == 1);
	CHECK(error.attempts[0].directory == tmp / ".redshift-driver/" + payload.set.set_sha256 + "/libtest.so");
	CHECK(error.attempts[0].reason == "REDSHIFT_DRIVER_DIR: the driver set this process wrote does not pass the check "
	                                  "(a file of the driver set does not match the embedded driver)");
}

TEST_CASE("A read-only extension directory falls back to the per-user cache", "[driver][install]") {
	if (::geteuid() == 0) {
		WARN("root can write a read-only directory; not tested as root");
		return;
	}
	TempDir tmp;
	if (!PrivateAncestry(tmp)) {
		return;
	}
	Payload payload;
	// Without a driver root, which cannot be made (the directory-safety refusal), and with an empty read-only one.
	MakeDir(tmp / "ext", 0500);
	MakeDir(tmp / "ext2", 0700);
	MakeDir(tmp / "ext2/.redshift-driver", 0500);
	for (const auto &ext : {tmp / "ext", tmp / "ext2"}) {
		INFO(ext);
		const auto cache = ext + "-cache";
		DriverError error;
		CHECK(Prepare(Inputs(ext, cache), payload.set, error) == cache + "/.redshift-driver/" + payload.set.set_sha256);
		CHECK(Names(ext + "/.redshift-driver").empty());
	}
	// What the read-only root's attempt says (a call that succeeds returns none).
	std::string set_dir, where, reason;
	CHECK(InstallDriverSet(tmp / "ext2/.redshift-driver", payload.set, ::geteuid(), set_dir, where, reason) ==
	      InstallResult::FAILED);
	CHECK(where == tmp / "ext2/.redshift-driver");
	CHECK(reason == "the driver root holds no valid copy of this driver version and cannot be written");
}

TEST_CASE("A read-only set of the user's is used without writing", "[driver][install]") {
	TempDir tmp;
	if (!PrivateAncestry(tmp)) {
		return;
	}
	Payload payload;
	MakeDir(tmp / "admin", 0755);
	DriverError error;
	const auto set_dir = Prepare(Override(tmp / "admin"), payload.set, error);
	REQUIRE(!set_dir.empty());
	REQUIRE(::chmod(set_dir.c_str(), 0500) == 0);
	REQUIRE(::chmod((tmp / "admin/.redshift-driver").c_str(), 0500) == 0);
	REQUIRE(::chmod((tmp / "admin").c_str(), 0555) == 0);
	const auto before = Identity(set_dir, payload.set);
	CHECK(Prepare(Override(tmp / "admin"), payload.set, error) == set_dir);
	CHECK(Identity(set_dir, payload.set) == before);
	CHECK(Names(tmp / "admin/.redshift-driver") == Installed(payload.set));
}

TEST_CASE("An administrator's root-owned, read-only set under REDSHIFT_DRIVER_DIR is used without writing",
          "[driver][install]") {
	if (::geteuid() != 0) {
		WARN("only root can provision a set for another user; not tested here "
		     "(test/client/driver/run-loader-tests-as-root.sh runs it as root)");
		return;
	}
	TempDir tmp;
	if (!PrivateAncestry(tmp)) {
		return;
	}
	REQUIRE(::chmod(tmp.path().c_str(), 0755) == 0);
	Payload payload;
	Payload newer(0, "a newer licence");
	MakeDir(tmp / "admin", 0755);
	const auto root = tmp / "admin/.redshift-driver";
	// Step 2: run once as root, then open the root and the set to everyone.
	DriverError error;
	const auto set_dir = Prepare(Override(tmp / "admin"), payload.set, error);
	REQUIRE(!set_dir.empty());
	REQUIRE(::chmod(root.c_str(), 0755) == 0);
	REQUIRE(::chmod(set_dir.c_str(), 0755) == 0);
	for (const auto &file : payload.set.files) {
		REQUIRE(::chmod((set_dir + "/" + file.name).c_str(), 0555) == 0);
	}
	const auto before = Identity(set_dir, payload.set);
	// As another user, who cannot write any of it.
	const auto as_other = [&](const DriverSet &set, const std::string &expected_reason) {
		return InChild([&] {
			if (::setgid(OTHER_USER) != 0 || ::setuid(OTHER_USER) != 0) {
				return Expect(false, "setuid");
			}
			DriverError child_error;
			const auto found = Prepare(Override(tmp / "admin"), set, child_error, OTHER_USER);
			if (expected_reason.empty()) {
				return Expect(found == set_dir, "the administrator's set: " + DescribeAttempts(child_error.attempts));
			}
			return Expect(found.empty() && child_error.attempts.size() == 1 &&
			                  child_error.attempts[0].reason == expected_reason,
			              DescribeAttempts(child_error.attempts));
		});
	};
	CHECK(as_other(payload.set, "") == 0);
	CHECK(Identity(set_dir, payload.set) == before);
	CHECK(Names(root) == Installed(payload.set));
	// A newer driver the administrator has not provisioned yet: read-only, so the next base, of which
	// REDSHIFT_DRIVER_DIR leaves none.
	CHECK(as_other(newer.set, "REDSHIFT_DRIVER_DIR: the driver root holds no valid copy of this driver version and "
	                          "cannot be written") == 0);
	CHECK(Names(root) == Installed(payload.set));
	// The same set with one file group-writable is refused with its reason: the other user cannot
	// open the lock to repair it, so the set is left as it is.
	REQUIRE(::chmod((set_dir + "/NOTICE").c_str(), 0575) == 0);
	CHECK(as_other(payload.set, "REDSHIFT_DRIVER_DIR: " + READ_ONLY +
	                                " (a file of the driver set is writable by another user)") == 0);
	CHECK(Identity(set_dir, payload.set) == before);
	CHECK(Names(root) == Installed(payload.set));
}

TEST_CASE("A root-owned set that group or others can write is refused, sticky or not", "[driver][install]") {
	const int result = AsRoot([](const std::string &top) {
		Payload payload;
		const auto base = top + "/admin";
		if (!Expect(::mkdir(base.c_str(), 0755) == 0 && ::chmod(base.c_str(), 0755) == 0, "setup")) {
			return false;
		}
		auto inputs = Override(base);
		std::string set_dir;
		DriverError error;
		if (!Expect(PrepareDriverSet(inputs, payload.set, 0, set_dir, error),
		            "the administrator's install: " + DescribeAttempts(error.attempts)) ||
		    !Expect(::chmod((base + "/.redshift-driver").c_str(), 0755) == 0, "chmod")) {
			return false;
		}
		bool ok = true;
		for (const mode_t mode : {0755, 0775, 0757, 01777, 01775}) {
			std::string where, reason;
			ok = ok && Expect(::chmod(set_dir.c_str(), mode) == 0, "chmod");
			const auto state = CheckInstalledSet(set_dir, payload.set.Manifest(), OTHER_USER, where, reason);
			ok = ok && Expect(mode == 0755 ? state == SetState::VALID
			                               : state == SetState::INVALID && where == set_dir &&
			                                     reason == "the driver set is writable by another user",
			                  std::to_string(mode) + ": " + reason);
		}
		return ok;
	});
	if (result == 77) {
		WARN("neither root nor user namespaces are available here; not tested");
		return;
	}
	CHECK(result == 0);
}

TEST_CASE("A set or a file of it owned by another user is invalid", "[driver][install]") {
	if (::geteuid() != 0) {
		WARN("only root can give a file to another user; not tested here "
		     "(test/client/driver/run-loader-tests-as-root.sh runs it as root)");
		return;
	}
	for (const bool whole_set : {false, true}) {
		TempDir tmp;
		if (!PrivateAncestry(tmp)) {
			return;
		}
		Payload payload;
		DriverError error;
		const auto set_dir = Prepare(Override(tmp.path()), payload.set, error);
		REQUIRE(!set_dir.empty());
		const auto target = whole_set ? set_dir : set_dir + "/NOTICE";
		REQUIRE(::chown(target.c_str(), OTHER_USER, static_cast<gid_t>(-1)) == 0);
		CHECK(Check(set_dir, payload.set, 0) == Finding {SetState::INVALID, target,
		                                                 whole_set
		                                                     ? "the driver set is owned by another user"
		                                                     : "a file of the driver set is owned by another user"});
	}
}

TEST_CASE("A set that breaks a rule is invalid with its reason, and is set aside and rewritten in the same call",
          "[driver][install][repair]") {
	Payload payload;
	struct Case {
		const char *what;
		std::function<void(const std::string &set_dir)> damage;
		std::string where; // after the set directory
		std::string rule;
	};
	const std::vector<Case> cases {
	    {"a group-writable file", [](const std::string &d) { REQUIRE(::chmod((d + "/NOTICE").c_str(), 0520) == 0); },
	     "/NOTICE", "a file of the driver set is writable by another user"},
	    {"an others-writable file", [](const std::string &d) { REQUIRE(::chmod((d + "/NOTICE").c_str(), 0502) == 0); },
	     "/NOTICE", "a file of the driver set is writable by another user"},
	    {"an extra amazon.redshiftodbc.ini",
	     [](const std::string &d) { std::ofstream(d + "/amazon.redshiftodbc.ini") << "[Driver]\nLogLevel=6\n"; },
	     "/amazon.redshiftodbc.ini", "the driver set holds a file that is not part of it"},
	    {"an extra root.crt",
	     [](const std::string &d) { std::ofstream(d + "/root.crt") << "-----BEGIN CERTIFICATE-----\n"; }, "/root.crt",
	     "the driver set holds a file that is not part of it"},
	    {"a missing file", [](const std::string &d) { REQUIRE(::unlink((d + "/LICENSE").c_str()) == 0); }, "/LICENSE",
	     "a file of the driver set is missing"},
	    // To the same bytes, so only O_NOFOLLOW tells.
	    {"a link in a file's place",
	     [](const std::string &d) {
		     REQUIRE(::rename((d + "/NOTICE").c_str(), (d + "/../elsewhere").c_str()) == 0);
		     REQUIRE(::symlink((d + "/../elsewhere").c_str(), (d + "/NOTICE").c_str()) == 0);
	     },
	     "/NOTICE", "a file of the driver set is a symbolic link"},
	    {"a directory in a file's place",
	     [](const std::string &d) {
		     REQUIRE(::unlink((d + "/NOTICE").c_str()) == 0);
		     REQUIRE(::mkdir((d + "/NOTICE").c_str(), 0500) == 0);
	     },
	     "/NOTICE", "a file of the driver set is not a regular file"},
	    // Opened without blocking, so the check does not wait for a writer.
	    {"a FIFO in a file's place",
	     [](const std::string &d) {
		     REQUIRE(::unlink((d + "/NOTICE").c_str()) == 0);
		     REQUIRE(::mkfifo((d + "/NOTICE").c_str(), 0400) == 0);
	     },
	     "/NOTICE", "a file of the driver set is not a regular file"},
	    // open fails with ENXIO; what fstatat says is still evidence.
	    {"a socket in a file's place",
	     [](const std::string &d) {
		     REQUIRE(::unlink((d + "/NOTICE").c_str()) == 0);
		     BindSocket(d, "NOTICE");
	     },
	     "/NOTICE", "a file of the driver set is not a regular file"},
	    // Not readable by its owner (not as root): the mode fstatat reports is still evidence.
	    {"an unreadable, group-writable file",
	     [](const std::string &d) { REQUIRE(::chmod((d + "/NOTICE").c_str(), 0020) == 0); }, "/NOTICE",
	     "a file of the driver set is writable by another user"},
	    {"an unreadable, group-writable set directory",
	     [](const std::string &d) { REQUIRE(::chmod(d.c_str(), 0320) == 0); }, "",
	     "the driver set is writable by another user"},
	    {"a changed byte",
	     [](const std::string &d) {
		     REQUIRE(::chmod((d + "/libtest.so").c_str(), 0700) == 0);
		     std::ofstream(d + "/libtest.so", std::ios::binary | std::ios::in | std::ios::out) << "D";
		     REQUIRE(::chmod((d + "/libtest.so").c_str(), 0500) == 0);
	     },
	     "/libtest.so", "a file of the driver set does not match the embedded driver"},
	    {"a group-writable set directory", [](const std::string &d) { REQUIRE(::chmod(d.c_str(), 0770) == 0); }, "",
	     "the driver set is writable by another user"},
	    {"an others-writable sticky set directory",
	     [](const std::string &d) { REQUIRE(::chmod(d.c_str(), 01703) == 0); }, "",
	     "the driver set is writable by another user"},
	    // To a valid copy, so only O_NOFOLLOW tells.
	    {"a link in the set's place",
	     [](const std::string &d) {
		     REQUIRE(::rename(d.c_str(), (d + ".real").c_str()) == 0);
		     REQUIRE(::symlink((d + ".real").c_str(), d.c_str()) == 0);
	     },
	     "", "the driver set is a symbolic link"},
	    {"a file in the set's place",
	     [](const std::string &d) {
		     REQUIRE(::rename(d.c_str(), (d + ".real").c_str()) == 0);
		     std::ofstream(d) << "x";
	     },
	     "", "the driver set is not a directory"},
	};
	for (const auto &c : cases) {
		INFO(c.what);
		TempDir tmp;
		if (!PrivateAncestry(tmp)) {
			return;
		}
		DriverError error;
		const auto set_dir = Prepare(Override(tmp.path()), payload.set, error);
		REQUIRE(!set_dir.empty());
		c.damage(set_dir);
		CHECK(Check(set_dir, payload.set) == Finding {SetState::INVALID, set_dir + c.where, c.rule});
		// With the override as the only base, the set is repaired there: what was at its name is
		// set aside, the very same entry, dated now, and a new set written in the same call.
		const auto root = tmp / ".redshift-driver";
		const auto root_names = Names(root);
		// Written long ago: what is set aside is dated from the repair, so a sweep keeps it an hour.
		Age(set_dir, 3 * STALE_SECONDS);
		const auto damaged = Stat(set_dir);
		CHECK(Prepare(Override(tmp.path()), payload.set, error) == set_dir);
		CHECK(Check(set_dir, payload.set) == Finding {SetState::VALID, "", ""});
		CHECK(Stat(set_dir).st_ino != damaged.st_ino);
		const auto aside = Prefixed(root, ".invalid-");
		REQUIRE(aside.size() == 1);
		const auto moved = Stat(root + "/" + aside[0]);
		CHECK(moved.st_ino == damaged.st_ino);
		CHECK((moved.st_mode & S_IFMT) == (damaged.st_mode & S_IFMT));
		CHECK(moved.st_mtime >= ::time(nullptr) - 60);
		auto names = Names(root);
		names.erase(std::find(names.begin(), names.end(), aside[0]));
		CHECK(names == root_names); // no staging directory left, nothing else changed
	}
}

TEST_CASE("A damaged set in the extension's directory is repaired there, without falling back",
          "[driver][install][repair]") {
	TempDir tmp;
	if (!PrivateAncestry(tmp)) {
		return;
	}
	Payload payload;
	MakeDir(tmp / "ext", 0700);
	DriverError error;
	const auto set_dir = Prepare(Inputs(tmp / "ext", tmp / "cache"), payload.set, error);
	REQUIRE(set_dir == tmp / "ext/.redshift-driver/" + payload.set.set_sha256);
	Tamper(set_dir + "/libtest.so");
	CHECK(Prepare(Inputs(tmp / "ext", tmp / "cache"), payload.set, error) == set_dir);
	CHECK(Check(set_dir, payload.set).state == SetState::VALID);
	CHECK(Prefixed(tmp / "ext/.redshift-driver", ".invalid-").size() == 1);
	CHECK(::access((tmp / "cache").c_str(), F_OK) != 0);
}

TEST_CASE("Corrupt embedded bytes give EMBEDDED_BYTES_CORRUPT and write no set", "[driver][install]") {
	TempDir tmp;
	if (!PrivateAncestry(tmp)) {
		return;
	}
	Payload payload;
	payload.contents[1][0] = 'T'; // the licence no longer matches the hash recorded for it
	payload.Point();
	MakeDir(tmp / "ext", 0700);
	DriverError error;
	CHECK(Prepare(Inputs(tmp / "ext", tmp / "cache"), payload.set, error).empty());
	CHECK(error.kind == DriverErrorKind::EMBEDDED_BYTES_CORRUPT);
	// The file's name in the set, never a path.
	CHECK(error.message ==
	      "LICENSE in the extension does not match the hash recorded when it was built; reinstall the extension");
	CHECK(error.attempts.empty());
	// Nothing but the lock, which is taken before the bytes are hashed again.
	CHECK(Names(tmp / "ext/.redshift-driver") == LOCK_ONLY);
	// Every base would get the same bytes, so the per-user cache is not even made.
	CHECK(::access((tmp / "cache").c_str(), F_OK) != 0);
}

TEST_CASE("An error that is no evidence against the set leaves it untouched and moves on", "[driver][install]") {
	TempDir tmp;
	if (!PrivateAncestry(tmp)) {
		return;
	}
	Payload payload;
	MakeDir(tmp / "ext", 0700);
	DriverError error;
	const auto set_dir = Prepare(Inputs(tmp / "ext", tmp / "cache"), payload.set, error);
	REQUIRE(set_dir == tmp / "ext/.redshift-driver/" + payload.set.set_sha256);
	const auto before = Identity(set_dir, payload.set);
	const auto cache_set = tmp / "cache/.redshift-driver/" + payload.set.set_sha256;
	// Left by a process that died: a base whose set cannot be checked is not touched at all, so not
	// even swept.
	MakeDir(tmp / "ext/.redshift-driver/.staging-left", 0700);
	std::vector<std::string> ext_names {".lock", ".staging-left", payload.set.set_sha256};
	struct Case {
		const char *what;
		std::function<void()> arm;
		std::function<void()> disarm;
		std::string where;
		std::string rule;
	};
	const auto ext_root = tmp / "ext/.redshift-driver";
	std::vector<Case> cases {
	    {"a read error",
	     [&] {
		     g_read_path = set_dir + "/libtest.so";
		     g_read_errno = EIO;
	     },
	     [] { Hooks::Reset(); }, set_dir + "/libtest.so",
	     "a file of the driver set cannot be read: Input/output error"},
	    {"a listing that cannot be made",
	     [&] {
		     g_fdopendir_path = set_dir;
		     g_fdopendir_errno = EMFILE;
	     },
	     [] { Hooks::Reset(); }, set_dir, "the driver set cannot be listed: Too many open files"},
	    // Part way through: what was read so far is not the whole directory.
	    {"a listing that fails after an entry", [&] { g_readdir_path = set_dir; }, [] { Hooks::Reset(); }, set_dir,
	     "the driver set cannot be listed: Input/output error"},
	    {"a driver root that cannot be opened",
	     [&] {
		     g_openat_fail_path = ext_root;
		     g_openat_errno = EMFILE;
	     },
	     [] { Hooks::Reset(); }, ext_root, "the driver root cannot be opened: Too many open files"},
	};
	if (::geteuid() != 0) {
		// Unreadable is not one of the rules: the owner or an administrator may have reasons.
		cases.push_back({"an unreadable file", [&] { REQUIRE(::chmod((set_dir + "/NOTICE").c_str(), 0) == 0); },
		                 [&] { REQUIRE(::chmod((set_dir + "/NOTICE").c_str(), 0500) == 0); }, set_dir + "/NOTICE",
		                 "a file of the driver set cannot be opened: Permission denied"});
		cases.push_back({"an unreadable set", [&] { REQUIRE(::chmod(set_dir.c_str(), 0300) == 0); },
		                 [&] { REQUIRE(::chmod(set_dir.c_str(), 0700) == 0); }, set_dir,
		                 "the driver set cannot be opened: Permission denied"});
	} else {
		WARN("root reads any file; the unreadable cases were not tested as root");
	}
	for (const auto &c : cases) {
		INFO(c.what);
		Hooks hooks;
		c.arm();
		const auto finding = Check(set_dir, payload.set);
		CHECK(finding == Finding {SetState::UNCHECKED, c.where, c.rule});
		CHECK(Prepare(Inputs(tmp / "ext", tmp / "cache"), payload.set, error) == cache_set);
		// Nothing written or flushed in the extension's root: no staging copy, no rename.
		for (const auto &path : g_fsynced) {
			CHECK_FALSE(StartsWith(path, tmp / "ext/"));
		}
		c.disarm();
		CHECK(Identity(set_dir, payload.set) == before);
		CHECK(Names(tmp / "ext/.redshift-driver") == ext_names);
		CHECK(Check(set_dir, payload.set).state == SetState::VALID);
	}
	// What the attempt says, for one of them.
	{
		Hooks hooks;
		g_read_path = set_dir + "/libtest.so";
		g_read_errno = EIO;
		DriverRoots roots(Inputs(tmp / "ext", tmp / "cache"), ::geteuid());
		std::string root, found, where, reason;
		REQUIRE(roots.Next(root));
		CHECK(InstallDriverSet(root, payload.set, ::geteuid(), found, where, reason) == InstallResult::FAILED);
		CHECK(g_fsynced.empty());
		CHECK(Staging(root) == std::vector<std::string> {".staging-left"}); // none made, none swept
		CHECK(where == set_dir + "/libtest.so");
		CHECK(reason == "a file of the driver set cannot be read: Input/output error");
	}
	// A directory above the set that breaks step 2's rule says nothing about the set itself.
	REQUIRE(::chmod((tmp / "ext/.redshift-driver").c_str(), 0770) == 0);
	CHECK(Check(set_dir, payload.set) ==
	      Finding {SetState::UNCHECKED, tmp / "ext/.redshift-driver", "the driver root is writable by another user"});
	REQUIRE(::chmod((tmp / "ext/.redshift-driver").c_str(), 0700) == 0);
	REQUIRE(::chmod((tmp / "ext").c_str(), 0770) == 0);
	CHECK(Check(set_dir, payload.set) ==
	      Finding {SetState::UNCHECKED, tmp / "ext", "a parent directory is writable by another user"});
	REQUIRE(::chmod((tmp / "ext").c_str(), 0700) == 0);
	CHECK(Check(tmp / "ext/.redshift-driver/missing", payload.set) ==
	      Finding {SetState::MISSING, tmp / "ext/.redshift-driver/missing", "the driver set does not exist"});
}

TEST_CASE("An ACL of the set or of a file of it that cannot be read leaves the set unchecked; one that breaks the rule "
          "makes it invalid",
          "[driver][install]") {
	// Linux's AclRule finds nothing, and on macOS the file's stat may be refused first or the owner may still read the
	// ACL, so a stand-in for AclRule answers for one path. An unreadable ACL is no evidence against the set; an entry
	// that grants another account a write right is. Above the set, on the driver root, either says nothing about it.
	TempDir tmp;
	if (!PrivateAncestry(tmp)) {
		return;
	}
	Payload payload;
	DriverError error;
	const auto set_dir = Prepare(Inputs(tmp.path(), ""), payload.set, error);
	REQUIRE(set_dir == tmp / ".redshift-driver/" + payload.set.set_sha256);
	static std::string target;
	static AclCheck answer;
	const auto rule = [](const std::string &path, int, uid_t, bool) {
		return path == target ? answer : AclCheck();
	};
	const auto check = [&] {
		Finding finding;
		finding.state = CheckInstalledSet(set_dir, payload.set.Manifest(), ::geteuid(), finding.where, finding.reason,
		                                  nullptr, nullptr, rule);
		return finding;
	};
	const std::string unreadable = std::string(ACL_UNREADABLE) + ": Permission denied";
	const std::string broken = "is writable by another user or group through its access control list";
	struct Case {
		std::string path;
		std::string what;
		SetState broken_state;
	};
	const std::vector<Case> cases {
	    {tmp / ".redshift-driver", "the driver root ", SetState::UNCHECKED},
	    {set_dir, "the driver set ", SetState::INVALID},
	    {set_dir + "/libtest.so", "a file of the driver set ", SetState::INVALID},
	    {set_dir + "/NOTICE", "a file of the driver set ", SetState::INVALID},
	};
	for (const auto &c : cases) {
		INFO(c.path);
		target = c.path;
		answer = AclReadFailure(EACCES);
		CHECK(check() == Finding {SetState::UNCHECKED, c.path, c.what + unreadable});
		answer = {AclFinding::BROKEN, broken};
		CHECK(check() == Finding {c.broken_state, c.path, c.what + broken});
	}
	target.clear();
	CHECK(check().state == SetState::VALID);
}

TEST_CASE("A first file whose descriptor cannot be kept for the load leaves the set unchecked, with the error",
          "[driver][install]") {
	TempDir tmp;
	if (!PrivateAncestry(tmp)) {
		return;
	}
	Payload payload;
	DriverError error;
	const auto set_dir = Prepare(Inputs(tmp.path(), ""), payload.set, error);
	REQUIRE(set_dir == tmp / ".redshift-driver/" + payload.set.set_sha256);
	// Once the first file's hash has matched, the descriptor limit is lowered to 3, so the duplicate's
	// lowest number, 3, is at the limit (F_DUPFD_CLOEXEC fails with EINVAL); the guard puts it back,
	// whether the case runs to its end or leaves early.
	const DescriptorLimit limit;
	const auto first = set_dir + "/" + payload.set.files[0].name;
	const auto lower_after_first = [](const std::string &) {
		DescriptorLimitInUse()->Lower(3);
	};
	Finding finding;
	int kept = 0;
	finding.state = CheckInstalledSet(set_dir, payload.set.Manifest(), ::geteuid(), finding.where, finding.reason,
	                                  &kept, lower_after_first);
	limit.Restore();
	CHECK(finding ==
	      Finding {SetState::UNCHECKED, first, "a file of the driver set cannot be kept open: Invalid argument"});
	CHECK(kept == -1);
	// With the limit as it was, the same check keeps a descriptor of the first file.
	finding.state =
	    CheckInstalledSet(set_dir, payload.set.Manifest(), ::geteuid(), finding.where, finding.reason, &kept);
	CHECK(finding.state == SetState::VALID);
	REQUIRE(kept >= 3);
	CHECK(FdPath(kept) == first);
	CHECK((::fcntl(kept, F_GETFD) & FD_CLOEXEC) != 0);
	::close(kept);
}

TEST_CASE("RLIMIT_FSIZE below the driver's size refuses the base without a signal or a staging directory",
          "[driver][install]") {
	TempDir tmp;
	if (!PrivateAncestry(tmp)) {
		return;
	}
	Payload payload;
	const int status = InChild([&] {
		struct rlimit limit;
		if (::getrlimit(RLIMIT_FSIZE, &limit) != 0) {
			return Expect(false, "getrlimit");
		}
		const auto original = limit;
		limit.rlim_cur = 16; // the notices fit, the driver does not
		if (::setrlimit(RLIMIT_FSIZE, &limit) != 0) {
			return Expect(false, "setrlimit");
		}
		DriverError error;
		const bool refused = Prepare(Override(tmp.path()), payload.set, error).empty();
		// Before anything is written to stderr, which may be a log file already past the limit.
		if (::setrlimit(RLIMIT_FSIZE, &original) != 0) {
			return false;
		}
		return Expect(refused && error.attempts.size() == 1 &&
		                  error.attempts[0].directory == tmp / ".redshift-driver" &&
		                  error.attempts[0].reason ==
		                      "REDSHIFT_DRIVER_DIR: the driver set cannot be written: a file of the driver set is "
		                      "larger than the process's file size limit (RLIMIT_FSIZE)",
		              DescribeAttempts(error.attempts));
	});
	CHECK(status == 0); // -1: SIGXFSZ ended it
	CHECK(Names(tmp / ".redshift-driver") == LOCK_ONLY);
}

TEST_CASE("A full file system refuses the base and leaves no staging directory", "[driver][install]") {
	TempDir tmp;
	if (!PrivateAncestry(tmp)) {
		return;
	}
	Payload payload;
	MakeDir(tmp / "ext", 0700);
	for (const int error_number : {ENOSPC, EDQUOT}) {
		INFO(ErrnoText(error_number));
		Hooks hooks;
		g_write_prefix = tmp / "ext/.redshift-driver/.staging-";
		g_write_errno = error_number;
		DriverError error;
		const auto cache = tmp / ("cache-" + std::to_string(error_number));
		CHECK(Prepare(Inputs(tmp / "ext", cache), payload.set, error) ==
		      cache + "/.redshift-driver/" + payload.set.set_sha256);
		Hooks::Reset();
		CHECK(Names(tmp / "ext/.redshift-driver") == LOCK_ONLY);
		DriverRoots roots(Inputs(tmp / "ext", cache), ::geteuid());
		std::string root, set_dir, where, reason;
		REQUIRE(roots.Next(root));
		g_write_prefix = tmp / "ext/.redshift-driver/.staging-";
		g_write_errno = error_number;
		CHECK(InstallDriverSet(root, payload.set, ::geteuid(), set_dir, where, reason) == InstallResult::FAILED);
		Hooks::Reset();
		CHECK(StartsWith(where, root + "/.staging-"));
		CHECK(reason == "a file of the driver set cannot be written: " + ErrnoText(error_number));
		CHECK(Names(root) == LOCK_ONLY);
	}

	// A real one: a small tmpfs, in a user namespace.
	const int result = RunAsRootOfPrivateTree([] {
		Payload big(256 * 1024);
		if (!Expect(::mkdir("/full", 0755) == 0 && ::mount("tmpfs", "/full", "tmpfs", 0, "size=16k,mode=0755") == 0,
		            "a 16 KiB tmpfs")) {
			return false;
		}
		auto inputs = Override("/full");
		DriverError error;
		std::string set_dir;
		const bool refused = !PrepareDriverSet(inputs, big.set, 0, set_dir, error);
		return Expect(
		           refused && error.attempts.size() == 1 &&
		               error.attempts[0].reason ==
		                   "REDSHIFT_DRIVER_DIR: a file of the driver set cannot be written: No space left on device",
		           DescribeAttempts(error.attempts)) &&
		       Expect(Staging("/full/.redshift-driver").empty(), "a staging directory was left");
	});
	if (result == 77) {
		WARN("user namespaces are not available here; the small tmpfs was not tested");
		return;
	}
	CHECK(result == 0);
}

TEST_CASE("Nothing already at a file's name in the staging directory is reused", "[driver][install]") {
	TempDir tmp;
	if (!PrivateAncestry(tmp)) {
		return;
	}
	Hooks hooks;
	Payload payload;
	g_plant_prefix = tmp / ".redshift-driver/.staging-";
	DriverError error;
	CHECK(Prepare(Override(tmp.path()), payload.set, error).empty());
	REQUIRE(error.attempts.size() == 1);
	CHECK(StartsWith(error.attempts[0].directory, tmp / ".redshift-driver/.staging-"));
	CHECK(error.attempts[0].reason == "REDSHIFT_DRIVER_DIR: a file of the driver set cannot be created: File exists");
	CHECK(Names(tmp / ".redshift-driver") == LOCK_ONLY);
}

TEST_CASE("A first install renames its set into place once", "[driver][install]") {
	TempDir tmp;
	if (!PrivateAncestry(tmp)) {
		return;
	}
	Payload payload;
	Hooks hooks;
	std::vector<std::string> renamed;
	g_after_rename = [&](const std::string &to) {
		renamed.push_back(to);
	};
	DriverError error;
	CHECK(Prepare(Override(tmp.path()), payload.set, error) == tmp / (".redshift-driver/" + payload.set.set_sha256));
	CHECK(renamed == std::vector<std::string> {payload.set.set_sha256});
}

TEST_CASE("A first install whose rename fails refuses the base after one try", "[driver][install]") {
	TempDir tmp;
	if (!PrivateAncestry(tmp)) {
		return;
	}
	Payload payload;
	const auto root = tmp / ".redshift-driver";
	const auto set_dir = root + "/" + payload.set.set_sha256;
	for (const int error_number : {EIO, EXDEV, EACCES}) {
		INFO(ErrnoText(error_number));
		Hooks hooks;
		g_rename_errno = error_number;
		DriverError error;
		CHECK(Prepare(Override(tmp.path()), payload.set, error).empty());
		CHECK(g_renames == 1); // no other try
		Hooks::Reset();
		REQUIRE(error.attempts.size() == 1);
		CHECK(error.attempts[0].directory == set_dir);
		CHECK(error.attempts[0].reason ==
		      "REDSHIFT_DRIVER_DIR: the driver set cannot be put in place: " + ErrnoText(error_number));
		// The message is the reason and the one piece of advice every refusal gets.
		CHECK(error.message == NO_DIRECTORY_START + error.attempts[0].reason + ")" + DIRECTORY_ADVICE);
		CHECK(Names(root) == LOCK_ONLY);
	}
}

TEST_CASE("rename(2) never puts a directory over one that holds anything, or over a file, on this file system",
          "[driver][install]") {
	// The property the install relies on: POSIX gives rename(2) this behaviour on every file system,
	// so a plain rename never replaces a finished set. Measured here on the local file system the
	// tests run on.
	TempDir tmp;
	MakeDir(tmp / "from", 0700);
	{ std::ofstream(tmp / "from/file") << "new"; }
	MakeDir(tmp / "full", 0700);
	{ std::ofstream(tmp / "full/file") << "old"; }
	errno = 0;
	CHECK(::renameat(AT_FDCWD, (tmp / "from").c_str(), AT_FDCWD, (tmp / "full").c_str()) != 0);
	CHECK((errno == ENOTEMPTY || errno == EEXIST));
	CHECK(Read(tmp / "full/file") == "old");
	{ std::ofstream(tmp / "plain") << "a file"; }
	errno = 0;
	CHECK(::renameat(AT_FDCWD, (tmp / "from").c_str(), AT_FDCWD, (tmp / "plain").c_str()) != 0);
	CHECK(errno == ENOTDIR);
	REQUIRE(::symlink((tmp / "full").c_str(), (tmp / "link").c_str()) == 0);
	errno = 0;
	CHECK(::renameat(AT_FDCWD, (tmp / "from").c_str(), AT_FDCWD, (tmp / "link").c_str()) != 0);
	CHECK(errno == ENOTDIR);
	CHECK(Read(tmp / "full/file") == "old");
	// An empty directory, which is no set, is replaced, and the name never disappears.
	MakeDir(tmp / "empty", 0700);
	CHECK(::renameat(AT_FDCWD, (tmp / "from").c_str(), AT_FDCWD, (tmp / "empty").c_str()) == 0);
	CHECK(Read(tmp / "empty/file") == "new");
}

TEST_CASE("A rename that loses to another process uses the winner's set, and never replaces what is there",
          "[driver][install][repair]") {
	TempDir tmp;
	if (!PrivateAncestry(tmp)) {
		return;
	}
	Payload payload;
	const auto root = tmp / ".redshift-driver";
	const auto set_dir = root + "/" + payload.set.set_sha256;
	{
		// Something that does not take the lock (an older loader, or the user) puts a valid set in
		// place while this process is about to rename its own.
		Hooks hooks;
		g_fsync_prefix = root + "/.staging-";
		std::vector<std::string> winner;
		g_before_fsync = [&] {
			MakeDir(set_dir, 0700);
			for (size_t i = 0; i < payload.set.files.size(); i++) {
				const auto path = set_dir + "/" + payload.set.files[i].name;
				std::ofstream(path, std::ios::binary) << payload.contents[i];
				REQUIRE(::chmod(path.c_str(), 0500) == 0);
			}
			winner = Identity(set_dir, payload.set);
		};
		DriverError error;
		CHECK(Prepare(Override(tmp.path()), payload.set, error) == set_dir);
		REQUIRE(!winner.empty());
		CHECK(Identity(set_dir, payload.set) == winner);
		CHECK(Names(root) == Installed(payload.set));
	}
	{
		// A directory that appears at the name holding anything at all, part of a set say, is not
		// replaced by the rename: the next try finds it invalid and sets it aside.
		TempDir other;
		Hooks hooks;
		const auto other_root = other / ".redshift-driver";
		const auto other_set = other_root + "/" + payload.set.set_sha256;
		g_fsync_prefix = other_root + "/.staging-";
		struct stat appeared {};
		g_before_fsync = [&] {
			REQUIRE(::mkdir(other_set.c_str(), 0700) == 0);
			{ std::ofstream(other_set + "/LICENSE") << "part of a set"; }
			appeared = Stat(other_set);
		};
		DriverError error;
		CHECK(Prepare(Override(other.path()), payload.set, error) == other_set);
		CHECK(Check(other_set, payload.set).state == SetState::VALID);
		CHECK(g_mkdtemps == 2);
		const auto aside = Prefixed(other_root, ".invalid-");
		REQUIRE(aside.size() == 1);
		CHECK(Stat(other_root + "/" + aside[0]).st_ino == appeared.st_ino);
		CHECK(Names(other_root + "/" + aside[0]) == std::vector<std::string> {"LICENSE"});
		CHECK(Staging(other_root).empty());
	}
	{
		// A file that appears at the name is not replaced either (ENOTDIR): the next try sets it aside.
		TempDir other;
		Hooks hooks;
		const auto other_root = other / ".redshift-driver";
		const auto other_set = other_root + "/" + payload.set.set_sha256;
		g_fsync_prefix = other_root + "/.staging-";
		g_before_fsync = [&] {
			std::ofstream(other_set) << "a file";
		};
		DriverError error;
		CHECK(Prepare(Override(other.path()), payload.set, error) == other_set);
		CHECK(Check(other_set, payload.set).state == SetState::VALID);
		CHECK(g_mkdtemps == 2);
		const auto aside = Prefixed(other_root, ".invalid-");
		REQUIRE(aside.size() == 1);
		CHECK(Read(other_root + "/" + aside[0]) == "a file");
	}
	{
		// An empty directory that appears there is no set, and the rename replaces it, as rename(2)
		// does: one try, nothing set aside.
		TempDir other;
		Hooks hooks;
		const auto other_root = other / ".redshift-driver";
		const auto other_set = other_root + "/" + payload.set.set_sha256;
		g_fsync_prefix = other_root + "/.staging-";
		struct stat appeared {};
		g_before_fsync = [&] {
			REQUIRE(::mkdir(other_set.c_str(), 0700) == 0);
			appeared = Stat(other_set);
		};
		DriverError error;
		CHECK(Prepare(Override(other.path()), payload.set, error) == other_set);
		CHECK(Check(other_set, payload.set).state == SetState::VALID);
		CHECK(g_mkdtemps == 1);
		CHECK(Stat(other_set).st_ino != appeared.st_ino);
		CHECK(Names(other_root) == Installed(payload.set));
	}
}

TEST_CASE("A set that changes under every try is tried three times, then refused", "[driver][install][repair]") {
	TempDir tmp;
	if (!PrivateAncestry(tmp)) {
		return;
	}
	Payload payload;
	// Every rename finds a name already taken, yet nothing is there when the set is checked again: each
	// errno a file system gives for that (XFS EEXIST, ext4 and tmpfs ENOTEMPTY, a file there ENOTDIR),
	// and ENOENT, which a rename that may have happened reports.
	for (const int error_number : {EEXIST, ENOTEMPTY, ENOTDIR, ENOENT}) {
		INFO(ErrnoText(error_number));
		Hooks hooks;
		g_rename_errno = error_number;
		DriverError error;
		CHECK(Prepare(Override(tmp.path()), payload.set, error).empty());
		const int calls = g_renames;
		const int staged = g_mkdtemps;
		Hooks::Reset();
		CHECK(calls == INSTALL_TRIES);
		CHECK(staged == INSTALL_TRIES);
		REQUIRE(error.attempts.size() == 1);
		CHECK(error.attempts[0].reason ==
		      "REDSHIFT_DRIVER_DIR: the driver set kept changing while this process put its "
		      "own in place (at the last try, the driver set does not exist)");
		CHECK(Names(tmp / ".redshift-driver") == LOCK_ONLY);
	}
}

TEST_CASE("The loader refuses a base whose set changes under every try, without calling the platform loader",
          "[driver][install][loader]") {
	TempDir tmp;
	if (!PrivateAncestry(tmp)) {
		return;
	}
	Payload payload;
	LoaderConfig config;
	config.set = payload.set;
	config.identity.set_sha256 = payload.set.set_sha256;
	DriverLoader loader(config);
	LoadInputs inputs;
	inputs.bases = Override(tmp.path());
	inputs.search.list = [](std::vector<std::string> &) {
		return true;
	};
	inputs.search.cache_path = nullptr;
	inputs.hooks.on_platform_loader_call = redshift_driver_test::CountLoaderCall;
	Hooks hooks;
	g_rename_errno = EEXIST;
	const int loads = PlatformLoaderCalls();
	auto result = loader.Load(inputs);
	CHECK(g_renames == INSTALL_TRIES);
	Hooks::Reset();
	REQUIRE_FALSE(result.ok());
	CHECK(result.error.kind == DriverErrorKind::NO_USABLE_DIRECTORY);
	REQUIRE(result.error.attempts.size() == 1);
	CHECK(result.error.attempts[0].reason ==
	      "REDSHIFT_DRIVER_DIR: the driver set kept changing while this process put its own in place (at the last "
	      "try, the driver set does not exist)");
	CHECK(PlatformLoaderCalls() == loads);

	// A base that cannot be written is not tried three times: the failure is its own.
	TempDir other;
	DriverLoader second(config);
	inputs.bases = Override(other.path());
	Hooks again;
	g_mkdtemp_errno = ENOSPC;
	CHECK_FALSE(second.Load(inputs).ok());
	CHECK(g_mkdtemps == 1);
}

TEST_CASE("Eight concurrent first installs leave one valid set and no staging directory", "[driver][install]") {
	TempDir tmp;
	if (!PrivateAncestry(tmp)) {
		return;
	}
	Payload payload(1 << 20);
	int go[2];
	REQUIRE(::pipe(go) == 0);
	std::fflush(nullptr);
	std::vector<pid_t> children;
	for (int i = 0; i < 8; i++) {
		const pid_t child = ::fork();
		REQUIRE(child >= 0);
		if (child == 0) {
			::close(go[1]);
			char byte;
			if (::read(go[0], &byte, 1) != 0) {
				::_exit(2);
			}
			DriverError error;
			const bool ok = !Prepare(Override(tmp.path()), payload.set, error).empty();
			if (!ok) {
				std::fprintf(stderr, "%s\n", DescribeAttempts(error.attempts).c_str());
			}
			::_exit(ok ? 0 : 1);
		}
		children.push_back(child);
	}
	::close(go[0]);
	::close(go[1]);
	for (const auto child : children) {
		int status = 0;
		REQUIRE(::waitpid(child, &status, 0) == child);
		CHECK((WIFEXITED(status) && WEXITSTATUS(status) == 0));
	}
	CHECK(Names(tmp / ".redshift-driver") == Installed(payload.set));
	CHECK(Check(tmp / ".redshift-driver/" + payload.set.set_sha256, payload.set).state == SetState::VALID);
}

TEST_CASE("A write that makes no progress fails the base instead of spinning", "[driver][install]") {
	TempDir tmp;
	if (!PrivateAncestry(tmp)) {
		return;
	}
	Hooks hooks;
	Payload payload;
	g_write_prefix = tmp / ".redshift-driver/.staging-";
	g_write_zero = true;
	DriverError error;
	CHECK(Prepare(Override(tmp.path()), payload.set, error).empty());
	const bool spun = g_write_spun;
	Hooks::Reset();
	CHECK_FALSE(spun); // retried a write that made no progress
	REQUIRE(error.attempts.size() == 1);
	CHECK(error.attempts[0].reason ==
	      "REDSHIFT_DRIVER_DIR: a file of the driver set cannot be written: Input/output error");
	CHECK(Names(tmp / ".redshift-driver") == LOCK_ONLY);
}

TEST_CASE("A driver root that cannot take a staging directory is read-only for EACCES, EPERM and EROFS",
          "[driver][install]") {
	TempDir tmp;
	if (!PrivateAncestry(tmp)) {
		return;
	}
	Payload payload;
	DriverRoots roots(Override(tmp.path()), ::geteuid());
	std::string root;
	REQUIRE(roots.Next(root));
	for (const int error_number : {EACCES, EPERM, EROFS, ENOSPC}) {
		INFO(ErrnoText(error_number));
		Hooks hooks;
		g_mkdtemp_errno = error_number;
		std::string set_dir, where, reason;
		CHECK(InstallDriverSet(root, payload.set, ::geteuid(), set_dir, where, reason) == InstallResult::FAILED);
		Hooks::Reset();
		CHECK(where == root);
		CHECK(reason == (error_number == ENOSPC ? "the staging directory cannot be created: No space left on device"
		                                        : READ_ONLY));
		CHECK(Names(root) == LOCK_ONLY);
	}
}

TEST_CASE("A failed flush of a file, the staging directory or the root fails the base", "[driver][install]") {
	struct Case {
		const char *what;
		std::function<bool(const std::string &root, const std::string &path)> fails;
		std::string rule;
	};
	const auto in_staging = [](const std::string &root, const std::string &path) {
		return StartsWith(path, root + "/.staging-");
	};
	const std::vector<Case> cases {
	    {"a file",
	     [&](const std::string &root, const std::string &path) {
		     return in_staging(root, path) && path.find('/', root.size() + 1) != std::string::npos;
	     },
	     "a file of the driver set cannot be flushed: Input/output error"},
	    {"the staging directory",
	     [&](const std::string &root, const std::string &path) {
		     return in_staging(root, path) && path.find('/', root.size() + 1) == std::string::npos;
	     },
	     "the staging directory cannot be flushed: Input/output error"},
	    {"the root", [](const std::string &root, const std::string &path) { return path == root; },
	     "the driver root cannot be flushed: Input/output error"},
	};
	Payload payload;
	for (const auto &c : cases) {
		INFO(c.what);
		TempDir tmp;
		if (!PrivateAncestry(tmp)) {
			return;
		}
		const auto root = tmp / ".redshift-driver";
		Hooks hooks;
		g_fsync_fails = [&](const std::string &path) {
			return c.fails(root, path);
		};
		DriverError error;
		CHECK(Prepare(Override(tmp.path()), payload.set, error).empty());
		Hooks::Reset();
		REQUIRE(error.attempts.size() == 1);
		CHECK(error.attempts[0].reason == "REDSHIFT_DRIVER_DIR: " + c.rule);
		CHECK(Staging(root).empty());
	}
}

// The exit status of a case's child when the kernel refused it the read-only remount of its tmpfs.
constexpr int REMOUNT_REFUSED = 78;

TEST_CASE("An administrator's read-only set is used without writing, without running as root", "[driver][install]") {
	// Root of a private tree in a user namespace stands in for the administrator; OTHER_USER, named
	// as the user, for everyone else; a tmpfs remounted read-only for what they cannot write.
	const int result = RunAsRootOfPrivateTree([] {
		Hooks::Reset();
		Payload payload;
		Payload newer(0, "a newer licence");
		auto inputs = Override("/admin");
		std::string set_dir, found;
		DriverError error;
		if (!Expect(::mkdir("/admin", 0755) == 0 && ::mount("tmpfs", "/admin", "tmpfs", 0, "mode=0755") == 0,
		            "a tmpfs") ||
		    !Expect(PrepareDriverSet(inputs, payload.set, 0, set_dir, error), DescribeAttempts(error.attempts))) {
			return false;
		}
		const std::string root = "/admin/.redshift-driver";
		bool ok = ::chmod(root.c_str(), 0755) == 0 && ::chmod(set_dir.c_str(), 0755) == 0;
		for (const auto &file : payload.set.files) {
			ok = ok && ::chmod((set_dir + "/" + file.name).c_str(), 0555) == 0;
		}
		// One file group-writable is refused with its reason.
		std::string where, reason;
		ok =
		    Expect(ok && ::chmod((set_dir + "/NOTICE").c_str(), 0575) == 0, "chmod") &&
		    Expect(CheckInstalledSet(set_dir, payload.set.Manifest(), OTHER_USER, where, reason) == SetState::INVALID &&
		               reason == "a file of the driver set is writable by another user",
		           "group-writable: " + reason) &&
		    Expect(::chmod((set_dir + "/NOTICE").c_str(), 0555) == 0, "chmod");
		struct stat before;
		ok = ok && Expect(::lstat((set_dir + "/libtest.so").c_str(), &before) == 0, "lstat");
		if (ok && ::mount("tmpfs", "/admin", "tmpfs", MS_REMOUNT | MS_RDONLY, nullptr) != 0) {
			// A permission refusal of the remount in a user namespace, as some kernels give (4.14 among
			// them), is a fact of the host, not of the loader: the case stops (REMOUNT_REFUSED) and says
			// so. Any other error, EBUSY from a file the install left open for writing among them, fails it.
			const int refused = errno;
			std::fprintf(stderr, "remount read-only: %s\n", std::strerror(refused));
			if (refused != EPERM && refused != EACCES) {
				return false;
			}
			std::fflush(nullptr);
			::_exit(REMOUNT_REFUSED);
		}
		g_fsynced.clear();
		error = DriverError();
		ok = ok && Expect(PrepareDriverSet(inputs, payload.set, OTHER_USER, found, error) && found == set_dir &&
		                      g_fsynced.empty(),
		                  "the administrator's set: " + DescribeAttempts(error.attempts));
		struct stat after;
		ok = ok && Expect(::lstat((set_dir + "/libtest.so").c_str(), &after) == 0 && after.st_ino == before.st_ino &&
		                      after.st_mtim.tv_nsec == before.st_mtim.tv_nsec,
		                  "unchanged");
		// A newer driver not provisioned yet: mkdtemp fails with EROFS, the read-only reason.
		error = DriverError();
		ok =
		    ok && Expect(!PrepareDriverSet(inputs, newer.set, OTHER_USER, found, error) && error.attempts.size() == 1 &&
		                     error.attempts[0].reason == "REDSHIFT_DRIVER_DIR: " + READ_ONLY,
		                 "the newer driver: " + DescribeAttempts(error.attempts));
		ok = ok && Expect(InstallDriverSet(root, newer.set, 0, found, where, reason) == InstallResult::FAILED &&
		                      where == root && reason == READ_ONLY,
		                  "EROFS: " + reason);
		return ok && Expect(Names(root) == Installed(payload.set), "the root's names");
	});
	if (result == 77) {
		WARN("user namespaces are not available here; not tested");
		return;
	}
	if (result == REMOUNT_REFUSED) {
		// Not HostCannotRun: a strict run would fail on a 4.14 arm64 kernel.
		WARN("this kernel refuses (EPERM or EACCES) to remount a tmpfs read-only in a user namespace; the "
		     "administrator's read-only set is not tested here");
		return;
	}
	CHECK(result == 0);
}

TEST_CASE("A file or a set another user owns is invalid, readable or not, without running as root",
          "[driver][install]") {
	// In a user namespace that maps only the caller (as its root), files the host's root owns show
	// as the overflow user: neither the caller nor root. They are bind-mounted into the set.
	if (::geteuid() == 0) {
		WARN("as root the namespace maps the host's root; the root-run case covers the owner rule");
		return;
	}
	struct Foreign {
		std::string host;
		bool directory;
	};
	// A file and a directory others can read, and by their modes on Amazon Linux (0640, 0550) a file and a
	// directory they cannot; the case says at the end which kind a host has none of. Not the account files.
	std::vector<Foreign> foreign;
	for (const auto &candidate : std::vector<Foreign> {
	         {"/etc/host.conf", false}, {"/etc/libaudit.conf", false}, {"/etc", true}, {"/root", true}}) {
		struct stat st;
		if (::lstat(candidate.host.c_str(), &st) == 0 && st.st_uid == 0 &&
		    (candidate.directory ? S_ISDIR(st.st_mode) : S_ISREG(st.st_mode))) {
			foreign.push_back(candidate);
		}
	}
	TempDir top;
	const auto dir = top.path();
	const int result = RunAsNamespaceRoot([&] {
		bool ok = Expect(::mount("tmpfs", dir.c_str(), "tmpfs", 0, "mode=0755") == 0, "a tmpfs");
		for (size_t i = 0; ok && i < foreign.size(); i++) {
			const auto at = dir + "/host" + std::to_string(i);
			ok = foreign[i].directory ? ::mkdir(at.c_str(), 0700) == 0 : ::close(::creat(at.c_str(), 0600)) == 0;
			ok = Expect(ok && ::mount(foreign[i].host.c_str(), at.c_str(), nullptr, MS_BIND | MS_REC, nullptr) == 0,
			            "bind " + foreign[i].host);
		}
		if (!ok || !Expect(::chroot(dir.c_str()) == 0 && ::chdir("/") == 0, "chroot")) {
			return false;
		}
		for (size_t i = 0; ok && i < foreign.size(); i++) {
			Payload payload;
			const auto base = "/b" + std::to_string(i);
			auto inputs = Override(base);
			std::string set_dir, where, reason;
			DriverError error;
			ok = Expect(::mkdir(base.c_str(), 0755) == 0 && PrepareDriverSet(inputs, payload.set, 0, set_dir, error),
			            DescribeAttempts(error.attempts));
			const auto target = foreign[i].directory ? set_dir : set_dir + "/NOTICE";
			if (foreign[i].directory) {
				ok = ok && ::rename(set_dir.c_str(), (set_dir + ".real").c_str()) == 0 &&
				     ::mkdir(set_dir.c_str(), 0700) == 0;
			} else {
				ok = ok && ::unlink(target.c_str()) == 0 && ::close(::creat(target.c_str(), 0400)) == 0;
			}
			const auto source = "/host" + std::to_string(i);
			ok = Expect(ok && ::mount(source.c_str(), target.c_str(), nullptr, MS_BIND | MS_REC, nullptr) == 0,
			            "bind into the set");
			const auto state = CheckInstalledSet(set_dir, payload.set.Manifest(), 0, where, reason);
			const std::string rule = foreign[i].directory ? "the driver set is owned by another user"
			                                              : "a file of the driver set is owned by another user";
			ok = ok && Expect(state == SetState::INVALID && where == target && reason == rule,
			                  foreign[i].host + ": " + reason);
		}
		return ok;
	});
	if (result == 77) {
		WARN("user namespaces are not available here; not tested");
		return;
	}
	CHECK(result == 0);
	// A readable file shows the owner rule on an open file, and the unreadable ones show fstatat's evidence, which
	// a file and a directory reach by different checks; say which kind the host has none of. /etc is a readable
	// root-owned directory on every host.
	bool readable_file = false, unreadable_file = false, unreadable_directory = false;
	for (const auto &f : foreign) {
		const bool unreadable = ::access(f.host.c_str(), R_OK) != 0;
		if (f.directory) {
			unreadable_directory |= unreadable;
		} else {
			(unreadable ? unreadable_file : readable_file) = true;
		}
	}
	if (!readable_file) {
		WARN("no root-owned file here is readable; the readable file's owner case was not tested");
	}
	if (!unreadable_file) {
		WARN("no root-owned file here is unreadable; the unreadable file's owner case was not tested");
	}
	if (!unreadable_directory) {
		WARN("no root-owned directory here is unreadable; the unreadable directory's owner case was not tested");
	}
}

// The lock, the repair, the stale sweep and crashes.

namespace {

const std::string LOCK_NOT_OURS = "the driver root's lock file is not a regular file the user owns";
const std::string BAD_BYTES = "a file of the driver set does not match the embedded driver";
const std::string FAILED_HERE =
    "the file system did not keep the driver set this process wrote here (" + BAD_BYTES + ")";
const std::string WROTE_BAD_SET = "the driver set this process wrote does not pass the check (" + BAD_BYTES + ")";

// The multi-process cases fork and exec a helper: this executable, running only the hidden case
// "[install-helper]", which reads its one load from the environment, so each helper starts with none
// of the test's state. It loads once and exits 0 when the load came out as expected.
struct Helper {
	std::string override_dir;  // REDSHIFT_DRIVER_DIR, the only base; or else
	std::string extension_dir; // the extension's directory, then
	std::string cache_dir;     // the per-user cache
	size_t driver_size = 0;
	// A step of the install to stop at: kill-after-mkdtemp, kill-after-write, kill-after-rename-<n>,
	// stop-after-mkdtemp; or flip-writes, for a file system that does not keep what it is given.
	std::string arm;
	int deadline_milliseconds = LOCK_DEADLINE_MILLISECONDS;
	std::string expect;               // the set directory the load gives; empty when it must fail
	std::vector<std::string> reasons; // then its one attempt's reason is one of these
};

const char *const HELPER_TAG = "[install-helper]";

// Starts `helper` in a process of its own; with `go`, a pipe's read end, it waits for that pipe to be
// closed first, so several start together.
pid_t StartHelper(const Helper &helper, int go = -1) {
	std::fflush(nullptr);
	const pid_t child = ::fork();
	REQUIRE(child >= 0);
	if (child != 0) {
		return child;
	}
	std::string reasons;
	for (const auto &reason : helper.reasons) {
		reasons += reason + "\n";
	}
	::setenv("RS_TEST_HELPER_OVERRIDE", helper.override_dir.c_str(), 1);
	::setenv("RS_TEST_HELPER_EXTENSION", helper.extension_dir.c_str(), 1);
	::setenv("RS_TEST_HELPER_CACHE", helper.cache_dir.c_str(), 1);
	::setenv("RS_TEST_HELPER_DRIVER_SIZE", std::to_string(helper.driver_size).c_str(), 1);
	::setenv("RS_TEST_HELPER_ARM", helper.arm.c_str(), 1);
	::setenv("RS_TEST_HELPER_DEADLINE", std::to_string(helper.deadline_milliseconds).c_str(), 1);
	::setenv("RS_TEST_HELPER_EXPECT", helper.expect.c_str(), 1);
	::setenv("RS_TEST_HELPER_REASONS", reasons.c_str(), 1);
	::setenv("RS_TEST_HELPER_GO", std::to_string(go).c_str(), 1);
	if (go >= 0) {
		::fcntl(go, F_SETFD, 0); // kept across exec, unlike the write end
	}
	// Catch2's banner is the helper's only output there.
	const int null = ::open("/dev/null", O_WRONLY | O_CLOEXEC);
	if (null >= 0) {
		::dup2(null, STDOUT_FILENO);
	}
	::execl("/proc/self/exe", "redshift_driver_tests", HELPER_TAG, static_cast<char *>(nullptr));
	::_exit(127);
}

// A child's exit status, -1 when a signal ended it.
int Wait(pid_t child) {
	int status = 0;
	REQUIRE(::waitpid(child, &status, 0) == child);
	return WIFEXITED(status) ? WEXITSTATUS(status) : -1;
}

// `count` helpers started together; each one's exit status.
std::vector<int> HelpersAtOnce(int count, const Helper &helper) {
	int go[2];
	REQUIRE(::pipe2(go, O_CLOEXEC) == 0);
	std::vector<pid_t> children;
	for (int i = 0; i < count; i++) {
		children.push_back(StartHelper(helper, go[0]));
	}
	::close(go[0]);
	::close(go[1]);
	std::vector<int> statuses;
	for (const auto child : children) {
		statuses.push_back(Wait(child));
	}
	return statuses;
}

// A child that is ended, if the test has not reaped it, when the case ends.
struct Reaper {
	pid_t child;
	~Reaper() {
		if (child > 0) {
			::kill(child, SIGKILL);
			::waitpid(child, nullptr, 0);
		}
	}
};

bool Exists(const std::string &path) {
	struct stat st;
	return ::lstat(path.c_str(), &st) == 0;
}

void Write(const std::string &path, const std::string &text, mode_t mode) {
	{ std::ofstream(path, std::ios::binary) << text; }
	REQUIRE(::chmod(path.c_str(), mode) == 0);
}

std::string Env(const char *name) {
	const char *value = std::getenv(name);
	return value ? value : "";
}

} // namespace

TEST_CASE("An administrator's read-only copy under REDSHIFT_DRIVER_DIR is used as found", "[driver][install][repair]") {
	TempDir tmp;
	if (!PrivateAncestry(tmp)) {
		return;
	}
	Payload payload;
	// Made in one place and copied keeping its modes, then made
	// read-only. It is used as found: nothing made, renamed, flushed or locked.
	MakeDir(tmp / "shared", 0700);
	const auto root = tmp / "shared/.redshift-driver";
	const auto set_dir = root + "/" + payload.set.set_sha256;
	MakeDir(tmp / "local", 0700);
	DriverError error;
	const auto prepared = Prepare(Override(tmp / "local"), payload.set, error);
	REQUIRE(!prepared.empty());
	MakeDir(root, 0700);
	MakeDir(set_dir, 0700);
	for (const auto &file : payload.set.files) {
		Write(set_dir + "/" + file.name, Read(prepared + "/" + file.name), Mode(prepared + "/" + file.name));
	}
	REQUIRE(::chmod(set_dir.c_str(), 0500) == 0);
	REQUIRE(::chmod(root.c_str(), 0500) == 0);
	REQUIRE(::chmod((tmp / "shared").c_str(), 0555) == 0);
	const auto before = Identity(set_dir, payload.set);
	{
		Hooks hooks;
		CHECK(Prepare(Override(tmp / "shared"), payload.set, error) == set_dir);
		CHECK(g_mkdtemps == 0);
		CHECK(g_renames == 0);
		CHECK(g_fsynced.empty());
	}
	CHECK(Identity(set_dir, payload.set) == before);
	CHECK(Names(root) == std::vector<std::string> {payload.set.set_sha256});
	REQUIRE(::chmod((tmp / "shared").c_str(), 0700) == 0);
	REQUIRE(::chmod(root.c_str(), 0700) == 0);
}

TEST_CASE("The multi-process cases' helper", "[.][install-helper]") {
	if (std::getenv("RS_TEST_HELPER_GO") == nullptr) {
		return; // not started by StartHelper
	}
	const int go = std::stoi(Env("RS_TEST_HELPER_GO"));
	if (go >= 0) {
		char byte;
		while (::read(go, &byte, 1) > 0) {
		}
		::close(go);
	}
	Hooks::Reset();
	Payload payload(std::stoul(Env("RS_TEST_HELPER_DRIVER_SIZE")));
	const auto override_dir = Env("RS_TEST_HELPER_OVERRIDE");
	const auto first_base = override_dir.empty() ? Env("RS_TEST_HELPER_EXTENSION") : override_dir;
	const auto staging = first_base + "/.redshift-driver/.staging-";
	const auto arm = Env("RS_TEST_HELPER_ARM");
	const auto die = [] {
		::raise(SIGKILL);
	};
	if (arm == "kill-after-mkdtemp") {
		g_after_mkdtemp = [&](const std::string &) {
			die();
		};
	} else if (arm == "stop-after-mkdtemp") {
		g_after_mkdtemp = [](const std::string &) {
			::raise(SIGSTOP);
		};
	} else if (arm == "kill-after-write") {
		g_fsync_prefix = staging;
		g_after_fsync = [&](const std::string &) {
			die();
		};
	} else if (StartsWith(arm, "kill-after-rename-")) {
		g_after_rename = [&, renames = 0, at = std::stoi(arm.substr(18))](const std::string &) mutable {
			if (++renames == at) {
				die();
			}
		};
	} else if (arm == "flip-writes") {
		g_write_prefix = staging;
		g_write_flip = true;
	}
	auto inputs = override_dir.empty() ? Inputs(Env("RS_TEST_HELPER_EXTENSION"), Env("RS_TEST_HELPER_CACHE"))
	                                   : Override(override_dir);
	inputs.lock_deadline_milliseconds = std::stoi(Env("RS_TEST_HELPER_DEADLINE"));
	DriverError error;
	const auto found = Prepare(inputs, payload.set, error);
	const auto expect = Env("RS_TEST_HELPER_EXPECT");
	const auto reasons = Env("RS_TEST_HELPER_REASONS");
	const bool ok = expect.empty() ? found.empty() && error.attempts.size() == 1 &&
	                                     reasons.find(error.attempts[0].reason + "\n") != std::string::npos
	                               : found == expect;
	if (!ok) {
		std::fprintf(stderr, "helper (%s): %s\n", arm.c_str(), DescribeAttempts(error.attempts).c_str());
	}
	std::fflush(nullptr);
	::_exit(ok ? 0 : 1);
}

TEST_CASE("Eight processes repairing one tampered set together leave one valid set and one set-aside copy",
          "[driver][repair]") {
	TempDir tmp;
	if (!PrivateAncestry(tmp)) {
		return;
	}
	Payload payload(1 << 20);
	const auto root = tmp / ".redshift-driver";
	DriverError error;
	const auto set_dir = Prepare(Override(tmp.path()), payload.set, error);
	REQUIRE(!set_dir.empty());
	Tamper(set_dir + "/libtest.so");
	const auto aside_bytes = Read(set_dir + "/libtest.so");
	Helper helper;
	helper.override_dir = tmp.path();
	helper.driver_size = 1 << 20;
	helper.expect = set_dir;
	for (const int status : HelpersAtOnce(8, helper)) {
		CHECK(status == 0);
	}
	CHECK(Check(set_dir, payload.set).state == SetState::VALID);
	const auto aside = Prefixed(root, ".invalid-");
	REQUIRE(aside.size() == 1);
	CHECK(Read(root + "/" + aside[0] + "/libtest.so") == aside_bytes);
	CHECK(Staging(root).empty());
	CHECK(Names(root).size() == 3);
}

TEST_CASE("A repair sets the invalid set aside, then renames the new one in; a reader sees one set or the other, or "
          "none, never part of one",
          "[driver][repair]") {
	TempDir tmp;
	if (!PrivateAncestry(tmp)) {
		return;
	}
	Payload payload;
	const auto root = tmp / ".redshift-driver";
	DriverError error;
	const auto set_dir = Prepare(Override(tmp.path()), payload.set, error);
	REQUIRE(!set_dir.empty());
	{
		// The two renames, in that order: the set's name is missing between them, and only then.
		Hooks hooks;
		std::vector<std::string> renamed;
		std::vector<bool> present;
		g_after_rename = [&](const std::string &to) {
			renamed.push_back(to);
			present.push_back(Exists(set_dir));
		};
		Tamper(set_dir + "/NOTICE");
		CHECK(Prepare(Override(tmp.path()), payload.set, error) == set_dir);
		REQUIRE(renamed.size() == 2);
		CHECK(StartsWith(renamed[0], ".invalid-"));
		CHECK(renamed[1] == payload.set.set_sha256);
		CHECK(present == std::vector<bool> {false, true});
	}
	// And a reader in another process, checking the set as a loader would, through twenty repairs. The
	// repairs start only once it has checked the set once and said so, so it is reading while they run
	// however the processes are scheduled. What it may find: the new set, the old one (the byte the
	// repair replaces), or no set between the two renames; never a set with a file missing, which a
	// rename of the staging directory as a whole rules out.
	int stop[2];
	int ready[2];
	REQUIRE(::pipe(stop) == 0);
	REQUIRE(::pipe(ready) == 0);
	std::fflush(nullptr);
	Reaper reader {::fork()};
	REQUIRE(reader.child >= 0);
	if (reader.child == 0) {
		::close(stop[1]);
		::close(ready[0]);
		::fcntl(stop[0], F_SETFL, O_NONBLOCK);
		long checked = 0;
		long other = 0;
		char byte = 'r';
		while (::read(stop[0], &byte, 1) < 0) {
			Finding found;
			found.state = CheckInstalledSet(set_dir, payload.set.Manifest(), ::geteuid(), found.where, found.reason);
			const bool expected = found.state == SetState::VALID || found.state == SetState::MISSING ||
			                      (found.state == SetState::INVALID && found.reason == BAD_BYTES);
			if (!expected && other++ == 0) {
				std::fprintf(stderr, "reader found %s: %s\n", found.where.c_str(), found.reason.c_str());
			}
			if (checked++ == 0 && (::write(ready[1], &byte, 1) != 1 || ::close(ready[1]) != 0)) {
				::_exit(2);
			}
		}
		::_exit(other == 0 ? 0 : 1);
	}
	::close(stop[0]);
	::close(ready[1]);
	char said = 0;
	REQUIRE(::read(ready[0], &said, 1) == 1);
	::close(ready[0]);
	for (int i = 0; i < 20; i++) {
		Tamper(set_dir + "/libtest.so");
		CHECK(Prepare(Override(tmp.path()), payload.set, error) == set_dir);
	}
	::close(stop[1]);
	int status = 0;
	REQUIRE(::waitpid(reader.child, &status, 0) == reader.child);
	reader.child = 0;
	CHECK((WIFEXITED(status) && WEXITSTATUS(status) == 0));
	// Each repair set its copy aside under a name of its own.
	CHECK(Prefixed(root, ".invalid-").size() == 21);
	CHECK(Staging(root).empty());
}

TEST_CASE("A process killed after making the staging directory, after writing or after renaming leaves the next "
          "start working",
          "[driver][repair]") {
	struct Kill {
		const char *what;
		bool repair;
		const char *arm;
	};
	const std::vector<Kill> kills {
	    {"a first install, after making the staging directory", false, "kill-after-mkdtemp"},
	    {"a first install, after writing the files", false, "kill-after-write"},
	    {"a first install, after renaming", false, "kill-after-rename-1"},
	    {"a repair, after making the staging directory", true, "kill-after-mkdtemp"},
	    {"a repair, after writing the files", true, "kill-after-write"},
	    {"a repair, after setting the old set aside", true, "kill-after-rename-1"},
	    {"a repair, after renaming the new set in", true, "kill-after-rename-2"},
	};
	for (const auto &kill : kills) {
		INFO(kill.what);
		TempDir tmp;
		if (!PrivateAncestry(tmp)) {
			return;
		}
		Payload payload;
		const auto root = tmp / ".redshift-driver";
		const auto set_dir = root + "/" + payload.set.set_sha256;
		DriverError error;
		if (kill.repair) {
			REQUIRE(Prepare(Override(tmp.path()), payload.set, error) == set_dir);
			Tamper(set_dir + "/libtest.so");
			// Written long ago, as a damaged set usually is.
			Age(set_dir, 3 * STALE_SECONDS);
		}
		Helper killed;
		killed.override_dir = tmp.path();
		killed.arm = kill.arm;
		killed.expect = set_dir;
		CHECK(Wait(StartHelper(killed)) == -1);
		// The next start loads: the set is valid, written now or by the process that died.
		Helper next;
		next.override_dir = tmp.path();
		next.expect = set_dir;
		CHECK(Wait(StartHelper(next)) == 0);
		CHECK(Check(set_dir, payload.set).state == SetState::VALID);
		// What the dead process left is swept by the next start that takes the lock, except a copy it
		// set aside, which is kept for an hour from the repair.
		const auto aside = Prefixed(root, ".invalid-");
		Tamper(set_dir + "/NOTICE");
		CHECK(Prepare(Override(tmp.path()), payload.set, error) == set_dir);
		CHECK(Staging(root).empty());
		for (const auto &name : aside) {
			CHECK(Exists(root + "/" + name));
		}
	}
}

TEST_CASE("A staging directory in use is never removed, and a stopped lock holder makes the others move on after the "
          "deadline",
          "[driver][repair]") {
	TempDir tmp;
	if (!PrivateAncestry(tmp)) {
		return;
	}
	// About 30 seconds by default; shorter here.
	CHECK(BaseInputs().lock_deadline_milliseconds == 30000);
	Payload payload;
	MakeDir(tmp / "ext", 0700);
	const auto ext_root = tmp / "ext/.redshift-driver";
	const auto ext_set = ext_root + "/" + payload.set.set_sha256;
	// The holder stops itself with the lock held and its staging directory made.
	Helper holding;
	holding.extension_dir = tmp / "ext";
	holding.cache_dir = tmp / "cache";
	holding.arm = "stop-after-mkdtemp";
	holding.expect = ext_set;
	Reaper holder {StartHelper(holding)};
	int status = 0;
	REQUIRE(::waitpid(holder.child, &status, WUNTRACED) == holder.child);
	REQUIRE(WIFSTOPPED(status));
	const auto in_use = Staging(ext_root);
	REQUIRE(in_use.size() == 1);

	Helper other;
	other.extension_dir = tmp / "ext";
	other.cache_dir = tmp / "cache";
	other.deadline_milliseconds = 1000;
	other.expect = tmp / "cache/.redshift-driver/" + payload.set.set_sha256;
	const auto start = std::chrono::steady_clock::now();
	for (const int child_status : HelpersAtOnce(8, other)) {
		CHECK(child_status == 0);
	}
	const auto waited = std::chrono::steady_clock::now() - start;
	CHECK(waited >= std::chrono::milliseconds(1000));
	CHECK(waited < std::chrono::seconds(20));
	std::string set_dir, where, reason;
	CHECK(InstallDriverSet(ext_root, payload.set, ::geteuid(), set_dir, where, reason, 300) == InstallResult::FAILED);
	CHECK(where == ext_root);
	CHECK(reason == "another process is installing the driver");
	CHECK(Staging(ext_root) == in_use);

	REQUIRE(::kill(holder.child, SIGCONT) == 0);
	REQUIRE(::waitpid(holder.child, &status, 0) == holder.child);
	holder.child = 0;
	CHECK((WIFEXITED(status) && WEXITSTATUS(status) == 0));
	CHECK(Staging(ext_root).empty());
	CHECK(Check(ext_set, payload.set).state == SetState::VALID);
}

TEST_CASE("Under the lock, staging is removed at any age, set-aside copies after an hour, and nothing else",
          "[driver][repair]") {
	TempDir tmp;
	if (!PrivateAncestry(tmp)) {
		return;
	}
	Payload payload;
	const auto root = tmp / ".redshift-driver";
	MakeDir(root, 0700);
	const auto leftover = [&](const std::string &name, mode_t mode, time_t age) {
		MakeDir(root + "/" + name, 0700);
		Write(root + "/" + name + "/libtest.so", "left over", 0400);
		REQUIRE(::chmod((root + "/" + name).c_str(), mode) == 0);
		if (age > 0) {
			Age(root + "/" + name, age);
		}
	};
	// Removed: staging of any age, even fresh and read-only; set-aside copies an hour old or more,
	// even unreadable ones, and files.
	leftover(".staging-fresh", 0500, 0);
	Write(root + "/.staging-file", "x", 0600);
	leftover(".invalid-old", 0500, 2 * STALE_SECONDS);
	if (::geteuid() != 0) {
		leftover(".invalid-unreadable", 0, 2 * STALE_SECONDS);
	}
	Write(root + "/.invalid-old-file", "x", 0600);
	Age(root + "/.invalid-old-file", 2 * STALE_SECONDS);
	// Kept: set-aside copies younger than an hour, and every other name.
	leftover(".invalid-fresh", 0700, 0);
	leftover(".invalid-nearly", 0700, STALE_SECONDS - 60);
	leftover("other", 0700, 2 * STALE_SECONDS);
	Write(root + "/.other", "", 0600);
	DriverError error;
	const auto set_dir = Prepare(Override(tmp.path()), payload.set, error);
	REQUIRE(!set_dir.empty());
	std::vector<std::string> kept {".invalid-fresh", ".invalid-nearly",      ".lock",
	                               ".other",         payload.set.set_sha256, "other"};
	std::sort(kept.begin(), kept.end());
	CHECK(Names(root) == kept);

	// Another user's entries are never removed.
	leftover(".staging-theirs", 0700, 0);
	SweepDriverRoot(root, ::geteuid() + 1, ::time(nullptr) + 2 * STALE_SECONDS);
	CHECK(Exists(root + "/.staging-theirs"));
	CHECK(Exists(root + "/.invalid-fresh"));
	// The fast path takes no lock and sweeps nothing.
	{
		Hooks hooks;
		CHECK(Prepare(Override(tmp.path()), payload.set, error) == set_dir);
		CHECK(g_fsynced.empty());
		CHECK(Exists(root + "/.staging-theirs"));
	}
	SweepDriverRoot(root, ::geteuid(), ::time(nullptr));
	CHECK_FALSE(Exists(root + "/.staging-theirs"));
	CHECK(Exists(root + "/.invalid-fresh"));
}

TEST_CASE("A stale entry that is a link, or holds a link or a directory, is removed without following it or left in "
          "place",
          "[driver][repair]") {
	TempDir tmp;
	if (!PrivateAncestry(tmp)) {
		return;
	}
	Payload payload;
	const auto root = tmp / ".redshift-driver";
	const auto outside = tmp / "outside";
	MakeDir(root, 0700);
	MakeDir(outside, 0700);
	Write(outside + "/keep", "not the sweep's", 0600);
	// A link at the entry's name, fresh or set aside long ago: the link goes, its target stays.
	REQUIRE(::symlink(outside.c_str(), (root + "/.staging-link").c_str()) == 0);
	REQUIRE(::symlink(outside.c_str(), (root + "/.invalid-link").c_str()) == 0);
	Age(root + "/.invalid-link", 2 * STALE_SECONDS);
	REQUIRE(::mkfifo((root + "/.staging-fifo").c_str(), 0600) == 0);
	// A directory holding a link: removed, the link with it, its target untouched.
	MakeDir(root + "/.staging-holds-link", 0700);
	REQUIRE(::symlink((outside + "/keep").c_str(), (root + "/.staging-holds-link/NOTICE").c_str()) == 0);
	REQUIRE(::symlink(outside.c_str(), (root + "/.staging-holds-link/dir-link").c_str()) == 0);
	Write(root + "/.staging-holds-link/libtest.so", "x", 0400);
	// A directory holding a directory, or anything else that is not a file or a link: left in place,
	// whole, since a set is flat.
	MakeDir(root + "/.staging-holds-dir", 0700);
	MakeDir(root + "/.staging-holds-dir/sub", 0700);
	Write(root + "/.staging-holds-dir/sub/file", "x", 0600);
	Write(root + "/.staging-holds-dir/libtest.so", "x", 0400);
	MakeDir(root + "/.staging-holds-fifo", 0700);
	REQUIRE(::mkfifo((root + "/.staging-holds-fifo/NOTICE").c_str(), 0600) == 0);
	DriverError error;
	const auto set_dir = Prepare(Override(tmp.path()), payload.set, error);
	REQUIRE(!set_dir.empty());
	for (const auto *name : {".staging-link", ".invalid-link", ".staging-fifo", ".staging-holds-link"}) {
		CHECK_FALSE(Exists(root + "/" + name));
	}
	CHECK(Read(outside + "/keep") == "not the sweep's");
	CHECK(Names(outside) == std::vector<std::string> {"keep"});
	CHECK(Names(root + "/.staging-holds-dir") == std::vector<std::string> {"libtest.so", "sub"});
	CHECK(Names(root + "/.staging-holds-dir/sub") == std::vector<std::string> {"file"});
	CHECK(Names(root + "/.staging-holds-fifo") == std::vector<std::string> {"NOTICE"});
}

TEST_CASE("A removal the sweep cannot make does not stop it, and the next install or repair retries it",
          "[driver][repair]") {
	TempDir tmp;
	if (!PrivateAncestry(tmp)) {
		return;
	}
	Payload payload;
	const auto root = tmp / ".redshift-driver";
	MakeDir(root, 0700);
	// In the order the sweep lists them: a set-aside copy whose rmdir fails (EBUSY, as for a mount
	// point), a staging file and a staging directory's file whose unlink fails (EPERM, as for an
	// immutable file), then a staging directory and a file that must still be removed.
	MakeDir(root + "/.invalid-a", 0700);
	Write(root + "/.invalid-a/libtest.so", "left over", 0400);
	Age(root + "/.invalid-a", 2 * STALE_SECONDS);
	Write(root + "/.staging-b", "left over", 0600);
	MakeDir(root + "/.staging-c", 0700);
	Write(root + "/.staging-c/LICENSE", "left over", 0400);
	Write(root + "/.staging-c/NOTICE", "left over", 0400);
	MakeDir(root + "/.staging-d", 0700);
	Write(root + "/.staging-d/libtest.so", "left over", 0400);
	Write(root + "/.staging-e", "left over", 0600);
	DriverError error;
	std::string set_dir;
	{
		Hooks hooks;
		g_unlinkat_fails = [&](const std::string &path) {
			if (path == root + "/.invalid-a") {
				return EBUSY;
			}
			return path == root + "/.staging-b" || path == root + "/.staging-c/LICENSE" ? EPERM : 0;
		};
		set_dir = Prepare(Override(tmp.path()), payload.set, error);
	}
	REQUIRE(set_dir == root + "/" + payload.set.set_sha256);
	std::vector<std::string> left {".invalid-a", ".lock", ".staging-b", ".staging-c", payload.set.set_sha256};
	std::sort(left.begin(), left.end());
	CHECK(Names(root) == left);
	CHECK(Names(root + "/.invalid-a").empty());
	CHECK(Names(root + "/.staging-c") == std::vector<std::string> {"LICENSE"});
	// A start that finds the set valid takes the fast path, with no lock and no sweep: all stays.
	CHECK(Prepare(Override(tmp.path()), payload.set, error) == set_dir);
	CHECK(Names(root) == left);
	// The next start that takes the lock, here to repair a damaged set, sweeps again. Removing its file
	// dated the set-aside copy now, so it is dated an hour back, as if that start came an hour later.
	Age(root + "/.invalid-a", 2 * STALE_SECONDS);
	Tamper(set_dir + "/libtest.so");
	CHECK(Prepare(Override(tmp.path()), payload.set, error) == set_dir);
	CHECK(Check(set_dir, payload.set).state == SetState::VALID);
	const auto aside = Prefixed(root, ".invalid-");
	REQUIRE(aside.size() == 1); // the damaged set, set aside by this repair
	CHECK(aside[0] != ".invalid-a");
	std::vector<std::string> after {aside[0], ".lock", payload.set.set_sha256};
	std::sort(after.begin(), after.end());
	CHECK(Names(root) == after);
}

TEST_CASE("The sweep never leaves the driver root's file system", "[driver][repair]") {
	// Another file system mounted at a staging name has another st_dev, and is left as it is.
	const int result = RunAsRootOfPrivateTree([] {
		Hooks::Reset();
		Payload payload;
		const std::string root = "/b/.redshift-driver";
		bool ok = Expect(::mkdir("/b", 0755) == 0 && ::mkdir(root.c_str(), 0700) == 0, "setup");
		ok = ok && Expect(::mkdir((root + "/.staging-tmpfs").c_str(), 0700) == 0 &&
		                      ::mount("tmpfs", (root + "/.staging-tmpfs").c_str(), "tmpfs", 0, "mode=0700") == 0 &&
		                      ::close(::open((root + "/.staging-tmpfs/keep").c_str(), O_WRONLY | O_CREAT | O_CLOEXEC,
		                                     0600)) == 0,
		                  "a tmpfs at a staging name");
		auto inputs = Override("/b");
		std::string set_dir;
		DriverError error;
		ok = ok && Expect(PrepareDriverSet(inputs, payload.set, 0, set_dir, error), DescribeAttempts(error.attempts));
		struct stat st;
		return ok && Expect(::lstat((root + "/.staging-tmpfs/keep").c_str(), &st) == 0, "the tmpfs was emptied");
	});
	if (result == 77) {
		WARN("user namespaces are not available here; not tested");
		return;
	}
	CHECK(result == 0);
}

TEST_CASE("The sweep empties a bind mount of the root's own file system at a staging name", "[driver][repair]") {
	// The sweep compares st_dev alone: inside the 0700 driver root only the user or root can mount
	// anything, and both are trusted.
	const int result = RunAsRootOfPrivateTree([] {
		Hooks::Reset();
		Payload payload;
		const std::string root = "/b/.redshift-driver";
		bool ok =
		    Expect(::mkdir("/b", 0755) == 0 && ::mkdir(root.c_str(), 0700) == 0 && ::mkdir("/elsewhere", 0700) == 0 &&
		               ::close(::open("/elsewhere/file", O_WRONLY | O_CREAT | O_CLOEXEC, 0600)) == 0,
		           "setup");
		ok = ok && Expect(::mkdir((root + "/.staging-bind").c_str(), 0700) == 0 &&
		                      ::mount("/elsewhere", (root + "/.staging-bind").c_str(), nullptr, MS_BIND, nullptr) == 0,
		                  "a bind mount at a staging name");
		auto inputs = Override("/b");
		std::string set_dir;
		DriverError error;
		ok = ok && Expect(PrepareDriverSet(inputs, payload.set, 0, set_dir, error), DescribeAttempts(error.attempts));
		struct stat st;
		return ok && Expect(::lstat("/elsewhere/file", &st) != 0 && errno == ENOENT, "the bind mount was not emptied");
	});
	if (result == 77) {
		WARN("user namespaces are not available here; not tested");
		return;
	}
	CHECK(result == 0);
}

TEST_CASE("Eight processes whose written sets all fail each try once, and nothing on disk stops the next",
          "[driver][repair]") {
	TempDir tmp;
	if (!PrivateAncestry(tmp)) {
		return;
	}
	Payload payload;
	const auto root = tmp / ".redshift-driver";
	const auto set_dir = root + "/" + payload.set.set_sha256;
	// A file system that does not keep what it is given: each process writes once, sees the rule its
	// own set broke, and sets that set aside. No process is refused for another's failure.
	Helper helper;
	helper.override_dir = tmp.path();
	helper.arm = "flip-writes";
	helper.reasons = {"REDSHIFT_DRIVER_DIR: " + WROTE_BAD_SET};
	for (const int status : HelpersAtOnce(8, helper)) {
		CHECK(status == 0);
	}
	CHECK(Prefixed(root, ".invalid-").size() == 8);
	CHECK(Staging(root).empty());
	CHECK_FALSE(Exists(set_dir));
	// Once the file system keeps what it is given, the next process writes the set, and nothing but
	// the lock, the set and the set-aside copies is in the root.
	DriverError error;
	CHECK(Prepare(Override(tmp.path()), payload.set, error) == set_dir);
	CHECK(Check(set_dir, payload.set).state == SetState::VALID);
	std::vector<std::string> others;
	for (const auto &name : Names(root)) {
		if (name != ".lock" && name != payload.set.set_sha256 && !StartsWith(name, ".invalid-")) {
			others.push_back(name);
		}
	}
	CHECK(others.empty());
}

TEST_CASE("A set-aside copy never replaces an older one of the same name", "[driver][repair]") {
	TempDir tmp;
	if (!PrivateAncestry(tmp)) {
		return;
	}
	Payload payload;
	const auto root = tmp / ".redshift-driver";
	DriverError error;
	const auto set_dir = Prepare(Override(tmp.path()), payload.set, error);
	REQUIRE(!set_dir.empty());
	Tamper(set_dir + "/libtest.so");
	const auto damaged = Stat(set_dir);
	// An older copy already has the name this repair's staging directory would give its own.
	// Empty, which a plain rename would replace.
	Hooks hooks;
	std::string older;
	struct stat older_st {};
	g_after_mkdtemp = [&](const std::string &made) {
		// No REQUIRE in here: mkdtemp is noexcept.
		older = root + "/.invalid-" + made.substr(made.rfind("/.staging-") + 10);
		if (::mkdir(older.c_str(), 0700) != 0 || ::lstat(older.c_str(), &older_st) != 0) {
			older.clear();
		}
	};
	CHECK(Prepare(Override(tmp.path()), payload.set, error) == set_dir);
	Hooks::Reset();
	REQUIRE(!older.empty());
	CHECK(Stat(older).st_ino == older_st.st_ino);
	CHECK(Names(older).empty());
	CHECK(Stat(older + "-1").st_ino == damaged.st_ino);
	CHECK(Prefixed(root, ".invalid-").size() == 2);
}

TEST_CASE("When every set-aside name is taken, the repair fails with that reason and replaces nothing",
          "[driver][repair]") {
	TempDir tmp;
	if (!PrivateAncestry(tmp)) {
		return;
	}
	Payload payload;
	const auto root = tmp / ".redshift-driver";
	DriverError error;
	const auto set_dir = Prepare(Override(tmp.path()), payload.set, error);
	REQUIRE(!set_dir.empty());
	Tamper(set_dir + "/libtest.so");
	const auto damaged = Stat(set_dir);
	// All 100 names this repair's staging directory would give the invalid set, each an empty
	// directory a plain rename would replace.
	Hooks hooks;
	std::vector<std::string> older;
	g_after_mkdtemp = [&](const std::string &made) {
		// No REQUIRE in here: mkdtemp is noexcept.
		const auto base = root + "/.invalid-" + made.substr(made.rfind("/.staging-") + 10);
		for (int count = 0; count < 100; count++) {
			const auto name = base + (count == 0 ? std::string() : "-" + std::to_string(count));
			if (::mkdir(name.c_str(), 0700) == 0) {
				older.push_back(name);
			}
		}
	};
	CHECK(Prepare(Override(tmp.path()), payload.set, error).empty());
	Hooks::Reset();
	REQUIRE(older.size() == 100);
	REQUIRE(error.attempts.size() == 1);
	CHECK(error.attempts[0].reason ==
	      "REDSHIFT_DRIVER_DIR: the driver set cannot be moved aside: " + ErrnoText(EEXIST) +
	          " (a file of the driver set does not match the embedded driver)");
	CHECK(Stat(set_dir).st_ino == damaged.st_ino);
	for (const auto &name : older) {
		CHECK(Names(name).empty());
	}
	CHECK(Staging(root).empty());
}

TEST_CASE("A process whose written set failed does not write that base again", "[driver][repair]") {
	TempDir tmp;
	if (!PrivateAncestry(tmp)) {
		return;
	}
	Payload payload;
	const auto root = tmp / ".redshift-driver";
	CHECK(InChild([&] {
		      Hooks::Reset();
		      g_write_prefix = root + "/.staging-";
		      g_write_flip = true;
		      DriverError error;
		      bool ok = Expect(Prepare(Override(tmp.path()), payload.set, error).empty() &&
		                           DescribeAttempts(error.attempts) == "REDSHIFT_DRIVER_DIR: " + WROTE_BAD_SET,
		                       DescribeAttempts(error.attempts));
		      Hooks::Reset();
		      error = DriverError();
		      ok = ok && Expect(Prepare(Override(tmp.path()), payload.set, error).empty() &&
		                            DescribeAttempts(error.attempts) == "REDSHIFT_DRIVER_DIR: " + FAILED_HERE,
		                        DescribeAttempts(error.attempts));
		      return ok && Expect(g_mkdtemps == 0 && g_fsynced.empty(), "written again");
	      }) == 0);
	// Another process has no such memory, and nothing on disk stops it.
	DriverError error;
	CHECK(Prepare(Override(tmp.path()), payload.set, error) == root + "/" + payload.set.set_sha256);
}

TEST_CASE("Eight processes killed one after another after writing staging leave at most one staging directory each "
          "time, and a surviving load leaves none",
          "[driver][repair]") {
	TempDir tmp;
	if (!PrivateAncestry(tmp)) {
		return;
	}
	Payload payload;
	const auto root = tmp / ".redshift-driver";
	Helper killed;
	killed.override_dir = tmp.path();
	killed.arm = "kill-after-write";
	for (int i = 0; i < 8; i++) {
		INFO("kill " << i);
		CHECK(Wait(StartHelper(killed)) == -1);
		CHECK(Staging(root).size() == 1);
	}
	Helper surviving;
	surviving.override_dir = tmp.path();
	surviving.expect = root + "/" + payload.set.set_sha256;
	CHECK(Wait(StartHelper(surviving)) == 0);
	CHECK(Staging(root).empty());
	CHECK(Names(root) == Installed(payload.set));
}

TEST_CASE("A lock file that is a link, a FIFO, a directory or another user's refuses the base without following it "
          "or waiting",
          "[driver][repair]") {
	Payload payload;
	struct Case {
		const char *what;
		std::function<void(const std::string &lock, const std::string &target)> make;
	};
	std::vector<Case> cases {
	    {"a link",
	     [](const std::string &lock, const std::string &target) {
		     REQUIRE(::symlink(target.c_str(), lock.c_str()) == 0);
	     }},
	    {"a FIFO",
	     [](const std::string &lock, const std::string &) {
		     REQUIRE(::mkfifo(lock.c_str(), 0600) == 0);
	     }},
	    {"a directory",
	     [](const std::string &lock, const std::string &) {
		     MakeDir(lock, 0700);
	     }},
	    {"a socket",
	     [](const std::string &lock, const std::string &) {
		     BindSocket(Parent(lock), ".lock");
	     }},
	};
	if (::geteuid() == 0) {
		cases.push_back({"another user's file", [](const std::string &lock, const std::string &) {
			                 Write(lock, "", 0600);
			                 REQUIRE(::chown(lock.c_str(), OTHER_USER, static_cast<gid_t>(-1)) == 0);
		                 }});
	} else {
		WARN("only root can give the lock file to another user; that case is not tested here "
		     "(test/client/driver/run-loader-tests-as-root.sh runs it as root)");
	}
	for (const auto &c : cases) {
		INFO(c.what);
		TempDir tmp;
		if (!PrivateAncestry(tmp)) {
			return;
		}
		const auto root = tmp / ".redshift-driver";
		MakeDir(root, 0700);
		c.make(root + "/.lock", tmp / "target");
		Hooks hooks;
		std::string set_dir, where, reason;
		CHECK(InstallDriverSet(root, payload.set, ::geteuid(), set_dir, where, reason) == InstallResult::FAILED);
		CHECK(where == root);
		CHECK(reason == LOCK_NOT_OURS);
		CHECK(g_mkdtemps == 0);
		CHECK_FALSE(Exists(tmp / "target"));
		CHECK(Names(root) == LOCK_ONLY);
	}
}

TEST_CASE("A file system without locking, or an interrupted wait, refuses the base", "[driver][repair]") {
	TempDir tmp;
	if (!PrivateAncestry(tmp)) {
		return;
	}
	Payload payload;
	const auto root = tmp / ".redshift-driver";
	MakeDir(root, 0700);
	for (const int error_number : {ENOLCK, EBADF, EINTR, EIO}) {
		INFO(ErrnoText(error_number));
		Hooks hooks;
		g_flock_errno = error_number;
		std::string set_dir, where, reason;
		CHECK(InstallDriverSet(root, payload.set, ::geteuid(), set_dir, where, reason) == InstallResult::FAILED);
		CHECK(where == root);
		CHECK(reason == (error_number == EINTR ? std::string("another process is installing the driver")
		                 : error_number == EIO
		                     ? "the driver root's lock cannot be taken: " + ErrnoText(EIO)
		                     : std::string("the driver root's file system does not support locking")));
		CHECK(g_mkdtemps == 0);
		CHECK(Names(root) == LOCK_ONLY);
	}
}

TEST_CASE("A repair whose rename aside fails leaves the invalid set as it was", "[driver][repair]") {
	TempDir tmp;
	if (!PrivateAncestry(tmp)) {
		return;
	}
	Payload payload;
	const auto root = tmp / ".redshift-driver";
	DriverError error;
	const auto set_dir = Prepare(Override(tmp.path()), payload.set, error);
	REQUIRE(!set_dir.empty());
	Tamper(set_dir + "/libtest.so");
	const auto before = Identity(set_dir, payload.set);
	const std::string rule = " (a file of the driver set does not match the embedded driver)";
	for (const int error_number : {EIO, EACCES}) {
		INFO(ErrnoText(error_number));
		// The rename aside fails: the invalid set stays where it was, as it was.
		Hooks hooks;
		g_rename_errno = error_number;
		g_rename_to = ".invalid-";
		CHECK(Prepare(Override(tmp.path()), payload.set, error).empty());
		CHECK(g_renames == 1);
		Hooks::Reset();
		REQUIRE(error.attempts.size() == 1);
		CHECK(error.attempts[0].directory == set_dir);
		CHECK(error.attempts[0].reason ==
		      "REDSHIFT_DRIVER_DIR: the driver set cannot be moved aside: " + ErrnoText(error_number) + rule);
		CHECK(Names(root) == Installed(payload.set));
		CHECK(Identity(set_dir, payload.set) == before);
	}
}

TEST_CASE("A repair whose rename in fails leaves the name empty for the next start to write", "[driver][repair]") {
	TempDir tmp;
	if (!PrivateAncestry(tmp)) {
		return;
	}
	Payload payload;
	const auto root = tmp / ".redshift-driver";
	DriverError error;
	const auto set_dir = Prepare(Override(tmp.path()), payload.set, error);
	REQUIRE(!set_dir.empty());
	Tamper(set_dir + "/libtest.so");
	const std::string rule = " (a file of the driver set does not match the embedded driver)";
	// The rename in fails after the invalid set went aside: the name is left empty, and the next start
	// writes the set as for a first install.
	{
		Hooks hooks;
		g_rename_errno = EIO;
		g_rename_to = payload.set.set_sha256;
		CHECK(Prepare(Override(tmp.path()), payload.set, error).empty());
		CHECK(g_renames == 2);
		Hooks::Reset();
		REQUIRE(error.attempts.size() == 1);
		CHECK(error.attempts[0].reason ==
		      "REDSHIFT_DRIVER_DIR: the driver set cannot be put in place: " + ErrnoText(EIO) + rule);
		CHECK(Staging(root).empty());
		CHECK_FALSE(Exists(set_dir));
		CHECK(Prefixed(root, ".invalid-").size() == 1);
	}
	CHECK(Prepare(Override(tmp.path()), payload.set, error) == set_dir);
	CHECK(Check(set_dir, payload.set).state == SetState::VALID);
}

TEST_CASE("A rename in that happened although it was reported as failed uses the set it put in place",
          "[driver][install]") {
	TempDir tmp;
	if (!PrivateAncestry(tmp)) {
		return;
	}
	Payload payload;
	const auto root = tmp / ".redshift-driver";
	const auto set_dir = root + "/" + payload.set.set_sha256;
	DriverError error;
	{
		// A first install whose rename in the file system did, and then reported as failed: what the
		// rename put in place is not emptied, and is used.
		Hooks hooks;
		g_rename_done_to = payload.set.set_sha256;
		CHECK(Prepare(Override(tmp.path()), payload.set, error) == set_dir);
		CHECK(g_rename_done_to.empty()); // the hook ran
		Hooks::Reset();
		CHECK(Check(set_dir, payload.set).state == SetState::VALID);
		CHECK(Names(root) == Installed(payload.set));
	}
	{
		// The same for a repair: the invalid set went aside, and the new one is in place.
		Tamper(set_dir + "/libtest.so");
		Hooks hooks;
		g_rename_done_to = payload.set.set_sha256;
		CHECK(Prepare(Override(tmp.path()), payload.set, error) == set_dir);
		CHECK(g_rename_done_to.empty());
		Hooks::Reset();
		CHECK(Check(set_dir, payload.set).state == SetState::VALID);
		CHECK(Staging(root).empty());
		CHECK(Prefixed(root, ".invalid-").size() == 1);
	}
	{
		// And when something made a directory at the staging name after the rename: that one is not
		// this call's, so the set is still not emptied through the staging directory's descriptor.
		Tamper(set_dir + "/libtest.so");
		Hooks hooks;
		std::string staging;
		g_after_mkdtemp = [&](const std::string &made) {
			staging = made;
		};
		g_after_rename = [&](const std::string &to) {
			if (to == payload.set.set_sha256) {
				::mkdir(staging.c_str(), 0700);
			}
		};
		g_rename_done_to = payload.set.set_sha256;
		CHECK(Prepare(Override(tmp.path()), payload.set, error) == set_dir);
		CHECK(g_rename_done_to.empty());
		CHECK(g_renames == 2); // aside and in, once: the set put in place was used, not written again
		Hooks::Reset();
		CHECK(Check(set_dir, payload.set).state == SetState::VALID);
	}
	const auto installed = Stat(set_dir);
	CHECK(Prepare(Override(tmp.path()), payload.set, error) == set_dir);
	CHECK(Stat(set_dir).st_ino == installed.st_ino);
}

TEST_CASE("A rename in that happened, was reported as failed and left a set that fails the check sets that set aside",
          "[driver][repair]") {
	TempDir tmp;
	if (!PrivateAncestry(tmp)) {
		return;
	}
	Payload payload;
	const auto root = tmp / ".redshift-driver";
	const auto set_dir = root + "/" + payload.set.set_sha256;
	DriverError error;
	REQUIRE(!Prepare(Override(tmp.path()), payload.set, error).empty());
	Tamper(set_dir + "/libtest.so");
	Hooks hooks;
	// The repair's rename in happens, the file system damages what it put in place, and the rename
	// reports ENOENT all the same. The set at the name is then the one this call staged, so it is
	// this call's own failing set: set aside and remembered, as if the rename had reported success,
	// rather than tried again as a lost race. One set-aside copy of the old set and one of this
	// call's, and no further try.
	g_after_rename = [&](const std::string &to) {
		if (to == payload.set.set_sha256) {
			Tamper(set_dir + "/libtest.so");
		}
	};
	g_rename_done_to = payload.set.set_sha256;
	CHECK(Prepare(Override(tmp.path()), payload.set, error).empty());
	CHECK(g_rename_done_to.empty());
	const int renames = g_renames;
	Hooks::Reset();
	CHECK(renames == 3); // the old set aside, the new one in, this call's failing set aside
	REQUIRE(error.attempts.size() == 1);
	CHECK(error.attempts[0].reason == "REDSHIFT_DRIVER_DIR: " + WROTE_BAD_SET);
	CHECK(Prefixed(root, ".invalid-").size() == 2);
	CHECK(Staging(root).empty());
	CHECK_FALSE(Exists(set_dir));
	// Remembered as a set this process wrote: the next call does not write that root again.
	error = DriverError();
	CHECK(Prepare(Override(tmp.path()), payload.set, error).empty());
	CHECK(g_mkdtemps == 0);
	REQUIRE(error.attempts.size() == 1);
	CHECK(error.attempts[0].reason == "REDSHIFT_DRIVER_DIR: " + FAILED_HERE);
}

TEST_CASE("A set-aside name that cannot be looked at fails the repair with that error and replaces nothing",
          "[driver][repair]") {
	TempDir tmp;
	if (!PrivateAncestry(tmp)) {
		return;
	}
	Payload payload;
	const auto root = tmp / ".redshift-driver";
	DriverError error;
	const auto set_dir = Prepare(Override(tmp.path()), payload.set, error);
	REQUIRE(!set_dir.empty());
	Tamper(set_dir + "/libtest.so");
	const auto before = Identity(set_dir, payload.set);
	Hooks hooks;
	g_fstatat_prefix = ".invalid-";
	g_fstatat_errno = EIO;
	const bool prepared = !Prepare(Override(tmp.path()), payload.set, error).empty();
	const int looked = g_fstatats;
	const int renames = g_renames;
	Hooks::Reset();
	// Checked first: where the stand-in is not reached (before glibc 2.33, above), no EIO is made and the repair
	// succeeds, which says nothing about this case.
	if (looked == 0) {
		WARN("the library's fstatat does not reach this executable's; the EIO case was not tested");
		return;
	}
	CHECK_FALSE(prepared);
	CHECK(renames == 0);
	REQUIRE(error.attempts.size() == 1);
	const std::string rule = " (a file of the driver set does not match the embedded driver)";
	CHECK(error.attempts[0].reason ==
	      "REDSHIFT_DRIVER_DIR: the driver set cannot be moved aside: " + ErrnoText(EIO) + rule);
	CHECK(Names(root) == Installed(payload.set));
	CHECK(Identity(set_dir, payload.set) == before);
}

TEST_CASE("A driver root the user cannot write is read-only by its lock, and a damaged set there is left as it is",
          "[driver][repair]") {
	if (::geteuid() == 0) {
		WARN("root can write a read-only directory; not tested as root");
		return;
	}
	TempDir tmp;
	if (!PrivateAncestry(tmp)) {
		return;
	}
	Payload payload;
	const auto root = tmp / ".redshift-driver";
	DriverError error;
	const auto set_dir = Prepare(Override(tmp.path()), payload.set, error);
	REQUIRE(!set_dir.empty());
	Tamper(set_dir + "/libtest.so");
	const auto before = Identity(set_dir, payload.set);
	const std::string rule = " (a file of the driver set does not match the embedded driver)";
	for (const bool with_lock : {false, true}) {
		INFO((with_lock ? "a lock file that can still be opened" : "no lock file"));
		if (!with_lock) {
			REQUIRE(::unlink((root + "/.lock").c_str()) == 0);
		} else {
			REQUIRE(::chmod(root.c_str(), 0700) == 0);
			Write(root + "/.lock", "", 0600);
		}
		REQUIRE(::chmod(root.c_str(), 0500) == 0);
		Hooks hooks;
		std::string found, where, reason;
		CHECK(InstallDriverSet(root, payload.set, ::geteuid(), found, where, reason) == InstallResult::FAILED);
		CHECK(where == root);
		CHECK(reason == READ_ONLY + rule);
		// Opening the lock is the probe; with a lock that opens, mkdtemp is what fails.
		CHECK(g_mkdtemps == (with_lock ? 1 : 0));
		CHECK(Identity(set_dir, payload.set) == before);
	}
	REQUIRE(::chmod(root.c_str(), 0700) == 0);
}

TEST_CASE("Only the set this call wrote is set aside when its check fails", "[driver][repair]") {
	TempDir tmp;
	if (!PrivateAncestry(tmp)) {
		return;
	}
	Payload payload;
	const auto root = tmp / ".redshift-driver";
	const auto set_dir = root + "/" + payload.set.set_sha256;
	Hooks hooks;
	g_write_prefix = root + "/.staging-";
	g_write_flip = true;
	// Once the set is in place and the root flushed, something that does not take the lock puts a
	// directory of its own at the set's name, before the set is checked again.
	g_fsync_prefix = root;
	bool replaced = false;
	struct stat foreign {};
	g_after_fsync = [&](const std::string &path) {
		if (path == root && !replaced) {
			replaced = ::rename(set_dir.c_str(), (root + "/moved").c_str()) == 0 &&
			           ::mkdir(set_dir.c_str(), 0700) == 0 && ::lstat(set_dir.c_str(), &foreign) == 0;
		}
	};
	DriverError error;
	CHECK(Prepare(Override(tmp.path()), payload.set, error).empty());
	Hooks::Reset();
	REQUIRE(replaced);
	// Not taken for this call's own: the next try found it invalid and set it aside, then set its own
	// failed set aside.
	REQUIRE(error.attempts.size() == 1);
	CHECK(error.attempts[0].reason == "REDSHIFT_DRIVER_DIR: " + WROTE_BAD_SET);
	const auto aside = Prefixed(root, ".invalid-");
	REQUIRE(aside.size() == 2);
	CHECK(
	    (Stat(root + "/" + aside[0]).st_ino == foreign.st_ino || Stat(root + "/" + aside[1]).st_ino == foreign.st_ino));
	CHECK(Exists(root + "/moved"));
	CHECK_FALSE(Exists(set_dir));
}

TEST_CASE("Under a umask of 0277 or 0477 the lock is 0600, and a later repair still works", "[driver][repair]") {
	for (const mode_t mask : {0277, 0477}) {
		INFO("umask " << std::oct << mask);
		TempDir tmp;
		if (!PrivateAncestry(tmp)) {
			return;
		}
		Payload payload;
		const auto root = tmp / ".redshift-driver";
		const auto set_dir = root + "/" + payload.set.set_sha256;
		DriverError error;
		const mode_t old_mask = ::umask(mask);
		const auto installed = Prepare(Override(tmp.path()), payload.set, error);
		::umask(old_mask);
		REQUIRE(installed == set_dir);
		CHECK(Mode(root + "/.lock") == 0600);
		// A later repair, under the usual umask, opens the lock again.
		Tamper(set_dir + "/libtest.so");
		CHECK(Prepare(Override(tmp.path()), payload.set, error) == set_dir);
		CHECK(Prefixed(root, ".invalid-").size() == 1);
		CHECK(Mode(root + "/.lock") == 0600);
	}
}

TEST_CASE("A lock file of the user's that its mode makes unopenable, or that others can read, is made 0600 and used",
          "[driver][repair]") {
	TempDir tmp;
	if (!PrivateAncestry(tmp)) {
		return;
	}
	Payload payload;
	const auto root = tmp / ".redshift-driver";
	DriverError error;
	const auto set_dir = Prepare(Override(tmp.path()), payload.set, error);
	REQUIRE(!set_dir.empty());
	for (const mode_t mode : {0400, 0200, 0, 0644, 0666}) {
		INFO("mode " << std::oct << mode);
		REQUIRE(::chmod((root + "/.lock").c_str(), mode) == 0);
		Tamper(set_dir + "/libtest.so");
		CHECK(Prepare(Override(tmp.path()), payload.set, error) == set_dir);
		CHECK(Check(set_dir, payload.set).state == SetState::VALID);
		CHECK(Mode(root + "/.lock") == 0600);
	}
}

TEST_CASE("The lock is released before it is closed, so a child forked during an install does not keep it",
          "[driver][repair]") {
	TempDir tmp;
	if (!PrivateAncestry(tmp)) {
		return;
	}
	Payload payload;
	const auto root = tmp / ".redshift-driver";
	// A host thread forks, without exec, while the install holds the lock: the child shares the lock's
	// open file description, and lives on.
	int hold[2];
	REQUIRE(::pipe(hold) == 0);
	Hooks hooks;
	pid_t child = -1;
	g_after_mkdtemp = [&](const std::string &) {
		std::fflush(nullptr);
		child = ::fork();
		if (child == 0) {
			char byte;
			::close(hold[1]);
			while (::read(hold[0], &byte, 1) > 0) {
			}
			::_exit(0);
		}
	};
	DriverError error;
	CHECK(Prepare(Override(tmp.path()), payload.set, error) == root + "/" + payload.set.set_sha256);
	Hooks::Reset();
	::close(hold[0]);
	Reaper forked {child};
	REQUIRE(child > 0);
	const UniqueFd lock(::open((root + "/.lock").c_str(), O_RDWR | O_CLOEXEC));
	REQUIRE(lock.get() >= 0);
	CHECK(::flock(lock.get(), LOCK_EX | LOCK_NB) == 0);
	::close(hold[1]);
	CHECK(Wait(child) == 0);
	forked.child = 0;
}

TEST_CASE("An invalid set that disappears before it is set aside is replaced by the new set in the same try",
          "[driver][repair]") {
	TempDir tmp;
	if (!PrivateAncestry(tmp)) {
		return;
	}
	Payload payload;
	const auto root = tmp / ".redshift-driver";
	DriverError error;
	const auto set_dir = Prepare(Override(tmp.path()), payload.set, error);
	REQUIRE(!set_dir.empty());
	Tamper(set_dir + "/libtest.so");
	// After the check under the lock, something that does not take the lock takes the set away, so the
	// rename aside finds nothing to move.
	Hooks hooks;
	g_fsync_prefix = root + "/.staging-";
	g_before_fsync = [&] {
		::rename(set_dir.c_str(), (root + "/gone").c_str());
	};
	std::vector<std::string> renamed;
	g_after_rename = [&](const std::string &to) {
		renamed.push_back(to);
	};
	CHECK(Prepare(Override(tmp.path()), payload.set, error) == set_dir);
	const int staged = g_mkdtemps;
	const int renames = g_renames;
	Hooks::Reset();
	CHECK(Check(set_dir, payload.set).state == SetState::VALID);
	CHECK(staged == 1);
	CHECK(renames == 2); // the rename aside that found nothing, then the rename in
	CHECK(renamed == std::vector<std::string> {payload.set.set_sha256});
	CHECK(Staging(root).empty());
	CHECK(Prefixed(root, ".invalid-").empty());
	CHECK(Exists(root + "/gone"));
}

TEST_CASE("A set this call wrote that disappears before its check is tried again; one replaced every time is "
          "reported as changing",
          "[driver][repair]") {
	TempDir tmp;
	if (!PrivateAncestry(tmp)) {
		return;
	}
	Payload payload;
	const auto root = tmp / ".redshift-driver";
	const auto set_dir = root + "/" + payload.set.set_sha256;
	{
		// Taken away once, after the root is flushed: the next try writes it again.
		Hooks hooks;
		g_fsync_prefix = root;
		bool taken = false;
		g_after_fsync = [&](const std::string &path) {
			if (path == root && !taken) {
				taken = ::rename(set_dir.c_str(), (root + "/taken").c_str()) == 0;
			}
		};
		DriverError error;
		CHECK(Prepare(Override(tmp.path()), payload.set, error) == set_dir);
		CHECK(taken);
		CHECK(g_mkdtemps == 2);
	}
	{
		// Replaced by an empty directory every time: never set aside as this process's, and the last
		// try's rule is the plain one.
		TempDir other;
		const auto other_root = other / ".redshift-driver";
		const auto other_set = other_root + "/" + payload.set.set_sha256;
		Hooks hooks;
		g_fsync_prefix = other_root;
		int replaced = 0;
		g_after_fsync = [&](const std::string &path) {
			if (path == other_root &&
			    ::rename(other_set.c_str(), (other_root + "/moved-" + std::to_string(replaced)).c_str()) == 0 &&
			    ::mkdir(other_set.c_str(), 0700) == 0) {
				replaced++;
			}
		};
		DriverError error;
		CHECK(Prepare(Override(other.path()), payload.set, error).empty());
		Hooks::Reset();
		CHECK(replaced == INSTALL_TRIES);
		REQUIRE(error.attempts.size() == 1);
		CHECK(error.attempts[0].reason ==
		      "REDSHIFT_DRIVER_DIR: the driver set kept changing while this process put its "
		      "own in place (at the last try, a file of the driver set is missing)");
	}
}

TEST_CASE("A set this call wrote that cannot be checked is left in place, not set aside", "[driver][repair]") {
	TempDir tmp;
	if (!PrivateAncestry(tmp)) {
		return;
	}
	Payload payload;
	const auto root = tmp / ".redshift-driver";
	const auto set_dir = root + "/" + payload.set.set_sha256;
	Hooks hooks;
	g_fsync_prefix = root;
	g_after_fsync = [&](const std::string &path) {
		if (path == root) {
			g_read_path = set_dir + "/libtest.so";
			g_read_errno = EIO;
		}
	};
	DriverError error;
	CHECK(Prepare(Override(tmp.path()), payload.set, error).empty());
	const int staged = g_mkdtemps;
	Hooks::Reset();
	CHECK(staged == 1);
	REQUIRE(error.attempts.size() == 1);
	CHECK(error.attempts[0].reason ==
	      "REDSHIFT_DRIVER_DIR: the driver set this process wrote cannot be checked (a file "
	      "of the driver set cannot be read: Input/output error)");
	CHECK(Check(set_dir, payload.set).state == SetState::VALID);
	CHECK(Prefixed(root, ".invalid-").empty());
}

TEST_CASE("A lost rename whose winner cannot be checked fails at once, without another try", "[driver][repair]") {
	TempDir tmp;
	if (!PrivateAncestry(tmp)) {
		return;
	}
	Payload payload;
	const auto root = tmp / ".redshift-driver";
	const auto set_dir = root + "/" + payload.set.set_sha256;
	Hooks hooks;
	g_fsync_prefix = root + "/.staging-";
	g_before_fsync = [&] {
		MakeDir(set_dir, 0700);
		for (size_t i = 0; i < payload.set.files.size(); i++) {
			Write(set_dir + "/" + payload.set.files[i].name, payload.contents[i], 0500);
		}
		// Once only: a retry would find the winner valid.
		g_read_path = set_dir + "/libtest.so";
		g_read_errno = EIO;
		g_read_once = true;
	};
	DriverError error;
	CHECK(Prepare(Override(tmp.path()), payload.set, error).empty());
	const int staged = g_mkdtemps;
	Hooks::Reset();
	CHECK(staged == 1);
	REQUIRE(error.attempts.size() == 1);
	CHECK(error.attempts[0].reason ==
	      "REDSHIFT_DRIVER_DIR: a file of the driver set cannot be read: Input/output error");
	CHECK(Check(set_dir, payload.set).state == SetState::VALID);
	CHECK(Staging(root).empty());
}

TEST_CASE("A process's memory of a failed set covers that set hash and that root only", "[driver][repair]") {
	TempDir tmp;
	if (!PrivateAncestry(tmp)) {
		return;
	}
	Payload payload;
	Payload newer(0, "a newer licence");
	const auto root = tmp / ".redshift-driver";
	CHECK(InChild([&] {
		      Hooks::Reset();
		      g_write_prefix = root + "/.staging-";
		      g_write_flip = true;
		      DriverError error;
		      bool ok = Expect(Prepare(Override(tmp.path()), payload.set, error).empty(), "the failed write");
		      Hooks::Reset();
		      // Another driver version in the same root is written.
		      error = DriverError();
		      ok = ok && Expect(Prepare(Override(tmp.path()), newer.set, error) == root + "/" + newer.set.set_sha256,
		                        "another set hash: " + DescribeAttempts(error.attempts));
		      // A root made again at the same path is another root.
		      ok = ok && Expect(::rename(root.c_str(), (tmp / "old-root").c_str()) == 0, "moving the root");
		      error = DriverError();
		      ok =
		          ok && Expect(Prepare(Override(tmp.path()), payload.set, error) == root + "/" + payload.set.set_sha256,
		                       "a new root at the same path: " + DescribeAttempts(error.attempts));
		      return ok;
	      }) == 0);
}

TEST_CASE("A swept directory that cannot be emptied as found is left in place, owner-only", "[driver][repair]") {
	// What the sweep cannot remove whole stays, made 0700 to be listed and emptied: it is garbage in the
	// private driver root, and nothing reads its mode.
	TempDir tmp;
	if (!PrivateAncestry(tmp)) {
		return;
	}
	Payload payload;
	const auto root = tmp / ".redshift-driver";
	MakeDir(root, 0700);
	std::vector<mode_t> modes {0500, 0700};
	if (::geteuid() != 0) {
		modes.push_back(0); // root lists any directory
	}
	for (const mode_t mode : modes) {
		const auto name = root + "/.staging-holds-dir-" + std::to_string(mode);
		MakeDir(name, 0700);
		MakeDir(name + "/sub", 0700);
		REQUIRE(::chmod(name.c_str(), mode) == 0);
	}
	// One that cannot be listed: the listing fails after a name.
	const auto unlisted = root + "/.staging-unlisted";
	MakeDir(unlisted, 0700);
	Write(unlisted + "/libtest.so", "x", 0400);
	Write(unlisted + "/NOTICE", "x", 0400);
	REQUIRE(::chmod(unlisted.c_str(), 0500) == 0);
	DriverError error;
	{
		Hooks hooks;
		g_readdir_path = unlisted;
		CHECK(!Prepare(Override(tmp.path()), payload.set, error).empty());
	}
	CHECK(Mode(unlisted) == 0700);
	CHECK(Names(unlisted) == std::vector<std::string> {"NOTICE", "libtest.so"});
	// Without /proc's descriptor entries: made 0700 through a second descriptor.
	const auto no_proc = root + "/.staging-no-proc";
	MakeDir(no_proc, 0700);
	MakeDir(no_proc + "/sub", 0700);
	REQUIRE(::chmod(no_proc.c_str(), 0500) == 0);
	// The set away, so the next load takes the lock and sweeps.
	REQUIRE(::rename((root + "/" + payload.set.set_sha256).c_str(), (root + "/moved").c_str()) == 0);
	{
		Hooks hooks;
		redshift_driver_test::g_chmod_fail_prefix = "/proc/thread-self/fd/";
		redshift_driver_test::g_chmod_fail_errno = ENOENT;
		CHECK(!Prepare(Override(tmp.path()), payload.set, error).empty());
	}
	CHECK(Mode(no_proc) == 0700);
	CHECK(Names(no_proc) == std::vector<std::string> {"sub"});
	// One made 0700 that then cannot be opened again to be listed: left as it is.
	const auto unopened = root + "/.staging-unopened";
	MakeDir(unopened, 0700);
	Write(unopened + "/libtest.so", "x", 0400);
	REQUIRE(::chmod(unopened.c_str(), 0500) == 0);
	REQUIRE(::rename((root + "/" + payload.set.set_sha256).c_str(), (root + "/moved-again").c_str()) == 0);
	{
		Hooks hooks;
		g_reopen_fail_path = unopened;
		CHECK(!Prepare(Override(tmp.path()), payload.set, error).empty());
	}
	CHECK(Names(unopened) == std::vector<std::string> {"libtest.so"});
	// One swapped for another directory between the O_PATH open and the reopen: the one found is made
	// 0700 through its descriptor, not its name, and neither is emptied.
	const auto swapped = root + "/.staging-swapped";
	// Moved within the root: moving a 0500 directory to another parent needs write permission on it.
	const auto away = root + "/swapped-away";
	MakeDir(swapped, 0700);
	Write(swapped + "/libtest.so", "x", 0400);
	REQUIRE(::chmod(swapped.c_str(), 0500) == 0);
	REQUIRE(::rename((root + "/" + payload.set.set_sha256).c_str(), (root + "/moved-third").c_str()) == 0);
	{
		Hooks hooks;
		g_swap_path = swapped;
		g_swap_to = away;
		CHECK(!Prepare(Override(tmp.path()), payload.set, error).empty());
		CHECK(g_swap_path.empty()); // the swap happened
	}
	CHECK(Mode(away) == 0700);
	CHECK(Names(away) == std::vector<std::string> {"libtest.so"});
	CHECK(Mode(swapped) == 0700);
	CHECK(Names(swapped).empty());
	for (const mode_t mode : modes) {
		INFO("mode " << std::oct << mode);
		const auto name = root + "/.staging-holds-dir-" + std::to_string(mode);
		CHECK(Mode(name) == 0700);
		CHECK(Names(name) == std::vector<std::string> {"sub"});
	}
}

TEST_CASE("A set written in the extension's directory that fails its check falls back to the per-user cache, and "
          "the process does not write the directory again",
          "[driver][repair]") {
	TempDir tmp;
	if (!PrivateAncestry(tmp)) {
		return;
	}
	Payload payload;
	MakeDir(tmp / "ext", 0700);
	const auto ext_root = tmp / "ext/.redshift-driver";
	const auto ext_set = ext_root + "/" + payload.set.set_sha256;
	const auto cache_set = tmp / "cache/.redshift-driver/" + payload.set.set_sha256;
	// The extension's directory does not keep what is written to it; the cache does.
	Helper first;
	first.extension_dir = tmp / "ext";
	first.cache_dir = tmp / "cache";
	first.arm = "flip-writes";
	first.expect = cache_set;
	CHECK(Wait(StartHelper(first)) == 0);
	CHECK(Prefixed(ext_root, ".invalid-").size() == 1);
	// Another process tries the extension's directory again, and uses it once it keeps what is
	// written: nothing on disk says to skip it.
	Helper second;
	second.extension_dir = tmp / "ext";
	second.cache_dir = tmp / "cache";
	second.expect = ext_set;
	CHECK(Wait(StartHelper(second)) == 0);
	REQUIRE(::rename(ext_set.c_str(), (ext_root + "/moved").c_str()) == 0);
	// And the process that wrote it, through its memory, makes no staging directory there again.
	CHECK(InChild([&] {
		      Hooks::Reset();
		      g_write_prefix = ext_root + "/.staging-";
		      g_write_flip = true;
		      auto inputs = Inputs(tmp / "ext", tmp / "cache2");
		      DriverError error;
		      bool ok = Expect(Prepare(inputs, payload.set, error) ==
		                           tmp / "cache2/.redshift-driver/" + payload.set.set_sha256,
		                       "the first load");
		      Hooks::Reset();
		      error = DriverError();
		      ok = ok && Expect(Prepare(inputs, payload.set, error) ==
		                            tmp / "cache2/.redshift-driver/" + payload.set.set_sha256,
		                        "the second load");
		      return ok && Expect(g_mkdtemps == 0, "a staging directory in the extension's directory again");
	      }) == 0);
}

TEST_CASE("A lock file that a concurrent first install made 0600 meanwhile is opened again and used",
          "[driver][repair]") {
	if (::geteuid() == 0) {
		WARN("root opens a file whatever its mode; not tested as root");
		return;
	}
	TempDir tmp;
	if (!PrivateAncestry(tmp)) {
		return;
	}
	Payload payload;
	const auto root = tmp / ".redshift-driver";
	DriverError error;
	const auto set_dir = Prepare(Override(tmp.path()), payload.set, error);
	REQUIRE(!set_dir.empty());
	// Made 0400 by the process that created it under umask 0277, which fixes it to 0600 just after
	// this process's open fails.
	REQUIRE(::chmod((root + "/.lock").c_str(), 0400) == 0);
	Tamper(set_dir + "/libtest.so");
	Hooks hooks;
	bool fixed = false;
	g_after_openat_fail = [&](const std::string &name, int failed) {
		if (name == ".lock" && failed == EACCES && !fixed) {
			fixed = ::chmod((root + "/.lock").c_str(), 0600) == 0;
		}
	};
	CHECK(Prepare(Override(tmp.path()), payload.set, error) == set_dir);
	Hooks::Reset();
	CHECK(fixed);
	CHECK(Check(set_dir, payload.set).state == SetState::VALID);
}

TEST_CASE("A lock file that is swapped for a link, or is the user's FIFO, is not made openable", "[driver][repair]") {
	if (::geteuid() == 0) {
		WARN("root opens a file whatever its mode; not tested as root");
		return;
	}
	Payload payload;
	for (const bool link : {true, false}) {
		INFO((link ? "a link to a file of the user's" : "a FIFO of the user's"));
		TempDir tmp;
		if (!PrivateAncestry(tmp)) {
			return;
		}
		const auto root = tmp / ".redshift-driver";
		const auto lock = root + "/.lock";
		const auto target = tmp / "target";
		MakeDir(root, 0700);
		Hooks hooks;
		if (link) {
			// The lock is unopenable when tried, then swapped for a link before it is made openable.
			Write(lock, "", 0400);
			Write(target, "", 0400);
			g_after_openat_fail = [&](const std::string &name, int failed) {
				if (name == ".lock" && failed == EACCES && !Exists(lock + ".was")) {
					::rename(lock.c_str(), (lock + ".was").c_str());
					::symlink(target.c_str(), lock.c_str());
				}
			};
		} else {
			REQUIRE(::mkfifo(lock.c_str(), 0600) == 0);
			REQUIRE(::chmod(lock.c_str(), 0) == 0);
		}
		std::string set_dir, where, reason;
		CHECK(InstallDriverSet(root, payload.set, ::geteuid(), set_dir, where, reason) == InstallResult::FAILED);
		Hooks::Reset();
		CHECK(reason == READ_ONLY);
		if (link) {
			CHECK(Mode(target) == 0400);
		} else {
			CHECK(Mode(lock) == 0);
		}
		CHECK_FALSE(Exists(set_dir));
	}
}
