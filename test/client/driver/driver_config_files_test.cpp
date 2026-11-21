// The configuration files the driver reads: the closed-port smoke, with the load before
// it, runs in a child traced as `strace -f` would trace open, openat, stat, access and their variants, and
// every path the driver opens or probes must be a row of policy/config-files.txt or one of the kinds of
// file below that configure nothing. Where the trace cannot be taken the case warns and stops, or fails
// with REDSHIFT_TEST_REQUIRE_HOST_LIBRARIES=1, which hidden children that refuse ptrace by a seccomp filter
// show.
#include "driver_info.h"
#include "embedded_test_helpers.hpp"
#include "loader_payload.hpp"
#include "temp_dir.hpp"

#include "catch.hpp"

#include <algorithm>
#include <cerrno>
#include <climits>
#include <cstddef>
#include <cstdio>
#include <csignal>
#include <cstdlib>
#include <fnmatch.h>
#include <fstream>
#include <map>
#include <set>
#include <sstream>
#include <string>
#include <string_view>
#include <thread>
#include <vector>

#include <dlfcn.h>
#include <elf.h>
#include <fcntl.h>
#include <sys/stat.h>
#include <sys/uio.h>
#include <sys/user.h>
#include <linux/filter.h>
#include <linux/seccomp.h>
#include <sys/prctl.h>
#include <sys/ptrace.h>
#include <sys/syscall.h>
#include <sys/wait.h>
#include <unistd.h>

using namespace duckdb::redshift_driver;
using redshift_driver_test::TempDir;

