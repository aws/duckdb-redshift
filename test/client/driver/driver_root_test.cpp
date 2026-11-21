// Directory safety: the ancestry walk, the driver root and
// the NO_USABLE_DIRECTORY error, in temporary directories.
#include "driver_loader_core.hpp"
#include "loader_test.hpp"
#include "temp_dir.hpp"

#include "catch.hpp"

#include <chrono>
#include <cstdio>
#include <fstream>
#include <thread>
#include <string>
#include <vector>

#include <fcntl.h>
#include <sched.h>
#include <sys/mount.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>

using duckdb::redshift_driver::DriverAttempt;
using duckdb::redshift_driver::DriverErrorKind;
using namespace duckdb::redshift_driver::detail;
using redshift_driver_test::DIRECTORY_ADVICE;
using redshift_driver_test::Expect;
using redshift_driver_test::Inputs;
using redshift_driver_test::MakeDir;
using redshift_driver_test::Mode;
using redshift_driver_test::NeverNoExec;
using redshift_driver_test::OTHER_USER;
using redshift_driver_test::PrivateAncestry;
using redshift_driver_test::RunAsNamespaceRoot;
using redshift_driver_test::RunAsRootOfPrivateTree;
using redshift_driver_test::TempDir;

namespace {

// A refusal: the directory it is about, and the rule, which names no path.
struct Refused {
	std::string where;
	std::string reason;
	bool operator==(const Refused &other) const {
		return where == other.where && reason == other.reason;
	}
};

Refused Refusal(const std::string &dir, bool is_base = true, uid_t user = ::geteuid()) {
	Refused refused;
	CHECK_FALSE(CheckAncestry(dir, is_base, user, refused.where, refused.reason));
	return refused;
}

bool Accepted(const std::string &dir, bool is_base = true, uid_t user = ::geteuid()) {
	std::string where, reason;
	return CheckAncestry(dir, is_base, user, where, reason);
}

bool Prepare(const std::string &base, uid_t user, std::string &root) {
	std::string where, reason;
	return PrepareDriverRoot(base, user, root, where, reason);
}

Refused PrepareRefusal(const std::string &base, uid_t user = ::geteuid()) {
	Refused refused;
	std::string root;
	CHECK_FALSE(PrepareDriverRoot(base, user, root, refused.where, refused.reason));
	return refused;
}

// A user other than the caller, for tests that need a directory someone else owns: as root the
// directory is given to that user; otherwise the caller pretends to be that user.
uid_t OwnedByOther(const std::string &path) {
	if (::geteuid() == 0) {
		REQUIRE(::chown(path.c_str(), OTHER_USER, static_cast<gid_t>(-1)) == 0);
		return OTHER_USER;
	}
	return ::geteuid();
}

// Every root DriverRoots yields, and its attempts.
std::vector<std::string> AllRoots(const BaseInputs &inputs, std::vector<DriverAttempt> &attempts) {
	DriverRoots roots(inputs, ::geteuid());
	std::vector<std::string> found;
	std::string root;
	while (roots.Next(root)) {
		found.push_back(root);
	}
	attempts = roots.attempts;
	return found;
}

} // namespace

namespace Catch {
template <>
struct StringMaker<Refused> {
	static std::string convert(const Refused &refused) {
		return "{" + refused.where + ", " + refused.reason + "}";
	}
};
} // namespace Catch

TEST_CASE("A private base passes, and its driver root is created owner-only", "[driver][ancestry]") {
	TempDir tmp;
	if (!PrivateAncestry(tmp)) {
		return;
	}
	std::string root;
	CHECK(Accepted(tmp.path()));
	REQUIRE(Prepare(tmp.path(), ::geteuid(), root));
	CHECK(root == tmp / ".redshift-driver");
	CHECK(Mode(root) == 0700);
	// An existing, private root is accepted again.
	REQUIRE(Prepare(tmp.path(), ::geteuid(), root));
}

TEST_CASE("A directory writable by another user is refused, on the path or as the base", "[driver][ancestry]") {
	TempDir tmp;
	if (!PrivateAncestry(tmp)) {
		return;
	}
	MakeDir(tmp / "open", 0777);
	MakeDir(tmp / "open/base", 0700);
	const Refused open_parent {tmp / "open", "a parent directory is writable by another user"};
	CHECK(Refusal(tmp / "open/base") == open_parent);
	// Nothing is created under a refused base.
	CHECK(PrepareRefusal(tmp / "open/base") == open_parent);
	CHECK(::access((tmp / "open/base/.redshift-driver").c_str(), F_OK) != 0);
	MakeDir(tmp / "group", 0770);
	CHECK(Refusal(tmp / "group") == Refused {tmp / "group", "the directory is writable by another user"});
	// Others' write bit alone, on the base and on a parent.
	MakeDir(tmp / "others", 0703);
	CHECK(Refusal(tmp / "others") == Refused {tmp / "others", "the directory is writable by another user"});
	MakeDir(tmp / "others-parent", 0757);
	MakeDir(tmp / "others-parent/base", 0700);
	CHECK(Refusal(tmp / "others-parent/base") ==
	      Refused {tmp / "others-parent", "a parent directory is writable by another user"});
	// Readable by others is fine for an ancestor and for the base; only the driver root is private.
	MakeDir(tmp / "readable", 0755);
	CHECK(Accepted(tmp / "readable"));
}

TEST_CASE("A directory owned by another user is refused", "[driver][ancestry]") {
	TempDir tmp;
	if (!PrivateAncestry(tmp)) {
		return;
	}
	MakeDir(tmp / "base", 0700);
	uid_t user = ::geteuid() + 1;
	if (::geteuid() == 0) {
		// As root every directory here is root's, so give the base to someone else instead.
		user = 0;
		REQUIRE(::chown((tmp / "base").c_str(), 12345, static_cast<gid_t>(-1)) == 0);
	}
	const auto refused = Refusal(tmp / "base", true, user);
	if (::geteuid() == 0) {
		CHECK(refused == Refused {tmp / "base", "the directory is owned by another user"});
	} else {
		// The first directory the caller owns on the way down: the temporary directory, or above it.
		CHECK(refused.reason == "a parent directory is owned by another user");
		CHECK((tmp / "base").compare(0, refused.where.size(), refused.where) == 0);
	}
}

