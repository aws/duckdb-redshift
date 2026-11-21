// Base-directory selection, in temporary directories.
#include "driver_loader_core.hpp"
#include "temp_dir.hpp"

#include "catch.hpp"

#include <cerrno>
#include <climits>
#include <cstddef>
#include <condition_variable>
#include <cstdlib>
#include <fstream>
#include <functional>
#include <mutex>
#include <sstream>
#include <string>
#include <thread>
#include <utility>
#include <vector>

#include <dlfcn.h>
#include <linux/capability.h>
#include <linux/filter.h>
#include <linux/seccomp.h>
#include <fcntl.h>
#include <sched.h>
#include <sys/mman.h>
#include <sys/mount.h>
#include <sys/prctl.h>
#include <sys/syscall.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>

using duckdb::redshift_driver::DriverAttempt;
using namespace duckdb::redshift_driver::detail;
using redshift_driver_test::RunAsNamespaceRoot;
using redshift_driver_test::ScopedEnv;
using redshift_driver_test::TempDir;

namespace {

bool NeverNoExec(const std::string &) {
	return false;
}

std::string g_noexec_dir;
bool StubNoExec(const std::string &dir) {
	return dir == g_noexec_dir;
}

BaseInputs Inputs(const std::string &extension_dir, const std::string &per_user_base) {
	BaseInputs inputs;
	inputs.extension_dir = extension_dir;
	inputs.per_user_base = per_user_base;
	inputs.is_noexec = NeverNoExec;
	return inputs;
}

BaseInputs WithOverride(BaseInputs inputs, const std::string &value) {
	inputs.override_set = true;
	inputs.override_value = value;
	return inputs;
}

// The per-user cache is made only below directories no other user can change, which includes those
// above $TMPDIR (or /tmp); where they are not private, the cases that make a cache warn and stop.
bool PrivateTemp(const TempDir &tmp) {
	std::string where, reason;
	if (CheckAncestry(tmp.path(), AncestryEnd::PARENT, ::geteuid(), where, reason)) {
		return true;
	}
	WARN("the temporary directory is not private here (" + reason + "); set TMPDIR to a private directory; not tested");
	return false;
}

std::string Join(const std::vector<DriverAttempt> &attempts) {
	std::string text;
	for (const auto &attempt : attempts) {
		text += attempt.directory + ": " + attempt.reason + "\n";
	}
	return text;
}

mode_t Mode(const std::string &path) {
	struct stat st;
	REQUIRE(::stat(path.c_str(), &st) == 0);
	return st.st_mode & 07777;
}

std::string SelfPath() {
	char exe[PATH_MAX];
	REQUIRE(::realpath("/proc/self/exe", exe) != nullptr);
	return exe;
}

std::string ReadFile(const std::string &path) {
	std::ifstream in(path);
	std::stringstream text;
	text << in.rdbuf();
	return text.str();
}

// Runs this executable's hidden '[directory_child]' case, which writes LoadedFromDirectory(), with
// `argv0` as its argv[0] and `cwd` as its current directory. With `hide_proc` the child is in a
// user and mount namespace with a tmpfs over /proc, so MappedFilePath names nothing there. Returns
// the child's exit status (77: user namespaces unavailable) and sets `directory` to what it wrote.
int RunLoadedFromDirectory(const std::string &argv0, const std::string &cwd, bool hide_proc, std::string &directory) {
	TempDir out;
	ScopedEnv output("REDSHIFT_DRIVER_TEST_OUTPUT", (out / "directory").c_str());
	const auto exe = SelfPath();
	auto run = [&] {
		if ((hide_proc && ::mount("tmpfs", "/proc", "tmpfs", 0, nullptr) != 0) || ::chdir(cwd.c_str()) != 0) {
			return false;
		}
		const int quiet = ::open("/dev/null", O_WRONLY);
		if (quiet >= 0) {
			::dup2(quiet, STDOUT_FILENO);
		}
		char *argv[] = {const_cast<char *>(argv0.c_str()), const_cast<char *>("[directory_child]"), nullptr};
		::execv(exe.c_str(), argv);
		return false;
	};
	int status;
	if (hide_proc) {
		status = RunAsNamespaceRoot(run);
	} else {
		const pid_t child = ::fork();
		REQUIRE(child >= 0);
		if (child == 0) {
			run();
			::_exit(1);
		}
		int wait_status = 0;
		REQUIRE(::waitpid(child, &wait_status, 0) == child);
		REQUIRE(WIFEXITED(wait_status));
		status = WEXITSTATUS(wait_status);
	}
	directory = ReadFile(out / "directory");
	return status;
}

// Stands in for the C library's mkdir in this executable. Once a test sets g_after_mkdir, a
// successful mkdir of g_mkdir_path runs it, to change the new directory in the window
// MakePrivateDirectory must not trust; every call goes to the C library's mkdir first.
std::string g_mkdir_path;
std::function<void()> g_after_mkdir;
// Set, mkdir of g_mkdir_path fails with it without creating anything, as NFS can for a name that
// exists in a directory the user cannot write.
int g_mkdir_errno = 0;

// Stands in for the C library's chmod the same way: the first chmod of a path that starts with
// g_chmod_prefix runs g_before_chmod, once, before the C library's chmod.
std::string g_chmod_prefix;
std::function<void()> g_before_chmod;

} // namespace

extern "C" int mkdir(const char *path, mode_t mode) noexcept {
	using Mkdir = int (*)(const char *, mode_t);
	static const auto real = reinterpret_cast<Mkdir>(::dlsym(RTLD_NEXT, "mkdir"));
	if (g_mkdir_errno != 0 && g_mkdir_path == path) {
		errno = g_mkdir_errno;
		return -1;
	}
	const int result = real(path, mode);
	if (result == 0 && g_after_mkdir && g_mkdir_path == path) {
		g_after_mkdir();
	}
	return result;
}

extern "C" int chmod(const char *path, mode_t mode) noexcept {
	using Chmod = int (*)(const char *, mode_t);
	static const auto real = reinterpret_cast<Chmod>(::dlsym(RTLD_NEXT, "chmod"));
	if (g_before_chmod && !g_chmod_prefix.empty() &&
	    std::string(path).compare(0, g_chmod_prefix.size(), g_chmod_prefix) == 0) {
		const auto hook = std::move(g_before_chmod);
		g_before_chmod = nullptr;
		hook();
	}
	using redshift_driver_test::g_chmod_fail_errno;
	using redshift_driver_test::g_chmod_fail_prefix;
	if (g_chmod_fail_errno != 0 && !g_chmod_fail_prefix.empty() &&
	    std::string(path).compare(0, g_chmod_fail_prefix.size(), g_chmod_fail_prefix) == 0) {
		errno = g_chmod_fail_errno;
		return -1;
	}
	return real(path, mode);
}