namespace {

// A system call as a syscall-stop shows it: its number, its arguments, and whether the stop is its entry.
struct SyscallInfo {
	bool entry;
	uint64_t nr;
	uint64_t args[6];
};

// The call thread `tid` is stopped in, from its registers (PTRACE_GETREGSET), which every supported kernel
// gives; false when they cannot be read. Not PTRACE_GET_SYSCALL_INFO, which needs Linux 5.3. The kernel says
// which stop is an entry: on x86_64 rax holds -ENOSYS there (a call that fails with ENOSYS reads as one again at
// its exit, with the same arguments, which adds nothing to a set of paths), and on arm64 x7 holds
// PTRACE_SYSCALL_ENTER.
bool ReadSyscall(pid_t tid, SyscallInfo &info) {
#if !defined(__x86_64__) && !defined(__aarch64__)
#error "the trace reads the registers of x86_64 and arm64 only"
#endif
	user_regs_struct regs {}; // glibc's <sys/user.h>, the kernel's NT_PRSTATUS layout on both
	iovec vector {&regs, sizeof(regs)};
	if (::ptrace(PTRACE_GETREGSET, tid, reinterpret_cast<void *>(NT_PRSTATUS), &vector) != 0) {
		return false;
	}
#if defined(__x86_64__)
	info.entry = static_cast<int64_t>(regs.rax) == -ENOSYS;
	info.nr = regs.orig_rax;
	const uint64_t args[6] = {regs.rdi, regs.rsi, regs.rdx, regs.r10, regs.r8, regs.r9};
#else
	info.entry = regs.regs[7] == 0;                // PTRACE_SYSCALL_ENTER
	info.nr = static_cast<uint32_t>(regs.regs[8]); // the kernel takes w8, the low half
	const uint64_t args[6] = {regs.regs[0], regs.regs[1], regs.regs[2], regs.regs[3], regs.regs[4], regs.regs[5]};
#endif
	std::copy(args, args + 6, info.args);
	return true;
}

// The argument that holds the path, for each traced system call; -1 for any other call.
int PathArgument(uint64_t nr) {
	switch (nr) {
#ifdef SYS_open
	case SYS_open:
	case SYS_stat:
	case SYS_lstat:
	case SYS_access:
		return 0;
#endif
	case SYS_openat:
	case SYS_newfstatat:
	case SYS_faccessat:
#ifdef SYS_statx
	case SYS_statx:
#endif
#ifdef SYS_openat2
	case SYS_openat2:
#endif
#ifdef SYS_faccessat2
	case SYS_faccessat2:
#endif
		return 1;
	default:
		return -1;
	}
}

std::string ReadLink(const std::string &path) {
	char buffer[PATH_MAX];
	const auto length = ::readlink(path.c_str(), buffer, sizeof(buffer) - 1);
	return length > 0 ? std::string(buffer, static_cast<size_t>(length)) : std::string();
}

// The NUL-terminated string at `address` in thread `tid`, up to PATH_MAX bytes.
std::string PeekString(pid_t tid, uint64_t address) {
	std::string text;
	for (size_t offset = 0; offset < PATH_MAX; offset += sizeof(long)) {
		errno = 0;
		const long word = ::ptrace(PTRACE_PEEKDATA, tid, reinterpret_cast<void *>(address + offset), nullptr);
		if (errno != 0) {
			break;
		}
		const auto *bytes = reinterpret_cast<const char *>(&word);
		for (size_t i = 0; i < sizeof(long); i++) {
			if (bytes[i] == '\0') {
				return text;
			}
			text += bytes[i];
		}
	}
	return text;
}

// The absolute path a traced call names: a relative one is taken from the thread's current directory or
// from the directory its descriptor argument is open on. Empty for a call on a descriptor alone.
std::string AbsolutePath(pid_t tid, const SyscallInfo &info, int argument) {
	const auto path = PeekString(tid, info.args[argument]);
	if (path.empty() || path[0] == '/') {
		return path;
	}
	const auto dirfd = static_cast<int>(info.args[0]);
	const std::string base =
	    ReadLink("/proc/" + std::to_string(tid) +
	             (argument == 0 || dirfd == AT_FDCWD ? std::string("/cwd") : "/fd/" + std::to_string(dirfd)));
	return base + "/" + path;
}

// What Traced returns when this host cannot trace: ptrace is refused (seccomp, Yama, or a tracer
// already attached, as under strace -f), or the tracee's registers cannot be read.
constexpr int CANNOT_TRACE = -1;
constexpr int NO_REGISTERS = -2;

// Runs `body` in a child traced from its first instruction to its exit, every thread and child
// process included (strace -f). `body` may write to the descriptor it is given, which is not traced,
// and its text is returned in `output`. Returns the child's exit status and, in `paths`, every path a
// traced call named.
template <class BODY>
int Traced(BODY body, std::set<std::string> &paths, std::string &output) {
	int pipe_fds[2];
	REQUIRE(::pipe(pipe_fds) == 0);
	std::fflush(nullptr);
	const pid_t child = ::fork();
	REQUIRE(child >= 0);
	if (child == 0) {
		::close(pipe_fds[0]);
		if (::ptrace(PTRACE_TRACEME, 0, nullptr, nullptr) != 0) {
			::_exit(75);
		}
		::raise(SIGSTOP);
		::_exit(body(pipe_fds[1]));
	}
	::close(pipe_fds[1]);
	int status = 0;
	REQUIRE(::waitpid(child, &status, 0) == child);
	if (WIFEXITED(status) && WEXITSTATUS(status) == 75) {
		::close(pipe_fds[0]);
		return CANNOT_TRACE;
	}
	REQUIRE(WIFSTOPPED(status));
	REQUIRE(::ptrace(PTRACE_SETOPTIONS, child, nullptr,
	                 PTRACE_O_TRACESYSGOOD | PTRACE_O_TRACECLONE | PTRACE_O_TRACEFORK | PTRACE_O_TRACEVFORK |
	                     PTRACE_O_EXITKILL) == 0);
	REQUIRE(::ptrace(PTRACE_SYSCALL, child, nullptr, nullptr) == 0);
	int exit_status = -1;
	bool registers_read = false;
	for (;;) {
		const pid_t tid = ::waitpid(-1, &status, __WALL);
		if (tid < 0) {
			REQUIRE(errno == ECHILD);
			break;
		}
		if (WIFEXITED(status) || WIFSIGNALED(status)) {
			if (tid == child) {
				exit_status = status;
			}
			continue;
		}
		int deliver = 0;
		const int signal = WSTOPSIG(status);
		if (signal == (SIGTRAP | 0x80)) {
			SyscallInfo info {};
			const bool got = ReadSyscall(tid, info);
			registers_read = registers_read || got;
			if (got && info.entry) {
				const int argument = PathArgument(info.nr);
				if (argument >= 0) {
					const auto path = AbsolutePath(tid, info, argument);
					if (!path.empty()) {
						paths.insert(path);
					}
				}
			}
		} else if (status >> 16 == 0 && signal != SIGSTOP) {
			// A signal for the tracee, passed on; a new thread's first stop and an event stop are not.
			deliver = signal;
		}
		::ptrace(PTRACE_SYSCALL, tid, nullptr, reinterpret_cast<void *>(static_cast<intptr_t>(deliver)));
	}
	char buffer[4096];
	for (ssize_t n; (n = ::read(pipe_fds[0], buffer, sizeof(buffer))) > 0;) {
		output.append(buffer, static_cast<size_t>(n));
	}
	::close(pipe_fds[0]);
	if (!registers_read) {
		return NO_REGISTERS;
	}
	REQUIRE(WIFEXITED(exit_status));
	return WEXITSTATUS(exit_status);
}

// The driver's bytes, as embedded_driver_elf.S embeds them.
extern "C" const unsigned char redshift_driver_driver_begin[];
extern "C" const unsigned char redshift_driver_driver_end[];

std::string_view DriverBytes() {
	return std::string_view(reinterpret_cast<const char *>(redshift_driver_driver_begin),
	                        static_cast<std::size_t>(redshift_driver_driver_end - redshift_driver_driver_begin));
}

// The directory the bundled Kerberos libraries were built for, which config-files.txt writes as
// <kerberos-prefix>/ so that the shipped file does not name it: the text before /lib/krb5/plugins (Kerberos's
// default plugin directory) in the one string of the driver that ends there.
std::string KerberosPrefix() {
	const auto bytes = DriverBytes();
	const std::string_view tail("/lib/krb5/plugins", sizeof("/lib/krb5/plugins")); // with its NUL
	std::vector<std::string> found;
	for (auto at = bytes.find(tail); at != std::string_view::npos; at = bytes.find(tail, at + 1)) {
		const auto nul = bytes.rfind('\0', at);
		const auto start = nul == std::string_view::npos ? 0 : nul + 1;
		found.emplace_back(bytes.substr(start, at - start));
	}
	INFO("strings of the driver that end in /lib/krb5/plugins: " << found.size());
	REQUIRE(found.size() == 1);
	REQUIRE(found[0].size() > 1);
	REQUIRE(found[0][0] == '/');
	return found[0];
}

// A row of policy/config-files.txt: its location as written and its kind (`ini` or `other`).
struct Row {
	std::string location;
	std::string kind;
};

// The rows of policy/config-files.txt as fnmatch patterns for this run: ~/ under `home`, ./ under
// `cwd`, <driver-dir>/ under `driver_dir`, <kerberos-prefix>/ under the driver's Kerberos prefix, <$NAME>
// no path (the smoke takes no path from a variable), and `*`, a %{...} conversion and %lu any text. Each
// pattern maps to its row.
std::map<std::string, Row> RowPatterns(const std::string &home, const std::string &cwd, const std::string &driver_dir) {
	std::ifstream in(std::string(REDSHIFT_TEST_POLICY_DIR) + "/config-files.txt");
	REQUIRE(in.good());
	const auto kerberos = KerberosPrefix();
	std::map<std::string, Row> patterns;
	std::string line;
	while (std::getline(in, line)) {
		if (line.empty() || line[0] == '#') {
			continue;
		}
		std::istringstream fields(line);
		std::string location, kind, variables, configures;
		fields >> location >> kind >> variables >> configures;
		if (location.find('\\') != std::string::npos) {
			continue; // a Windows row: its form is checked below, and a Windows trace is to judge it
		}
		INFO("config-files.txt: " << line);
		REQUIRE_FALSE(configures.empty()); // location, kind, variables (or -), what it configures
		REQUIRE((kind == "ini" || kind == "other"));
		std::string pattern = location;
		const auto replace_start = [&](const std::string &prefix, const std::string &with) {
			if (pattern.compare(0, prefix.size(), prefix) == 0) {
				pattern = with + pattern.substr(prefix.size());
			}
		};
		replace_start("~/", home + "/");
		replace_start("./", cwd + "/");
		replace_start("<driver-dir>/", driver_dir + "/");
		replace_start("<kerberos-prefix>/", kerberos + "/");
		if (pattern.compare(0, 2, "<$") == 0) {
			pattern = "<variable>" + pattern; // never a path
		}
		for (auto at = pattern.find('%'); at != std::string::npos; at = pattern.find('%')) {
			// %{name}, or % and letters (%lu)
			auto end = pattern.compare(at, 2, "%{") == 0
			               ? pattern.find('}', at) + 1
			               : pattern.find_first_not_of("abcdefghijklmnopqrstuvwxyz", at + 1);
			pattern.replace(at, (end == std::string::npos || end == 0 ? pattern.size() : end) - at, "*");
		}
		REQUIRE((pattern[0] == '/' || pattern[0] == '<'));
		patterns[pattern] = Row {location, kind};
	}
	REQUIRE_FALSE(patterns.empty());
	return patterns;
}

bool Under(const std::string &path, const std::string &dir) {
	return path == dir || path.compare(0, dir.size() + 1, dir + "/") == 0;
}

// Why `path` configures nothing, or empty: the loader's own files (the base and the set in it, the
// directories above the base, which the ancestry walk checks, and the libraries it loads first, from
// the library cache or a system directory), the empty file ODBCINI names during the environment
// handle's allocation, and the process's own state in /proc.
std::string NotConfiguration(const std::string &path, const std::string &base) {
	if (Under(path, base)) {
		return "the base and the set in it";
	}
	if (path == "/" || Under(base, path)) {
		return "a directory above the base";
	}
	if (path == "/dev/null") {
		return "ODBCINI during the environment handle's allocation";
	}
	if (path == "/etc/ld.so.cache") {
		return "the library cache";
	}
	const auto slash = path.rfind('/');
	const auto dir = path.substr(0, slash);
	const auto name = path.substr(slash + 1);
	for (const auto &system : duckdb::redshift_driver::detail::SystemLibraryDirectories()) {
		if (Under(system, path)) {
			return "a system library directory, or one above it";
		}
		if (dir == system && (::fnmatch("*.so", name.c_str(), 0) == 0 || ::fnmatch("*.so.*", name.c_str(), 0) == 0)) {
			return "a library the driver needs";
		}
	}
	if (Under(path, "/proc/self") || Under(path, "/proc/sys/kernel") ||
	    (dir == "/proc/thread-self/fd" && !name.empty() && name.find_first_not_of("0123456789") == std::string::npos)) {
		return "the process's own state";
	}
	return std::string();
}

// Whether Traced could not trace here (CANNOT_TRACE or NO_REGISTERS): the case warns and stops, or, where
// the run must take every row (REDSHIFT_TEST_REQUIRE_HOST_LIBRARIES=1), fails.
bool TraceUnavailable(int status) {
	const char *why;
	if (status == CANNOT_TRACE) {
		why = "ptrace is refused here (seccomp, Yama, or a tracer already attached)";
	} else if (status == NO_REGISTERS) {
		why = "ptrace cannot read the traced threads' registers here (PTRACE_GETREGSET)";
	} else {
		return false;
	}
	redshift_driver_test::HostCannotRun(why);
	return true;
}

// Makes ptrace with `request` fail with `error` in this process and the children it forks from now on, by a
// seccomp filter, so a case can meet a host that cannot trace. True when the filter is in place.
bool RefusePtrace(unsigned request, int error) {
	sock_filter filter[] = {
	    BPF_STMT(BPF_LD | BPF_W | BPF_ABS, offsetof(seccomp_data, nr)),
	    BPF_JUMP(BPF_JMP | BPF_JEQ | BPF_K, SYS_ptrace, 0, 3),
	    BPF_STMT(BPF_LD | BPF_W | BPF_ABS, offsetof(seccomp_data, args)), // args[0]'s low half (little-endian)
	    BPF_JUMP(BPF_JMP | BPF_JEQ | BPF_K, request, 0, 1),
	    BPF_STMT(BPF_RET | BPF_K, SECCOMP_RET_ERRNO | (static_cast<unsigned>(error) & SECCOMP_RET_DATA)),
	    BPF_STMT(BPF_RET | BPF_K, SECCOMP_RET_ALLOW),
	};
	sock_fprog program {static_cast<unsigned short>(sizeof(filter) / sizeof(filter[0])), filter};
	return ::prctl(PR_SET_NO_NEW_PRIVS, 1, 0, 0, 0) == 0 && ::prctl(PR_SET_SECCOMP, SECCOMP_MODE_FILTER, &program) == 0;
}

// For the hidden cases below: a trace this process cannot take, `request` refused, so Traced returns `want`.
void TraceRefused(unsigned request, int want) {
	if (!RefusePtrace(request, request == PTRACE_TRACEME ? EPERM : EIO)) {
		std::printf("no seccomp filter here\n");
		return;
	}
	std::set<std::string> paths;
	std::string output;
	const int status = Traced([](int) { return 0; }, paths, output);
	REQUIRE(status == want);
	if (TraceUnavailable(status)) {
		std::printf("the trace case stopped\n");
	}
}

} // namespace