TEST_CASE("A symbolic link anywhere in the path is refused", "[driver][ancestry]") {
	TempDir tmp;
	if (!PrivateAncestry(tmp)) {
		return;
	}
	MakeDir(tmp / "real", 0700);
	MakeDir(tmp / "real/base", 0700);
	REQUIRE(::symlink((tmp / "real").c_str(), (tmp / "link").c_str()) == 0);
	CHECK(Refusal(tmp / "link/base") == Refused {tmp / "link", "a parent directory is a symbolic link"});
	CHECK(Refusal(tmp / "link") == Refused {tmp / "link", "the directory is a symbolic link"});
	REQUIRE(::symlink((tmp / "real").c_str(), (tmp / "real/base/.redshift-driver").c_str()) == 0);
	CHECK(PrepareRefusal(tmp / "real/base") ==
	      Refused {tmp / "real/base/.redshift-driver", "the driver root is a symbolic link"});
}

TEST_CASE("The walk's own refusals: not absolute, missing, and a base no one may write", "[driver][ancestry]") {
	TempDir tmp;
	if (!PrivateAncestry(tmp)) {
		return;
	}
	CHECK(Refusal("relative/dir") == Refused {"relative/dir", "the directory is not an absolute path"});
	CHECK(Refusal("") == Refused {"", "the directory is not an absolute path"});
	CHECK(Refusal(tmp / "missing") ==
	      Refused {tmp / "missing", "the directory cannot be examined: No such file or directory"});
	CHECK(Refusal(tmp / "missing/below") ==
	      Refused {tmp / "missing", "a parent directory cannot be examined: No such file or directory"});
	if (::geteuid() == 0) {
		WARN("root can write a read-only directory; the read-only base was not tested");
		return;
	}
	MakeDir(tmp / "ro", 0500);
	CHECK(PrepareRefusal(tmp / "ro") ==
	      Refused {tmp / "ro/.redshift-driver", "the driver root cannot be created: Permission denied"});
	// "/" as a base names its root without a doubled slash. A read-only root file system refuses
	// the mkdir before the permission check.
	const auto at_root = PrepareRefusal("/");
	CHECK(at_root.where == "/.redshift-driver");
	CHECK((at_root.reason == "the driver root cannot be created: Permission denied" ||
	       at_root.reason == "the driver root cannot be created: Read-only file system"));
}

TEST_CASE("A root-owned sticky directory may be an ancestor but not the base", "[driver][ancestry]") {
	struct stat st;
	if (::lstat("/tmp", &st) != 0 || st.st_uid != 0 || (st.st_mode & 01777) != 01777) {
		WARN("/tmp is not a root-owned sticky directory here; not tested");
		return;
	}
	const Refused shared {"/tmp", "the directory is shared (sticky, writable by others) and cannot be the base"};
	CHECK(Refusal("/tmp") == shared);
	CHECK(PrepareRefusal("/tmp") == shared);
	// Below it, a private directory is a valid base (every TempDir is one when TMPDIR is unset).
	CHECK(Accepted("/tmp", false));
	char pattern[] = "/tmp/redshift-driver-sticky-XXXXXX";
	REQUIRE(::mkdtemp(pattern) != nullptr);
	CHECK(Accepted(pattern));
	// The same directory, for another user, belongs to someone else (unless root made it).
	if (::geteuid() != 0) {
		CHECK(Refusal(pattern, true, ::geteuid() + 1) == Refused {pattern, "the directory is owned by another user"});
	}
	::rmdir(pattern);
}

TEST_CASE("A sticky directory owned by a user is not shared-safe", "[driver][ancestry]") {
	TempDir tmp;
	if (!PrivateAncestry(tmp)) {
		return;
	}
	MakeDir(tmp / "sticky", 01777);
	MakeDir(tmp / "sticky/base", 0700);
	const uid_t user = OwnedByOther(tmp / "sticky");
	if (user == OTHER_USER) {
		REQUIRE(::chown((tmp / "sticky/base").c_str(), OTHER_USER, static_cast<gid_t>(-1)) == 0);
	}
	CHECK(Refusal(tmp / "sticky/base", true, user) ==
	      Refused {tmp / "sticky", "a parent directory is writable by another user"});
}

TEST_CASE("Root's exceptions: a writable root-owned directory must be sticky, and a root-owned driver root "
          "may be readable but never writable",
          "[driver][ancestry]") {
	const int result = RunAsRootOfPrivateTree([] {
		const uid_t root_user = 0;
		auto make = [](const std::string &path, mode_t mode) {
			return ::mkdir(path.c_str(), 0700) == 0 && ::chmod(path.c_str(), mode) == 0;
		};
		std::string root, where, reason;
		bool ok = Expect(make("/open", 0777) && make("/open/base", 0700) && make("/base", 0755) &&
		                     make("/base/.redshift-driver", 0755),
		                 "setup");
		// Root-owned and world-writable but not sticky: refused like any other.
		ok = ok && Expect(!CheckAncestry("/open/base", true, OTHER_USER, where, reason) && where == "/open" &&
		                      reason == "a parent directory is writable by another user",
		                  "a root-owned 0777 parent: " + reason);
		// An administrator's root-owned 0755 driver root is accepted for any user, and for root.
		ok = ok &&
		     Expect(PrepareDriverRoot("/base", OTHER_USER, root, where, reason), "a root-owned 0755 root: " + reason);
		ok =
		    ok && Expect(PrepareDriverRoot("/base", root_user, root, where, reason), "root's own 0755 root: " + reason);
		// A root-owned sticky driver root that group or others can write would let them fill it.
		for (const mode_t mode : {01777, 01770, 01703}) {
			ok = ok && Expect(::chmod("/base/.redshift-driver", mode) == 0 &&
			                      !PrepareDriverRoot("/base", OTHER_USER, root, where, reason) &&
			                      where == "/base/.redshift-driver" &&
			                      reason == "the driver root is writable by group or others",
			                  "a root-owned " + std::to_string(mode) + " root: " + reason);
		}
		return ok;
	});
	if (result == 77) {
		WARN("user namespaces are not available here; root's exceptions were not tested");
		return;
	}
	CHECK(result == 0);
}