TEST_CASE("Without an override, the extension's directory comes first, then the per-user cache",
          "[driver][directory]") {
	// A umask that leaves the owner without write, read or any access does not change the mode.
	for (const mode_t mask : {0277, 0477, 0777}) {
		INFO("umask " << std::oct << mask);
		TempDir tmp;
		if (!PrivateTemp(tmp)) {
			return;
		}
		REQUIRE(::mkdir((tmp / "ext").c_str(), 0755) == 0);
		const auto cache = tmp / "home/.cache/duckdb-redshift";
		std::vector<DriverAttempt> attempts;
		const mode_t old_umask = ::umask(mask);
		const auto bases = ChooseBaseDirectories(Inputs(tmp / "ext", cache), attempts);
		::umask(old_umask);
		CHECK(bases == std::vector<std::string> {tmp / "ext", cache});
		CHECK(Join(attempts).empty());
		// The cache and the parents it needed were created, owner-only.
		CHECK(Mode(cache) == 0700);
		CHECK(Mode(tmp / "home/.cache") == 0700);
		CHECK(Mode(tmp / "home") == 0700);
	}
}

TEST_CASE("The per-user cache is resolved, and tried once if the extension lives there", "[driver][directory]") {
	TempDir tmp;
	if (!PrivateTemp(tmp)) {
		return;
	}
	REQUIRE(::mkdir((tmp / "real").c_str(), 0700) == 0);
	REQUIRE(::symlink((tmp / "real").c_str(), (tmp / "home").c_str()) == 0);
	std::vector<DriverAttempt> attempts;
	auto bases = ChooseBaseDirectories(Inputs("", tmp / "home/duckdb-redshift"), attempts);
	CHECK(bases == std::vector<std::string> {tmp / "real/duckdb-redshift"});
	REQUIRE(attempts.size() == 1);
	CHECK(attempts[0].reason == "the extension's own directory could not be determined");

	attempts.clear();
	bases = ChooseBaseDirectories(Inputs(tmp / "real/duckdb-redshift", tmp / "home/duckdb-redshift"), attempts);
	CHECK(bases == std::vector<std::string> {tmp / "real/duckdb-redshift"});
	CHECK(attempts.empty());

	// Decided by the paths, not by what the caller's list already holds.
	attempts = {{tmp / "real/duckdb-redshift", "an attempt the caller recorded"}};
	REQUIRE(::mkdir((tmp / "ext").c_str(), 0700) == 0);
	bases = ChooseBaseDirectories(Inputs(tmp / "ext", tmp / "home/duckdb-redshift"), attempts);
	CHECK(bases == std::vector<std::string> {tmp / "ext", tmp / "real/duckdb-redshift"});
	CHECK(attempts.size() == 1);

	// Skipped once too, when that directory is on a no-exec mount.
	auto inputs = Inputs(tmp / "real/duckdb-redshift", tmp / "home/duckdb-redshift");
	inputs.is_noexec = StubNoExec;
	g_noexec_dir = tmp / "real/duckdb-redshift";
	attempts.clear();
	CHECK(ChooseBaseDirectories(inputs, attempts).empty());
	CHECK(Join(attempts) == tmp / "real/duckdb-redshift" + ": the extension's directory is mounted noexec\n");
	g_noexec_dir.clear();
}

TEST_CASE("REDSHIFT_DRIVER_DIR is the only candidate when it is set", "[driver][directory]") {
	TempDir tmp;
	REQUIRE(::mkdir((tmp / "admin").c_str(), 0755) == 0);
	REQUIRE(::mkdir((tmp / "ext").c_str(), 0755) == 0);
	std::vector<DriverAttempt> attempts;
	const auto bases = ChooseBaseDirectories(WithOverride(Inputs(tmp / "ext", tmp / "cache"), tmp / "admin"), attempts);
	CHECK(bases == std::vector<std::string> {tmp / "admin"});
	CHECK(attempts.empty());
	// The default per-user cache was not even created.
	CHECK(::access((tmp / "cache").c_str(), F_OK) != 0);
}

TEST_CASE("A refused REDSHIFT_DRIVER_DIR has no fallback", "[driver][directory]") {
	TempDir tmp;
	REQUIRE(::mkdir((tmp / "ext").c_str(), 0755) == 0);
	REQUIRE(::mkdir((tmp / "real").c_str(), 0755) == 0);
	REQUIRE(::symlink((tmp / "real").c_str(), (tmp / "link").c_str()) == 0);
	{ std::ofstream(tmp / "file") << "x"; }
	REQUIRE(::mkdir((tmp / "closed").c_str(), 0700) == 0);
	REQUIRE(::mkdir((tmp / "closed/admin").c_str(), 0700) == 0);
	REQUIRE(::chmod((tmp / "closed").c_str(), 0) == 0);
	// Each reason names the variable and no path; the attempt keeps the component it is about.
	struct Case {
		std::string value;
		std::string where;
		std::string reason;
	};
	const std::string link = "REDSHIFT_DRIVER_DIR contains a symbolic link; set it to the path the link resolves to";
	const std::vector<Case> cases {
	    {"relative/dir", "", "REDSHIFT_DRIVER_DIR is not an absolute path"},
	    {"", "", "REDSHIFT_DRIVER_DIR is not an absolute path"},
	    {tmp / "missing", tmp / "missing", "REDSHIFT_DRIVER_DIR does not exist"},
	    {tmp / "link", tmp / "link", link},
	    {tmp / "link/sub", tmp / "link", link},
	    {tmp / "file", tmp / "file", "REDSHIFT_DRIVER_DIR is not a directory"},
	    {tmp / "ext/../ext", tmp / "ext", "REDSHIFT_DRIVER_DIR contains '..'"},
	};
	for (const auto &c : cases) {
		INFO(c.value);
		std::vector<DriverAttempt> attempts;
		const auto bases = ChooseBaseDirectories(WithOverride(Inputs(tmp / "ext", tmp / "cache"), c.value), attempts);
		CHECK(bases.empty());
		REQUIRE(attempts.size() == 1);
		CHECK(attempts[0].directory == c.where);
		CHECK(attempts[0].reason == c.reason);
		CHECK(::access((tmp / "cache").c_str(), F_OK) != 0);
	}
	// A directory the user cannot search is reported as such, not as missing (root can search it).
	if (::geteuid() != 0) {
		std::vector<DriverAttempt> attempts;
		CHECK(ChooseBaseDirectories(WithOverride(Inputs("", ""), tmp / "closed/admin"), attempts).empty());
		REQUIRE(attempts.size() == 1);
		CHECK(attempts[0].directory == tmp / "closed/admin");
		CHECK(attempts[0].reason == "REDSHIFT_DRIVER_DIR cannot be examined: Permission denied");
		// Below it, the part that cannot be examined is a parent.
		attempts.clear();
		CHECK(ChooseBaseDirectories(WithOverride(Inputs("", ""), tmp / "closed/admin/sub"), attempts).empty());
		REQUIRE(attempts.size() == 1);
		CHECK(attempts[0].directory == tmp / "closed/admin");
		CHECK(attempts[0].reason == "REDSHIFT_DRIVER_DIR: a parent directory cannot be examined: Permission denied");
	} else {
		WARN("root can search any directory; the unsearchable override was not tested");
	}
	REQUIRE(::chmod((tmp / "closed").c_str(), 0700) == 0);
}

