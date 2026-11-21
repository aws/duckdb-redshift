// The macOS loader: the extended-ACL rule on the walk, the driver root and the set; the
// per-user cache; the no-exec and ownership tests on statfs; ClearAcl on a file system without extended ACLs;
// the DYLD_ variables; and, on the embedded driver, that nothing it exports is visible to the process and no
// call meant for it reaches a decoy or unixODBC loaded first, and that the files the loader writes carry no
// quarantine.
#include "driver_info.h"
#include "driver_loader_core.hpp"
#include "embedded_test_helpers.hpp"
#include "loader_payload.hpp"
#include "loader_test.hpp"
#include "sha256.hpp"
#include "temp_dir.hpp"

#include "catch.hpp"

#include <algorithm>
#include <cerrno>
#include <climits>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <cstdlib>
#include <fstream>
#include <string>
#include <vector>

#include <dlfcn.h>
#include <fcntl.h>
#include <mach-o/dyld.h>
#include <mach-o/fat.h>
#include <mach-o/loader.h>
#include <membership.h>
#include <pwd.h>
#include <sys/acl.h>
#include <sys/param.h>
#include <sys/mount.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <sys/xattr.h>
#include <unistd.h>

using namespace duckdb::redshift_driver;
using namespace duckdb::redshift_driver::detail;
using redshift_driver_test::ClosedPort;
using redshift_driver_test::InChild;
using redshift_driver_test::LoadFrom;
using redshift_driver_test::MachOCpuTypes;
using redshift_driver_test::PlatformLoaderCalls;
using redshift_driver_test::PrepareDriverSet;
using redshift_driver_test::ReadWholeFile;
using redshift_driver_test::ScopedEnv;
using redshift_driver_test::TempDir;
using redshift_driver_test::TestPayload;

namespace {

// Adds or removes ACL entries with chmod(1), as an administrator would: "+a <entry>", or "-N" for none.
void Chmod(const std::string &arguments, const std::string &path) {
	const std::string command = "/bin/chmod " + arguments + " '" + path + "'";
	REQUIRE(std::system(command.c_str()) == 0);
}

const char *const WRITABLE_BY_ACL = "is writable by another user or group through its access control list";

// CheckAncestry's reason for `dir` as a base, or empty when it passes.
std::string AncestryReason(const std::string &dir) {
	std::string where, reason;
	return CheckAncestry(dir, AncestryEnd::BASE, ::geteuid(), where, reason) ? std::string() : reason;
}

// The [real] cases load the real driver in a forked child (InChild, embedded_test_helpers.hpp), where the
// system frameworks it links run Objective-C +initialize. Once this process has had a second thread (the threaded
// cases run first), the Objective-C runtime kills such a child with SIGABRT ("+[NSString initialize] may have been
// in progress in another thread when fork() was called"): Apple's rule that a forked child must exec.
// The children here do not exec, so the suite runs itself again, before main, with the runtime's check off; the
// runtime reads OBJC_DISABLE_INITIALIZE_FORK_SAFETY only at start-up. dyld passes an initializer argc and argv.
__attribute__((constructor)) void RunWithoutObjcForkCheck(int, char **argv) {
	if (std::getenv("OBJC_DISABLE_INITIALIZE_FORK_SAFETY") != nullptr || argv == nullptr) {
		return;
	}
	char self[PATH_MAX];
	uint32_t size = sizeof(self);
	if (::_NSGetExecutablePath(self, &size) != 0 || ::setenv("OBJC_DISABLE_INITIALIZE_FORK_SAFETY", "YES", 1) != 0) {
		return;
	}
	::execv(self, argv);
	::unsetenv("OBJC_DISABLE_INITIALIZE_FORK_SAFETY"); // not run again: the [real] children report what happens
}

// Whether the Mach-O file at `path` has code for this process's architecture: a thin file of its CPU type, or a
// universal file with a slice of it. dlopen cannot load any other, as with Homebrew's arm64 unixODBC in an x86_64
// process under Rosetta 2. Its first page holds the header and any slice table.
bool HasThisArchitecture(const std::string &path) {
#if defined(__x86_64__)
	const uint32_t mine = CPU_TYPE_X86_64;
#else
	const uint32_t mine = CPU_TYPE_ARM64;
#endif
	std::ifstream in(path, std::ios::binary);
	unsigned char head[4096];
	in.read(reinterpret_cast<char *>(head), sizeof(head));
	const auto types = redshift_driver_test::MachOCpuTypes(head, static_cast<size_t>(in.gcount()));
	return std::find(types.begin(), types.end(), mine) != types.end();
}

// Runs hdiutil with `arguments`, quietly. perl's alarm bounds each call, so a host where hdiutil hangs fails
// here rather than stalling the suite.
bool Hdiutil(const std::string &arguments) {
	const std::string command =
	    "/usr/bin/perl -e 'alarm 120; exec @ARGV' /usr/bin/hdiutil " + arguments + " > /dev/null 2>&1";
	return std::system(command.c_str()) == 0;
}

// Detaches the disk image attached at `mount` when it goes, before the TempDir holding it is removed (declared
// after it, so destroyed first).
struct DetachImage {
	std::string mount;
	~DetachImage() {
		(void)Hdiutil("detach -quiet -force '" + mount + "'");
	}
};

} // namespace