TEST_CASE("Only a root-owned driver root may be readable by others, by its mode or its ACL", "[driver][ancestry]") {
	// PrepareDriverRoot's exception, a function of the root's owner. Its mode use is reached by root's exceptions
	// above, in a user namespace. Linux's AclRule finds nothing, so for its ACL use a stand-in finds access for
	// others in the owner-only check, the only one that asks about any access: a root-owned root is used anyway,
	// for another user and for root. As real root (run-loader-tests-as-root.sh, whose container has no user
	// namespaces) a TempDir is root's own, so that half runs there directly. The user's own root is refused, as
	// in the inherited-ACL case below; that half shows the stand-in reaches the check.
	CHECK(DriverRootMayBeReadable(0));
	CHECK_FALSE(DriverRootMayBeReadable(OTHER_USER));
	CHECK_FALSE(DriverRootMayBeReadable(501));
	static const std::string rule = "grants access to another user or group through its access control list";
	static const AclRuleHook stand_in = [](const std::string &, int, uid_t, bool any_access) {
		AclCheck check;
		if (any_access) {
			check.finding = AclFinding::BROKEN;
			check.rule = rule;
		}
		return check;
	};
	// A root-owned 0755 root under `base`, used for another user and for root itself, as the mode's exception is
	// checked above. Each call first, so the message holds its reason.
	auto root_owned_root_is_used = [](const std::string &base) {
		const auto dir = base + "/" + DRIVER_ROOT_NAME;
		std::string root, where, reason;
		if (!Expect(::mkdir(base.c_str(), 0700) == 0 && ::chmod(base.c_str(), 0755) == 0 &&
		                ::mkdir(dir.c_str(), 0700) == 0 && ::chmod(dir.c_str(), 0755) == 0,
		            "setup")) {
			return false;
		}
		const bool used = PrepareDriverRoot(base, OTHER_USER, root, where, reason, VolumeRule, stand_in);
		if (!Expect(used, "a root-owned root readable by its ACL: " + reason)) {
			return false;
		}
		const bool used_by_root = PrepareDriverRoot(base, 0, root, where, reason, VolumeRule, stand_in);
		return Expect(used_by_root, "root's own root readable by its ACL: " + reason);
	};
	TempDir tmp;
	if (::geteuid() == 0) {
		if (PrivateAncestry(tmp)) {
			CHECK(root_owned_root_is_used(tmp / "base"));
		}
		return;
	}
	const int result = RunAsRootOfPrivateTree([&] { return root_owned_root_is_used("/base"); });
	if (result == 77) {
		WARN("user namespaces are not available here; a root-owned root's ACL exception was not tested");
	} else {
		CHECK(result == 0);
	}
	if (!PrivateAncestry(tmp)) {
		return;
	}
	MakeDir(tmp / "base", 0700);
	std::string root, where, reason;
	CHECK_FALSE(PrepareDriverRoot(tmp / "base", ::geteuid(), root, where, reason, VolumeRule, stand_in));
	CHECK(where == tmp / "base" + "/" + DRIVER_ROOT_NAME);
	CHECK(reason == "the driver root " + rule);
}

TEST_CASE("A directory on the walk whose ACL cannot be read is refused, named by its role", "[driver][ancestry]") {
	// Linux's AclRule finds nothing and macOS's walk may refuse such a directory's lstat first, so the ACL rule's own
	// refusal is reached with a stand-in for AclRule that cannot read the ACL of one directory.
	TempDir tmp;
	if (!PrivateAncestry(tmp)) {
		return;
	}
	const auto base = tmp / "base";
	MakeDir(base, 0700);
	static std::string unreadable;
	const auto rule = [](const std::string &path, int, uid_t, bool) {
		return path == unreadable ? AclReadFailure(EACCES) : AclCheck();
	};
	for (const auto &dir : {tmp.path(), base}) {
		INFO(dir);
		unreadable = dir;
		std::string where, reason;
		CHECK_FALSE(CheckAncestry(base, AncestryEnd::BASE, ::geteuid(), where, reason, rule));
		CHECK(where == dir);
		CHECK(reason == (dir == base ? "the directory " : "a parent directory ") + std::string(ACL_UNREADABLE) +
		                    ": Permission denied");
	}
	unreadable.clear();
	std::string where, reason;
	CHECK(CheckAncestry(base, AncestryEnd::BASE, ::geteuid(), where, reason, rule));
}

TEST_CASE("A root of the user's refused only for ACL entries it inherited is looked at again, as a concurrent "
          "first run leaves it until it clears them; any other ACL refusal counts at once",
          "[driver][ancestry]") {
	// Linux's AclRule finds nothing, so a stand-in answers for the root's owner-only ACL rule: a refusal, for entries
	// inherited or not, at its first `looks` reads, then nothing, as once the run that made the root has cleared them.
	// Root's own driver root is exempt from the owner-only rule, so the stand-in is never asked under root.
	TempDir tmp;
	if (!PrivateAncestry(tmp) || ::geteuid() == 0) {
		return;
	}
	const auto base = tmp / "base";
	MakeDir(base, 0700);
	static std::string root;
	static int looks;
	static int reads;
	static bool inherited;
	root = base + "/" + DRIVER_ROOT_NAME;
	const std::string rule =
	    "grants access to another user or group through its access control list (it must be owner-only)";
	auto inputs = Inputs("", "");
	inputs.override_set = true;
	inputs.override_value = base;
	inputs.acl_rule = [](const std::string &path, int, uid_t, bool any_access) {
		AclCheck check;
		if (path == root && any_access && ++reads <= looks) {
			check.finding = AclFinding::BROKEN;
			check.rule = "grants access to another user or group through its access control list (it must be "
			             "owner-only)";
			check.inherited = inherited;
		}
		return check;
	};
	struct Case {
		const char *what;
		bool inherited;
		int looks;
		bool used;
		int reads;
	};
	for (const auto &c : std::vector<Case> {
	         {"inherited, cleared after three looks: used", true, 3, true, 4},
	         {"set on the root itself: refused at once", false, 3, false, 1},
	         {"inherited, never cleared: refused after the retries", true, 1000, false, CACHE_MODE_RETRIES + 1},
	     }) {
		INFO(c.what);
		inherited = c.inherited;
		looks = c.looks;
		reads = 0;
		std::vector<DriverAttempt> attempts;
		const auto roots = AllRoots(inputs, attempts);
		CHECK(reads == c.reads);
		if (c.used) {
			CHECK(roots == std::vector<std::string> {root});
			CHECK(attempts.empty());
		} else {
			CHECK(roots.empty());
			REQUIRE(attempts.size() == 1);
			CHECK(attempts[0].directory == root);
			CHECK(attempts[0].reason == std::string(DRIVER_DIR_VARIABLE) + ": the driver root " + rule);
		}
	}
}