TEST_CASE("An ACL that cannot be read refuses; only a missing ACL, or none on the file system, passes",
          "[driver][directory]") {
	// macOS's acl_get_link_np and acl_get_fd_np give EACCES when an entry denies the user readsecurity. The entries
	// it hides may grant anyone anything, so the directory is refused, as one that cannot be examined is: the walk's
	// reason names it by role ("a parent directory has an access control list that cannot be read: Permission
	// denied"), and the error points to REDSHIFT_DRIVER_DIR. A set is left UNCHECKED by it, not INVALID.
	for (const int none : {ENOENT, ENOTSUP, EOPNOTSUPP}) {
		INFO(none);
		CHECK(AclReadFailure(none).finding == AclFinding::NONE);
		CHECK(AclReadFailure(none).rule.empty());
	}
	for (const auto &unreadable : std::vector<std::pair<int, std::string>> {{EACCES, "Permission denied"},
	                                                                        {EPERM, "Operation not permitted"},
	                                                                        {EIO, "Input/output error"},
	                                                                        {ENOMEM, "Cannot allocate memory"}}) {
		INFO(unreadable.second);
		CHECK(AclReadFailure(unreadable.first).finding == AclFinding::UNREADABLE);
		CHECK(AclReadFailure(unreadable.first).rule == std::string(ACL_UNREADABLE) + ": " + unreadable.second);
	}
}

TEST_CASE("An ACL entry that grants a counted right to a UUID that names no account, a group or another user "
          "breaks the rule; the user's own and root's do not",
          "[driver][directory]") {
	// macOS's AclRule reads the entry's qualifier with mbr_uuid_to_id and asks AclQualifierGrantsToAnother, which is
	// common code, so each case runs here too.
	const uid_t user = 501;
	CHECK(AclQualifierGrantsToAnother(AclQualifier::UNRESOLVED, 0, user));
	CHECK(AclQualifierGrantsToAnother(AclQualifier::UNRESOLVED, user, user));
	CHECK(AclQualifierGrantsToAnother(AclQualifier::GROUP, 0, user)); // wheel's ID is 0 too
	CHECK(AclQualifierGrantsToAnother(AclQualifier::GROUP, user, user));
	CHECK(AclQualifierGrantsToAnother(AclQualifier::USER, 502, user));
	CHECK_FALSE(AclQualifierGrantsToAnother(AclQualifier::USER, user, user));
	CHECK_FALSE(AclQualifierGrantsToAnother(AclQualifier::USER, 0, user));
}

TEST_CASE("A base on a no-exec mount is skipped with its reason", "[driver][directory]") {
	TempDir tmp;
	if (!PrivateTemp(tmp)) {
		return;
	}
	REQUIRE(::mkdir((tmp / "ext").c_str(), 0755) == 0);
	auto inputs = Inputs(tmp / "ext", tmp / "cache");
	inputs.is_noexec = StubNoExec;
	g_noexec_dir = tmp / "ext";
	std::vector<DriverAttempt> attempts;
	auto bases = ChooseBaseDirectories(inputs, attempts);
	CHECK(bases == std::vector<std::string> {tmp / "cache"});
	REQUIRE(attempts.size() == 1);
	CHECK(attempts[0].directory == tmp / "ext");
	CHECK(attempts[0].reason == "the extension's directory is mounted noexec");

	// The per-user cache too, which is common for a home directory mounted noexec.
	g_noexec_dir = tmp / "cache";
	attempts.clear();
	bases = ChooseBaseDirectories(inputs, attempts);
	CHECK(bases == std::vector<std::string> {tmp / "ext"});
	CHECK(Join(attempts) == tmp / "cache" + ": the per-user cache is mounted noexec\n");
	g_noexec_dir = tmp / "ext";

	// The override too, and still without fallback.
	attempts.clear();
	bases = ChooseBaseDirectories(WithOverride(inputs, tmp / "ext"), attempts);
	CHECK(bases.empty());
	CHECK(Join(attempts) == tmp / "ext" + ": REDSHIFT_DRIVER_DIR is mounted noexec\n");
	g_noexec_dir.clear();
}

TEST_CASE("IsNoExecMount reads the mount's flags", "[driver][directory]") {
	// systemd and container runtimes mount /proc noexec, as /proc/self/mounts shows, but a plain
	// mount of proc is not, so a host without it relies on the tmpfs case below. The directory this
	// test runs from allows execution.
	std::ifstream mounts("/proc/self/mounts");
	std::string device, point, type, options;
	bool proc_noexec = false;
	while (mounts >> device >> point >> type >> options) {
		mounts.ignore(1024, '\n');
		if (point == "/proc") {
			proc_noexec = ("," + options + ",").find(",noexec,") != std::string::npos;
		}
	}
	if (proc_noexec) {
		CHECK(IsNoExecMount("/proc"));
	} else {
		WARN("/proc is not mounted noexec here; IsNoExecMount was not tested on it");
	}
	CHECK_FALSE(IsNoExecMount(LoadedFromDirectory()));
	CHECK_FALSE(IsNoExecMount("/nonexistent/directory"));
	// The pre-load check counts a directory it cannot examine as a failure, not as "exec allowed".
	CHECK(NoExecState(LoadedFromDirectory()) == 0);
	CHECK(NoExecState("/nonexistent/directory") == -1);
}

TEST_CASE("IsNoExecMount sees a tmpfs mounted noexec", "[driver][directory]") {
	TempDir tmp;
	REQUIRE(::mkdir((tmp / "noexec").c_str(), 0700) == 0);
	REQUIRE(::mkdir((tmp / "exec").c_str(), 0700) == 0);
	const int result = RunAsNamespaceRoot([&] {
		// Once the namespace exists a failed mount is a failure, not an unavailable feature.
		if (::mount("tmpfs", (tmp / "noexec").c_str(), "tmpfs", MS_NOEXEC, nullptr) != 0 ||
		    ::mount("tmpfs", (tmp / "exec").c_str(), "tmpfs", 0, nullptr) != 0) {
			return false;
		}
		// No per-user cache: in this namespace, which does not map the host's root, the cache's
		// parent directories belong to another user.
		BaseInputs inputs;
		inputs.extension_dir = tmp / "noexec";
		std::vector<DriverAttempt> attempts;
		const auto bases = ChooseBaseDirectories(inputs, attempts);
		// The pre-load check's own version of the test: 1 for noexec, 0 for exec, -1 when it cannot tell.
		if (NoExecState(tmp / "noexec") != 1 || NoExecState(tmp / "exec") != 0 || NoExecState(tmp / "missing") != -1) {
			return false;
		}
		return IsNoExecMount(tmp / "noexec") && !IsNoExecMount(tmp / "exec") && bases.empty() && attempts.size() == 2 &&
		       attempts[0].directory == tmp / "noexec" &&
		       attempts[0].reason == "the extension's directory is mounted noexec";
	});
	if (result == 77) {
		// The stubbed test above covers the rule; this host only lacks unprivileged user namespaces.
		WARN("user namespaces are not available here; the real no-exec mount was not tested");
		return;
	}
	CHECK(result == 0);
}