TEST_CASE("An ACL entry that lets another account change a directory on the walk refuses it", "[driver][macos]") {
	TempDir tmp;
	if (!redshift_driver_test::PrivateAncestry(tmp)) {
		return;
	}
	const auto parent = tmp / "parent";
	const auto base = parent + "/base";
	redshift_driver_test::MakeDir(parent, 0755);
	redshift_driver_test::MakeDir(base, 0700);
	CHECK(AncestryReason(base).empty());
	// Each right the ancestor rule names, for another user and for a group (wheel's members included).
	for (const char *entry :
	     {"nobody allow add_file", "nobody allow add_subdirectory", "nobody allow delete_child", "nobody allow delete",
	      "nobody allow writesecurity", "nobody allow chown", "group:wheel allow add_file",
	      "group:everyone allow delete_child", "nobody allow add_file,directory_inherit,only_inherit"}) {
		INFO(entry);
		Chmod("+a '" + std::string(entry) + "'", parent);
		CHECK(AncestryReason(base) == std::string("a parent directory ") + WRITABLE_BY_ACL);
		Chmod("-N", parent);
		Chmod("+a '" + std::string(entry) + "'", base);
		CHECK(AncestryReason(base) == std::string("the directory ") + WRITABLE_BY_ACL);
		Chmod("-N", base);
	}
	// Read rights, deny entries and the user's own entries pass.
	const struct passwd *me = ::getpwuid(::geteuid());
	REQUIRE(me != nullptr);
	for (const std::string &entry :
	     {std::string("nobody allow list,search,readattr"), std::string("everyone deny delete"),
	      std::string("group:everyone deny add_file"), "user:" + std::string(me->pw_name) + " allow add_file"}) {
		INFO(entry);
		Chmod("+a '" + entry + "'", parent);
		CHECK(AncestryReason(base).empty());
		Chmod("-N", parent);
	}
	// An allow entry for a UUID that names no account (as a deleted account's entries do) is refused too. chmod(1)
	// takes only names, so the entry is made with the ACL calls. The UUID is a fixed random one, of neither form
	// that membership derives from a user or group ID.
	guid_t unknown;
	const unsigned char unknown_bytes[16] = {0x5b, 0x0e, 0x6c, 0x8e, 0x2f, 0x4a, 0x4d, 0x1c,
	                                         0x9a, 0x77, 0x31, 0xd2, 0x6e, 0x0f, 0x88, 0x41};
	std::memcpy(unknown.g_guid, unknown_bytes, sizeof(unknown_bytes));
	id_t unknown_id = 0;
	int unknown_type = -1;
	if (::mbr_uuid_to_id(unknown.g_guid, &unknown_id, &unknown_type) == 0) {
		WARN("the test's random UUID resolves to an account here (type " + std::to_string(unknown_type) +
		     "); the unresolvable UUID was not tested");
		return;
	}
	for (const auto &dir : {parent, base}) {
		INFO(dir);
		acl_t acl = ::acl_init(1);
		REQUIRE(acl != nullptr);
		acl_entry_t entry;
		acl_permset_t permissions;
		const bool made = ::acl_create_entry(&acl, &entry) == 0 && ::acl_set_tag_type(entry, ACL_EXTENDED_ALLOW) == 0 &&
		                  ::acl_set_qualifier(entry, &unknown) == 0 && ::acl_get_permset(entry, &permissions) == 0 &&
		                  ::acl_add_perm(permissions, ACL_ADD_FILE) == 0 &&
		                  ::acl_set_permset(entry, permissions) == 0 &&
		                  ::acl_set_link_np(dir.c_str(), ACL_TYPE_EXTENDED, acl) == 0;
		::acl_free(acl);
		REQUIRE(made);
		CHECK(AncestryReason(base) ==
		      (dir == parent ? "a parent directory " : "the directory ") + std::string(WRITABLE_BY_ACL));
		Chmod("-N", dir);
	}
	CHECK(AncestryReason(base).empty());
}

TEST_CASE("An ACL entry whose tag, rights or qualifier cannot be read counts as granting another account",
          "[driver][macos]") {
	// No entry acl_get_entry gives fails these reads, so stand-ins make each fail in turn, on an entry that passes as
	// it is: the user's own allow entry for add_file.
	guid_t mine;
	REQUIRE(::mbr_uid_to_uuid(::geteuid(), mine.g_guid) == 0);
	acl_t acl = ::acl_init(1);
	REQUIRE(acl != nullptr);
	acl_entry_t entry;
	acl_permset_t permissions;
	const bool made = ::acl_create_entry(&acl, &entry) == 0 && ::acl_set_tag_type(entry, ACL_EXTENDED_ALLOW) == 0 &&
	                  ::acl_set_qualifier(entry, &mine) == 0 && ::acl_get_permset(entry, &permissions) == 0 &&
	                  ::acl_add_perm(permissions, ACL_ADD_FILE) == 0 && ::acl_set_permset(entry, permissions) == 0;
	REQUIRE(made);
	AclEntryReads no_tag, no_rights, no_qualifier;
	no_tag.tag_type = [](acl_entry_t, acl_tag_t *) {
		errno = EINVAL;
		return -1;
	};
	no_rights.permset = [](acl_entry_t, acl_permset_t *) {
		errno = EINVAL;
		return -1;
	};
	no_qualifier.qualifier = [](acl_entry_t) -> void * {
		errno = ENOMEM;
		return nullptr;
	};
	for (const bool any_access : {false, true}) {
		INFO("any_access " << any_access);
		CHECK_FALSE(GrantsToAnother(entry, ::geteuid(), any_access));
		CHECK(GrantsToAnother(entry, ::geteuid(), any_access, no_tag));
		CHECK(GrantsToAnother(entry, ::geteuid(), any_access, no_rights));
		CHECK(GrantsToAnother(entry, ::geteuid(), any_access, no_qualifier));
	}
	::acl_free(acl);
}

TEST_CASE("An inherited ACL entry whose flags cannot be read counts as set on the directory itself",
          "[driver][macos]") {
	// No entry acl_get_entry gives fails these reads either, so stand-ins make each fail, on a directory whose one
	// entry, which breaks the rule, it inherited from its parent: AclRule finds it inherited as it is.
	TempDir tmp;
	const auto parent = tmp / "parent";
	const auto dir = parent + "/dir";
	redshift_driver_test::MakeDir(parent, 0700);
	Chmod("+a 'nobody allow add_file,directory_inherit'", parent);
	REQUIRE(::mkdir(dir.c_str(), 0700) == 0);
	AclEntryReads no_flagset, no_flag;
	no_flagset.flagset = [](void *, acl_flagset_t *) {
		errno = EINVAL;
		return -1;
	};
	// The failed flagset read short-circuits the flag read, so the flagset it left uninitialised is never read.
	// Stand in for that read too, saying "inherited": dropping the flagset check then reads this instead of an
	// uninitialised flagset, so the case below fails every time rather than by chance. `no_flag` needs no such
	// stand-in, as it keeps the real flagset read, which succeeds and leaves the flagset set.
	no_flagset.flag = [](acl_flagset_t, acl_flag_t) {
		return 1;
	};
	no_flag.flag = [](acl_flagset_t, acl_flag_t) {
		errno = EINVAL;
		return -1;
	};
	for (const bool any_access : {false, true}) {
		INFO("any_access " << any_access);
		const auto as_is = AclRule(dir, -1, ::geteuid(), any_access);
		CHECK(as_is.finding == AclFinding::BROKEN);
		CHECK(as_is.inherited);
		for (const AclEntryReads *reads : {&no_flagset, &no_flag}) {
			INFO((reads == &no_flagset ? "the flagset read fails" : "the flag read gives -1"));
			const auto check = AclRule(dir, -1, ::geteuid(), any_access, *reads);
			CHECK(check.finding == AclFinding::BROKEN);
			CHECK_FALSE(check.inherited);
		}
	}
	Chmod("-N", parent);
}