TEST_CASE("The driver root grants group and others nothing", "[driver][ancestry]") {
	TempDir tmp;
	if (!PrivateAncestry(tmp)) {
		return;
	}
	MakeDir(tmp / ".redshift-driver", 0750);
	const uid_t user = OwnedByOther(tmp / ".redshift-driver");
	if (user == OTHER_USER) {
		REQUIRE(::chown(tmp.path().c_str(), OTHER_USER, static_cast<gid_t>(-1)) == 0);
	}
	const Refused not_private {tmp / ".redshift-driver",
	                           "the driver root grants access to group or others (it must be owner-only, such as mode "
	                           "0700)"};
	CHECK(PrepareRefusal(tmp.path(), user) == not_private);
	// Each of group's and others' read and search bits on its own, and an administrator's 0755,
	// which only root may use.
	for (const mode_t mode : {0701, 0704, 0710, 0740, 0755}) {
		INFO("mode " << std::oct << mode);
		REQUIRE(::chmod((tmp / ".redshift-driver").c_str(), mode) == 0);
		CHECK(PrepareRefusal(tmp.path(), user) == not_private);
	}
	std::string root;
	REQUIRE(::chmod((tmp / ".redshift-driver").c_str(), 0500) == 0);
	CHECK(Prepare(tmp.path(), user, root));
	REQUIRE(::chmod((tmp / ".redshift-driver").c_str(), 0700) == 0);
	CHECK(Prepare(tmp.path(), user, root));
	// A file in its place is refused rather than replaced.
	TempDir other;
	REQUIRE(::close(::creat((other / ".redshift-driver").c_str(), 0600)) == 0);
	CHECK(PrepareRefusal(other.path()) == Refused {other / ".redshift-driver", "the driver root is not a directory"});
}

TEST_CASE("When every base is refused, NO_USABLE_DIRECTORY names every attempt and REDSHIFT_DRIVER_DIR",
          "[driver][ancestry]") {
	TempDir tmp;
	if (!PrivateAncestry(tmp)) {
		return;
	}
	MakeDir(tmp / "open", 0777);
	MakeDir(tmp / "open/ext", 0700);
	std::vector<DriverAttempt> attempts;
	CHECK(AllRoots(Inputs(tmp / "open/ext", tmp / "open/cache"), attempts).empty());
	REQUIRE(attempts.size() == 2);
	// Each attempt keeps the directory the rule is about, for a debug log.
	CHECK(attempts[0].directory == tmp / "open");
	CHECK(attempts[0].reason == "the extension's directory: a parent directory is writable by another user");
	CHECK(attempts[1].directory == tmp / "open");
	CHECK(attempts[1].reason == "the per-user cache: a parent directory is writable by another user");
	const auto error = NoUsableDirectory(attempts);
	CHECK(error.kind == DriverErrorKind::NO_USABLE_DIRECTORY);
	CHECK(error.attempts.size() == 2);
	const std::string start = "no usable directory to install the Redshift ODBC driver in (";
	const std::string advice = ". Set REDSHIFT_DRIVER_DIR to the absolute path of an existing directory on a file "
	                           "system that allows executing code, with no symbolic link in its path, where it and "
	                           "every directory above it are owned by you or root and writable by no one else";
	// The message names each base by its role and the rule, never a path.
	CHECK(error.message == start + attempts[0].reason + "; " + attempts[1].reason + ")" + advice);
	CHECK(error.message.find(tmp.path()) == std::string::npos);

	// Attempts without a directory, and none at all.
	CHECK(AllRoots(Inputs("", ""), attempts).empty());
	CHECK(NoUsableDirectory(attempts).message ==
	      start +
	          "the extension's own directory could not be determined; the per-user cache cannot be found: neither "
	          "XDG_CACHE_HOME nor HOME is an absolute path)" +
	          advice);
	CHECK(NoUsableDirectory({}).message == "no usable directory to install the Redshift ODBC driver in" + advice);

	// A refused override is the only attempt, and its message holds no path either, however the value
	// is spelled.
	{ std::ofstream(tmp / "file") << "x"; }
	MakeDir(tmp / "real", 0700);
	REQUIRE(::symlink((tmp / "real").c_str(), (tmp / "link").c_str()) == 0);
	auto inputs = Inputs(tmp / "open/ext", tmp / "open/cache");
	inputs.override_set = true;
	struct Case {
		std::string value;
		std::string where;
		std::string reason;
	};
	for (const auto &c : std::vector<Case> {
	         {"relative", "", "REDSHIFT_DRIVER_DIR is not an absolute path"},
	         {tmp / "file", tmp / "file", "REDSHIFT_DRIVER_DIR is not a directory"},
	         {tmp / "file/", tmp / "file", "REDSHIFT_DRIVER_DIR is not a directory"},
	         {tmp.path() + "//file", tmp / "file", "REDSHIFT_DRIVER_DIR is not a directory"},
	         {tmp.path() + "/./file", tmp / "file", "REDSHIFT_DRIVER_DIR is not a directory"},
	         {tmp / "file//", tmp / "file", "REDSHIFT_DRIVER_DIR is not a directory"},
	         {tmp / "file/.", tmp / "file", "REDSHIFT_DRIVER_DIR is not a directory"},
	         {tmp / "file/below", tmp / "file", "REDSHIFT_DRIVER_DIR: a parent directory is not a directory"},
	         {tmp / "file/./below/", tmp / "file", "REDSHIFT_DRIVER_DIR: a parent directory is not a directory"},
	         {tmp / "missing", tmp / "missing", "REDSHIFT_DRIVER_DIR does not exist"},
	         {tmp / "link", tmp / "link",
	          "REDSHIFT_DRIVER_DIR contains a symbolic link; set it to the path the link resolves to"},
	         {tmp / "real/../real", tmp / "real", "REDSHIFT_DRIVER_DIR contains '..'"},
	     }) {
		INFO(c.value);
		inputs.override_value = c.value;
		CHECK(AllRoots(inputs, attempts).empty());
		REQUIRE(attempts.size() == 1);
		CHECK(attempts[0].directory == c.where);
		CHECK(attempts[0].reason == c.reason);
		CHECK(NoUsableDirectory(attempts).message == start + c.reason + ")" + advice);
	}
}