TEST_CASE("The per-user cache follows XDG_CACHE_HOME only when it is absolute", "[driver][directory]") {
	{
		ScopedEnv xdg("XDG_CACHE_HOME", "/x/cache");
		ScopedEnv home("HOME", "/h");
		CHECK(PerUserCacheBase() == "/x/cache/duckdb-redshift");
	}
	{
		ScopedEnv xdg("XDG_CACHE_HOME", "relative/cache");
		ScopedEnv home("HOME", "/h");
		CHECK(PerUserCacheBase() == "/h/.cache/duckdb-redshift");
	}
	{
		ScopedEnv xdg("XDG_CACHE_HOME", "");
		ScopedEnv home("HOME", "/h");
		CHECK(PerUserCacheBase() == "/h/.cache/duckdb-redshift");
	}
	// Without an absolute HOME there is no per-user cache: the password database is not read,
	// whatever it holds for this user. (Stricter than DuckDB, which takes a relative HOME as given.)
	for (const char *value : {"relative", "", static_cast<const char *>(nullptr)}) {
		ScopedEnv xdg("XDG_CACHE_HOME", nullptr);
		ScopedEnv home("HOME", value);
		CHECK(PerUserCacheBase().empty());
	}
	{
		ScopedEnv xdg("XDG_CACHE_HOME", "relative/cache");
		ScopedEnv home("HOME", nullptr);
		CHECK(PerUserCacheBase().empty());
	}
}

TEST_CASE("A per-user cache that cannot be created is recorded", "[driver][directory]") {
	TempDir tmp;
	if (!PrivateTemp(tmp)) {
		return;
	}
	{ std::ofstream(tmp / "file") << "x"; }
	std::vector<DriverAttempt> attempts;
	auto bases = ChooseBaseDirectories(Inputs("", tmp / "file/duckdb-redshift"), attempts);
	CHECK(bases.empty());
	REQUIRE(attempts.size() == 2);
	CHECK(attempts[1].directory == tmp / "file");
	CHECK(attempts[1].reason == "the per-user cache: a parent directory is not a directory");

	// No cache could be determined at all.
	REQUIRE(::mkdir((tmp / "ext").c_str(), 0700) == 0);
	attempts.clear();
	bases = ChooseBaseDirectories(Inputs(tmp / "ext", ""), attempts);
	CHECK(bases == std::vector<std::string> {tmp / "ext"});
	CHECK(Join(attempts) ==
	      ": the per-user cache cannot be found: neither XDG_CACHE_HOME nor HOME is an absolute path\n");

	// A link at the cache's name to a directory that has gone is refused, not followed to create
	// what it names.
	REQUIRE(::symlink((tmp / "gone").c_str(), (tmp / "dangling").c_str()) == 0);
	attempts.clear();
	CHECK(ChooseBaseDirectories(Inputs("", tmp / "dangling"), attempts).empty());
	REQUIRE(attempts.size() == 2);
	CHECK(attempts[1].directory == tmp / "dangling");
	CHECK(attempts[1].reason == "the per-user cache: the directory is a symbolic link that leads to no directory");
	CHECK(::access((tmp / "gone").c_str(), F_OK) != 0);

	if (::geteuid() == 0) {
		WARN("root can write a read-only directory; the permission cases were not tested");
		return;
	}
	// A parent the user cannot write, and one the user cannot search.
	REQUIRE(::mkdir((tmp / "ro").c_str(), 0500) == 0);
	attempts.clear();
	CHECK(ChooseBaseDirectories(Inputs("", tmp / "ro/cache"), attempts).empty());
	CHECK(attempts.back().directory == tmp / "ro/cache");
	CHECK(attempts.back().reason == "the per-user cache: the directory cannot be created: Permission denied");
	REQUIRE(::mkdir((tmp / "shut").c_str(), 0) == 0);
	attempts.clear();
	CHECK(ChooseBaseDirectories(Inputs("", tmp / "shut/cache"), attempts).empty());
	CHECK(attempts.back().directory == tmp / "shut");
	CHECK(attempts.back().reason == "the per-user cache: a parent directory cannot be examined: Permission denied");
	REQUIRE(::chmod((tmp / "shut").c_str(), 0700) == 0);

	// A cache an earlier run left without the owner's read or search permission, or both, is
	// refused, not tried.
	REQUIRE(::mkdir((tmp / "closed-cache").c_str(), 0700) == 0);
	for (const mode_t mode : {0, 0300, 0600}) {
		INFO("mode " << std::oct << mode);
		REQUIRE(::chmod((tmp / "closed-cache").c_str(), mode) == 0);
		attempts.clear();
		CHECK(ChooseBaseDirectories(Inputs("", tmp / "closed-cache"), attempts).empty());
		REQUIRE(attempts.size() == 2);
		CHECK(attempts[1].directory == tmp / "closed-cache");
		CHECK(attempts[1].reason == "the per-user cache cannot be read and searched: Permission denied");
	}
	REQUIRE(::chmod((tmp / "closed-cache").c_str(), 0700) == 0);

	// A missing top-level directory is named as the one that cannot be created, as for the nobody
	// user's HOME=/nonexistent: EACCES, or EROFS for a read-only root.
	const auto top = "/redshift-driver-test-missing-" + std::to_string(::getpid());
	const bool writable = ::access("/", W_OK) == 0;
	const int root_error = errno;
	if (writable || (root_error != EACCES && root_error != EROFS) || ::access(top.c_str(), F_OK) == 0) {
		WARN("/ is writable here or " + top + " exists; the top-level case was not tested");
		return;
	}
	attempts.clear();
	CHECK(ChooseBaseDirectories(Inputs("", top + "/.cache/duckdb-redshift"), attempts).empty());
	// access reports the permission first and mkdir the read-only mount first, so on a read-only
	// root the two differ.
	const auto &reason = attempts.back().reason;
	CHECK(attempts.back().directory == top);
	const std::string created = "the per-user cache: a parent directory cannot be created: ";
	CHECK((reason == created + ErrnoText(root_error) || reason == created + ErrnoText(EROFS)));
}