TEST_CASE("A directory on the walk whose ACL cannot be read is refused, and the error points to REDSHIFT_DRIVER_DIR",
          "[driver][macos]") {
	// An entry that denies the user readsecurity makes acl_get_link_np fail with EACCES: what the hidden entries grant
	// cannot be seen, so that is a refusal, never "no ACL". macOS may refuse lstat there too (it can need
	// readsecurity as well), so the walk may stop at lstat, "cannot be examined"; the ACL rule is the refusal where
	// the stat passes and the ACL read does not.
	CHECK(AclReadFailure(ENOENT).finding == AclFinding::NONE);
	CHECK(AclReadFailure(EACCES).finding == AclFinding::UNREADABLE);
	CHECK(AclReadFailure(EACCES).rule == std::string(ACL_UNREADABLE) + ": Permission denied");
	TempDir tmp;
	if (!redshift_driver_test::PrivateAncestry(tmp)) {
		return;
	}
	const auto parent = tmp / "parent";
	const auto base = parent + "/base";
	redshift_driver_test::MakeDir(parent, 0755);
	redshift_driver_test::MakeDir(base, 0700);
	CHECK(AncestryReason(base).empty());
	TestPayload payload(redshift_driver_test::TestInputPath(REDSHIFT_TEST_DRIVER_COMPLETE));
	for (const auto &dir : {parent, base}) {
		INFO(dir);
		const std::string what = dir == parent ? "a parent directory " : "the directory ";
		const std::string unexamined = what + "cannot be examined: Permission denied";
		const std::string unreadable = what + ACL_UNREADABLE + ": Permission denied";
		Chmod("+a 'everyone deny readsecurity'", dir);
		errno = 0;
		acl_t acl = ::acl_get_link_np(dir.c_str(), ACL_TYPE_EXTENDED);
		const int error = errno;
		if (acl != nullptr) {
			::acl_free(acl);
		}
		if (acl != nullptr || error != EACCES) {
			Chmod("-N", dir);
			WARN("the directory's owner reads its ACL despite an entry denying everyone readsecurity (errno " +
			     std::to_string(error) + "); the unreadable ACL was not reached here");
			continue;
		}
		const auto reason = AncestryReason(base);
		CHECK((reason == unexamined || reason == unreadable));
		// Through the loader: no usable directory, the base named by its role and the rule, the directory
		// in the attempts (not in the message), and REDSHIFT_DRIVER_DIR as the way out.
		DriverLoader loader(payload.config);
		auto result = loader.Load(LoadFrom(base));
		REQUIRE_FALSE(result.ok());
		CHECK(result.error.kind == DriverErrorKind::NO_USABLE_DIRECTORY);
		CHECK(result.error.message.find("the extension's directory: " + reason) != std::string::npos);
		CHECK(result.error.message.find(". Set REDSHIFT_DRIVER_DIR to the absolute path of an existing directory") !=
		      std::string::npos);
		CHECK(result.error.message.find(tmp.path()) == std::string::npos);
		REQUIRE_FALSE(result.error.attempts.empty());
		CHECK(result.error.attempts[0].directory == dir);
		CHECK(::access((base + "/.redshift-driver").c_str(), F_OK) != 0);
		Chmod("-N", dir);
	}
	CHECK(AncestryReason(base).empty());
}

TEST_CASE("The driver root must be owner-only through its ACL too; an ancestor may grant reading", "[driver][macos]") {
	TempDir tmp;
	if (!redshift_driver_test::PrivateAncestry(tmp)) {
		return;
	}
	const auto base = tmp / "base";
	redshift_driver_test::MakeDir(base, 0700);
	std::string root, where, reason;
	REQUIRE(PrepareDriverRoot(base, ::geteuid(), root, where, reason));
	Chmod("+a 'nobody allow list,search'", base);
	CHECK(PrepareDriverRoot(base, ::geteuid(), root, where, reason));
	Chmod("-N", base);
	// Each right the ancestor rule names, alone: the walk's own ACL check on the root refuses it, before the
	// owner-only rule.
	for (const char *right : {"add_file", "add_subdirectory", "delete_child", "delete", "writesecurity", "chown"}) {
		INFO(right);
		Chmod("+a 'nobody allow " + std::string(right) + "'", root);
		reason.clear();
		CHECK_FALSE(PrepareDriverRoot(base, ::geteuid(), root, where, reason));
		CHECK(reason == std::string("the driver root ") + WRITABLE_BY_ACL);
		CHECK(where == root);
		Chmod("-N", root);
	}
	// Each right of ALL_RIGHTS that WRITE_RIGHTS lacks, alone: the owner-only rule refuses it.
	for (const char *right :
	     {"list", "search", "readattr", "writeattr", "readextattr", "writeextattr", "readsecurity"}) {
		INFO(right);
		Chmod("+a 'nobody allow " + std::string(right) + "'", root);
		reason.clear();
		CHECK_FALSE(PrepareDriverRoot(base, ::geteuid(), root, where, reason));
		CHECK(reason == "the driver root grants access to another user or group through its access control list (it "
		                "must be owner-only)");
		CHECK(where == root);
		Chmod("-N", root);
	}
	CHECK(PrepareDriverRoot(base, ::geteuid(), root, where, reason));
	// The user's own entry is not another account's.
	const struct passwd *me = ::getpwuid(::geteuid());
	REQUIRE(me != nullptr);
	Chmod("+a 'user:" + std::string(me->pw_name) + " allow list,search,add_file'", root);
	CHECK(PrepareDriverRoot(base, ::geteuid(), root, where, reason));
	Chmod("-N", root);
	// A root the loader makes under a base whose ACL gives a new directory an inheritable read entry does not
	// keep it: the loader clears what the root inherited.
	TempDir other;
	const auto inheriting = other / "base";
	redshift_driver_test::MakeDir(inheriting, 0700);
	Chmod("+a 'nobody allow list,search,directory_inherit'", inheriting);
	reason.clear();
	{
		const bool prepared = PrepareDriverRoot(inheriting, ::geteuid(), root, where, reason);
		INFO(reason);
		CHECK(prepared);
	}
	Chmod("-N", inheriting);
}