TEST_CASE("Each Windows row of config-files.txt has the documented form", "[driver][config-files]") {
	// No Windows trace reads these rows, so their form is checked here: a location, a kind, the
	// variables (or -) and what it configures; a key row exactly for a registry location, which
	// ends in a backslash; and a location the header allows.
	std::ifstream in(std::string(REDSHIFT_TEST_POLICY_DIR) + "/config-files.txt");
	REQUIRE(in.good());
	size_t rows = 0;
	std::string line;
	while (std::getline(in, line)) {
		if (line.empty() || line[0] == '#') {
			continue;
		}
		std::istringstream fields(line);
		std::string location, kind, variables, configures;
		fields >> location >> kind >> variables >> configures;
		if (location.find('\\') == std::string::npos) {
			continue;
		}
		INFO("config-files.txt: " << line);
		rows++;
		CHECK_FALSE(configures.empty());
		CHECK((kind == "ini" || kind == "other" || kind == "key"));
		const bool registry = location.rfind("HKLM\\", 0) == 0 || location.rfind("HKCU\\", 0) == 0;
		CHECK((kind == "key") == registry);
		if (registry) {
			CHECK(location.back() == '\\');
			continue;
		}
		const auto closing = location[0] == '%' ? location.find('%', 1) : std::string::npos;
		const bool variable =
		    closing != std::string::npos && closing > 1 && location.compare(closing, 2, "%\\") == 0 &&
		    location.find_first_not_of("ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789_", 1) == closing;
		const bool drive =
		    location.size() > 3 && location[0] >= 'A' && location[0] <= 'Z' && location.compare(1, 2, ":\\") == 0;
		CHECK((variable || drive || location.rfind("<driver-dir>\\", 0) == 0 || location[0] == '\\'));
	}
	CHECK(rows >= 26);
}