TEST_CASE("The cache's read and search check works where faccessat2 is refused", "[driver][directory]") {
	// A seccomp profile older than faccessat2 refuses it with EPERM; glibc 2.33 and later use it
	// for faccessat with AT_EACCESS. The check runs in a child under such a filter.
	TempDir tmp;
	if (!PrivateTemp(tmp)) {
		return;
	}
	const pid_t child = ::fork();
	REQUIRE(child >= 0);
	if (child == 0) {
#ifdef __NR_faccessat2
		const unsigned faccessat2 = __NR_faccessat2;
#else
		const unsigned faccessat2 = 439;
#endif
		struct sock_filter rules[] = {
		    BPF_STMT(BPF_LD | BPF_W | BPF_ABS, offsetof(struct seccomp_data, nr)),
		    BPF_JUMP(BPF_JMP | BPF_JEQ | BPF_K, faccessat2, 0, 1),
		    BPF_STMT(BPF_RET | BPF_K, SECCOMP_RET_ERRNO | (EPERM & SECCOMP_RET_DATA)),
		    BPF_STMT(BPF_RET | BPF_K, SECCOMP_RET_ALLOW),
		};
		struct sock_fprog program = {static_cast<unsigned short>(sizeof(rules) / sizeof(rules[0])), rules};
		if (::prctl(PR_SET_NO_NEW_PRIVS, 1, 0, 0, 0) != 0 ||
		    ::prctl(PR_SET_SECCOMP, SECCOMP_MODE_FILTER, &program, 0, 0) != 0) {
			::_exit(77);
		}
		std::vector<DriverAttempt> attempts;
		const auto bases = ChooseBaseDirectories(Inputs("", tmp / "cache"), attempts);
		::_exit(bases == std::vector<std::string> {tmp / "cache"} ? 0 : 1);
	}
	int status = 0;
	REQUIRE(::waitpid(child, &status, 0) == child);
	REQUIRE(WIFEXITED(status));
	if (WEXITSTATUS(status) == 77) {
		WARN("a seccomp filter cannot be installed here; the faccessat2 case was not tested");
		return;
	}
	CHECK(WEXITSTATUS(status) == 0);
}

TEST_CASE("MakePrivateDirectory creates owner-only from the start and accepts an existing directory",
          "[driver][directory]") {
	TempDir tmp;
	// The mode mkdir is given keeps the directory closed until its mode is set, whatever the umask.
	g_mkdir_path = tmp / "new";
	mode_t at_mkdir = 07777;
	g_after_mkdir = [&] {
		struct stat st;
		if (::stat((tmp / "new").c_str(), &st) == 0) {
			at_mkdir = st.st_mode & 07777;
		}
	};
	const mode_t old_umask = ::umask(0002);
	std::string reason;
	const bool made = MakePrivateDirectory(tmp / "new", reason);
	::umask(old_umask);
	g_after_mkdir = nullptr;
	CHECK(made);
	CHECK((at_mkdir & 077) == 0);
	CHECK(Mode(tmp / "new") == 0700);

	// Another process may have created it first; the caller checks what it finds.
	reason.clear();
	CHECK(MakePrivateDirectory(tmp / "new", reason));
	CHECK(reason.empty());

	// Without /proc the mode is set through a descriptor opened for reading, checked to be the same
	// directory.
	const int result = RunAsNamespaceRoot([&] {
		if (::mount("tmpfs", "/proc", "tmpfs", 0, nullptr) != 0) {
			return false;
		}
		::umask(0277);
		std::string no_proc_reason;
		struct stat st;
		if (!MakePrivateDirectory(tmp / "no-proc", no_proc_reason) || ::stat((tmp / "no-proc").c_str(), &st) != 0 ||
		    (st.st_mode & 07777) != 0700) {
			return false;
		}
		// A umask that takes the owner's read bit makes that descriptor fail. Without the
		// namespace root's capabilities, which would read the directory anyway, the call fails and
		// takes away the directory it made rather than leave it at its name for every later call to
		// refuse.
		__user_cap_header_struct header {_LINUX_CAPABILITY_VERSION_3, 0};
		__user_cap_data_struct none[2] {};
		if (::syscall(SYS_capset, &header, none) != 0) {
			return false;
		}
		::umask(0477);
		return !MakePrivateDirectory(tmp / "no-read", no_proc_reason) &&
		       no_proc_reason == "cannot be opened after it was created: Permission denied" &&
		       ::lstat((tmp / "no-read").c_str(), &st) != 0;
	});
	if (result == 77) {
		WARN("user namespaces are not available here; the case without /proc was not tested");
		return;
	}
	CHECK(result == 0);
}

TEST_CASE("Whatever error mkdir gives, an existing directory is left to the checks and a link or file refused",
          "[driver][directory]") {
	TempDir tmp;
	REQUIRE(::mkdir((tmp / "existing").c_str(), 0700) == 0);
	REQUIRE(::symlink((tmp / "existing").c_str(), (tmp / "link").c_str()) == 0);
	std::ofstream((tmp / "file").c_str()) << "x";
	std::string reason;
	g_mkdir_errno = EACCES;
	g_mkdir_path = tmp / "existing";
	CHECK(MakePrivateDirectory(tmp / "existing", reason));
	g_mkdir_path = tmp / "link";
	CHECK_FALSE(MakePrivateDirectory(tmp / "link", reason));
	CHECK(reason == "is a symbolic link");
	g_mkdir_path = tmp / "file";
	CHECK_FALSE(MakePrivateDirectory(tmp / "file", reason));
	CHECK(reason == "is not a directory");
	g_mkdir_path = tmp / "missing";
	CHECK_FALSE(MakePrivateDirectory(tmp / "missing", reason));
	CHECK(reason == "cannot be created: Permission denied");
	g_mkdir_errno = 0;
	// EEXIST, as a local file system reports it, is treated the same.
	CHECK_FALSE(MakePrivateDirectory(tmp / "link", reason));
	CHECK(reason == "is a symbolic link");
	CHECK(MakePrivateDirectory(tmp / "existing", reason));
}

TEST_CASE("A new directory whose inherited ACL cannot be cleared is refused and removed", "[driver][directory]") {
	// macOS gives a new directory its parent's inheritable ACL entries, which 0700 does not take away; when they
	// cannot be cleared the directory is not private. A stand-in for ClearAcl fails as acl_set_fd_np would, so the
	// case runs on Linux, where ClearAcl itself never fails.
	TempDir tmp;
	std::string reason;
	CHECK_FALSE(MakePrivateDirectory(tmp / "new", reason, [](int) {
		errno = EPERM;
		return false;
	}));
	CHECK(reason == "cannot be made private: its access control list cannot be cleared: Operation not permitted");
	// Not left half-made at its name, where every later call would find it and refuse it.
	struct stat st;
	CHECK(::lstat((tmp / "new").c_str(), &st) != 0);
	CHECK(errno == ENOENT);
}

TEST_CASE("A per-user cache path that is a file is named as the directory, not a parent", "[driver][directory]") {
	TempDir tmp;
	if (!PrivateTemp(tmp)) {
		return;
	}
	std::ofstream((tmp / "file").c_str()) << "x";
	REQUIRE(::symlink((tmp / "file").c_str(), (tmp / "to-file").c_str()) == 0);
	for (const auto &name : {std::string("file"), std::string("to-file")}) {
		const auto cache = ResolveCache(Inputs("", tmp / name), ::geteuid());
		CHECK(cache.attempt.reason == "the per-user cache: the directory is not a directory");
	}
}

