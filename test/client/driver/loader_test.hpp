// Helpers the loader's tests share (driver_root_test.cpp, driver_install_test.cpp, and through loader_payload.hpp the
// rest; environment_allocation_test.cpp includes it for InBoundedChild and Expect).
#pragma once

#include "driver_loader_core.hpp"
#include "temp_dir.hpp"

#include "catch.hpp"

#include <atomic>
#include <chrono>
#include <csignal>
#include <cstdio>
#include <exception>
#include <functional>
#include <string>

#include <sys/resource.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>

namespace redshift_driver_test {

// How many times a load in these tests called the platform loader to load a driver: LoadFrom() gives
// every load this counter as its on_platform_loader_call hook, so the cases can say that a refusal
// "made no loader call". The shipped loader keeps no such count.
inline std::atomic<int> &LoaderCallCounter() {
	static std::atomic<int> counter {0};
	return counter;
}
inline void CountLoaderCall() {
	LoaderCallCounter()++;
}
inline int PlatformLoaderCalls() {
	return LoaderCallCounter().load();
}

// How long a case waits for another thread before it fails.
constexpr std::chrono::seconds WAIT_LIMIT {10};

inline bool NeverNoExec(const std::string &) {
	return false;
}

// PrepareWith for a case that only wants the installed set: the first root whose driver root holds,
// or now receives, a valid copy of `set`. Fills `set_dir` and returns true, or returns false with
// `error` as PrepareWith's.
inline bool PrepareDriverSet(const duckdb::redshift_driver::detail::BaseInputs &inputs,
                             const duckdb::redshift_driver::detail::DriverSet &set, uid_t user, std::string &set_dir,
                             duckdb::redshift_driver::DriverError &error) {
	using namespace duckdb::redshift_driver::detail;
	return PrepareWith(
	    inputs, set, user,
	    [](const std::string &, const DriverRoots &, std::string &, std::string &) { return SetUse::DONE; }, set_dir,
	    error);
}

inline void MakeDir(const std::string &path, mode_t mode) {
	REQUIRE(::mkdir(path.c_str(), 0700) == 0);
	// chmod, so the umask cannot narrow the mode under test.
	REQUIRE(::chmod(path.c_str(), mode) == 0);
}

inline mode_t Mode(const std::string &path) {
	struct stat st;
	REQUIRE(::lstat(path.c_str(), &st) == 0);
	return st.st_mode & 07777;
}

// A user other than the caller, for tests that need a directory or file someone else owns.
constexpr uid_t OTHER_USER = 12345;

// NO_USABLE_DIRECTORY's message: how it starts, and its advice on REDSHIFT_DRIVER_DIR.
const std::string NO_DIRECTORY_START = "no usable directory to install the Redshift ODBC driver in (";
const std::string DIRECTORY_ADVICE = ". Set REDSHIFT_DRIVER_DIR to the absolute path of an existing directory on a "
                                     "file system that allows executing code, with no symbolic link in its path, "
                                     "where it and every directory above it are owned by you or root and writable by "
                                     "no one else";

// The cases build their directories in a TempDir, under $TMPDIR or /tmp, and the walk checks every
// directory above it too. Where one of those is not private (a /tmp that is not sticky, a
// group-writable TMPDIR) the case warns and stops.
inline bool PrivateAncestry(const TempDir &tmp) {
	using duckdb::redshift_driver::detail::AncestryEnd;
	std::string where, reason;
	if (duckdb::redshift_driver::detail::CheckAncestry(tmp.path(), AncestryEnd::BASE, ::geteuid(), where, reason)) {
		return true;
	}
	WARN("the temporary directory is not private here (" + where + ": " + reason +
	     "); set TMPDIR to a private directory; not tested");
	return false;
}

class DescriptorLimit;

// The guard a hook reaches, since the hooks are plain function pointers with no state of their own.
// Null outside a case that holds one, so a hook written without a guard fails loudly instead of using
// a stale pointer.
inline const DescriptorLimit *&DescriptorLimitInUse() {
	static const DescriptorLimit *guard = nullptr;
	return guard;
}

// The process's descriptor limit, lowered for as long as the guard lives and restored when it dies,
// so a case that leaves early (a failed REQUIRE throws) cannot leave the rest of the suite with no
// descriptors. It publishes itself in DescriptorLimitInUse() for the hook that must trip the failure
// under test to call Lower() through, and takes that back when it dies.
class DescriptorLimit {
public:
	DescriptorLimit() {
		REQUIRE(::getrlimit(RLIMIT_NOFILE, &saved_) == 0);
		previous_ = DescriptorLimitInUse();
		DescriptorLimitInUse() = this;
	}
	~DescriptorLimit() {
		(void)::setrlimit(RLIMIT_NOFILE, &saved_);
		DescriptorLimitInUse() = previous_;
	}
	DescriptorLimit(const DescriptorLimit &) = delete;
	DescriptorLimit &operator=(const DescriptorLimit &) = delete;
	// To `soft` descriptors, which fails every open and every F_DUPFD above it.
	void Lower(rlim_t soft) const {
		struct rlimit limit = saved_;
		limit.rlim_cur = soft;
		(void)::setrlimit(RLIMIT_NOFILE, &limit);
	}
	void Restore() const {
		(void)::setrlimit(RLIMIT_NOFILE, &saved_);
	}

private:
	struct rlimit saved_;
	const DescriptorLimit *previous_ = nullptr;
};

// For a body run in a child: false, with what failed on stderr, unless `ok`.
inline bool Expect(bool ok, const std::string &what) {
	if (!ok) {
		std::fprintf(stderr, "failed: %s\n", what.c_str());
	}
	return ok;
}

// The bound for the cases that wait on a lock of the code under test: longer than the WAIT_LIMIT waits inside them,
// which end first with their own message.
constexpr unsigned LOCK_CASE_BOUND_SECONDS = 30;

// Runs `body` in a forked child under alarm(`seconds`), so a check that blocks (a FIFO opened, a lock never
// released) or never ends (a link followed forever) fails its case instead of stopping the suite. True when the
// child's body returned true; otherwise the child's exit status or signal (SIGALRM: it timed out) is reported.
// The body reports with Expect, not Catch2's assertions, which the child would record only in its own copy.
inline bool InBoundedChild(const std::function<bool()> &body, unsigned seconds = 10) {
	std::fflush(nullptr);
	const pid_t child = ::fork();
	REQUIRE(child >= 0);
	if (child == 0) {
		::signal(SIGALRM, SIG_DFL); // an ignored SIGALRM would survive into the child
		::alarm(seconds);
		try {
			const bool ok = body();
			std::fflush(nullptr);
			::_exit(ok ? 0 : 1);
		} catch (const std::exception &error) {
			std::fprintf(stderr, "threw: %s\n", error.what());
			std::fflush(nullptr);
			::_exit(2); // never back into the parent's Catch2 run, or its TempDir's destructor
		} catch (...) {
			std::fflush(nullptr); // a failed REQUIRE's own report
			::_exit(2);
		}
	}
	int status = 0;
	REQUIRE(::waitpid(child, &status, 0) == child);
	if (WIFSIGNALED(status)) {
		UNSCOPED_INFO("the bounded child was killed by signal " << WTERMSIG(status)
		                                                        << (WTERMSIG(status) == SIGALRM ? " (timed out)" : ""));
	} else if (WIFEXITED(status) && WEXITSTATUS(status) != 0) {
		UNSCOPED_INFO("the bounded child exited with " << WEXITSTATUS(status) << " (1: a wrong answer, 2: it threw)");
	}
	return WIFEXITED(status) && WEXITSTATUS(status) == 0;
}

inline duckdb::redshift_driver::detail::BaseInputs Inputs(const std::string &extension_dir,
                                                          const std::string &per_user_base) {
	duckdb::redshift_driver::detail::BaseInputs inputs;
	inputs.extension_dir = extension_dir;
	inputs.per_user_base = per_user_base;
	inputs.is_noexec = NeverNoExec;
	return inputs;
}

} // namespace redshift_driver_test