TEST_CASE("A new driver root whose maker has not yet cleared the entries it inherited is looked at again; an entry "
          "set on the root is refused at once",
          "[driver][macos]") {
	// A base whose ACL gives a new directory an inheritable read entry: the root a concurrent first run has just made
	// holds it, marked inherited, until that run clears it (MakePrivateDirectory). This process makes the root as
	// that run does and leaves the entry; a stand-in answers as AclRule does and, after its first refusal at the
	// root, clears the root's ACL, as the other run then does. Root's own driver root is exempt from that rule.
	TempDir tmp;
	if (!redshift_driver_test::PrivateAncestry(tmp) || ::geteuid() == 0) {
		return;
	}
	const auto base = tmp / "base";
	redshift_driver_test::MakeDir(base, 0700);
	Chmod("+a 'nobody allow list,search,directory_inherit'", base);
	static std::string root;
	static int refusals;
	root = base + "/" + DRIVER_ROOT_NAME;
	auto inputs = redshift_driver_test::Inputs("", "");
	inputs.override_set = true;
	inputs.override_value = base;
	inputs.acl_rule = [](const std::string &path, int fd, uid_t user, bool any_access) {
		auto check = AclRule(path, fd, user, any_access);
		if (path == root && check.finding != AclFinding::NONE && refusals++ == 0) {
			const std::string clear = "/bin/chmod -N '" + root + "'";
			if (std::system(clear.c_str()) != 0) {
				refusals = 100; // fails the CHECK below
			}
		}
		return check;
	};
	const auto first_run = [&](bool expect_used) {
		refusals = 0;
		DriverRoots roots(inputs, ::geteuid());
		std::string found;
		const bool used = roots.Next(found);
		CHECK(refusals == 1);
		CHECK(used == expect_used);
		if (used) {
			CHECK(found == root);
		} else {
			REQUIRE(roots.attempts.size() == 1);
			CHECK(roots.attempts[0].directory == root);
			CHECK(roots.attempts[0].reason == "REDSHIFT_DRIVER_DIR: the driver root grants access to another user or "
			                                  "group through its access control list (it must be owner-only)");
		}
	};
	// Only inherited entries: AclRule says so, and the root is looked at again, after the other run cleared them.
	REQUIRE(::mkdir(root.c_str(), 0700) == 0);
	{
		const auto check = AclRule(root, -1, ::geteuid(), true);
		CHECK(check.finding == AclFinding::BROKEN);
		CHECK(check.inherited);
	}
	first_run(true);
	// An entry set on the root itself, alone or beside an inherited one: refused at once, though a second look would
	// find the ACL cleared.
	Chmod("+a 'nobody allow readattr'", root);
	CHECK_FALSE(AclRule(root, -1, ::geteuid(), true).inherited);
	first_run(false);
	REQUIRE(::rmdir(root.c_str()) == 0);
	REQUIRE(::mkdir(root.c_str(), 0700) == 0);
	Chmod("+a 'nobody allow readattr'", root);
	{
		const auto check = AclRule(root, -1, ::geteuid(), true);
		CHECK(check.finding == AclFinding::BROKEN);
		CHECK_FALSE(check.inherited);
	}
	first_run(false);
	Chmod("-N", base);
}

TEST_CASE("A set file another account may write through its ACL makes the set invalid, and the install repairs it",
          "[driver][macos]") {
	TempDir tmp;
	if (!redshift_driver_test::PrivateAncestry(tmp)) {
		return;
	}
	TestPayload payload(redshift_driver_test::TestInputPath(REDSHIFT_TEST_DRIVER_COMPLETE));
	DriverError error;
	std::string set_dir;
	REQUIRE(PrepareDriverSet(redshift_driver_test::Inputs(tmp.path(), ""), payload.config.set, ::geteuid(), set_dir,
	                         error));
	const auto notice = set_dir + "/NOTICE";
	std::string where, reason;
	for (const char *entry : {"nobody allow write", "nobody allow delete"}) {
		INFO(entry);
		Chmod("+a '" + std::string(entry) + "'", notice);
		CHECK(CheckInstalledSet(set_dir, payload.config.set.Manifest(), ::geteuid(), where, reason) ==
		      SetState::INVALID);
		CHECK(reason == std::string("a file of the driver set ") + WRITABLE_BY_ACL);
		CHECK(where == notice);
		Chmod("-N", notice);
	}
	// The set directory itself.
	Chmod("+a 'nobody allow add_file'", set_dir);
	CHECK(CheckInstalledSet(set_dir, payload.config.set.Manifest(), ::geteuid(), where, reason) == SetState::INVALID);
	CHECK(reason == std::string("the driver set ") + WRITABLE_BY_ACL);
	// The install sets it aside and writes a clean copy.
	REQUIRE(PrepareDriverSet(redshift_driver_test::Inputs(tmp.path(), ""), payload.config.set, ::geteuid(), set_dir,
	                         error));
	CHECK(CheckInstalledSet(set_dir, payload.config.set.Manifest(), ::geteuid(), where, reason) == SetState::VALID);
}

TEST_CASE("A set file whose ACL cannot be read leaves the set unchecked: the base fails, and nothing is repaired",
          "[driver][macos]") {
	// An unreadable ACL is no evidence that the set is bad, so it is not set aside and rewritten (that would be
	// INVALID); it is UNCHECKED, which ends the base.
	TempDir tmp;
	if (!redshift_driver_test::PrivateAncestry(tmp)) {
		return;
	}
	TestPayload payload(redshift_driver_test::TestInputPath(REDSHIFT_TEST_DRIVER_COMPLETE));
	DriverError error;
	std::string set_dir;
	const auto inputs = redshift_driver_test::Inputs(tmp.path(), "");
	REQUIRE(PrepareDriverSet(inputs, payload.config.set, ::geteuid(), set_dir, error));
	const auto notice = set_dir + "/NOTICE";
	Chmod("+a 'everyone deny readsecurity'", notice);
	errno = 0;
	acl_t acl = ::acl_get_link_np(notice.c_str(), ACL_TYPE_EXTENDED);
	const int acl_error = errno;
	if (acl != nullptr || acl_error != EACCES) {
		if (acl != nullptr) {
			::acl_free(acl);
		}
		Chmod("-N", notice);
		WARN("the owner reads the set file's ACL despite an entry denying everyone readsecurity (errno " +
		     std::to_string(acl_error) + "); the unreadable ACL was not reached here");
		return;
	}
	std::string where, reason;
	const auto state = CheckInstalledSet(set_dir, payload.config.set.Manifest(), ::geteuid(), where, reason);
	CHECK(state == SetState::UNCHECKED);
	CHECK(where == notice);
	if (reason == std::string("a file of the driver set ") + ACL_UNREADABLE + ": Permission denied") {
		// Not repaired: the base fails, and the file keeps its entry.
		CHECK_FALSE(PrepareDriverSet(inputs, payload.config.set, ::geteuid(), set_dir, error));
		errno = 0;
		acl = ::acl_get_link_np(notice.c_str(), ACL_TYPE_EXTENDED);
		CHECK((acl == nullptr && errno == EACCES));
		if (acl != nullptr) {
			::acl_free(acl);
		}
	} else {
		// macOS may refuse the file's stat first, as it does a directory's on the walk: still UNCHECKED.
		WARN("the set file was refused before its ACL was read (\"" + reason + "\"); the ACL rule was not reached");
	}
	Chmod("-N", notice);
}