TEST_CASE("A link put where the per-user cache is being made is refused, not followed", "[driver][directory]") {
	TempDir tmp;
	if (!PrivateTemp(tmp)) {
		return;
	}
	REQUIRE(::mkdir((tmp / "elsewhere").c_str(), 0700) == 0);
	g_mkdir_path = tmp / "a";
	g_after_mkdir = [&] {
		::symlink((tmp / "elsewhere").c_str(), (tmp / "a/cache").c_str());
	};
	const auto cache = ResolveCache(Inputs("", tmp / "a/cache"), ::geteuid());
	g_after_mkdir = nullptr;
	CHECK(cache.attempt.directory == tmp / "a/cache");
	CHECK(cache.attempt.reason == "the per-user cache: the directory is a symbolic link");
	CHECK(::access((tmp / "elsewhere/.redshift-driver").c_str(), F_OK) != 0);
}

TEST_CASE("A new directory is made private through a descriptor, not its name", "[driver][directory]") {
	TempDir tmp;
	REQUIRE(::mkdir((tmp / "target").c_str(), 0755) == 0);
	REQUIRE(::chmod((tmp / "target").c_str(), 0755) == 0);
	// Whoever can write the parent renames the new directory away and puts a link in its place.
	g_mkdir_path = tmp / "new";
	bool swapped = false;
	g_after_mkdir = [&] {
		swapped = ::rename((tmp / "new").c_str(), (tmp / "moved").c_str()) == 0 &&
		          ::symlink((tmp / "target").c_str(), (tmp / "new").c_str()) == 0;
	};
	std::string reason;
	const bool made = MakePrivateDirectory(tmp / "new", reason);
	g_after_mkdir = nullptr;
	REQUIRE(swapped);
	CHECK_FALSE(made);
	// Linux reports a link opened with O_DIRECTORY | O_NOFOLLOW as not a directory.
	CHECK(reason == "cannot be opened after it was created: Not a directory");
	CHECK(Mode(tmp / "target") == 0755);

	// Another user's directory in its place is refused before any change. In a user namespace the
	// host's root is unmapped, so /etc, bound over the new directory, stands in for one; run as root,
	// the test would find /etc the user's own, so this case needs another user.
	if (::geteuid() == 0) {
		WARN("as root no directory belongs to another mapped user; the ownership case was not tested");
		return;
	}
	const int result = RunAsNamespaceRoot([&] {
		g_mkdir_path = tmp / "other";
		g_after_mkdir = [&] {
			::mount("/etc", (tmp / "other").c_str(), nullptr, MS_BIND | MS_REC, nullptr);
		};
		std::string other_reason;
		return !MakePrivateDirectory(tmp / "other", other_reason) &&
		       other_reason == "is not owned by the user after it was created";
	});
	if (result == 77) {
		WARN("user namespaces are not available here; the ownership case was not tested");
		return;
	}
	CHECK(result == 0);
}

TEST_CASE("A link put at the name after the directory is opened does not get its mode", "[driver][directory]") {
	TempDir tmp;
	REQUIRE(::mkdir((tmp / "target").c_str(), 0755) == 0);
	REQUIRE(::chmod((tmp / "target").c_str(), 0755) == 0);
	// The swap comes after the O_PATH open, just before the mode is set, so only setting it
	// through the descriptor leaves the link's target alone.
	g_chmod_prefix = "/proc/";
	bool swapped = false;
	g_before_chmod = [&] {
		swapped = ::rename((tmp / "new").c_str(), (tmp / "moved").c_str()) == 0 &&
		          ::symlink((tmp / "target").c_str(), (tmp / "new").c_str()) == 0;
	};
	std::string reason;
	const bool made = MakePrivateDirectory(tmp / "new", reason);
	g_before_chmod = nullptr;
	REQUIRE(swapped);
	CHECK(made);
	CHECK(Mode(tmp / "target") == 0755);
	CHECK(Mode(tmp / "moved") == 0700);

	// Without /proc the mode is set through a second descriptor, which is refused when the name
	// has become another directory by then.
	const int result = RunAsNamespaceRoot([&] {
		if (::mount("tmpfs", "/proc", "tmpfs", 0, nullptr) != 0) {
			return false;
		}
		bool replaced = false;
		g_chmod_prefix = "/proc/";
		g_before_chmod = [&] {
			replaced = ::rename((tmp / "no-proc").c_str(), (tmp / "no-proc-moved").c_str()) == 0 &&
			           ::mkdir((tmp / "no-proc").c_str(), 0755) == 0 && ::chmod((tmp / "no-proc").c_str(), 0755) == 0;
		};
		std::string no_proc_reason;
		const bool no_proc_made = MakePrivateDirectory(tmp / "no-proc", no_proc_reason);
		struct stat st;
		return replaced && !no_proc_made && no_proc_reason == "changed after it was created" &&
		       ::stat((tmp / "no-proc").c_str(), &st) == 0 && (st.st_mode & 07777) == 0755;
	});
	if (result == 77) {
		WARN("user namespaces are not available here; the case without /proc was not tested");
		return;
	}
	CHECK(result == 0);
}

TEST_CASE("A thread with its own descriptor table sets the mode of its own directory", "[driver][directory]") {
	TempDir tmp;
	{ std::ofstream(tmp / "file") << "x"; }
	REQUIRE(::chmod((tmp / "file").c_str(), 0644) == 0);
	const int file = ::open((tmp / "file").c_str(), O_RDONLY | O_CLOEXEC);
	REQUIRE(file >= 0);
	// The thread finds the number its next descriptor will get; the main thread's table then has
	// the file at that number, so a mode set through the main thread's /proc/self/fd entry would
	// land on the file.
	int next = -1, unshare_error = 0;
	bool ready = false, go = false, made = false;
	std::string reason;
	std::mutex lock;
	std::condition_variable changed;
	std::thread worker([&] {
		const bool own = ::unshare(CLONE_FILES) == 0;
		const int error = own ? 0 : errno;
		const int probe = own ? ::open("/", O_RDONLY | O_CLOEXEC) : -1;
		if (probe >= 0) {
			::close(probe);
		}
		{
			std::unique_lock<std::mutex> guard(lock);
			unshare_error = error;
			next = probe;
			ready = true;
			changed.notify_all();
			changed.wait(guard, [&] { return go; });
		}
		if (probe >= 0) {
			made = MakePrivateDirectory(tmp / "new", reason);
		}
	});
	bool placed = false;
	{
		std::unique_lock<std::mutex> guard(lock);
		changed.wait(guard, [&] { return ready; });
		placed = next >= 0 && ::fcntl(next, F_GETFD) < 0 && ::dup2(file, next) == next;
		go = true;
		changed.notify_all();
	}
	worker.join();
	if (placed) {
		::close(next);
	}
	::close(file);
	if (unshare_error != 0) {
		// Docker's default seccomp profile refuses unshare without CAP_SYS_ADMIN, whatever the flags.
		WARN("unshare(CLONE_FILES) is refused here (" + ErrnoText(unshare_error) +
		     "); the per-thread descriptor table case was not tested");
		return;
	}
	REQUIRE(next >= 0);
	REQUIRE(placed);
	CHECK(made);
	CHECK(reason.empty());
	CHECK(Mode(tmp / "new") == 0700);
	CHECK(Mode(tmp / "file") == 0644);
}