// The trace meets no <kerberos-prefix> row before a connection, so this is what holds those rows to the driver:
// each, its token resolved, is a path the driver holds whole (a string that ends there, or a directory whose
// path continues with a slash), as when the rows were taken from its strings.
TEST_CASE("Each <kerberos-prefix> row of config-files.txt is a path the driver holds", "[driver][config-files]") {
	const auto bytes = DriverBytes();
	const auto kerberos = KerberosPrefix();
	int rows = 0;
	for (const auto &pattern : RowPatterns("/home", "/cwd", "/driver")) {
		const auto &location = pattern.second.location;
		const std::string token = "<kerberos-prefix>/";
		if (location.compare(0, token.size(), token) != 0) {
			continue;
		}
		rows++;
		INFO("config-files.txt: " << location);
		CHECK(pattern.first.compare(0, kerberos.size() + 1, kerberos + "/") == 0);
		auto path = kerberos + "/" + location.substr(token.size());
		if (path.back() == '/') {
			path.pop_back();
		}
		CHECK((bytes.find(path + '\0') != std::string_view::npos || bytes.find(path + '/') != std::string_view::npos));
	}
	CHECK(rows > 0);
}

namespace {

// Calls the tracer must see, each on its own path in the base `dir`, made before the load: an open, a stat and an
// access; one from another thread and one from a child process; two by relative paths, from the current
// directory and from a directory descriptor; and each traced system call made directly, where the platform has
// it, with its path where the kernel takes it (0 for the old calls, 1 after a directory descriptor for the *at
// ones).
struct Sentinels {
	struct RawCall {
		long nr;
		const char *name;
		int path_argument;
	};
	std::vector<RawCall> raw = {
	    {SYS_openat, "openat", 1}, {SYS_newfstatat, "newfstatat", 1}, {SYS_faccessat, "faccessat", 1}};
	std::string dir;
	std::vector<std::string> paths;
	size_t first_raw = 0; // where the raw calls' paths start in `paths`