TEST_CASE("Attempts are listed in the order the bases were tried", "[driver][ancestry]") {
	// The extension's directory is refused by the walk, the cache by selection.
	TempDir tmp;
	if (!PrivateAncestry(tmp)) {
		return;
	}
	MakeDir(tmp / "ext", 0770);
	{ std::ofstream(tmp / "file") << "x"; }
	std::vector<DriverAttempt> attempts;
	CHECK(AllRoots(Inputs(tmp / "ext", tmp / "file/cache"), attempts).empty());
	REQUIRE(attempts.size() == 2);
	CHECK(attempts[0].directory == tmp / "ext");
	CHECK(attempts[0].reason == "the extension's directory: the directory is writable by another user");
	CHECK(attempts[1].directory == tmp / "file");
	CHECK(attempts[1].reason == "the per-user cache: a parent directory is not a directory");
}

TEST_CASE("The first usable base wins, and a later base is not touched", "[driver][ancestry]") {
	TempDir tmp;
	if (!PrivateAncestry(tmp)) {
		return;
	}
	MakeDir(tmp / "ext", 0700);
	// Neither the per-user cache nor its driver root is made until the cache is reached.
	DriverRoots roots(Inputs(tmp / "ext", tmp / "cache"), ::geteuid());
	CHECK(::access((tmp / "cache").c_str(), F_OK) != 0);
	std::string root;
	REQUIRE(roots.Next(root));
	CHECK(root == tmp / "ext/.redshift-driver");
	CHECK(::access((tmp / "cache").c_str(), F_OK) != 0);
	// Only when the caller asks for the next one.
	REQUIRE(roots.Next(root));
	CHECK(root == tmp / "cache/.redshift-driver");
	CHECK(Mode(tmp / "cache") == 0700);
	CHECK_FALSE(roots.Next(root));
	CHECK(roots.attempts.empty());

	// A refused first base is recorded and skipped.
	TempDir other;
	MakeDir(other / "open", 0777);
	MakeDir(other / "open/ext", 0700);
	std::vector<DriverAttempt> attempts;
	CHECK(AllRoots(Inputs(other / "open/ext", other / "cache"), attempts) ==
	      std::vector<std::string> {other / "cache/.redshift-driver"});
	REQUIRE(attempts.size() == 1);
	CHECK(attempts[0].directory == other / "open");
}

TEST_CASE("REDSHIFT_DRIVER_DIR is normalized before the walk", "[driver][ancestry]") {
	TempDir tmp;
	if (!PrivateAncestry(tmp)) {
		return;
	}
	MakeDir(tmp / "admin", 0700);
	for (const auto &value : {tmp / "admin/", tmp.path() + "//admin", tmp.path() + "/./admin", tmp / "admin/."}) {
		INFO(value);
		auto inputs = Inputs("", "");
		inputs.override_set = true;
		inputs.override_value = value;
		std::vector<DriverAttempt> attempts;
		CHECK(AllRoots(inputs, attempts) == std::vector<std::string> {tmp / "admin/.redshift-driver"});
	}
	auto inputs = Inputs("", "");
	inputs.override_set = true;
	inputs.override_value = "/";
	std::vector<DriverAttempt> ignored;
	CHECK(ChooseBaseDirectories(inputs, ignored) == std::vector<std::string> {"/"});
}

TEST_CASE("A user namespace's overflow user cannot tell its own directories from others'", "[driver][ancestry]") {
	// Mapped to the overflow user, or not mapped at all, the caller sees itself as that user, which
	// every owner the namespace does not map also appears as.
	TempDir tmp;
	if (!PrivateAncestry(tmp)) {
		return;
	}
	const auto dir = tmp.path();
	const uid_t uid = ::geteuid();
	for (const bool map_to_overflow : {true, false}) {
		INFO(std::string(map_to_overflow ? "mapped to 65534" : "not mapped"));
		const pid_t child = ::fork();
		REQUIRE(child >= 0);
		if (child == 0) {
			if (::unshare(CLONE_NEWUSER) != 0) {
				::_exit(77);
			}
			if (map_to_overflow) {
				const int fd = ::open("/proc/self/uid_map", O_WRONLY);
				const std::string line = "65534 " + std::to_string(uid) + " 1";
				if (fd < 0 || ::write(fd, line.data(), line.size()) != static_cast<ssize_t>(line.size())) {
					::_exit(77);
				}
				::close(fd);
			}
			const std::string overflow = "the user (uid " + std::to_string(::geteuid()) +
			                             ") is a user namespace's overflow user, as which every unmapped owner "
			                             "appears, so no directory can be told to be its own";
			std::string where, reason;
			bool ok = Expect(!CheckAncestry(dir, true, ::geteuid(), where, reason) && reason == overflow,
			                 "the walk: " + reason);
			// Said once, not once per base, with advice that fits: no directory would do.
			std::vector<DriverAttempt> attempts;
			ok = ok && Expect(AllRoots(Inputs(dir, dir + "/cache"), attempts).empty() && attempts.size() == 1 &&
			                      attempts[0].reason == overflow,
			                  "the bases");
			ok = ok && Expect(NoUsableDirectory(attempts).message ==
			                      "no usable directory to install the Redshift ODBC driver in (" + overflow +
			                          "). Run it as a user that its user namespace maps",
			                  "the message: " + NoUsableDirectory(attempts).message);
			::_exit(ok ? 0 : 1);
		}
		int status = 0;
		REQUIRE(::waitpid(child, &status, 0) == child);
		REQUIRE(WIFEXITED(status));
		if (WEXITSTATUS(status) == 77) {
			WARN("user namespaces are not available here; not tested");
			return;
		}
		CHECK(WEXITSTATUS(status) == 0);
	}
	// In the initial namespace, which maps every id to itself, the overflow user is a real one: "/"
	// is root's and closed to others, so it is a valid base for that user too.
	std::ifstream map("/proc/self/uid_map");
	unsigned long long inside = 1, outside = 1, count = 0;
	if (!(map >> inside >> outside >> count) || inside != 0 || outside != 0 || count != 4294967295ULL) {
		WARN("the tests run inside a user namespace; the initial namespace's overflow user was not tested");
		return;
	}
	CHECK(Accepted("/", true, 65534));
	// Without /proc nothing shows which namespace this is, so the overflow user is refused.
	const int result = RunAsRootOfPrivateTree([] {
		std::string where, reason;
		return Expect(!CheckAncestry("/", true, 65534, where, reason) &&
		                  reason == "the user (uid 65534) is a user namespace's overflow user, as which every "
		                            "unmapped owner appears, so no directory can be told to be its own",
		              "without /proc: " + reason);
	});
	if (result == 77) {
		WARN("user namespaces are not available here; the case without /proc was not tested");
		return;
	}
	CHECK(result == 0);
}