TEST_CASE("The per-user cache is ~/Library/Caches/duckdb-redshift, from HOME only, and root has none; "
          "XDG_CACHE_HOME is not read",
          "[driver][macos]") {
	ScopedEnv xdg("XDG_CACHE_HOME", "/x/cache");
	const uid_t user = 501; // any user but root
	{
		ScopedEnv home("HOME", "/h");
		CHECK(PerUserCacheBaseFor(user) == "/h/Library/Caches/duckdb-redshift");
		// Root, as under sudo, which keeps the caller's HOME, has none, whatever HOME is.
		CHECK(PerUserCacheBaseFor(0).empty());
		// The hook asks for the process's own user.
		CHECK(PerUserCacheBase() == PerUserCacheBaseFor(::geteuid()));
	}
	for (const char *home : {"relative", "", static_cast<const char *>(nullptr)}) {
		ScopedEnv set("HOME", home);
		CHECK(PerUserCacheBaseFor(user).empty());
		CHECK(PerUserCacheBaseFor(0).empty());
		CHECK(PerUserCacheBase().empty());
	}
	// The shared code's reason for none names both conditions, in this platform's words, and no Linux variable.
	CHECK(std::string(PerUserCacheRule()) ==
	      "HOME is not an absolute path, or the user is root, whose HOME may be another user's (sudo keeps the "
	      "caller's)");
}

TEST_CASE("statfs answers the no-exec and ownership tests for every local mount", "[driver][macos]") {
	struct statfs *mounts = nullptr;
	const int count = ::getmntinfo(&mounts, MNT_NOWAIT);
	REQUIRE(count > 0);
	for (int i = 0; i < count; i++) {
		const std::string point = mounts[i].f_mntonname;
		const std::string type = mounts[i].f_fstypename;
		INFO(point << " (" << type << ")");
		if (type == "autofs" || (mounts[i].f_flags & MNT_LOCAL) == 0) {
			continue; // a trigger point may mount; a network mount may not answer
		}
		CHECK(IsNoExecMount(point) == ((mounts[i].f_flags & MNT_NOEXEC) != 0));
		CHECK(NoExecState(point) == ((mounts[i].f_flags & MNT_NOEXEC) != 0 ? 1 : 0));
		CHECK(VolumeRule(point).empty() == ((mounts[i].f_flags & MNT_IGNORE_OWNERSHIP) == 0));
	}
	CHECK(NoExecState("/nonexistent/directory") == -1);
	CHECK(VolumeRule("/nonexistent/directory") ==
	      "the driver root's volume cannot be examined: " + std::string(std::strerror(ENOENT)));
	TempDir tmp;
	CHECK(VolumeRule(tmp.path()).empty());
	// The rule on flags: ignored ownership is refused whatever else the volume is (every base meets it,
	// REDSHIFT_DRIVER_DIR too); a network or FUSE volume is not.
	const std::string ignored =
	    "the driver root is on a volume that ignores ownership, where every local user can change it (turn off "
	    "\"Ignore ownership on this volume\" in its Get Info window, or set REDSHIFT_DRIVER_DIR on another volume)";
	for (const uint32_t flags : {uint32_t {MNT_LOCAL | MNT_IGNORE_OWNERSHIP}, uint32_t {MNT_IGNORE_OWNERSHIP},
	                             uint32_t {MNT_LOCAL | MNT_NOEXEC | MNT_IGNORE_OWNERSHIP}}) {
		CHECK(VolumeRuleFromFlags(flags) == ignored);
	}
	for (const uint32_t flags : {uint32_t {MNT_LOCAL}, uint32_t {0}, uint32_t {MNT_LOCAL | MNT_NOEXEC}}) {
		CHECK(VolumeRuleFromFlags(flags).empty());
	}
}

TEST_CASE("A base on a volume attached with ownership ignored is refused before a driver root is made there",
          "[driver][macos]") {
	// A disk image attached with `hdiutil attach -owners off` has MNT_IGNORE_OWNERSHIP, as an external drive with
	// "Ignore ownership on this volume" does. Where the image cannot be made or attached (no DiskArbitration
	// session), or the volume does not report the flag, the case warns and stops.
	TempDir tmp;
	if (!redshift_driver_test::PrivateAncestry(tmp)) {
		return;
	}
	const auto image = tmp / "volume.dmg";
	const auto mount = tmp / "mount";
	redshift_driver_test::MakeDir(mount, 0700);
	if (!Hdiutil("create -quiet -size 8m -fs HFS+ -volname redshift-test -o '" + image + "'")) {
		WARN("hdiutil could not create a disk image here; a volume that ignores ownership was not tried");
		return;
	}
	if (!Hdiutil("attach -quiet -nobrowse -noautoopen -owners off -mountpoint '" + mount + "' '" + image + "'")) {
		WARN("hdiutil could not attach a disk image here; a volume that ignores ownership was not tried");
		return;
	}
	DetachImage detach {mount};
	struct statfs volume;
	REQUIRE(::statfs(mount.c_str(), &volume) == 0);
	if ((volume.f_flags & MNT_IGNORE_OWNERSHIP) == 0) {
		WARN("the attached image does not report MNT_IGNORE_OWNERSHIP here; the volume rule was not reached");
		return;
	}
	const auto rule = VolumeRuleFromFlags(MNT_IGNORE_OWNERSHIP);
	// The volume's root as the walk wants it: anyone may change it there, so this needs no privilege.
	::chmod(mount.c_str(), 0755);
	const auto base = mount + "/base";
	REQUIRE(::mkdir(base.c_str(), 0700) == 0);
	CHECK(VolumeRule(base) == rule);
	std::string root, where, reason;
	CHECK_FALSE(PrepareDriverRoot(base, ::geteuid(), root, where, reason));
	CHECK(::access((base + "/.redshift-driver").c_str(), F_OK) != 0);
	if (reason != rule) {
		// Still refused, but by the walk, which met an owner or mode it refuses first.
		WARN("the walk refused the volume before its rule (\"" + reason + "\"); the volume rule was not reached");
		return;
	}
	CHECK(where == base);
	// Through the loader, as REDSHIFT_DRIVER_DIR, which has no fallback: no usable directory, with the rule.
	TestPayload payload(redshift_driver_test::TestInputPath(REDSHIFT_TEST_DRIVER_COMPLETE));
	DriverLoader loader(payload.config);
	auto inputs = LoadFrom(tmp.path());
	inputs.bases.override_set = true;
	inputs.bases.override_value = base;
	const int calls = PlatformLoaderCalls();
	auto result = loader.Load(inputs);
	REQUIRE_FALSE(result.ok());
	CHECK(result.error.kind == DriverErrorKind::NO_USABLE_DIRECTORY);
	CHECK(result.error.message.find("REDSHIFT_DRIVER_DIR: " + rule) != std::string::npos);
	CHECK(PlatformLoaderCalls() == calls);
	CHECK(::access((base + "/.redshift-driver").c_str(), F_OK) != 0);
}