	explicit Sentinels(const std::string &base) : dir(base) {
#ifdef SYS_open
		raw.insert(raw.end(),
		           {{SYS_open, "open", 0}, {SYS_stat, "stat", 0}, {SYS_lstat, "lstat", 0}, {SYS_access, "access", 0}});
#endif
#ifdef SYS_statx
		raw.push_back({SYS_statx, "statx", 1});
#endif
#ifdef SYS_openat2
		raw.push_back({SYS_openat2, "openat2", 1});
#endif
#ifdef SYS_faccessat2
		raw.push_back({SYS_faccessat2, "faccessat2", 1});
#endif
		for (const char *name : {"open", "stat", "access", "thread", "fork", "relative-cwd", "relative-at"}) {
			paths.push_back(dir + "/" + name);
		}
		first_raw = paths.size();
		for (const auto &call : raw) {
			paths.push_back(dir + "/raw-" + call.name);
		}
	}

	// Makes every call, in the traced child, and returns to `cwd`; false when a step that is not traced failed.
	bool Touch(const char *cwd) const {
		struct stat st;
		::close(::open(paths[0].c_str(), O_RDONLY));
		(void)::stat(paths[1].c_str(), &st);
		(void)::access(paths[2].c_str(), F_OK);
		std::thread([&] { (void)::access(paths[3].c_str(), F_OK); }).join();
		const pid_t child = ::fork();
		if (child == 0) {
			(void)::access(paths[4].c_str(), F_OK);
			::_exit(0);
		}
		int status = 0;
		if (child < 0 || ::waitpid(child, &status, 0) != child || ::chdir(dir.c_str()) != 0) {
			return false;
		}
		(void)::access("relative-cwd", F_OK);
		const int at = ::open(dir.c_str(), O_RDONLY | O_DIRECTORY | O_CLOEXEC);
		if (at < 0 || ::chdir(cwd) != 0) {
			return false;
		}
		(void)::faccessat(at, "relative-at", F_OK, 0);
		::close(at);
		for (size_t i = 0; i < raw.size(); i++) {
			// The calls fail; only their entry is traced.
			const char *path = paths[first_raw + i].c_str();
			if (raw[i].path_argument == 1) {
				(void)::syscall(raw[i].nr, AT_FDCWD, path, 0, 0, 0);
			} else {
				(void)::syscall(raw[i].nr, path, 0, 0);
			}
		}
		return true;
	}
};

} // namespace

