// The Windows install and repair under a test driver root, run by the Windows driver suite. The set
// is a synthetic payload: nothing here is loaded.

#include "windows_temp_dir.hpp"

#include "sha256.hpp"

#include <atomic>
#include <fstream>
#include <thread>

using namespace duckdb::redshift_driver;
using namespace duckdb::redshift_driver::windows_driver;
using duckdb::redshift_driver::windows_rules::Narrow;
using duckdb::redshift_driver::windows_rules::Widen;
using redshift_driver_test::WindowsTempDir;

namespace {

struct Payload {
	std::vector<std::vector<unsigned char>> bytes;
	std::vector<SetFile> files;
	std::string set_sha256 = std::string(64, 'a');
};

Payload MakePayload() {
	Payload payload;
	payload.bytes = {std::vector<unsigned char>(4096, 0x5a), {'n', 'o', 't', 'i', 'c', 'e'}};
	const std::vector<std::string> names = {"rsodbc.dll", "NOTICE"};
	for (size_t i = 0; i < names.size(); i++) {
		const auto &bytes = payload.bytes[i];
		payload.files.push_back({names[i], detail::Sha256Hex(bytes.data(), bytes.size()), bytes.data(), bytes.size()});
	}
	return payload;
}

std::string InstallInto(const std::string &root, const Payload &set, InstallResult expected, int deadline = 30000) {
	std::string set_dir, where, reason;
	CHECK(InstallSet(root, set.set_sha256, set.files, CurrentUserSid(), deadline, set_dir, where, reason) == expected);
	return reason;
}

} // namespace

TEST_CASE("Bytes that differ from their recorded SHA-256 give BYTES_CORRUPT with no set written", "[windows_driver]") {
	WindowsTempDir temp;
	auto set = MakePayload();
	set.files[1].sha256 = std::string(64, '0');
	const auto root = temp.Make(".redshift-driver");
	CHECK(InstallInto(root, set, InstallResult::BYTES_CORRUPT) ==
	      "NOTICE in the extension does not match the hash recorded when it was built; reinstall the extension");
	CHECK(GetFileAttributesW(Widen(root + "\\" + set.set_sha256).c_str()) == INVALID_FILE_ATTRIBUTES);
}

TEST_CASE("A set with one changed file is set aside, then written again as a whole", "[windows_driver]") {
	WindowsTempDir temp;
	auto set = MakePayload();
	const auto root = temp.Make(".redshift-driver");
	InstallInto(root, set, InstallResult::INSTALLED);
	std::ofstream(root + "\\" + set.set_sha256 + "\\NOTICE", std::ios::binary | std::ios::app) << "x";
	InstallInto(root, set, InstallResult::INSTALLED);
	std::string where, reason;
	CHECK(CheckSet(root + "\\" + set.set_sha256, set.files, CurrentUserSid(), where, reason) == SetState::VALID);
	bool set_aside = false;
	WIN32_FIND_DATAW entry;
	HANDLE find = FindFirstFileW(Widen(root + "\\.invalid-*").c_str(), &entry);
	set_aside = find != INVALID_HANDLE_VALUE;
	FindClose(find);
	CHECK(set_aside);
}

TEST_CASE("A root.crt in the set folder makes the set invalid", "[windows_driver]") {
	WindowsTempDir temp;
	auto set = MakePayload();
	const auto root = temp.Make(".redshift-driver");
	InstallInto(root, set, InstallResult::INSTALLED);
	std::ofstream(root + "\\" + set.set_sha256 + "\\root.crt") << "planted";
	std::string where, reason;
	CHECK(CheckSet(root + "\\" + set.set_sha256, set.files, CurrentUserSid(), where, reason) == SetState::INVALID);
	CHECK(reason == "the driver set does not hold exactly the set's files");
}

TEST_CASE("The set folder, made as staging, has a protected list that grants only the user", "[windows_driver]") {
	WindowsTempDir temp;
	auto set = MakePayload();
	const auto root = temp.Make(".redshift-driver");
	InstallInto(root, set, InstallResult::INSTALLED);
	redshift_driver_test::CheckOwnerOnlyList(root + "\\" + set.set_sha256);
}

TEST_CASE("A hard-linked set file makes the set invalid", "[windows_driver]") {
	WindowsTempDir temp;
	auto set = MakePayload();
	const auto root = temp.Make(".redshift-driver");
	InstallInto(root, set, InstallResult::INSTALLED);
	const auto notice = root + "\\" + set.set_sha256 + "\\NOTICE";
	REQUIRE(CreateHardLinkW(Widen(temp.path() + "\\link").c_str(), Widen(notice).c_str(), nullptr));
	std::string where, reason;
	CHECK(CheckSet(root + "\\" + set.set_sha256, set.files, CurrentUserSid(), where, reason) == SetState::INVALID);
	CHECK(reason == "a file of the driver set has more than one hard link");
}