TEST_CASE("A new directory on a file system without extended ACLs counts as cleared", "[driver][macos]") {
	// ClearAcl's own call: acl_set_fd_np fails with ENOTSUP or EOPNOTSUPP where the file system has no extended
	// ACLs, which leaves none to inherit, so MakePrivateDirectory must not refuse such a directory for it. An MS-DOS
	// (FAT) disk image is one. It is called directly, not through PrepareDriverRoot, whose volume rule may refuse
	// the volume first (a FAT volume ignores ownership). Where the image cannot be made or attached, or the file
	// system takes the call, the case warns and stops. Nothing here walks the ancestry, so the temporary
	// directory need not be private.
	TempDir tmp;
	const auto image = tmp / "fat.dmg";
	const auto mount = tmp / "mount";
	redshift_driver_test::MakeDir(mount, 0700);
	if (!Hdiutil("create -quiet -size 8m -fs MS-DOS -volname REDSHIFT -o '" + image + "'")) {
		WARN("hdiutil could not create an MS-DOS disk image here; ClearAcl's call there was not tried");
		return;
	}
	if (!Hdiutil("attach -quiet -nobrowse -noautoopen -mountpoint '" + mount + "' '" + image + "'")) {
		WARN("hdiutil could not attach a disk image here; ClearAcl's call there was not tried");
		return;
	}
	DetachImage detach {mount};
	const auto dir = mount + "/made";
	REQUIRE(::mkdir(dir.c_str(), 0700) == 0);
	const UniqueFd opened(::open(dir.c_str(), O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC));
	REQUIRE(opened.get() >= 0);
	acl_t empty = ::acl_init(0);
	REQUIRE(empty != nullptr);
	const int set = ::acl_set_fd_np(opened.get(), empty, ACL_TYPE_EXTENDED);
	const int error = errno;
	::acl_free(empty);
	if (set == 0 || (error != ENOTSUP && error != EOPNOTSUPP)) {
		WARN("this MS-DOS volume " + (set == 0 ? std::string("takes an extended ACL") : std::strerror(error)) +
		     "; ClearAcl's call there was not tried");
		return;
	}
	CHECK(ClearAcl(opened.get()));
	// Through the loader's own maker of private directories, which calls it after setting the mode.
	std::string reason;
	if (!MakePrivateDirectory(mount + "/private", reason)) {
		CHECK(reason.find("access control list") == std::string::npos);
		WARN("MakePrivateDirectory refused the MS-DOS volume (\"" + reason + "\")");
	}
}

TEST_CASE("A set DYLD_ search variable refuses the load before the platform loader call, in the loader's words",
          "[driver][macos]") {
	TempDir tmp;
	TestPayload payload(redshift_driver_test::TestInputPath(REDSHIFT_TEST_DRIVER_COMPLETE));
	DriverLoader loader(payload.config);
	const int calls = PlatformLoaderCalls();
	// The names themselves, so a wrong or missing one in PlatformLoaderVariables fails here.
	const std::vector<std::string> refused {
	    "DYLD_LIBRARY_PATH",           "DYLD_FRAMEWORK_PATH",           "DYLD_IMAGE_SUFFIX",
	    "DYLD_VERSIONED_LIBRARY_PATH", "DYLD_VERSIONED_FRAMEWORK_PATH", "DYLD_FORCE_FLAT_NAMESPACE"};
	CHECK(PlatformLoaderVariables() == refused);
	for (const auto &name : refused) {
		INFO(name);
		ScopedEnv set(name.c_str(), "/planted");
		auto result = loader.Load(LoadFrom(tmp.path()));
		REQUIRE_FALSE(result.ok());
		CHECK(result.error.kind == DriverErrorKind::REFUSED_ENVIRONMENT);
		CHECK(result.error.message == name + " is set; the system's library loader reads it, and it can make the "
		                                     "loader load another library in place of the Redshift driver. Unset it "
		                                     "and start the program again to connect.");
		CHECK(result.error.message.find("/planted") == std::string::npos);
		CHECK(PlatformLoaderCalls() == calls);
		CHECK(::access((tmp / ".redshift-driver").c_str(), F_OK) != 0);
	}
	CHECK(loader.Load(LoadFrom(tmp.path())).ok());
}

