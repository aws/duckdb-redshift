//===----------------------------------------------------------------------===//
//                         DuckDB - Redshift
//
// src/transport/driver/driver_windows.hpp
//
// The Windows directory checks and install, which the Windows loader and its tests call. Includes neither
// windows.h nor the interface header, whose removed Win32 macros the access-list and shell headers
// need, so the directory file never sees the interface header.
//===----------------------------------------------------------------------===//

#pragma once

#include "driver_windows_rules.hpp"

#include <cstdint>
#include <string>
#include <vector>

namespace duckdb {
namespace redshift_driver {
namespace windows_driver {

using windows_rules::Attempt;

// A Win32 file or directory handle, closed when it goes out of scope.
class FileHandle {
public:
	FileHandle() = default;
	explicit FileHandle(void *handle);
	~FileHandle();
	FileHandle(const FileHandle &) = delete;
	FileHandle &operator=(const FileHandle &) = delete;
	FileHandle(FileHandle &&other) noexcept;
	FileHandle &operator=(FileHandle &&other) noexcept;
	void *get() const {
		return handle_;
	}
	bool valid() const {
		return handle_ != nullptr;
	}

private:
	void *handle_ = nullptr; // nullptr, never INVALID_HANDLE_VALUE, when there is none
};

// The text of a Win32 error code, for reasons: its number, which reads the same in every locale.
std::string ErrorText(unsigned long code);
// The current account's SID as text (S-1-5-21-...), or empty when the token cannot be read.
std::string CurrentUserSid();
// GetDriveTypeW, which tests replace to report a drive that is not local.
unsigned SystemDriveType(const wchar_t *root);

enum class BaseKind : uint8_t { OVERRIDE, EXTENSION, CACHE };
const char *BaseRole(BaseKind kind);

// What base selection reads. CurrentBaseInputs reads the process's own on every
// call; tests give their own, so no test writes into the account's real per-user cache.
struct BaseInputs {
	bool override_set = false;     // REDSHIFT_DRIVER_DIR is set (an empty value counts as set)
	std::wstring override_value;   // its value
	std::string override_failure;  // why it could not be read, which refuses it; empty when it was read
	std::wstring extension_file;   // the extension's own file
	std::string extension_failure; // why that file could not be named; empty when it was
	std::wstring per_user_base;    // <LocalAppData>\duckdb-redshift; empty when there is no known folder
	unsigned (*drive_type)(const wchar_t *root) = SystemDriveType;
	int lock_deadline_milliseconds = 30000;
};
BaseInputs CurrentBaseInputs();

// The rule the open file or folder `handle` at `path` breaks, or empty:
// the wrong kind, a reparse point or cloud placeholder, a file with another hard link, a final path
// other than `path`, or its owner or access list. `unreadable` says the facts could not be read, so
// the rule is no evidence; `id`, when given, gets the file's identity.
std::string HandleRule(void *handle, const std::string &path, bool directory, const std::string &user, bool *unreadable,
                       windows_rules::FileId *id);
// HandleRule for the folder `dir`, the base or a folder below it, opened without following a link.
std::string OwnDirectoryRule(const std::string &dir, const std::string &user, bool *unreadable = nullptr);
// Whether `path`, not followed if it is a link, is owned by `user`.
bool OwnedBy(const std::string &path, const std::string &user);
// The identity of the file at `path`; false when it cannot be opened or read.
bool FileIdOf(const std::string &path, windows_rules::FileId &id);
// Creates `dir` with a protected access list that grants only the current account, unless it
// exists, then applies OwnDirectoryRule.
std::string MakeOwnDirectory(const std::string &dir, const std::string &user);
// The base directory rules for `base`: its drive is local, every folder above it passes the
// ancestor rule, it is made with MakeOwnDirectory when `create` is set and it is missing, it has no
// 8.3 name, and it passes OwnDirectoryRule. False with `where` and `reason` (a rule, no path).
bool CheckBase(const std::string &base, bool create, const BaseInputs &inputs, const std::string &user,
               std::string &where, std::string &reason);

// The driver root of each usable base, in order, created and checked only when the caller asks for
// the next one; each refusal is in `attempts` as "<role>: <rule>".
class DriverRoots {
public:
	DriverRoots(const BaseInputs &inputs, std::string user, std::vector<std::string> file_names);
	bool Next(std::string &root);
	// Records a failure of the caller's with the base Next gave last.
	void Fail(const std::string &where, const std::string &rule);
	const char *Role() const;
	const std::string &Base() const {
		return base_;
	}
	std::vector<Attempt> attempts;

private:
	struct Candidate {
		BaseKind kind;
		std::string base;
		std::string refusal;
	};
	BaseInputs inputs_;
	std::string user_;
	std::vector<std::string> file_names_;
	std::vector<Candidate> candidates_;
	size_t next_ = 0;
	BaseKind kind_ = BaseKind::OVERRIDE;
	std::string base_;
};

// A file of the set to install: its bytes (embedded, or a test payload) and their SHA-256.
struct SetFile {
	std::string name;
	std::string sha256;
	const unsigned char *data = nullptr;
	size_t size = 0;
};
// A set file opened for the load, which no other opener may write, rename or delete until it closes.
struct HeldFile {
	std::string path;
	windows_rules::FileId id;
	FileHandle handle;
};
// What an existing set is: INVALID only on evidence; UNCHECKED when a check could
// not run, which leaves the set as it is and fails the base.
enum class SetState : uint8_t { VALID, MISSING, INVALID, UNCHECKED };
// The set check for `set_dir`: exactly `files`, each opened with
// FILE_SHARE_READ alone and passing HandleRule and its SHA-256, read through that handle. On VALID
// the handles go to `held` when it is given. `where` and `reason` (a rule, no path) otherwise.
SetState CheckSet(const std::string &set_dir, const std::vector<SetFile> &files, const std::string &user,
                  std::string &where, std::string &reason, std::vector<HeldFile> *held = nullptr);

// Busy files are tried this often, this far apart; a set-aside folder is kept
// this long before the sweep removes it.
constexpr int BUSY_TRIES = 20;
constexpr unsigned long BUSY_WAIT_MILLISECONDS = 50;
constexpr long long STALE_SECONDS = 3600;

enum class InstallResult : uint8_t { INSTALLED, BYTES_CORRUPT, FAILED };
// The install under `root`: a valid set is used without the lock. Otherwise, under `<root>\.lock`
// (LockFileEx, until the deadline): the sweep, the set checked again, the bytes hashed, a new set
// written into an owner-only staging folder, an invalid set moved aside, and the staging folder
// moved into place without replacing anything. Sets `set_dir`; `where` and `reason` on failure.
InstallResult InstallSet(const std::string &root, const std::string &set_sha256, const std::vector<SetFile> &files,
                         const std::string &user, int lock_deadline_milliseconds, std::string &set_dir,
                         std::string &where, std::string &reason);
// Removes `user`'s staging folders, whatever their age, and set-aside folders older than STALE_SECONDS
// under `root`, under its lock, without following a link; other accounts' and other set folders stay.
void SweepRoot(const std::string &root, const std::string &user);

// Where the ODBC client writes its trust file for a set loaded from `base`: `<base>\trust`,
// else the per-user cache's `trust`, each made with MakeOwnDirectory; empty when neither is usable.
std::string ChooseTrustDirectory(const std::string &base, const BaseInputs &inputs, const std::string &user);

} // namespace windows_driver
} // namespace redshift_driver
} // namespace duckdb