TEST_CASE("A folder where a set file should be makes the set invalid, and the install writes it again",
          "[windows_driver]") {
	WindowsTempDir temp;
	auto set = MakePayload();
	const auto root = temp.Make(".redshift-driver");
	InstallInto(root, set, InstallResult::INSTALLED);
	const auto set_dir = root + "\\" + set.set_sha256;
	REQUIRE(DeleteFileW(Widen(set_dir + "\\NOTICE").c_str()));
	REQUIRE(CreateDirectoryW(Widen(set_dir + "\\NOTICE").c_str(), nullptr));
	std::string where, reason;
	CHECK(CheckSet(set_dir, set.files, CurrentUserSid(), where, reason) == SetState::INVALID);
	CHECK(reason == "a file of the driver set is not a file");
	InstallInto(root, set, InstallResult::INSTALLED);
	CHECK(CheckSet(set_dir, set.files, CurrentUserSid(), where, reason) == SetState::VALID);
}

TEST_CASE("Eight threads installing into one root leave one set", "[windows_driver]") {
	WindowsTempDir temp;
	auto set = MakePayload();
	const auto root = temp.Make(".redshift-driver");
	std::atomic<int> installed {0};
	std::vector<std::thread> threads;
	for (int i = 0; i < 8; i++) {
		threads.emplace_back([&] {
			std::string set_dir, where, reason;
			installed += InstallSet(root, set.set_sha256, set.files, CurrentUserSid(), 30000, set_dir, where, reason) ==
			             InstallResult::INSTALLED;
		});
	}
	for (auto &thread : threads) {
		thread.join();
	}
	CHECK(installed == 8);
	CHECK(std::distance(std::filesystem::directory_iterator(Widen(root)), std::filesystem::directory_iterator()) == 2);
}

TEST_CASE("A set file held open by another program is retried until it is released", "[windows_driver]") {
	WindowsTempDir temp;
	auto set = MakePayload();
	const auto root = temp.Make(".redshift-driver");
	InstallInto(root, set, InstallResult::INSTALLED);
	FileHandle scanner(CreateFileW(Widen(root + "\\" + set.set_sha256 + "\\NOTICE").c_str(), GENERIC_READ, 0, nullptr,
	                               OPEN_EXISTING, 0, nullptr));
	REQUIRE(scanner.valid());
	std::thread release([&] {
		Sleep(200);
		scanner = FileHandle();
	});
	std::string where, reason;
	const auto state = CheckSet(root + "\\" + set.set_sha256, set.files, CurrentUserSid(), where, reason);
	release.join();
	CHECK(state == SetState::VALID);
}

TEST_CASE("A set file held open past the retries leaves the set unchecked", "[windows_driver]") {
	WindowsTempDir temp;
	auto set = MakePayload();
	const auto root = temp.Make(".redshift-driver");
	InstallInto(root, set, InstallResult::INSTALLED);
	FileHandle scanner(CreateFileW(Widen(root + "\\" + set.set_sha256 + "\\NOTICE").c_str(), GENERIC_READ, 0, nullptr,
	                               OPEN_EXISTING, 0, nullptr));
	REQUIRE(scanner.valid());
	std::string where, reason;
	CHECK(CheckSet(root + "\\" + set.set_sha256, set.files, CurrentUserSid(), where, reason) == SetState::UNCHECKED);
	CHECK(reason == "a file of the driver set cannot be opened: Windows error 32");
}

TEST_CASE("A lock held past the deadline ends the install with its rule", "[windows_driver]") {
	WindowsTempDir temp;
	auto set = MakePayload();
	const auto root = temp.Make(".redshift-driver");
	FileHandle holder(CreateFileW(Widen(root + "\\.lock").c_str(), GENERIC_READ | GENERIC_WRITE,
	                              FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr, OPEN_ALWAYS, 0, nullptr));
	OVERLAPPED whole = {};
	REQUIRE(LockFileEx(holder.get(), LOCKFILE_EXCLUSIVE_LOCK, 0, 1, 0, &whole));
	CHECK(InstallInto(root, set, InstallResult::FAILED, 200) ==
	      "another process held the driver root's lock past the deadline");
}