TEST_CASE("A library dyld put in the driver's place through DYLD_LIBRARY_PATH is found after the load",
          "[driver][macos]") {
	// dyld reads DYLD_LIBRARY_PATH when the process starts; a host that unsets it afterwards passes the refusal,
	// and dyld still looks for the driver's file name in it. The check after the load must catch that. Run in a
	// fresh exec of this suite started with the variable, which then unsets it and loads.
	if (const char *base = std::getenv("REDSHIFT_TEST_DYLD_BASE")) {
		::unsetenv("DYLD_LIBRARY_PATH");
		TestPayload payload(redshift_driver_test::TestInputPath(REDSHIFT_TEST_DRIVER_COMPLETE));
		DriverLoader loader(payload.config);
		auto result = loader.Load(LoadFrom(base));
		if (result.ok()) {
			// Loaded: the checked file, or the decoy with the check after the load missing.
			Dl_info where;
			const bool decoy = ::dladdr(reinterpret_cast<void *>(result.driver->api.DriverConnect), &where) != 0 &&
			                   std::string(where.dli_fname).find(std::getenv("REDSHIFT_TEST_DYLD_PLANTED")) == 0;
			::_exit(decoy ? 1 : 3); // 3: dyld did not substitute, so there was nothing to catch
		}
		::_exit(result.error.kind == DriverErrorKind::LOAD_FAILED &&
		                result.error.message.find("loaded a file other than the one that was checked (start the "
		                                          "program without DYLD_ variables set to connect)") !=
		                    std::string::npos
		            ? 0
		            : 1);
	}
	TempDir tmp, planted;
	// The decoy, under the name the payload's driver file has in the set.
	{
		TestPayload payload(redshift_driver_test::TestInputPath(REDSHIFT_TEST_DRIVER_COMPLETE));
		std::ofstream(planted / payload.config.set.files[0].name, std::ios::binary)
		    << ReadWholeFile(redshift_driver_test::TestInputPath(REDSHIFT_TEST_DRIVER_DECOY));
	}
	auto self = redshift_driver_test::ExecutablePath();
	std::fflush(nullptr);
	const pid_t child = ::fork();
	REQUIRE(child >= 0);
	if (child == 0) {
		::setenv("DYLD_LIBRARY_PATH", planted.path().c_str(), 1);
		::setenv("REDSHIFT_TEST_DYLD_BASE", tmp.path().c_str(), 1);
		::setenv("REDSHIFT_TEST_DYLD_PLANTED", planted.path().c_str(), 1);
		char warn[] = "-w", no_tests[] = "NoTests", name[] = "A library dyld put in the driver*";
		char *argv[] = {&self[0], warn, no_tests, name, nullptr};
		::execv(self.c_str(), argv);
		::_exit(127);
	}
	int status = 0;
	REQUIRE(::waitpid(child, &status, 0) == child);
	REQUIRE(WIFEXITED(status));
	if (WEXITSTATUS(status) == 3) {
		WARN("dyld did not take the planted library here; the check after the load was not reached");
		return;
	}
	CHECK(WEXITSTATUS(status) == 0);
}

TEST_CASE("The macOS hooks answer from the system", "[driver][macos]") {
	// The check after the load compares the file dyld names for an image: here the suite's own executable
	// (the system's libraries are in the shared cache, with no file at their names).
	const auto *own = reinterpret_cast<const void *>(&PerUserCacheBase);
	struct stat self, other;
	REQUIRE(::stat(redshift_driver_test::ExecutablePath().c_str(), &self) == 0);
	REQUIRE(::stat("/bin/sh", &other) == 0);
	Dl_info info;
	REQUIRE(::dladdr(own, &info) != 0);
	if (info.dli_fname != nullptr && info.dli_fname[0] == '/') {
		CHECK(LoadedFileIsChecked(own, self));
		CHECK_FALSE(LoadedFileIsChecked(own, other));
	} else {
		// If dyld reports a relative name for the suite: refused, as any image dyld names no absolute file for.
		CHECK_FALSE(LoadedFileIsChecked(own, self));
		WARN("the suite was started by a relative path; its own identity was not compared");
	}
	// An address in no image (the heap): refused, since there is no file to compare.
	std::vector<char> heap(16);
	CHECK_FALSE(LoadedFileIsChecked(heap.data(), self));
	bool mapped = true;
	CHECK(MappedFilePath(own, &mapped).empty());
	CHECK_FALSE(mapped);
	CHECK(DescriptorPath(0).empty());
}

TEST_CASE("dyld's refusal names a prerequisite by its install name's last component", "[driver][macos]") {
	const std::vector<std::string> prerequisites {"libexample.1.dylib"};
	CHECK(MissingPrerequisite("dlopen(/x/librsodbc64.dylib, 0x0006): Library not loaded: /opt/lib/libexample.1.dylib\n"
	                          "  Referenced from: /x/librsodbc64.dylib",
	                          prerequisites) == &prerequisites[0]);
	CHECK(MissingPrerequisite("dlopen(/x/a.dylib, 0x0006): Library not loaded: @rpath/libother.dylib", prerequisites) ==
	      nullptr);
	CHECK(MissingPrerequisite("dlopen(/x/a.dylib, 0x0006): tried: '/x/a.dylib' (not a mach-o file)", prerequisites) ==
	      nullptr);
	CHECK(MissingPrerequisite(nullptr, prerequisites) == nullptr);
}

TEST_CASE("The Mach-O reader gives every slice's CPU type, of either universal format, and none for a slice table "
          "or header cut short or a file that is not Mach-O",
          "[driver][macos]") {
	// A universal file's header and two slices, x86_64 then arm64, each CPU type big-endian at its slice's start.
	std::vector<unsigned char> fat(sizeof(fat_header) + 2 * sizeof(fat_arch), 0);
	const auto put = [](std::vector<unsigned char> &bytes, std::size_t at, uint32_t value) {
		for (std::size_t i = 0; i < 4; i++) {
			bytes[at + i] = static_cast<unsigned char>(value >> (24 - 8 * i));
		}
	};
	put(fat, 0, FAT_MAGIC);
	put(fat, 4, 2);
	put(fat, sizeof(fat_header), CPU_TYPE_X86_64);
	put(fat, sizeof(fat_header) + sizeof(fat_arch), CPU_TYPE_ARM64);
	const std::vector<uint32_t> both {static_cast<uint32_t>(CPU_TYPE_X86_64), static_cast<uint32_t>(CPU_TYPE_ARM64)};
	CHECK(MachOCpuTypes(fat.data(), fat.size()) == both);
	// Exactly the bytes the two CPU types need, the last slice's description cut off just after its type: the
	// reader needs no more than that, so both types are still read.
	CHECK(MachOCpuTypes(fat.data(), sizeof(fat_header) + sizeof(fat_arch) + sizeof(uint32_t)) == both);
	// The second slice's CPU type a byte short, and no slice table at all.
	CHECK(MachOCpuTypes(fat.data(), sizeof(fat_header) + sizeof(fat_arch) + sizeof(uint32_t) - 1).empty());
	CHECK(MachOCpuTypes(fat.data(), sizeof(fat_header)).empty());
	// An exact copy, a byte shorter than a universal file's header: the header-size guard is what keeps the
	// slice-count read inside `size`, as the count's last byte lies past this copy's end.
	const std::vector<unsigned char> short_header(fat.begin(), fat.begin() + sizeof(fat_header) - 1);
	CHECK(MachOCpuTypes(short_header.data(), short_header.size()).empty());
	// A 64-bit universal file, the same two slices described by the wider `fat_arch_64`: read with the 32-bit
	// width the second type would come from the low half of the first description's `size` field, not from the
	// second slice.
	std::vector<unsigned char> fat64(sizeof(fat_header) + 2 * sizeof(fat_arch_64), 0);
	put(fat64, 0, FAT_MAGIC_64);
	put(fat64, 4, 2);
	put(fat64, sizeof(fat_header), CPU_TYPE_X86_64);
	put(fat64, sizeof(fat_header) + sizeof(fat_arch_64), CPU_TYPE_ARM64);
	CHECK(MachOCpuTypes(fat64.data(), fat64.size()) == both);
	CHECK(MachOCpuTypes(fat64.data(), sizeof(fat_header) + sizeof(fat_arch_64) + sizeof(uint32_t) - 1).empty());
	// A thin 64-bit file's header, and a byte short of it.
	mach_header_64 header {};
	header.magic = MH_MAGIC_64;
	header.cputype = CPU_TYPE_ARM64;
	std::vector<unsigned char> thin(sizeof(header));
	std::memcpy(thin.data(), &header, sizeof(header));
	CHECK(MachOCpuTypes(thin.data(), thin.size()) == std::vector<uint32_t> {static_cast<uint32_t>(CPU_TYPE_ARM64)});
	CHECK(MachOCpuTypes(thin.data(), thin.size() - 1).empty());
	// Not Mach-O: an ELF file's first bytes.
	std::vector<unsigned char> elf(sizeof(header), 0);
	const unsigned char ident[] = {0x7f, 'E', 'L', 'F', 2, 1, 1};
	std::memcpy(elf.data(), ident, sizeof(ident));
	CHECK(MachOCpuTypes(elf.data(), elf.size()).empty());
}