TEST_CASE("The process's own inputs come from the environment and the loaded file", "[driver][directory]") {
	{
		ScopedEnv dir("REDSHIFT_DRIVER_DIR", "");
		const auto inputs = CurrentBaseInputs();
		CHECK(inputs.override_set);
		CHECK(inputs.override_value.empty());
	}
	{
		ScopedEnv dir("REDSHIFT_DRIVER_DIR", "/srv/redshift");
		ScopedEnv xdg("XDG_CACHE_HOME", "/x/cache");
		const auto inputs = CurrentBaseInputs();
		CHECK(inputs.override_set);
		CHECK(inputs.override_value == "/srv/redshift");
		CHECK(inputs.per_user_base == "/x/cache/duckdb-redshift");
	}
	{
		ScopedEnv dir("REDSHIFT_DRIVER_DIR", nullptr);
		CHECK_FALSE(CurrentBaseInputs().override_set);
	}
	// The tests link the loader statically, so the loaded file is this executable.
	char exe[PATH_MAX];
	REQUIRE(::realpath("/proc/self/exe", exe) != nullptr);
	std::string expected(exe);
	expected.erase(expected.find_last_of('/'));
	CHECK(LoadedFromDirectory() == expected);
	CHECK(CurrentBaseInputs().extension_dir == expected);
	CHECK(MappedFilePath(reinterpret_cast<const void *>(&NeverNoExec)) == exe);
	CHECK(MappedFilePath(nullptr).empty());
	// The stack's mapping has a name, [stack], but no path.
	int on_stack = 0;
	CHECK(MappedFilePath(&on_stack).empty());
	// An address in a library mapped above the executable names that library, not the executable.
	void *libc = ::dlopen("libc.so.6", RTLD_NOW | RTLD_NOLOAD);
	REQUIRE(libc != nullptr);
	const void *in_libc = ::dlsym(libc, "getpwuid_r");
	::dlclose(libc);
	Dl_info info;
	REQUIRE(::dladdr(in_libc, &info) != 0);
	char libc_path[PATH_MAX];
	REQUIRE(::realpath(info.dli_fname, libc_path) != nullptr);
	CHECK(std::string(libc_path) != exe);
	CHECK(MappedFilePath(in_libc) == libc_path);

	// Not resolved against the current directory: argv[0], which dladdr reports for the main
	// program, may be relative, as may a path given to LOAD.
	char cwd[PATH_MAX];
	REQUIRE(::getcwd(cwd, sizeof(cwd)) != nullptr);
	REQUIRE(::chdir("/") == 0);
	const auto from_root = LoadedFromDirectory();
	REQUIRE(::chdir(cwd) == 0);
	CHECK(from_root == expected);
}

TEST_CASE("MappedFilePath names a mapped file until it is removed", "[driver][directory]") {
	TempDir tmp;
	// A name may end in a space, which is part of the path.
	const auto file = tmp / "mapped ";
	{ std::ofstream(file) << std::string(4096, 'x'); }
	const int fd = ::open(file.c_str(), O_RDONLY | O_CLOEXEC);
	REQUIRE(fd >= 0);
	void *mapped = ::mmap(nullptr, 4096, PROT_READ, MAP_PRIVATE, fd, 0);
	::close(fd);
	REQUIRE(mapped != MAP_FAILED);
	bool found = false;
	CHECK(MappedFilePath(mapped, &found) == file);
	CHECK(found);
	REQUIRE(::unlink(file.c_str()) == 0);
	// Still mapped, so LoadedFromDirectory does not fall back to dladdr's old name.
	found = false;
	CHECK(MappedFilePath(mapped, &found).empty());
	CHECK(found);
	::munmap(mapped, 4096);
}

TEST_CASE("MappedFilePath names nothing for a path the kernel escaped", "[driver][directory]") {
	// The kernel writes the newline in this path as the four characters \012, the name of another
	// directory whoever can write the parent could make.
	TempDir tmp;
	REQUIRE(::mkdir((tmp / "a\nb").c_str(), 0700) == 0);
	REQUIRE(::mkdir((tmp / "a\\012b").c_str(), 0700) == 0);
	const auto file = tmp / "a\nb/lib.so";
	{ std::ofstream(file) << std::string(4096, 'x'); }
	const int fd = ::open(file.c_str(), O_RDONLY | O_CLOEXEC);
	REQUIRE(fd >= 0);
	void *mapped = ::mmap(nullptr, 4096, PROT_READ, MAP_PRIVATE, fd, 0);
	::close(fd);
	REQUIRE(mapped != MAP_FAILED);
	CHECK(MappedFilePath(mapped).empty());
	::munmap(mapped, 4096);
}

TEST_CASE("MappedFilePath reads a maps line whole, however long its path", "[driver][directory]") {
	// The path's column, which every line of /proc/self/maps pads its path to.
	std::ifstream maps("/proc/self/maps");
	std::string first;
	REQUIRE(std::getline(maps, first));
	const auto column = first.find('/');
	REQUIRE(column != std::string::npos);
	// A path whose text from where a reader of PATH_MAX + 256 bytes would cut the line reads as a
	// mapping of every address, in a file reached through directories longer than PATH_MAX, so
	// each is made relative to the last.
	TempDir tmp;
	const std::string forged = "0-ffffffffffffffff r-xp 0 0 0 ";
	const size_t cut = PATH_MAX + 256 - 1 - column;
	std::vector<std::string> names;
	size_t length = tmp.path().size();
	while (length + 1 + 200 < cut) {
		names.push_back(std::string(200, 'd'));
		length += 1 + 200;
	}
	names.push_back(std::string(cut - length - 1, 'e') + forged);
	int dir = ::open(tmp.path().c_str(), O_RDONLY | O_DIRECTORY | O_CLOEXEC);
	REQUIRE(dir >= 0);
	std::vector<int> dirs {dir};
	std::string path = tmp.path();
	for (const auto &name : names) {
		REQUIRE(::mkdirat(dirs.back(), name.c_str(), 0700) == 0);
		dirs.push_back(::openat(dirs.back(), name.c_str(), O_RDONLY | O_DIRECTORY | O_CLOEXEC));
		REQUIRE(dirs.back() >= 0);
		path += "/" + name;
	}
	const int fd = ::openat(dirs.back(), "lib.so", O_RDWR | O_CREAT | O_EXCL | O_CLOEXEC, 0600);
	REQUIRE(fd >= 0);
	REQUIRE(::ftruncate(fd, 4096) == 0);
	void *mapped = ::mmap(nullptr, 4096, PROT_READ, MAP_PRIVATE, fd, 0);
	::close(fd);
	REQUIRE(mapped != MAP_FAILED);
	path += "/lib.so";
	REQUIRE(path.size() > PATH_MAX);
	CHECK(MappedFilePath(mapped) == path);
	// The stack is mapped above the file, so a reader that took the cut-off text for a line would
	// find the forged range first.
	int on_stack = 0;
	CHECK(MappedFilePath(&on_stack).empty());
	::munmap(mapped, 4096);
	// TempDir's removal works by full path, which is too long here.
	::unlinkat(dirs.back(), "lib.so", 0);
	for (size_t i = names.size(); i > 0; i--) {
		::close(dirs[i]);
		::unlinkat(dirs[i - 1], names[i - 1].c_str(), AT_REMOVEDIR);
	}
	::close(dirs[0]);
}