namespace {

bool Exists(const std::string &path) {
	return GetFileAttributesW(Widen(path).c_str()) != INVALID_FILE_ATTRIBUTES;
}

bool AnySetAside(const std::string &root) {
	WIN32_FIND_DATAW entry;
	HANDLE find = FindFirstFileW(Widen(root + "\\.invalid-*").c_str(), &entry);
	const bool found = find != INVALID_HANDLE_VALUE;
	if (found) {
		FindClose(find);
	}
	return found;
}

void SetLastWriteTime(const std::string &path, const FILETIME &time) {
	FileHandle entry(CreateFileW(Widen(path).c_str(), FILE_WRITE_ATTRIBUTES, FILE_SHARE_READ, nullptr, OPEN_EXISTING,
	                             FILE_FLAG_BACKUP_SEMANTICS, nullptr));
	REQUIRE(entry.valid());
	REQUIRE(SetFileTime(entry.get(), nullptr, nullptr, &time));
}

// Gives `path`, a file or a folder, a last-write time in 1601, well past the sweep's threshold.
void BackDate(const std::string &path) {
	SetLastWriteTime(path, FILETIME {0, 1});
}

// Gives `path` a last-write time a day after now, as after the clock is set back.
void ForwardDate(const std::string &path) {
	FILETIME now;
	GetSystemTimeAsFileTime(&now);
	ULARGE_INTEGER ahead;
	ahead.LowPart = now.dwLowDateTime;
	ahead.HighPart = now.dwHighDateTime;
	ahead.QuadPart += 24ULL * 3600 * 10000000; // a day, in 100 ns ticks
	SetLastWriteTime(path, FILETIME {ahead.LowPart, ahead.HighPart});
}

} // namespace

TEST_CASE("The sweep removes the user's stale staging and set-aside folders, never a junction's target",
          "[windows_driver]") {
	WindowsTempDir temp;
	const auto root = temp.Make(".redshift-driver");
	temp.Make(".redshift-driver\\.staging-left");
	const auto old_invalid = temp.Make(".redshift-driver\\.invalid-old");
	temp.Make(".redshift-driver\\.invalid-new");
	// Another set's folder, older than the threshold: another process may still map it.
	const auto other_set = temp.Make(".redshift-driver\\" + std::string(64, 'b'));
	const auto target = temp.Make("target");
	std::ofstream(target + "\\kept") << "kept";
	redshift_driver_test::MakeJunction(root + "\\.staging-link", target);
	BackDate(old_invalid);
	BackDate(other_set);
	SweepRoot(root, CurrentUserSid());
	CHECK_FALSE(Exists(root + "\\.staging-left"));
	CHECK_FALSE(Exists(old_invalid));
	CHECK(Exists(root + "\\.invalid-new"));
	CHECK_FALSE(Exists(root + "\\.staging-link"));
	CHECK(Exists(target + "\\kept"));
	CHECK(Exists(other_set));
	// The sweeps of an install and of a repair keep it too.
	auto set = MakePayload();
	InstallInto(root, set, InstallResult::INSTALLED);
	std::ofstream(root + "\\" + set.set_sha256 + "\\NOTICE", std::ios::binary | std::ios::app) << "x";
	InstallInto(root, set, InstallResult::INSTALLED);
	CHECK(Exists(other_set));
}

TEST_CASE("The sweep keeps a set-aside folder dated after now", "[windows_driver]") {
	WindowsTempDir temp;
	const auto root = temp.Make(".redshift-driver");
	const auto ahead = temp.Make(".redshift-driver\\.invalid-ahead");
	ForwardDate(ahead);
	SweepRoot(root, CurrentUserSid());
	CHECK(Exists(ahead));
}

TEST_CASE("The sweep leaves another account's stale staging and set-aside folders", "[windows_driver]") {
	WindowsTempDir temp;
	const auto root = temp.Make(".redshift-driver");
	const auto staging = temp.Make(".redshift-driver\\.staging-theirs");
	const auto invalid = temp.Make(".redshift-driver\\.invalid-theirs");
	BackDate(invalid);
	// These folders are this account's, so a sweep for another SID finds them owned by someone else.
	SweepRoot(root, "S-1-5-21-1-2-3-1001");
	CHECK(Exists(staging));
	CHECK(Exists(invalid));
}

TEST_CASE("A set installed long ago and set aside now outlives the next sweep", "[windows_driver]") {
	WindowsTempDir temp;
	auto set = MakePayload();
	const auto root = temp.Make(".redshift-driver");
	const auto set_dir = root + "\\" + set.set_sha256;
	InstallInto(root, set, InstallResult::INSTALLED);
	// Changing a file's bytes leaves its folder's last-write time as back-dated here.
	std::ofstream(set_dir + "\\NOTICE", std::ios::binary | std::ios::app) << "x";
	BackDate(set_dir);
	InstallInto(root, set, InstallResult::INSTALLED);
	SweepRoot(root, CurrentUserSid());
	CHECK(AnySetAside(root));
}

