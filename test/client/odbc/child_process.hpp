//===----------------------------------------------------------------------===//
//                         DuckDB - Redshift
//
// test/client/odbc/child_process.hpp
//
// A case whose environment must be set before anything in the process starts, as a variable the
// driver reads at load must be, runs one of this suite's hidden cases in a new process. The child
// can have the kernel kill it at its first socket or new process, which shows that nothing before
// made one, and can tell whether a file under a directory is mapped into it; a watch tells whether
// any process opened a file. Linux only: /proc, seccomp and inotify.
//===----------------------------------------------------------------------===//

#pragma once

#include "catch.hpp"
#include "duckdb/common/numeric_utils.hpp"

#include <cerrno>
#include <chrono>
#include <csignal>
#include <cstddef>
#include <cstdlib>
#include <dirent.h>
#include <fcntl.h>
#include <fstream>
#include <istream>
#include <iterator>
#include <linux/audit.h>
#include <linux/filter.h>
#include <linux/sched.h>
#include <linux/seccomp.h>
#include <string>
#include <sys/inotify.h>
#include <sys/prctl.h>
#include <sys/resource.h>
#include <sys/syscall.h>
#include <sys/wait.h>
#include <thread>
#include <unistd.h>
#include <vector>

// glibc before 2.34 names no clone3, which both architectures number 435.
#ifndef SYS_clone3
#define SYS_clone3 435
#endif

namespace redshift_test {

//! The variable that tells a child which of its hidden case's modes to run.
const char *const CHILD_MODE = "REDSHIFT_TEST_CHILD_MODE";

//! How a child ended, and what it wrote to its standard output and error.
struct ChildRun {
	//! Its exit code, or -1 when a signal ended it.
	int exit_code = -1;
	//! The signal that ended it, or 0.
	int signal = 0;
	//! Whether it was still running at its timeout, and was killed.
	bool timed_out = false;
	std::string output;