TEST_CASE("The embedded driver is invisible to the process, and keeps its calls from a decoy loaded first",
          "[driver][macos][real]") {
	// The driver is loaded RTLD_LOCAL in its own two-level namespace, so dlsym(RTLD_DEFAULT) finds
	// none of its entry points, and a decoy loaded first with the same names, RTLD_GLOBAL, gets none of the
	// driver's own calls (it logs every call it gets), while the driver answers a closed port with its own error.
	// With Homebrew's unixODBC loaded first, where it is installed for this process's architecture, the process
	// still sees unixODBC's names and the driver's entry points are its own; unixODBC logs nothing, so its calls are
	// not counted.
	TempDir tmp;
	const int port = ClosedPort();
	const auto decoy = redshift_driver_test::TestInputPath(REDSHIFT_TEST_DRIVER_DECOY);
	// The control for the architecture test below: the decoy is a thin file built for this process.
	REQUIRE(HasThisArchitecture(decoy));
	for (const std::string &first :
	     {decoy, std::string("/opt/homebrew/lib/libodbc.2.dylib"), std::string("/usr/local/lib/libodbc.2.dylib")}) {
		INFO(first);
		if (::access(first.c_str(), R_OK) != 0) {
			WARN(first + " is not installed here; not tried");
			continue;
		}
		if (!HasThisArchitecture(first)) {
			WARN(first + " has no slice for this process's architecture; not tried");
			continue;
		}
		TempDir dir;
		ScopedEnv calls("REDSHIFT_TEST_DECOY_LOG", (tmp / "calls.log").c_str());
		const int status = InChild(dir.path(), [&] {
			void *before = ::dlopen(first.c_str(), RTLD_NOW | RTLD_GLOBAL);
			if (before == nullptr) {
				return 10;
			}
			void *preloaded = ::dlsym(RTLD_DEFAULT, "SQLDriverConnect");
			if (preloaded == nullptr) {
				return 15; // nothing visible to compare with: the library did not export it globally
			}
			// The control: a call made to the decoy is logged, so a log with nothing else in it shows that none
			// of the driver's calls reached it.
			if (first == decoy) {
				auto *alloc = reinterpret_cast<SQLRETURN (*)(SQLSMALLINT, SQLHANDLE, SQLHANDLE *)>(
				    ::dlsym(before, "SQLAllocHandle"));
				SQLHANDLE ignored = nullptr;
				if (alloc == nullptr) {
					return 14;
				}
				alloc(SQL_HANDLE_ENV, SQL_NULL_HANDLE, &ignored);
			}
			auto result = LoadEmbeddedDriver();
			if (!result.ok()) {
				return 11;
			}
			// What the process sees is what was there before; the driver's own is elsewhere.
			if (::dlsym(RTLD_DEFAULT, "SQLDriverConnect") != preloaded ||
			    reinterpret_cast<void *>(result.driver->api.DriverConnect) == preloaded) {
				return 12;
			}
			// The closed-port smoke: the driver answers with its own error.
			std::string answer;
			if (!redshift_driver_test::ClosedPortSmoke(*result.driver, port, answer)) {
				std::fprintf(stderr, "smoke: %s\n", answer.c_str());
				return 13;
			}
			return 0;
		});
		CHECK(status == 0);
		// Only the control's call reached the decoy.
		if (first == decoy) {
			CHECK(ReadWholeFile(tmp / "calls.log") == "SQLAllocHandle\n");
		}
	}
	// Without anything loaded first, none of the driver's names is visible at all.
	TempDir dir;
	const int status = InChild(dir.path(), [&] {
		auto result = LoadEmbeddedDriver();
		if (!result.ok()) {
			return 10;
		}
		for (const char *name : {"SQLDriverConnect", "SQLAllocHandle", "SQLGetDiagRec"}) {
			if (::dlsym(RTLD_DEFAULT, name) != nullptr) {
				return 11;
			}
		}
		return 0;
	});
	CHECK(status == 0);
}

TEST_CASE("The files the loader writes carry no quarantine attribute", "[driver][macos][real]") {
	// The test executable does not opt in to quarantine (LSFileQuarantineEnabled), as a host that does not
	// would not, so this shows what such a host gets; a host that opts in is not covered here.
	TempDir tmp;
	const int status = InChild(tmp.path(), [&] { return LoadEmbeddedDriver().ok() ? 0 : 10; });
	REQUIRE(status == 0);
	const auto set_dir = tmp / ".redshift-driver/" REDSHIFT_DRIVER_SET_SHA256;
#define REDSHIFT_CHECK_QUARANTINE(file_name, file_size, file_sha256, stem)                                             \
	CHECK(                                                                                                             \
	    (::getxattr((set_dir + "/" + file_name).c_str(), "com.apple.quarantine", nullptr, 0, 0, XATTR_NOFOLLOW) < 0 && \
	     errno == ENOATTR));
	REDSHIFT_DRIVER_FILES(REDSHIFT_CHECK_QUARANTINE)
#undef REDSHIFT_CHECK_QUARANTINE
	// The written driver (the universal file the lock names for both macOS platforms) has this process's slice.
	CHECK(HasThisArchitecture(set_dir + "/librsodbc64.dylib"));
	CHECK((::getxattr(set_dir.c_str(), "com.apple.quarantine", nullptr, 0, 0, XATTR_NOFOLLOW) < 0 && errno == ENOATTR));
}