TEST_CASE("A failure the caller adds is named by the base it was given", "[driver][ancestry]") {
	TempDir tmp;
	if (!PrivateAncestry(tmp)) {
		return;
	}
	MakeDir(tmp / "ext", 0700);
	DriverRoots roots(Inputs(tmp / "ext", tmp / "cache"), ::geteuid());
	std::string root;
	REQUIRE(roots.Next(root));
	roots.Fail(root, "the driver set cannot be written: No space left on device");
	REQUIRE(roots.Next(root));
	roots.Fail(root, "the driver set does not match its hashes");
	CHECK_FALSE(roots.Next(root));
	REQUIRE(roots.attempts.size() == 2);
	CHECK(roots.attempts[0].directory == tmp / "ext/.redshift-driver");
	CHECK(roots.attempts[0].reason ==
	      "the extension's directory: the driver set cannot be written: No space left on device");
	CHECK(roots.attempts[1].reason == "the per-user cache: the driver set does not match its hashes");
}

TEST_CASE("In a user namespace that does not map root, the host's directories belong to another user",
          "[driver][ancestry]") {
	// The kernel shows an unmapped owner, the host's root included, as the overflow user, which
	// may be anyone; the walk refuses it rather than guess. Run as root,
	// the namespace maps the host's root, so there is nothing to show.
	if (::geteuid() == 0) {
		WARN("the tests run as root, which the namespace maps; not tested");
		return;
	}
	TempDir tmp;
	const auto dir = tmp.path();
	const int result = RunAsNamespaceRoot([&] {
		std::string where, reason;
		return !CheckAncestry(dir, true, ::geteuid(), where, reason) && where == "/" &&
		       reason == "a parent directory is owned by another user";
	});
	if (result == 77) {
		WARN("user namespaces are not available here; not tested");
		return;
	}
	CHECK(result == 0);
}

TEST_CASE("No path with a '$' is used, since the platform loader would rewrite it", "[driver][ancestry]") {
	TempDir tmp;
	if (!PrivateAncestry(tmp)) {
		return;
	}
	MakeDir(tmp / "a$LIB", 0700);
	CHECK(Refusal(tmp / "a$LIB") ==
	      Refused {tmp / "a$LIB", "the directory's path contains a '$', which the platform loader would rewrite"});
	std::vector<DriverAttempt> attempts;
	CHECK(AllRoots(Inputs(tmp / "a$LIB", tmp / "$ORIGIN/cache"), attempts).empty());
	REQUIRE(attempts.size() == 2);
	CHECK(attempts[0].reason ==
	      "the extension's directory: the directory's path contains a '$', which the platform loader would rewrite");
	CHECK(attempts[1].reason == "the per-user cache's path contains a '$', which the platform loader would rewrite");
	CHECK(::access((tmp / "$ORIGIN").c_str(), F_OK) != 0);
	auto inputs = Inputs("", "");
	inputs.override_set = true;
	inputs.override_value = tmp / "a$LIB";
	CHECK(AllRoots(inputs, attempts).empty());
	REQUIRE(attempts.size() == 1);
	CHECK(attempts[0].reason == "REDSHIFT_DRIVER_DIR contains a '$', which the platform loader would rewrite");
}

