//===----------------------------------------------------------------------===//
// Installing and repairing the driver set on Windows: the set check, staging with
// CREATE_NEW and FlushFileBuffers, the lock with a deadline, setting an invalid set aside, bounded
// retries while another program holds a file, and the stale sweep. Includes no header that includes
// the interface header, as the directory file does not.
//===----------------------------------------------------------------------===//

#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>

#include "driver_windows.hpp"
#include "sha256.hpp"

#include <algorithm>
#include <random>
#include <string>
#include <vector>

namespace duckdb {
namespace redshift_driver {
namespace windows_driver {

using windows_rules::SamePath;
using windows_rules::Widen;

namespace {

// Calls `attempt`, which returns a Win32 error, until it succeeds or fails other than as busy (a
// sharing violation or access denied, as an antivirus scanner's open handle causes), at most
// BUSY_TRIES times.
template <class ATTEMPT>
DWORD RetryWhileBusy(ATTEMPT attempt) {
	DWORD error = ERROR_SUCCESS;
	for (int tries = 0; tries < BUSY_TRIES; tries++) {
		if (tries > 0) {
			Sleep(BUSY_WAIT_MILLISECONDS);
		}
		error = attempt();
		if (error != ERROR_SHARING_VIOLATION && error != ERROR_ACCESS_DENIED) {
			return error;
		}
	}
	return error;
}

DWORD Move(const std::string &from, const std::string &to) {
	// Never MOVEFILE_REPLACE_EXISTING, so a set another process finished is never replaced.
	return RetryWhileBusy(
	    [&] { return MoveFileExW(Widen(from).c_str(), Widen(to).c_str(), 0) ? DWORD(ERROR_SUCCESS) : GetLastError(); });
}

std::string RandomSuffix() {
	static const char HEX[] = "0123456789abcdef";
	std::random_device random;
	std::string suffix;
	while (suffix.size() < windows_rules::RANDOM_NAME_LENGTH) {
		suffix.push_back(HEX[random() % 16]);
	}
	return suffix;
}

// The names in `dir`. `error` is ERROR_SUCCESS once every entry is listed, else the failed call's
// error, taken before FindClose, which may change GetLastError.
std::vector<std::string> EntryNames(const std::string &dir, DWORD &error) {
	std::vector<std::string> names;
	WIN32_FIND_DATAW entry;
	HANDLE find =
	    FindFirstFileExW(Widen(dir + "\\*").c_str(), FindExInfoBasic, &entry, FindExSearchNameMatch, nullptr, 0);
	if (find == INVALID_HANDLE_VALUE) {
		error = GetLastError();
		return names;
	}
	do {
		const std::wstring name = entry.cFileName;
		if (name != L"." && name != L"..") {
			names.push_back(windows_rules::Narrow(name));
		}
	} while (FindNextFileW(find, &entry));
	error = GetLastError();
	if (error == ERROR_NO_MORE_FILES) {
		error = ERROR_SUCCESS;
	}
	FindClose(find);
	return names;
}

// The names EntryNames lists, for a caller that treats a folder it cannot list as empty.
std::vector<std::string> EntryNames(const std::string &dir) {
	DWORD error = ERROR_SUCCESS;
	return EntryNames(dir, error);
}

// Removes the folder `dir` and the files in it, without following a link: a reparse point, at `dir`
// or inside it, is removed itself, and a folder inside it is left with `dir`, as the set is flat. A
// plain file at `dir`, set aside where a set should have been, is deleted.
void RemoveFlatFolder(const std::string &dir) {
	const DWORD attributes = GetFileAttributesW(Widen(dir).c_str());
	if (attributes != INVALID_FILE_ATTRIBUTES && (attributes & FILE_ATTRIBUTE_DIRECTORY) == 0) {
		DeleteFileW(Widen(dir).c_str());
		return;
	}
	if (attributes != INVALID_FILE_ATTRIBUTES && (attributes & FILE_ATTRIBUTE_REPARSE_POINT) == 0) {
		for (const auto &name : EntryNames(dir)) {
			const auto path = dir + "\\" + name;
			const DWORD inner = GetFileAttributesW(Widen(path).c_str());
			if (inner != INVALID_FILE_ATTRIBUTES && (inner & FILE_ATTRIBUTE_DIRECTORY) != 0) {
				RemoveDirectoryW(Widen(path).c_str()); // a junction goes; a real folder that is not empty stays
			} else {
				DeleteFileW(Widen(path).c_str());
			}
		}
	}
	RemoveDirectoryW(Widen(dir).c_str());
}

bool WriteSet(const std::string &staging, const std::vector<SetFile> &files, std::string &reason) {
	for (const auto &file : files) {
		FileHandle out(CreateFileW(Widen(staging + "\\" + file.name).c_str(), GENERIC_WRITE, 0, nullptr, CREATE_NEW,
		                           FILE_ATTRIBUTE_NORMAL, nullptr));
		bool written = out.valid();
		for (size_t offset = 0; written && offset < file.size;) {
			const DWORD chunk = static_cast<DWORD>(std::min<size_t>(file.size - offset, 1 << 20));
			DWORD wrote = 0;
			written = WriteFile(out.get(), file.data + offset, chunk, &wrote, nullptr);
			if (written && wrote != chunk) {
				// A short write sets no Windows error, so GetLastError would name an older one.
				reason = "a file of the driver set cannot be written: Windows wrote fewer bytes than given";
				return false;
			}
			offset += wrote;
		}
		if (!written || !FlushFileBuffers(out.get())) {
			reason = "a file of the driver set cannot be written: " + ErrorText(GetLastError());
			return false;
		}
	}
	return true;
}

// Sets the last-write time of `path` to now. A rename keeps it, and the sweep ages a set-aside
// folder by it, so a set installed long ago and set aside now still gets its hour.
// A failure is ignored, as on POSIX: it only lets the sweep remove the folder sooner.
void TouchNow(const std::string &path) {
	FileHandle entry(CreateFileW(Widen(path).c_str(), FILE_WRITE_ATTRIBUTES,
	                             FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr, OPEN_EXISTING,
	                             FILE_FLAG_BACKUP_SEMANTICS | FILE_FLAG_OPEN_REPARSE_POINT, nullptr));
	FILETIME now;
	GetSystemTimeAsFileTime(&now);
	if (entry.valid()) {
		SetFileTime(entry.get(), nullptr, nullptr, &now);
	}
}

// Takes the exclusive lock on `lock`, retrying until `deadline_milliseconds`.
bool Lock(const FileHandle &lock, int deadline_milliseconds, std::string &reason) {
	const ULONGLONG start = GetTickCount64();
	for (;;) {
		OVERLAPPED whole = {};
		if (LockFileEx(lock.get(), LOCKFILE_EXCLUSIVE_LOCK | LOCKFILE_FAIL_IMMEDIATELY, 0, 1, 0, &whole)) {
			return true;
		}
		const DWORD error = GetLastError();
		if (error != ERROR_LOCK_VIOLATION) {
			reason = "the driver root's lock cannot be taken: " + ErrorText(error);
			return false;
		}
		if (GetTickCount64() - start >= static_cast<ULONGLONG>(deadline_milliseconds)) {
			reason = "another process held the driver root's lock past the deadline";
			return false;
		}
		Sleep(10);
	}
}

} // namespace

SetState CheckSet(const std::string &set_dir, const std::vector<SetFile> &files, const std::string &user,
                  std::string &where, std::string &reason, std::vector<HeldFile> *held) {
	where = set_dir;
	const DWORD attributes = GetFileAttributesW(Widen(set_dir).c_str());
	const DWORD missing = GetLastError();
	if (attributes == INVALID_FILE_ATTRIBUTES && (missing == ERROR_FILE_NOT_FOUND || missing == ERROR_PATH_NOT_FOUND)) {
		reason = "the driver set is missing";
		return SetState::MISSING;
	}
	bool unreadable = false;
	auto rule = OwnDirectoryRule(set_dir, user, &unreadable);
	if (!rule.empty()) {
		reason = "the driver set " + rule;
		return unreadable ? SetState::UNCHECKED : SetState::INVALID;
	}
	DWORD list_error = ERROR_SUCCESS;
	const auto names = EntryNames(set_dir, list_error);
	if (list_error != ERROR_SUCCESS) {
		reason = "the driver set cannot be listed: " + ErrorText(list_error);
		return SetState::UNCHECKED;
	}
	// Exactly the manifest's files: never a root.crt or an ini the driver would read.
	const auto in_set = [&](const std::string &name) {
		return std::any_of(files.begin(), files.end(), [&](const SetFile &file) { return SamePath(file.name, name); });
	};
	if (names.size() != files.size() || !std::all_of(names.begin(), names.end(), in_set)) {
		reason = "the driver set does not hold exactly the set's files";
		return SetState::INVALID;
	}
	std::vector<HeldFile> opened;
	for (const auto &file : files) {
		const auto path = set_dir + "\\" + file.name;
		where = path;
		// CreateFileW opens no folder without backup semantics: it gives access denied, which would read
		// as busy and leave the set unchecked, so a folder at a file's name is evidence first.
		const DWORD kind = GetFileAttributesW(Widen(path).c_str());
		if (kind != INVALID_FILE_ATTRIBUTES && (kind & FILE_ATTRIBUTE_DIRECTORY) != 0) {
			reason = "a file of the driver set is not a file";
			return SetState::INVALID;
		}
		// Others may read and execute it, never write, rename or delete it while this handle is open.
		FileHandle handle;
		const DWORD error = RetryWhileBusy([&] {
			handle = FileHandle(CreateFileW(Widen(path).c_str(), GENERIC_READ | READ_CONTROL, FILE_SHARE_READ, nullptr,
			                                OPEN_EXISTING, FILE_FLAG_OPEN_REPARSE_POINT, nullptr));
			return handle.valid() ? DWORD(ERROR_SUCCESS) : GetLastError();
		});
		if (!handle.valid()) {
			reason = "a file of the driver set cannot be opened: " + ErrorText(error);
			return SetState::UNCHECKED;
		}
		windows_rules::FileId id;
		rule = HandleRule(handle.get(), path, false, user, &unreadable, &id);
		if (!rule.empty()) {
			reason = "a file of the driver set " + rule;
			return unreadable ? SetState::UNCHECKED : SetState::INVALID;
		}
		// Hashed through the handle that stays open until the load, so what loads is what was hashed.
		detail::Sha256 hash;
		std::vector<unsigned char> chunk(1 << 16);
		for (DWORD got = 1; got > 0;) {
			if (!ReadFile(handle.get(), chunk.data(), static_cast<DWORD>(chunk.size()), &got, nullptr)) {
				reason = "a file of the driver set cannot be read: " + ErrorText(GetLastError());
				return SetState::UNCHECKED;
			}
			hash.Update(chunk.data(), got);
		}
		if (hash.FinishHex() != file.sha256) {
			reason = "a file of the driver set has the wrong SHA-256";
			return SetState::INVALID;
		}
		opened.push_back({path, id, std::move(handle)});
	}
	if (held != nullptr) {
		*held = std::move(opened);
	}
	where = set_dir;
	return SetState::VALID;
}

void SweepRoot(const std::string &root, const std::string &user) {
	FILETIME now;
	GetSystemTimeAsFileTime(&now);
	const auto ticks = [](const FILETIME &time) {
		return (static_cast<ULONGLONG>(time.dwHighDateTime) << 32) | time.dwLowDateTime;
	};
	const ULONGLONG stale = static_cast<ULONGLONG>(STALE_SECONDS) * 10000000ULL; // in 100 ns ticks
	for (const auto &name : EntryNames(root)) {
		const auto path = root + "\\" + name;
		const bool staging =
		    name.compare(0, std::string(windows_rules::STAGING_PREFIX).size(), windows_rules::STAGING_PREFIX) == 0;
		const bool invalid =
		    name.compare(0, std::string(windows_rules::INVALID_PREFIX).size(), windows_rules::INVALID_PREFIX) == 0;
		WIN32_FILE_ATTRIBUTE_DATA data;
		if ((!staging && !invalid) || !GetFileAttributesExW(Widen(path).c_str(), GetFileExInfoStandard, &data) ||
		    !OwnedBy(path, user)) {
			continue;
		}
		// Staging happens under the lock, so a staging folder found here was left by a process that died.
		// A set-aside folder dated after now (the clock was set back) is not stale yet.
		const auto written = ticks(data.ftLastWriteTime);
		if (staging || (ticks(now) > written && ticks(now) - written >= stale)) {
			RemoveFlatFolder(path);
		}
	}
}

InstallResult InstallSet(const std::string &root, const std::string &set_sha256, const std::vector<SetFile> &files,
                         const std::string &user, int lock_deadline_milliseconds, std::string &set_dir,
                         std::string &where, std::string &reason) {
	set_dir = root + "\\" + set_sha256;
	auto state = CheckSet(set_dir, files, user, where, reason);
	if (state == SetState::VALID || state == SetState::UNCHECKED) {
		return state == SetState::VALID ? InstallResult::INSTALLED : InstallResult::FAILED;
	}
	where = root + "\\" + windows_rules::LOCK_NAME;
	FileHandle lock(CreateFileW(Widen(where).c_str(), GENERIC_READ | GENERIC_WRITE | READ_CONTROL,
	                            FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr, OPEN_ALWAYS, FILE_FLAG_OPEN_REPARSE_POINT,
	                            nullptr));
	if (!lock.valid()) {
		reason = "the driver root's lock cannot be opened: " + ErrorText(GetLastError());
		return InstallResult::FAILED;
	}
	bool unreadable = false;
	const auto lock_rule = HandleRule(lock.get(), where, false, user, &unreadable, nullptr);
	if (!lock_rule.empty()) {
		reason = "the driver root's lock " + lock_rule;
		return InstallResult::FAILED;
	}
	if (!Lock(lock, lock_deadline_milliseconds, reason)) {
		return InstallResult::FAILED;
	}
	SweepRoot(root, user);
	// Another process may have written the set while this one waited.
	state = CheckSet(set_dir, files, user, where, reason);
	if (state == SetState::VALID || state == SetState::UNCHECKED) {
		return state == SetState::VALID ? InstallResult::INSTALLED : InstallResult::FAILED;
	}
	for (const auto &file : files) {
		if (detail::Sha256Hex(file.data, file.size) != file.sha256) {
			// The POSIX loader's sentence; it names the file, never a path.
			where.clear();
			reason = file.name + " in the extension does not match the hash recorded when it was built; reinstall the "
			                     "extension";
			return InstallResult::BYTES_CORRUPT;
		}
	}
	const auto staging = root + "\\" + windows_rules::STAGING_PREFIX + RandomSuffix();
	where = staging;
	const auto staging_rule = MakeOwnDirectory(staging, user);
	if (!staging_rule.empty()) {
		reason = "the staging folder " + staging_rule;
		RemoveFlatFolder(staging);
		return InstallResult::FAILED;
	}
	if (!WriteSet(staging, files, reason)) {
		RemoveFlatFolder(staging);
		return InstallResult::FAILED;
	}
	if (state == SetState::INVALID) {
		TouchNow(set_dir);
		const DWORD error = Move(set_dir, root + "\\" + windows_rules::INVALID_PREFIX + RandomSuffix());
		if (error != ERROR_SUCCESS) {
			RemoveFlatFolder(staging);
			where = set_dir;
			reason = error == ERROR_SHARING_VIOLATION || error == ERROR_ACCESS_DENIED
			             ? std::string(windows_rules::SET_IN_USE_RULE)
			             : "the invalid driver set cannot be set aside: " + ErrorText(error);
			return InstallResult::FAILED;
		}
	}
	const DWORD error = Move(staging, set_dir);
	if (error != ERROR_SUCCESS) {
		RemoveFlatFolder(staging);
		reason = "the new driver set cannot be moved into place: " + ErrorText(error);
		return InstallResult::FAILED;
	}
	state = CheckSet(set_dir, files, user, where, reason);
	return state == SetState::VALID ? InstallResult::INSTALLED : InstallResult::FAILED;
}

} // namespace windows_driver
} // namespace redshift_driver
} // namespace duckdb