	//! Whether the kernel killed it at a system call a trap refuses.
	bool Trapped() const {
		return signal == SIGSYS;
	}
};

//! Starts this suite's hidden case tagged `tag` in a new process with only `environment` and the
//! suite's own TSAN_OPTIONS, in `directory`, its output in `output_path`, and without a core file,
//! which a trapped child would otherwise leave. Returns its process ID.
inline pid_t StartSuiteChild(const std::string &tag, std::vector<std::string> environment, const std::string &directory,
                             const std::string &output_path) {
	// A thread-sanitized suite's child reads the same suppressions as the suite.
	if (const char *sanitizer = std::getenv("TSAN_OPTIONS")) {
		environment.push_back(std::string("TSAN_OPTIONS=") + sanitizer);
	}
	// Built before the fork: the child makes only async-signal-safe calls, as the suite runs threads.
	std::vector<char *> arguments {const_cast<char *>("redshift_client_tests"), const_cast<char *>("-w"),
	                               const_cast<char *>("NoTests"), const_cast<char *>(tag.c_str()), nullptr};
	std::vector<char *> variables;
	for (auto &entry : environment) {
		variables.push_back(&entry[0]);
	}
	variables.push_back(nullptr);
	const rlimit no_core {0, 0};
	const pid_t child = fork();
	if (child == 0) {
		const int output = open(output_path.c_str(), O_WRONLY | O_CREAT | O_TRUNC, 0600);
		const int input = open("/dev/null", O_RDONLY);
		if (output < 0 || input < 0 || chdir(directory.c_str()) != 0 || dup2(input, STDIN_FILENO) < 0 ||
		    dup2(output, STDOUT_FILENO) < 0 || dup2(output, STDERR_FILENO) < 0 ||
		    setrlimit(RLIMIT_CORE, &no_core) != 0) {
			_exit(126);
		}
		execve("/proc/self/exe", arguments.data(), variables.data());
		_exit(127);
	}
	REQUIRE(child > 0);
	return child;
}

//! Runs StartSuiteChild's child in `directory`, its output there in child.out. One still running
//! after `timeout` (steady_clock: no clock step moves it) is killed, so a hung driver cannot hang the suite.
inline ChildRun RunSuiteChild(const std::string &tag, const std::vector<std::string> &environment,
                              const std::string &directory, std::chrono::seconds timeout) {
	const auto output_path = directory + "/child.out";
	const pid_t child = StartSuiteChild(tag, environment, directory, output_path);
	int status = 0;
	const auto deadline = std::chrono::steady_clock::now() + timeout;
	pid_t ended = 0;
	while ((ended = waitpid(child, &status, WNOHANG)) == 0 && std::chrono::steady_clock::now() < deadline) {
		std::this_thread::sleep_for(std::chrono::milliseconds(10));
	}
	ChildRun run;
	if (ended == 0) {
		kill(child, SIGKILL);
		ended = waitpid(child, &status, 0);
		run.timed_out = true;
	}
	REQUIRE(ended == child);
	std::ifstream in(output_path.c_str(), std::ios::binary);
	run.output = std::string(std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>());
	run.exit_code = WIFEXITED(status) ? WEXITSTATUS(status) : -1;
	run.signal = WIFSIGNALED(status) ? WTERMSIG(status) : 0;
	return run;
}

#if defined(__x86_64__)
constexpr unsigned TRAP_ARCHITECTURE = AUDIT_ARCH_X86_64;
constexpr unsigned TRAP_X32_CALL = 0x40000000;
#elif defined(__aarch64__)
constexpr unsigned TRAP_ARCHITECTURE = AUDIT_ARCH_AARCH64;
constexpr unsigned TRAP_X32_CALL = 0xffffffff;
#else
#error "child_process.hpp knows the system call numbers of x86_64 and aarch64 only"
#endif

//! The start of every trap: a call of another architecture than the build's, or an x32 call, is
//! killed rather than let through unchecked, and the call's number is loaded for what follows.
inline std::vector<sock_filter> TrapPrologue() {
	return {
	    BPF_STMT(BPF_LD | BPF_W | BPF_ABS, offsetof(seccomp_data, arch)),
	    BPF_JUMP(BPF_JMP | BPF_JEQ | BPF_K, TRAP_ARCHITECTURE, 1, 0),
	    BPF_STMT(BPF_RET | BPF_K, SECCOMP_RET_KILL_PROCESS),
	    BPF_STMT(BPF_LD | BPF_W | BPF_ABS, offsetof(seccomp_data, nr)),
	    BPF_JUMP(BPF_JMP | BPF_JGE | BPF_K, TRAP_X32_CALL, 0, 1),
	    BPF_STMT(BPF_RET | BPF_K, SECCOMP_RET_KILL_PROCESS),
	};
}

//! Installs `filter`, irreversibly: for a child alone.
inline void InstallTrap(std::vector<sock_filter> filter) {
	sock_fprog program {duckdb::NumericCast<unsigned short>(filter.size()), filter.data()};
	// Without CAP_SYS_ADMIN the kernel takes a filter only from a process that can gain no privileges.
	REQUIRE(prctl(PR_SET_NO_NEW_PRIVS, 1, 0, 0, 0) == 0);
	REQUIRE(prctl(PR_SET_SECCOMP, SECCOMP_MODE_FILTER, &program) == 0);
}

//! Has the kernel kill this process at its first socket(2) or socketpair(2), before the call runs, so
//! a child that exits normally opened none.
inline void TrapSockets() {
	auto filter = TrapPrologue();
	filter.insert(filter.end(), {
	                                BPF_JUMP(BPF_JMP | BPF_JEQ | BPF_K, SYS_socket, 1, 0),
	                                BPF_JUMP(BPF_JMP | BPF_JEQ | BPF_K, SYS_socketpair, 0, 1),
	                                BPF_STMT(BPF_RET | BPF_K, SECCOMP_RET_KILL_PROCESS),
	                                BPF_STMT(BPF_RET | BPF_K, SECCOMP_RET_ALLOW),
	                            });
	InstallTrap(std::move(filter));
}

//! Whether a file under `directory`, an absolute path with no link in it, is mapped into this
//! process, as /proc/self/maps names each mapped file by its path.
inline bool MapsFileUnder(const std::string &directory) {
	std::ifstream maps("/proc/self/maps");
	REQUIRE(maps.is_open());
	const auto under = " " + directory + "/";
	for (std::string line; std::getline(maps, line);) {
		if (line.find(under) != std::string::npos) {
			return true;
		}
	}
	return false;
}

//! The names in `directory`, other than `.` and `..`, in no order.
inline std::vector<std::string> NamesIn(const std::string &directory) {
	DIR *listing = opendir(directory.c_str());
	REQUIRE(listing != nullptr);
	std::vector<std::string> names;
	for (dirent *entry = readdir(listing); entry != nullptr; entry = readdir(listing)) {
		const std::string name = entry->d_name;
		if (name != "." && name != "..") {
			names.push_back(name);
		}
	}
	closedir(listing);
	return names;
}

//! The seccomp filter count a /proc/<pid>/status text gives, or -1 when it has no Seccomp_filters line,
//! as a kernel older than Linux 5.9 writes none.
inline int SeccompFiltersIn(std::istream &status) {
	for (std::string line; std::getline(status, line);) {
		if (line.rfind("Seccomp_filters:", 0) == 0) {
			return std::atoi(line.c_str() + std::string("Seccomp_filters:").size());
		}
	}
	return -1;
}

//! How many seccomp filters this process runs under, or -1 on a kernel that does not count them.
inline int SeccompFilters() {
	std::ifstream status("/proc/self/status");
	REQUIRE(status.is_open());
	return SeccompFiltersIn(status);
}

//! Has the kernel kill this process when it starts another, before it runs: at a fork, a vfork, or a
//! clone that is no new thread, as posix_spawn makes. Threads still start: clone3, whose flags a filter
//! cannot read, answers ENOSYS, and the C library then makes the thread with clone.
inline void TrapNewProcesses() {
	auto filter = TrapPrologue();
	filter.push_back(BPF_JUMP(BPF_JMP | BPF_JEQ | BPF_K, SYS_clone3, 0, 1));
	filter.push_back(BPF_STMT(BPF_RET | BPF_K, SECCOMP_RET_ERRNO | ENOSYS));
#if defined(SYS_fork)
	filter.push_back(BPF_JUMP(BPF_JMP | BPF_JEQ | BPF_K, SYS_fork, 4, 0));
	filter.push_back(BPF_JUMP(BPF_JMP | BPF_JEQ | BPF_K, SYS_vfork, 3, 0));
#endif
	filter.push_back(BPF_JUMP(BPF_JMP | BPF_JEQ | BPF_K, SYS_clone, 0, 3));
	// The flags are the first argument; on both architectures its low word.
	filter.push_back(BPF_STMT(BPF_LD | BPF_W | BPF_ABS, offsetof(seccomp_data, args[0])));
	filter.push_back(BPF_JUMP(BPF_JMP | BPF_JSET | BPF_K, CLONE_THREAD, 1, 0));
	filter.push_back(BPF_STMT(BPF_RET | BPF_K, SECCOMP_RET_KILL_PROCESS));
	filter.push_back(BPF_STMT(BPF_RET | BPF_K, SECCOMP_RET_ALLOW));
	InstallTrap(std::move(filter));
}

//! Waits for the host to finish with files just made: a host scanner may open a new file within a
//! second of its creation, which a FileOpenWatch begun then would report.
inline void LetNewFilesSettle() {
	std::this_thread::sleep_for(std::chrono::seconds(2));
}

//! Whether any process opens the file at `path` while this lives, from the kernel's open events. Begin
//! it after LetNewFilesSettle.
class FileOpenWatch {
public:
	explicit FileOpenWatch(const std::string &path) : watch {inotify_init1(IN_NONBLOCK | IN_CLOEXEC)} {
		REQUIRE(watch.fd >= 0);
		REQUIRE(inotify_add_watch(watch.fd, path.c_str(), IN_OPEN) >= 0);
	}
	FileOpenWatch(const FileOpenWatch &) = delete;
	FileOpenWatch &operator=(const FileOpenWatch &) = delete;

	//! Whether an open event has arrived since the watch began.
	bool Opened() {
		alignas(inotify_event) char events[4096];
		return read(watch.fd, events, sizeof(events)) > 0;
	}

private:
	//! The inotify descriptor. It closes itself: a REQUIRE that throws in the constructor skips
	//! ~FileOpenWatch, but not a member's destructor.
	struct Descriptor {
		const int fd;
		~Descriptor() {
			if (fd >= 0) {
				close(fd);
			}
		}
	};
	const Descriptor watch;
};

} // namespace redshift_test