TEST_CASE("Nothing of the per-user cache is made under a directory another user could change or owns",
          "[driver][ancestry]") {
	TempDir tmp;
	if (!PrivateAncestry(tmp)) {
		return;
	}
	// A parent another user can write: refused before anything is made in it.
	MakeDir(tmp / "open", 0777);
	std::vector<DriverAttempt> attempts;
	CHECK(AllRoots(Inputs("", tmp / "open/.cache/duckdb-redshift"), attempts).empty());
	REQUIRE(attempts.size() == 2);
	CHECK(attempts[1].directory == tmp / "open");
	CHECK(attempts[1].reason == "the per-user cache: a parent directory is writable by another user");
	CHECK(::access((tmp / "open/.cache").c_str(), F_OK) != 0);
	// A home that belongs to someone else, as for a root process run with a user's HOME: the
	// caller here is another user, so the temporary directory is that home.
	uid_t user = ::geteuid() + 1;
	if (::geteuid() == 0) {
		// Root trusts its own directories, so the home is given to someone else instead.
		REQUIRE(::chown(tmp.path().c_str(), OTHER_USER, static_cast<gid_t>(-1)) == 0);
		user = 0;
	}
	const auto cache = ResolveCache(Inputs("", tmp / ".cache/duckdb-redshift"), user);
	CHECK_FALSE(cache.skip);
	// The temporary directory, or one above it (a private TMPDIR is the user's too).
	CHECK(tmp.path().compare(0, cache.attempt.directory.size(), cache.attempt.directory) == 0);
	CHECK(cache.attempt.reason == "the per-user cache: a parent directory is owned by another user");
	CHECK(::access((tmp / ".cache").c_str(), F_OK) != 0);
	if (::geteuid() == 0) {
		REQUIRE(::chown(tmp.path().c_str(), 0, static_cast<gid_t>(-1)) == 0);
	}
	// A link on the way is followed, and the directory it leads to is walked.
	MakeDir(tmp / "real", 0700);
	REQUIRE(::symlink((tmp / "real").c_str(), (tmp / "home").c_str()) == 0);
	const auto linked = ResolveCache(Inputs("", tmp / "home/.cache/duckdb-redshift"), ::geteuid());
	CHECK(linked.attempt.reason.empty());
	CHECK(linked.attempt.directory == tmp / "real/.cache/duckdb-redshift");
	REQUIRE(::symlink((tmp / "open").c_str(), (tmp / "to-open").c_str()) == 0);
	const auto to_open = ResolveCache(Inputs("", tmp / "to-open/cache"), ::geteuid());
	CHECK(to_open.attempt.reason == "the per-user cache: a parent directory is writable by another user");
	CHECK(::access((tmp / "open/cache").c_str(), F_OK) != 0);
	// A '..' would resolve the part not yet made through a directory never walked.
	const auto dotdot = ResolveCache(Inputs("", tmp / "missing/../open/cache"), ::geteuid());
	CHECK(dotdot.attempt.reason == "the per-user cache's path contains '..'");
	CHECK(::access((tmp / "open/cache").c_str(), F_OK) != 0);
	// A link another user owns on the way decides nothing (a link's owner can be set only as root).
	if (::geteuid() == 0) {
		MakeDir(tmp / "elsewhere", 0700);
		REQUIRE(::symlink((tmp / "elsewhere").c_str(), (tmp / "theirs").c_str()) == 0);
		REQUIRE(::lchown((tmp / "theirs").c_str(), OTHER_USER, static_cast<gid_t>(-1)) == 0);
		const auto theirs = ResolveCache(Inputs("", tmp / "theirs/cache"), ::geteuid());
		CHECK(theirs.attempt.directory == tmp / "theirs");
		CHECK(theirs.attempt.reason == "the per-user cache: a parent directory is a symbolic link another user owns");
		CHECK(::access((tmp / "elsewhere/cache").c_str(), F_OK) != 0);
		// An existing cache that is another user's directory, which they could swap for a link
		// before it is resolved.
		MakeDir(tmp / "their-dir", 0700);
		REQUIRE(::chown((tmp / "their-dir").c_str(), OTHER_USER, static_cast<gid_t>(-1)) == 0);
		const auto dir = ResolveCache(Inputs("", tmp / "their-dir"), ::geteuid());
		CHECK(dir.attempt.directory == tmp / "their-dir");
		CHECK(dir.attempt.reason == "the per-user cache: the directory is owned by another user");
		// Nor when it is the cache itself, which exists, nor inside the target of a link of root's.
		REQUIRE(::symlink((tmp / "elsewhere").c_str(), (tmp / "their-cache").c_str()) == 0);
		REQUIRE(::lchown((tmp / "their-cache").c_str(), OTHER_USER, static_cast<gid_t>(-1)) == 0);
		const auto whole = ResolveCache(Inputs("", tmp / "their-cache"), ::geteuid());
		CHECK(whole.attempt.directory == tmp / "their-cache");
		CHECK(whole.attempt.reason == "the per-user cache: the directory is a symbolic link another user owns");
		REQUIRE(::symlink((tmp / "theirs").c_str(), (tmp / "mine").c_str()) == 0);
		const auto through = ResolveCache(Inputs("", tmp / "mine/cache"), ::geteuid());
		CHECK(through.attempt.directory == tmp / "theirs");
		CHECK(through.attempt.reason == "the per-user cache: a parent directory is a symbolic link another user owns");
		CHECK(::access((tmp / "elsewhere/cache").c_str(), F_OK) != 0);
	} else {
		WARN("only root can make a directory or link another user owns; those cache cases were not tested");
	}
}

TEST_CASE("A per-user cache under a root-owned sticky /tmp is made, as an ancestor there may be",
          "[driver][ancestry]") {
	struct stat st;
	if (::lstat("/tmp", &st) != 0 || st.st_uid != 0 || (st.st_mode & 01777) != 01777) {
		WARN("/tmp is not a root-owned sticky directory here; not tested");
		return;
	}
	const std::string dir = "/tmp/redshift-driver-sticky-cache-" + std::to_string(::getpid());
	const auto cache = ResolveCache(Inputs("", dir), ::geteuid());
	CHECK(cache.attempt.reason.empty());
	CHECK(cache.attempt.directory == dir);
	CHECK(Mode(dir) == 0700);
	::rmdir(dir.c_str());
}

TEST_CASE("The per-user cache that is the extension's directory is tried once", "[driver][ancestry]") {
	TempDir tmp;
	if (!PrivateAncestry(tmp)) {
		return;
	}
	MakeDir(tmp / "real", 0700);
	MakeDir(tmp / "real/duckdb-redshift", 0700);
	REQUIRE(::symlink((tmp / "real").c_str(), (tmp / "home").c_str()) == 0);
	std::vector<DriverAttempt> attempts;
	CHECK(AllRoots(Inputs(tmp / "real/duckdb-redshift", tmp / "home/duckdb-redshift"), attempts) ==
	      std::vector<std::string> {tmp / "real/duckdb-redshift/.redshift-driver"});
	CHECK(attempts.empty());
	// Refused, it is one attempt, not two.
	REQUIRE(::chmod((tmp / "real/duckdb-redshift").c_str(), 0770) == 0);
	CHECK(AllRoots(Inputs(tmp / "real/duckdb-redshift", tmp / "home/duckdb-redshift"), attempts).empty());
	CHECK(attempts.size() == 1);
}

