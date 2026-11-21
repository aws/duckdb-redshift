// A private temporary directory for the driver loader's tests, removed with everything in it.
#pragma once

#include "catch.hpp"

#include <climits>
#include <cstdio>
#include <cstdlib>
#include <string>

#include <fcntl.h>
#include <ftw.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>
#if defined(__linux__)
#include <sched.h>
#include <sys/mount.h>
#endif

namespace redshift_driver_test {

// The chmod stand-in in driver_directory_test.cpp fails a path that starts with
// g_chmod_fail_prefix with g_chmod_fail_errno (nonzero), as /proc's descriptor entries do where
// /proc is missing. One stand-in per executable, so the install tests reach it from here.
inline std::string g_chmod_fail_prefix;
inline int g_chmod_fail_errno = 0;

class TempDir {
public:
	// Under $TMPDIR (or /tmp), made with mkdtemp and resolved, so it has no symbolic link.
	TempDir() {
		const char *tmp = std::getenv("TMPDIR");
		std::string pattern = std::string(tmp && tmp[0] == '/' ? tmp : "/tmp") + "/redshift-driver-test-XXXXXX";
		REQUIRE(::mkdtemp(&pattern[0]) != nullptr);
		char resolved[PATH_MAX];
		REQUIRE(::realpath(pattern.c_str(), resolved) != nullptr);
		path_ = resolved;
	}
	~TempDir() {
		// Children may have been made read-only by a test; make them writable to remove them.
		::nftw(path_.c_str(), MakeWritable, 16, FTW_PHYS);
		::nftw(path_.c_str(), Remove, 16, FTW_DEPTH | FTW_PHYS);
	}
	TempDir(const TempDir &) = delete;
	TempDir &operator=(const TempDir &) = delete;

	const std::string &path() const {
		return path_;
	}
	std::string operator/(const std::string &name) const {
		return path_ + "/" + name;
	}

private:
	static int MakeWritable(const char *path, const struct stat *st, int type, struct FTW *) {
		if (type == FTW_D || type == FTW_DNR) {
			::chmod(path, 0700);
		}
		return 0;
	}
	static int Remove(const char *path, const struct stat *, int, struct FTW *) {
		::remove(path);
		return 0;
	}
	std::string path_;
};

// Sets or unsets an environment variable for one scope and restores it afterwards.
class ScopedEnv {
public:
	ScopedEnv(const char *name, const char *value) : name_(name) {
		const char *old = std::getenv(name);
		had_ = old != nullptr;
		old_ = old ? old : "";
		if (value) {
			::setenv(name, value, 1);
		} else {
			::unsetenv(name);
		}
	}
	~ScopedEnv() {
		if (had_) {
			::setenv(name_.c_str(), old_.c_str(), 1);
		} else {
			::unsetenv(name_.c_str());
		}
	}
	ScopedEnv(const ScopedEnv &) = delete;
	ScopedEnv &operator=(const ScopedEnv &) = delete;

private:
	std::string name_;
	bool had_;
	std::string old_;
};

#if defined(__linux__)
// Runs `body` in a child process that is root in a user and mount namespace of its own, mapped to
// the caller's user: files the child creates are owned by root there, and it may mount a tmpfs.
// Returns 0 when body returned true, 1 when false, and 77 when user namespaces are unavailable.
template <class BODY>
int RunAsNamespaceRoot(BODY body) {
	const uid_t uid = ::geteuid();
	const gid_t gid = ::getegid();
	const pid_t child = ::fork();
	REQUIRE(child >= 0);
	if (child == 0) {
		if (::unshare(CLONE_NEWUSER | CLONE_NEWNS) != 0) {
			::_exit(77);
		}
		auto write = [](const char *path, const std::string &text) {
			const int fd = ::open(path, O_WRONLY);
			const bool ok = fd >= 0 && ::write(fd, text.data(), text.size()) == static_cast<ssize_t>(text.size());
			if (fd >= 0) {
				::close(fd);
			}
			return ok;
		};
		write("/proc/self/setgroups", "deny");
		if (!write("/proc/self/uid_map", "0 " + std::to_string(uid) + " 1") ||
		    !write("/proc/self/gid_map", "0 " + std::to_string(gid) + " 1") ||
		    ::mount("none", "/", nullptr, MS_REC | MS_PRIVATE, nullptr) != 0) {
			::_exit(77);
		}
		::_exit(body() ? 0 : 1);
	}
	int status = 0;
	REQUIRE(::waitpid(child, &status, 0) == child);
	REQUIRE(WIFEXITED(status));
	return WEXITSTATUS(status);
}

// Runs `body` in a child that is root of a file system of its own: in a user namespace, with a
// tmpfs as `/`, where every directory belongs to the namespace's root and so counts as root's, as
// an administrator's would. Root's exceptions are tested this way without running as root.
// Returns the body's result as an exit status, 77 where user namespaces are unavailable.
template <class BODY>
int RunAsRootOfPrivateTree(BODY body) {
	TempDir top;
	const auto dir = top.path();
	return RunAsNamespaceRoot([&] {
		if (::mount("tmpfs", dir.c_str(), "tmpfs", 0, "mode=0755") != 0 || ::chroot(dir.c_str()) != 0 ||
		    ::chdir("/") != 0) {
			std::fprintf(stderr, "cannot make a private tmpfs root\n");
			return false;
		}
		return body();
	});
}
#endif

} // namespace redshift_driver_test