TEST_CASE("The closed-port smoke opens no configuration file that config-files.txt does not list",
          "[driver][config-files][real]") {
	if (!redshift_driver_test::HostHasDriverPrerequisites()) {
		return;
	}
	TempDir tmp;
	for (const char *dir : {"driver", "home", "cwd"}) {
		REQUIRE(::mkdir((tmp / dir).c_str(), 0700) == 0);
	}
	const int port = redshift_driver_test::ClosedPort();
	const Sentinels sentinels(tmp / "driver");
	// A path under HOME that no row lists, probed before the load: the check below must flag it, and it alone.
	const std::string unlisted_control = tmp / "home/not-a-configuration-file";
	// The smoke names a CA file, as the ODBC client's connection string always does (CaFile beside
	// TrustStore), so the trace measures the driver as the extension uses it: with a CA file named,
	// driver 2.3.1.0 looks for no CA file of its own. A real certificate, as the driver loads it
	// before it connects; copied before the trace, so the copy is not the driver's.
	const std::string ca_file = tmp / "ca.pem";
	{
		std::ifstream in(std::string(REDSHIFT_TEST_POLICY_DIR) +
		                     "/../signing/linux/DigiCertTrustedG4CodeSigningRSA4096SHA3842021CA1.pem",
		                 std::ios::binary);
		REQUIRE(in.is_open());
		std::ofstream out(ca_file, std::ios::binary);
		out << in.rdbuf();
		REQUIRE(out.good());
	}
	std::set<std::string> paths;
	std::string driver_dir;
	const int status = Traced(
	    [&](int out) {
		    redshift_driver_test::ClearRefusedEnvironment();
		    ::unsetenv("LD_LIBRARY_PATH");
		    ::setenv("REDSHIFT_DRIVER_DIR", (tmp / "driver").c_str(), 1);
		    ::setenv("HOME", (tmp / "home").c_str(), 1);
		    if (!sentinels.Touch((tmp / "cwd").c_str())) {
			    return 10;
		    }
		    (void)::access(unlisted_control.c_str(), F_OK);
		    auto result = LoadEmbeddedDriver();
		    std::string answer;
		    if (!result.ok() ||
		        !redshift_driver_test::ClosedPortSmoke(*result.driver, port, answer, "CaFile=" + ca_file + ";")) {
			    return 11;
		    }
		    // The directory the driver finds itself in (dladdr), which <driver-dir> rows are under.
		    Dl_info info;
		    if (::dladdr(reinterpret_cast<void *>(result.driver->api.DriverConnect), &info) == 0) {
			    return 12;
		    }
		    const std::string self = info.dli_fname;
		    const std::string dir = self.substr(0, self.rfind('/'));
		    return ::write(out, dir.data(), dir.size()) == static_cast<ssize_t>(dir.size()) ? 0 : 13;
	    },
	    paths, driver_dir);
	if (TraceUnavailable(status)) {
		return;
	}
	REQUIRE(status == 0);
	REQUIRE_FALSE(driver_dir.empty());
	for (const auto &sentinel : sentinels.paths) {
		INFO("the tracer did not see " << sentinel);
		CHECK(paths.count(sentinel) == 1);
	}
	const auto patterns = RowPatterns(tmp / "home", tmp / "cwd", driver_dir);
	std::map<std::string, std::string> rows_seen; // location -> kind
	std::vector<std::string> unlisted;
	for (const auto &path : paths) {
		bool listed = false;
		for (const auto &pattern : patterns) {
			if (::fnmatch(pattern.first.c_str(), path.c_str(), 0) == 0 ||
			    (pattern.first.back() == '/' && Under(path, pattern.first.substr(0, pattern.first.size() - 1)))) {
				rows_seen[pattern.second.location] = pattern.second.kind;
				listed = true;
			}
		}
		// The CA file the connection names is the connection's, not configuration the driver finds.
		if (!listed && path != ca_file && NotConfiguration(path, tmp / "driver").empty()) {
			unlisted.push_back(path);
		}
	}
	std::string seen;
	for (const auto &row : rows_seen) {
		seen += " " + row.first + " (" + row.second + ")";
	}
	INFO("rows the trace met:" << seen << "; the driver directory: " << driver_dir);
	// Before a connection the driver opens or probes only its own settings file, beside itself, and the
	// /dev/null that ODBCINI names; the first shows the trace sees the driver's
	// calls and not only the loader's. With the CA file named, driver 2.3.1.0 also looks for its
	// built-in root.crt beside itself, to validate against both, as it does on the extension's
	// connections (rslibpq.c resolveSslConnectionOptions); the private directory never holds one.
	CHECK(rows_seen == std::map<std::string, std::string> {{"<driver-dir>/amazon.redshiftodbc.ini", "ini"},
	                                                       {"<driver-dir>/root.crt", "other"}});
	CHECK(paths.count("/dev/null") == 1);
	// The driver read the CA file the smoke names, so the smoke did reach the driver's CA check.
	CHECK(paths.count(ca_file) == 1);
	CHECK(std::count(unlisted.begin(), unlisted.end(), unlisted_control) == 1);
	for (const auto &path : unlisted) {
		if (path != unlisted_control) {
			FAIL_CHECK("the driver opened or probed " << path << ", which config-files.txt does not list");
		}
	}
}