TEST_CASE("A driver root its owner cannot read and search is refused, a read-only one is not", "[driver][ancestry]") {
	TempDir tmp;
	if (!PrivateAncestry(tmp)) {
		return;
	}
	if (::geteuid() == 0) {
		// Root's own 0700 root, as running the loader as root makes it, is refused for another user
		// with a reason that says who must act.
		REQUIRE(::chmod(tmp.path().c_str(), 0755) == 0);
		MakeDir(tmp / ".redshift-driver", 0700);
		const pid_t child = ::fork();
		REQUIRE(child >= 0);
		if (child == 0) {
			if (::setgid(OTHER_USER) != 0 || ::setuid(OTHER_USER) != 0) {
				::_exit(77);
			}
			std::string root, where, reason;
			const bool refused = !PrepareDriverRoot(tmp.path(), OTHER_USER, root, where, reason);
			::_exit(Expect(refused && where == tmp / ".redshift-driver" &&
			                   reason == "the driver root belongs to root and cannot be read and searched by you; "
			                             "an administrator must make it and the driver's files readable (mode 0755)",
			               reason)
			            ? 0
			            : 1);
		}
		int status = 0;
		REQUIRE(::waitpid(child, &status, 0) == child);
		REQUIRE(WIFEXITED(status));
		CHECK(WEXITSTATUS(status) == 0);
		return;
	}
	MakeDir(tmp / ".redshift-driver", 0700);
	std::string root;
	for (const mode_t mode : {0400, 0200, 0100}) {
		INFO("mode " << std::oct << mode);
		REQUIRE(::chmod((tmp / ".redshift-driver").c_str(), mode) == 0);
		CHECK(PrepareRefusal(tmp.path()) ==
		      Refused {tmp / ".redshift-driver", "the driver root cannot be read and searched: Permission denied"});
	}
	REQUIRE(::chmod((tmp / ".redshift-driver").c_str(), 0500) == 0);
	CHECK(Prepare(tmp.path(), ::geteuid(), root));
	REQUIRE(::chmod((tmp / ".redshift-driver").c_str(), 0700) == 0);
}

TEST_CASE("Concurrent first runs under a umask that takes the owner's bits all find the cache", "[driver][ancestry]") {
	TempDir tmp;
	if (!PrivateAncestry(tmp) || ::geteuid() == 0) {
		return;
	}
	for (const mode_t mask : {0277, 0477, 0777}) {
		INFO("umask " << std::oct << mask);
		const auto home = tmp / ("home-" + std::to_string(mask));
		MakeDir(home, 0700);
		int go[2];
		REQUIRE(::pipe(go) == 0);
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
				::umask(mask);
				DriverRoots roots(Inputs("", home + "/.cache/duckdb-redshift"), ::geteuid());
				std::string root;
				const bool found = roots.Next(root);
				if (!found) {
					for (const auto &attempt : roots.attempts) {
						std::fprintf(stderr, "%s: %s\n", attempt.directory.c_str(), attempt.reason.c_str());
					}
				}
				::_exit(found ? 0 : 1);
			}
			children.push_back(child);
		}
		::close(go[0]);
		::close(go[1]);
		int failed = 0;
		for (const auto child : children) {
			int status = 0;
			REQUIRE(::waitpid(child, &status, 0) == child);
			failed += WIFEXITED(status) && WEXITSTATUS(status) == 0 ? 0 : 1;
		}
		CHECK(failed == 0);
	}
}

TEST_CASE("A driver root on a no-exec mount is refused like a base", "[driver][ancestry]") {
	TempDir tmp;
	if (!PrivateAncestry(tmp)) {
		return;
	}
	MakeDir(tmp / "ext", 0700);
	static std::string noexec_root;
	noexec_root = tmp / "ext/.redshift-driver";
	auto inputs = Inputs(tmp / "ext", tmp / "cache");
	inputs.is_noexec = [](const std::string &dir) {
		return dir == noexec_root;
	};
	std::vector<DriverAttempt> attempts;
	CHECK(AllRoots(inputs, attempts) == std::vector<std::string> {tmp / "cache/.redshift-driver"});
	REQUIRE(attempts.size() == 1);
	CHECK(attempts[0].directory == noexec_root);
	CHECK(attempts[0].reason == "the extension's directory: the driver root is on a file system mounted noexec");
}

TEST_CASE("NO_USABLE_DIRECTORY gives the same advice whatever each base broke", "[driver][ancestry]") {
	// Pure: built from the attempts alone, with no directory touched, so it runs anywhere. The advice
	// is the same for every set of reasons: a base is never refused for its kind of file system, except
	// macOS's volume that ignores ownership, whose reason names its own remedy, and a failure to put the
	// set in place is one rule among others, so no reason earns advice of its own.
	const std::string start = "no usable directory to install the Redshift ODBC driver in (";
	const std::string open = "the extension's directory: a parent directory is writable by another user";
	for (const auto &reasons : std::vector<std::vector<std::string>> {
	         {open},
	         {"the extension's directory: the driver root is on a file system mounted noexec",
	          "the per-user cache is mounted noexec"},
	         {"REDSHIFT_DRIVER_DIR: the driver set cannot be put in place: Input/output error"},
	         {open, "the per-user cache cannot be found: neither XDG_CACHE_HOME nor HOME is an absolute path"},
	     }) {
		std::vector<DriverAttempt> attempts;
		std::string described;
		for (const auto &reason : reasons) {
			attempts.push_back({"/some/dir", reason});
			described += (described.empty() ? "" : "; ") + reason;
		}
		INFO(described);
		CHECK(NoUsableDirectory(attempts).message == start + described + ")" + DIRECTORY_ADVICE);
	}
	// The overflow user's single attempt gets its own advice.
	const std::vector<DriverAttempt> overflow {{"", OverflowUserReason(65534)}};
	CHECK(NoUsableDirectory(overflow).message ==
	      start + OverflowUserReason(65534) + "). Run it as a user that its user namespace maps");
	CHECK(NoUsableDirectory({}).message ==
	      "no usable directory to install the Redshift ODBC driver in" + DIRECTORY_ADVICE);
}

TEST_CASE("The user's own new cache whose mode is still being set is looked at again", "[driver][ancestry]") {
	TempDir tmp;
	if (!PrivateAncestry(tmp) || ::geteuid() == 0) {
		return;
	}
	// As a concurrent first run under a umask without the owner's read bit leaves it, until that run
	// sets its mode.
	MakeDir(tmp / "cache", 0300);
	std::thread fixer([&] {
		std::this_thread::sleep_for(std::chrono::milliseconds(20));
		::chmod((tmp / "cache").c_str(), 0700);
	});
	const auto cache = ResolveCache(Inputs("", tmp / "cache"), ::geteuid());
	fixer.join();
	CHECK(cache.attempt.reason.empty());
	CHECK(cache.attempt.directory == tmp / "cache");
}