TEST_CASE("A set file held past the retries fails the install and leaves the set in place", "[windows_driver]") {
	WindowsTempDir temp;
	auto set = MakePayload();
	const auto root = temp.Make(".redshift-driver");
	InstallInto(root, set, InstallResult::INSTALLED);
	FileHandle scanner(CreateFileW(Widen(root + "\\" + set.set_sha256 + "\\NOTICE").c_str(), GENERIC_READ, 0, nullptr,
	                               OPEN_EXISTING, 0, nullptr));
	REQUIRE(scanner.valid());
	CHECK(InstallInto(root, set, InstallResult::FAILED) ==
	      "a file of the driver set cannot be opened: Windows error 32");
	CHECK(Exists(root + "\\" + set.set_sha256 + "\\NOTICE"));
	CHECK_FALSE(AnySetAside(root));
}

TEST_CASE("A set folder whose access list cannot be read leaves the set unchecked", "[windows_driver]") {
	if (redshift_driver_test::BackupPrivilegeEnabled()) {
		WARN("SeBackupPrivilege is enabled, so no deny entry can stop the read; run as a standard account");
		return;
	}
	WindowsTempDir temp;
	auto set = MakePayload();
	const auto root = temp.Make(".redshift-driver");
	InstallInto(root, set, InstallResult::INSTALLED);
	const auto set_dir = root + "\\" + set.set_sha256;
	// An OWNER RIGHTS deny entry takes READ_CONTROL from the owner too.
	redshift_driver_test::SetAccessList(set_dir, "D:P(D;;RC;;;OW)(A;OICI;FA;;;<user>)");
	std::string where, reason;
	CHECK(CheckSet(set_dir, set.files, CurrentUserSid(), where, reason) == SetState::UNCHECKED);
	CHECK(reason.rfind("the driver set cannot be opened: ", 0) == 0);
	redshift_driver_test::ReplaceAccessList(set_dir, "D:P(A;OICI;FA;;;<user>)");
}

TEST_CASE("A valid set is used without the lock, even while another process holds it", "[windows_driver]") {
	WindowsTempDir temp;
	auto set = MakePayload();
	const auto root = temp.Make(".redshift-driver");
	InstallInto(root, set, InstallResult::INSTALLED);
	FileHandle holder(CreateFileW(Widen(root + "\\.lock").c_str(), GENERIC_READ | GENERIC_WRITE,
	                              FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr, OPEN_ALWAYS, 0, nullptr));
	OVERLAPPED whole = {};
	REQUIRE(LockFileEx(holder.get(), LOCKFILE_EXCLUSIVE_LOCK, 0, 1, 0, &whole));
	InstallInto(root, set, InstallResult::INSTALLED, 200);
}

TEST_CASE("A lock file with a second hard link fails the install with its rule", "[windows_driver]") {
	WindowsTempDir temp;
	auto set = MakePayload();
	const auto root = temp.Make(".redshift-driver");
	const auto other = temp.path() + "\\other";
	std::ofstream(other) << "";
	REQUIRE(CreateHardLinkW(Widen(root + "\\.lock").c_str(), Widen(other).c_str(), nullptr));
	CHECK(InstallInto(root, set, InstallResult::FAILED) == "the driver root's lock has more than one hard link");
}

TEST_CASE("The sweep keeps a stale staging folder that holds a real folder with files", "[windows_driver]") {
	WindowsTempDir temp;
	const auto root = temp.Make(".redshift-driver");
	temp.Make(".redshift-driver\\.staging-deep");
	const auto inner = temp.Make(".redshift-driver\\.staging-deep\\inner");
	std::ofstream(inner + "\\kept") << "kept";
	SweepRoot(root, CurrentUserSid());
	CHECK(Exists(inner + "\\kept"));
}

TEST_CASE("The sweep deletes a stale set-aside entry that is a plain file", "[windows_driver]") {
	WindowsTempDir temp;
	const auto root = temp.Make(".redshift-driver");
	const auto file = root + "\\.invalid-file";
	std::ofstream(file) << "a file where a set was";
	BackDate(file);
	SweepRoot(root, CurrentUserSid());
	CHECK_FALSE(Exists(file));
}

TEST_CASE("A handle whose path cannot be read is unreadable, not a link", "[windows_driver]") {
	HANDLE read_end = nullptr, write_end = nullptr;
	REQUIRE(CreatePipe(&read_end, &write_end, nullptr, 0));
	FileHandle reader(read_end), writer(write_end);
	bool unreadable = false;
	const auto rule = HandleRule(reader.get(), "C:\\a-pipe", false, CurrentUserSid(), &unreadable, nullptr);
	CHECK(unreadable);
	CHECK(rule.rfind("cannot be examined: ", 0) == 0);
}