TEST_CASE("trace refused child", "[.trace-refused-child]") {
	TraceRefused(PTRACE_TRACEME, CANNOT_TRACE);
}

TEST_CASE("registers unreadable child", "[.registers-unreadable-child]") {
	TraceRefused(PTRACE_GETREGSET, NO_REGISTERS);
}

// TraceUnavailable's rule, as the trace case above meets it on a host that cannot trace; that case's own call of
// it is not exercised here.
TEST_CASE("A trace this host cannot take warns and stops the case, or fails it when the run requires every row",
          "[driver][config-files]") {
	// Where ptrace is refused already (a tracer attached, Yama, a seccomp profile), PTRACE_TRACEME fails before
	// the register read could be refused, so only the first child can show its outcome.
	std::set<std::string> paths;
	std::string unused;
	const bool can_trace = Traced([](int) { return 0; }, paths, unused) != CANNOT_TRACE;
	if (!can_trace) {
		WARN("ptrace is refused here already; only the refused-ptrace child is run");
	}
	for (const char *child : {"[.trace-refused-child]", "[.registers-unreadable-child]"}) {
		if (!can_trace && std::string(child) == "[.registers-unreadable-child]") {
			continue;
		}
		for (const bool required : {false, true}) {
			TempDir tmp;
			const int status = redshift_driver_test::RerunHidden(
			    child, "", tmp.path(), {{"REDSHIFT_TEST_REQUIRE_HOST_LIBRARIES", required ? "1" : "0"}});
			const auto output = redshift_driver_test::ReadWholeFile(tmp / "child-output.txt");
			INFO(child << (required ? " with" : " without") << " REDSHIFT_TEST_REQUIRE_HOST_LIBRARIES=1: " << output);
			if (output.find("no seccomp filter here") != std::string::npos) {
				WARN("this host cannot install a seccomp filter, so a trace that cannot be taken is not shown");
				return;
			}
			REQUIRE(WIFEXITED(status));
			CHECK(WEXITSTATUS(status) == (required ? 1 : 0));
			CHECK((output.find("the trace case stopped") != std::string::npos) == !required);
			const std::string says =
			    required ? "REDSHIFT_TEST_REQUIRE_HOST_LIBRARIES=1 requires this row to run" : "this row was not run";
			CHECK(redshift_driver_test::Unwrapped(output).find(redshift_driver_test::Unwrapped(says)) !=
			      std::string::npos);
		}
	}
}