// Run only by RunLoadedFromDirectory in a child process ('[.]' keeps it out of the suite).
TEST_CASE("LoadedFromDirectory, written for a parent test", "[.][directory_child]") {
	const char *output = std::getenv("REDSHIFT_DRIVER_TEST_OUTPUT");
	REQUIRE(output != nullptr);
	std::ofstream(output) << LoadedFromDirectory();
}

TEST_CASE("The extension's directory does not come from argv[0]", "[driver][directory]") {
	auto expected = SelfPath();
	expected.erase(expected.find_last_of('/'));
	// The main program's dladdr name is argv[0], which whoever starts the process chooses.
	std::string directory;
	CHECK(RunLoadedFromDirectory("/usr/bin/true", "/", false, directory) == 0);
	CHECK(directory == expected);

	// Without /proc only dladdr is left: an absolute name is resolved, so a link in it is not kept,
	// and a relative one is not resolved against the current directory, even where it would find
	// the file.
	TempDir tmp;
	REQUIRE(::symlink(expected.c_str(), (tmp / "bin").c_str()) == 0);
	const auto name = SelfPath().substr(expected.size() + 1);
	const int result = RunLoadedFromDirectory(tmp / "bin/" + name, "/", true, directory);
	if (result == 77) {
		WARN("user namespaces are not available here; the dladdr fallback was not tested");
		return;
	}
	CHECK(result == 0);
	CHECK(directory == expected);
	CHECK(RunLoadedFromDirectory("bin/" + name, tmp.path(), true, directory) == 0);
	CHECK(directory.empty());
}

TEST_CASE("A relative link with '..' on the cache's path is followed as the kernel follows it", "[driver][directory]") {
	TempDir tmp;
	if (!PrivateTemp(tmp)) {
		return;
	}
	REQUIRE(::mkdir((tmp / "home").c_str(), 0700) == 0);
	REQUIRE(::mkdir((tmp / "real").c_str(), 0700) == 0);
	REQUIRE(::symlink("../real", (tmp / "home/c").c_str()) == 0);
	const auto made = ResolveCache(Inputs("", tmp / "home/c/duckdb-redshift"), ::geteuid());
	CHECK(made.attempt.reason == "");
	CHECK(made.attempt.directory == tmp / "real/duckdb-redshift");
	const auto existing = ResolveCache(Inputs("", tmp / "home/c"), ::geteuid());
	CHECK(existing.attempt.reason == "");
	CHECK(existing.attempt.directory == tmp / "real");
}

TEST_CASE("A doubled or '.' component in the part to make is not a change", "[driver][directory]") {
	TempDir tmp;
	if (!PrivateTemp(tmp)) {
		return;
	}
	const auto cache = ResolveCache(Inputs("", tmp.path() + "/a/.//duckdb-redshift"), ::geteuid());
	CHECK(cache.attempt.reason == "");
	CHECK(cache.attempt.directory == tmp / "a/duckdb-redshift");
}

// O_NOFOLLOW guards only the last component, so the swap is seen when the result resolves elsewhere.
TEST_CASE("A parent swapped for a link while the cache is made is refused", "[driver][directory]") {
	TempDir tmp;
	if (!PrivateTemp(tmp)) {
		return;
	}
	REQUIRE(::mkdir((tmp / "elsewhere").c_str(), 0700) == 0);
	REQUIRE(::mkdir((tmp / "elsewhere/b").c_str(), 0700) == 0);
	g_mkdir_path = tmp / "a/b";
	g_after_mkdir = [&] {
		::rename((tmp / "a").c_str(), (tmp / "a.old").c_str());
		::symlink((tmp / "elsewhere").c_str(), (tmp / "a").c_str());
	};
	const auto cache = ResolveCache(Inputs("", tmp / "a/b"), ::geteuid());
	g_after_mkdir = nullptr;
	CHECK(cache.attempt.directory == tmp / "a/b");
	CHECK(cache.attempt.reason == "the per-user cache: a parent directory changed while it was created");
}

// Run only by the case below in a child process ('[.]' keeps it out of the suite).
TEST_CASE("LoadedFromDirectory after removing its own file, written for a parent test",
          "[.][directory_removed_child]") {
	const char *output = std::getenv("REDSHIFT_DRIVER_TEST_OUTPUT");
	REQUIRE(output != nullptr);
	char self[PATH_MAX];
	const auto n = ::readlink("/proc/self/exe", self, sizeof(self) - 1);
	REQUIRE(n > 0);
	self[n] = 0;
	REQUIRE(::unlink(self) == 0);
	std::ofstream(output) << LoadedFromDirectory();
}

// The mapping is still there, named " (deleted)"; dladdr would give argv[0], /usr/bin/true.
TEST_CASE("A removed extension file does not fall back to dladdr's name", "[driver][directory]") {
	TempDir tmp;
	TempDir out;
	const auto copy = tmp / "copy";
	{
		std::ifstream in(SelfPath(), std::ios::binary);
		std::ofstream o(copy, std::ios::binary);
		o << in.rdbuf();
	}
	REQUIRE(::chmod(copy.c_str(), 0700) == 0);
	ScopedEnv output("REDSHIFT_DRIVER_TEST_OUTPUT", (out / "directory").c_str());
	const pid_t child = ::fork();
	REQUIRE(child >= 0);
	if (child == 0) {
		const int quiet = ::open("/dev/null", O_WRONLY);
		::dup2(quiet, STDOUT_FILENO);
		char *argv[] = {const_cast<char *>("/usr/bin/true"), const_cast<char *>("[directory_removed_child]"), nullptr};
		::execv(copy.c_str(), argv);
		::_exit(1);
	}
	int status = 0;
	REQUIRE(::waitpid(child, &status, 0) == child);
	REQUIRE(WIFEXITED(status));
	CHECK(WEXITSTATUS(status) == 0);
	CHECK(ReadFile(out / "directory") == "");
}
