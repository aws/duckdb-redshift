//===----------------------------------------------------------------------===//
// The Windows private directory checks: the base folders, the ancestor and own-folder
// rules, owner-only folders and the trust directory. Includes no header that includes the interface
// header, since <aclapi.h> and <shlobj.h> need the Win32 macros it removes.
//===----------------------------------------------------------------------===//

#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>

#include <aclapi.h>
#include <sddl.h>
#include <shlobj.h>

#include "driver_windows.hpp"

#include <string>
#include <utility>
#include <vector>

#pragma comment(lib, "advapi32.lib")
#pragma comment(lib, "ole32.lib")
#pragma comment(lib, "shell32.lib")

namespace duckdb {
namespace redshift_driver {
namespace windows_driver {

using windows_rules::Narrow;
using windows_rules::PathFormRule;
using windows_rules::PathPlace;
using windows_rules::SamePath;
using windows_rules::Widen;

static_assert(windows_rules::DRIVE_KIND_UNKNOWN == DRIVE_UNKNOWN &&
                  windows_rules::DRIVE_KIND_NO_ROOT_DIR == DRIVE_NO_ROOT_DIR &&
                  windows_rules::DRIVE_KIND_REMOVABLE == DRIVE_REMOVABLE &&
                  windows_rules::DRIVE_KIND_FIXED == DRIVE_FIXED && windows_rules::DRIVE_KIND_REMOTE == DRIVE_REMOTE &&
                  windows_rules::DRIVE_KIND_CDROM == DRIVE_CDROM && windows_rules::DRIVE_KIND_RAMDISK == DRIVE_RAMDISK,
              "the drive rule's numbers are GetDriveTypeW's");

namespace {

std::string SidText(PSID sid) {
	LPSTR text = nullptr;
	if (sid == nullptr || !ConvertSidToStringSidA(sid, &text)) {
		return "";
	}
	std::string result(text);
	LocalFree(text);
	return result;
}

} // namespace

FileHandle::FileHandle(void *handle) : handle_(handle == INVALID_HANDLE_VALUE ? nullptr : handle) {
}

FileHandle::~FileHandle() {
	if (handle_ != nullptr) {
		CloseHandle(handle_);
	}
}

FileHandle::FileHandle(FileHandle &&other) noexcept : handle_(other.handle_) {
	other.handle_ = nullptr;
}

FileHandle &FileHandle::operator=(FileHandle &&other) noexcept {
	if (this != &other) {
		if (handle_ != nullptr) {
			CloseHandle(handle_);
		}
		handle_ = other.handle_;
		other.handle_ = nullptr;
	}
	return *this;
}

std::string ErrorText(unsigned long code) {
	return "Windows error " + std::to_string(code);
}

std::string CurrentUserSid() {
	HANDLE token = nullptr;
	if (!OpenThreadToken(GetCurrentThread(), TOKEN_QUERY, TRUE, &token) &&
	    !OpenProcessToken(GetCurrentProcess(), TOKEN_QUERY, &token)) {
		return "";
	}
	FileHandle held(token);
	DWORD size = 0;
	GetTokenInformation(token, TokenUser, nullptr, 0, &size);
	std::vector<unsigned char> buffer(size);
	if (size == 0 || !GetTokenInformation(token, TokenUser, buffer.data(), size, &size)) {
		return "";
	}
	return SidText(reinterpret_cast<TOKEN_USER *>(buffer.data())->User.Sid);
}

unsigned SystemDriveType(const wchar_t *root) {
	return GetDriveTypeW(root);
}

const char *BaseRole(BaseKind kind) {
	switch (kind) {
	case BaseKind::OVERRIDE:
		return "REDSHIFT_DRIVER_DIR";
	case BaseKind::EXTENSION:
		return "the extension's directory";
	case BaseKind::CACHE:
		return "the per-user cache";
	}
	return "";
}

namespace {

constexpr DWORD LONG_PATH_BUFFER = 32768;
static_assert(windows_rules::ATTRIBUTE_DIRECTORY == FILE_ATTRIBUTE_DIRECTORY &&
                  windows_rules::ATTRIBUTE_REPARSE_POINT == FILE_ATTRIBUTE_REPARSE_POINT &&
                  windows_rules::ATTRIBUTE_OFFLINE == FILE_ATTRIBUTE_OFFLINE &&
                  windows_rules::ATTRIBUTE_RECALL_ON_OPEN == FILE_ATTRIBUTE_RECALL_ON_OPEN &&
                  windows_rules::ATTRIBUTE_PINNED == FILE_ATTRIBUTE_PINNED &&
                  windows_rules::ATTRIBUTE_UNPINNED == FILE_ATTRIBUTE_UNPINNED &&
                  windows_rules::ATTRIBUTE_RECALL_ON_DATA_ACCESS == FILE_ATTRIBUTE_RECALL_ON_DATA_ACCESS,
              "the link rule's numbers are winnt.h's");

// The owner and access list of the open `handle`, for SecurityRule; the error when they cannot be read.
DWORD ReadSecurity(HANDLE handle, windows_rules::SecurityFacts &facts) {
	PSID owner = nullptr;
	PACL dacl = nullptr;
	PSECURITY_DESCRIPTOR descriptor = nullptr;
	const DWORD error = GetSecurityInfo(handle, SE_FILE_OBJECT, OWNER_SECURITY_INFORMATION | DACL_SECURITY_INFORMATION,
	                                    &owner, nullptr, &dacl, nullptr, &descriptor);
	if (error != ERROR_SUCCESS) {
		return error;
	}
	facts.owner = SidText(owner);
	facts.has_dacl = dacl != nullptr;
	for (DWORD i = 0; dacl != nullptr && i < dacl->AceCount; i++) {
		ACE_HEADER *header = nullptr;
		if (!GetAce(dacl, i, reinterpret_cast<void **>(&header))) {
			facts.unknown_entry = true;
			break;
		}
		if (header->AceType != ACCESS_ALLOWED_ACE_TYPE && header->AceType != ACCESS_DENIED_ACE_TYPE) {
			facts.unknown_entry = true;
			continue;
		}
		// Both types have the same layout: the header, the mask, then the SID.
		auto *entry = reinterpret_cast<ACCESS_ALLOWED_ACE *>(header);
		windows_rules::AccessEntry access;
		access.sid = SidText(&entry->SidStart);
		access.allow = header->AceType == ACCESS_ALLOWED_ACE_TYPE;
		access.inherit_only = (header->AceFlags & INHERIT_ONLY_ACE) != 0;
		access.mask = entry->Mask;
		facts.entries.push_back(access);
	}
	LocalFree(descriptor);
	return facts.owner.empty() ? ERROR_INVALID_SID : ERROR_SUCCESS;
}

std::string SecurityRuleOf(HANDLE handle, PathPlace place, const std::string &user) {
	windows_rules::SecurityFacts facts;
	const DWORD error = ReadSecurity(handle, facts);
	if (error != ERROR_SUCCESS) {
		return "has an owner or access control list that cannot be read: " + ErrorText(error);
	}
	return windows_rules::SecurityRule(facts, place, user);
}

HANDLE OpenFolder(const std::string &path, DWORD flags) {
	return CreateFileW(Widen(path).c_str(), READ_CONTROL | FILE_READ_ATTRIBUTES,
	                   FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr, OPEN_EXISTING,
	                   FILE_FLAG_BACKUP_SEMANTICS | flags, nullptr);
}

// The folder `path` is in; the drive root for a name directly under it.
std::wstring ParentOf(const std::wstring &path) {
	const auto slash = path.find_last_of(L'\\');
	return slash == std::wstring::npos ? path : path.substr(0, slash <= 2 ? 3 : slash);
}

} // namespace

std::string HandleRule(void *handle, const std::string &path, bool directory, const std::string &user, bool *unreadable,
                       windows_rules::FileId *id) {
	bool ignored = false;
	bool &cannot_read = unreadable != nullptr ? *unreadable : ignored;
	cannot_read = false;
	BY_HANDLE_FILE_INFORMATION info;
	if (!GetFileInformationByHandle(handle, &info)) {
		cannot_read = true;
		return "cannot be examined: " + ErrorText(GetLastError());
	}
	const std::string attribute_rule = windows_rules::AttributeRule(info.dwFileAttributes, directory);
	if (!attribute_rule.empty()) {
		return attribute_rule;
	}
	if (!directory && info.nNumberOfLinks != 1) {
		return "has more than one hard link";
	}
	std::vector<wchar_t> final_path(LONG_PATH_BUFFER);
	const DWORD length =
	    GetFinalPathNameByHandleW(handle, final_path.data(), LONG_PATH_BUFFER, FILE_NAME_NORMALIZED | VOLUME_NAME_DOS);
	if (length == 0 || length >= LONG_PATH_BUFFER) {
		// An unread path is no evidence of a link: unchecked, not invalid.
		cannot_read = true;
		return "cannot be examined: " + ErrorText(length == 0 ? GetLastError() : DWORD(ERROR_INSUFFICIENT_BUFFER));
	}
	const std::wstring prefix = L"\\\\?\\";
	std::wstring resolved(final_path.data(), length);
	if (resolved.compare(0, prefix.size(), prefix) == 0) {
		resolved = resolved.substr(prefix.size());
	}
	if (!SamePath(Narrow(resolved), path)) {
		return "resolves to another path, through a link or a renamed folder";
	}
	windows_rules::SecurityFacts facts;
	const DWORD error = ReadSecurity(handle, facts);
	if (error != ERROR_SUCCESS) {
		cannot_read = true;
		return "has an owner or access control list that cannot be read: " + ErrorText(error);
	}
	if (id != nullptr) {
		id->volume = info.dwVolumeSerialNumber;
		id->index = (static_cast<uint64_t>(info.nFileIndexHigh) << 32) | info.nFileIndexLow;
	}
	return windows_rules::SecurityRule(facts, PathPlace::OWN, user);
}

std::string OwnDirectoryRule(const std::string &dir, const std::string &user, bool *unreadable) {
	FileHandle folder(OpenFolder(dir, FILE_FLAG_OPEN_REPARSE_POINT));
	if (!folder.valid()) {
		if (unreadable != nullptr) {
			*unreadable = true;
		}
		return "cannot be opened: " + ErrorText(GetLastError());
	}
	return HandleRule(folder.get(), dir, true, user, unreadable, nullptr);
}

bool OwnedBy(const std::string &path, const std::string &user) {
	FileHandle entry(OpenFolder(path, FILE_FLAG_OPEN_REPARSE_POINT));
	windows_rules::SecurityFacts facts;
	return entry.valid() && ReadSecurity(entry.get(), facts) == ERROR_SUCCESS && facts.owner == user;
}

bool FileIdOf(const std::string &path, windows_rules::FileId &id) {
	FileHandle file(OpenFolder(path, 0));
	BY_HANDLE_FILE_INFORMATION info;
	if (!file.valid() || !GetFileInformationByHandle(file.get(), &info)) {
		return false;
	}
	id.volume = info.dwVolumeSerialNumber;
	id.index = (static_cast<uint64_t>(info.nFileIndexHigh) << 32) | info.nFileIndexLow;
	return true;
}

std::string MakeOwnDirectory(const std::string &dir, const std::string &user) {
	// Owned by the current account, not an elevated token's default owner (Administrators), so the
	// sweep knows the folder as its own; protected (P), so nothing is inherited; full access for
	// that account alone.
	const std::string sddl = "O:" + user + "D:P(A;OICI;FA;;;" + user + ")";
	PSECURITY_DESCRIPTOR descriptor = nullptr;
	if (!ConvertStringSecurityDescriptorToSecurityDescriptorA(sddl.c_str(), SDDL_REVISION_1, &descriptor, nullptr)) {
		return "cannot be given an owner-only access list: " + ErrorText(GetLastError());
	}
	SECURITY_ATTRIBUTES attributes = {static_cast<DWORD>(sizeof(SECURITY_ATTRIBUTES)), descriptor, FALSE};
	const BOOL created = CreateDirectoryW(Widen(dir).c_str(), &attributes);
	const DWORD error = GetLastError();
	LocalFree(descriptor);
	if (!created && error != ERROR_ALREADY_EXISTS) {
		return "cannot be created: " + ErrorText(error);
	}
	return OwnDirectoryRule(dir, user);
}

bool CheckBase(const std::string &base, bool create, const BaseInputs &inputs, const std::string &user,
               std::string &where, std::string &reason) {
	where = base;
	const std::wstring drive_root = Widen(base.substr(0, 3));
	reason = windows_rules::DriveTypeRule(inputs.drive_type(drive_root.c_str()));
	if (!reason.empty()) {
		return false;
	}
	// Every folder above the base, from the drive root down, by the ancestor rule.
	std::vector<std::string> ancestors;
	if (base.size() > 3) {
		ancestors.push_back(base.substr(0, 3));
	}
	for (size_t slash = base.find('\\', 3); slash != std::string::npos; slash = base.find('\\', slash + 1)) {
		ancestors.push_back(base.substr(0, slash));
	}
	for (const auto &ancestor : ancestors) {
		FileHandle folder(OpenFolder(ancestor, 0));
		const auto rule = folder.valid() ? SecurityRuleOf(folder.get(), PathPlace::ANCESTOR, user)
		                                 : "cannot be opened: " + ErrorText(GetLastError());
		if (!rule.empty()) {
			where = ancestor;
			reason = "a parent folder " + rule;
			return false;
		}
	}
	if (create && GetFileAttributesW(Widen(base).c_str()) == INVALID_FILE_ATTRIBUTES) {
		const auto rule = MakeOwnDirectory(base, user);
		if (!rule.empty()) {
			reason = "the folder " + rule;
			return false;
		}
	}
	std::vector<wchar_t> long_form(LONG_PATH_BUFFER);
	const DWORD length = GetLongPathNameW(Widen(base).c_str(), long_form.data(), LONG_PATH_BUFFER);
	if (length == 0 || length >= LONG_PATH_BUFFER) {
		reason = "the folder cannot be opened: " + ErrorText(GetLastError());
		return false;
	}
	if (!SamePath(Narrow(std::wstring(long_form.data(), length)), base)) {
		reason = "the path has an 8.3 short name that differs from its long form";
		return false;
	}
	const auto rule = OwnDirectoryRule(base, user);
	if (!rule.empty()) {
		reason = "the folder " + rule;
		return false;
	}
	return true;
}

BaseInputs CurrentBaseInputs() {
	BaseInputs inputs;
	// The environment block, as the driver interface reads it, so a value set after LOAD is seen now.
	const wchar_t *name = L"REDSHIFT_DRIVER_DIR";
	SetLastError(ERROR_SUCCESS);
	const DWORD size = GetEnvironmentVariableW(name, nullptr, 0);
	DWORD error = GetLastError();
	inputs.override_set = size != 0 || error != ERROR_ENVVAR_NOT_FOUND;
	if (size > 0) {
		std::vector<wchar_t> value(size);
		SetLastError(ERROR_SUCCESS);
		const DWORD read = GetEnvironmentVariableW(name, value.data(), size);
		error = read < size ? GetLastError() : ERROR_INSUFFICIENT_BUFFER;
		if (error == ERROR_SUCCESS) {
			inputs.override_value.assign(value.data(), read);
		}
	}
	if (inputs.override_set && error != ERROR_SUCCESS) {
		inputs.override_failure = "it cannot be read: " + ErrorText(error);
	}
	// The file this code is in: the loadable extension, or the program that links it.
	HMODULE module = nullptr;
	std::vector<wchar_t> file(LONG_PATH_BUFFER);
	if (!GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
	                        reinterpret_cast<LPCWSTR>(&CurrentBaseInputs), &module)) {
		inputs.extension_failure = "its file cannot be found: " + ErrorText(GetLastError());
	} else {
		const DWORD length = GetModuleFileNameW(module, file.data(), LONG_PATH_BUFFER);
		if (length == 0 || length >= LONG_PATH_BUFFER) {
			inputs.extension_failure = "its file's name cannot be read: " + ErrorText(GetLastError());
		} else {
			inputs.extension_file.assign(file.data(), length);
		}
	}
	PWSTR local_app_data = nullptr;
	if (SHGetKnownFolderPath(FOLDERID_LocalAppData, KF_FLAG_DEFAULT, nullptr, &local_app_data) == S_OK) {
		inputs.per_user_base = std::wstring(local_app_data) + L"\\duckdb-redshift";
	}
	CoTaskMemFree(local_app_data);
	return inputs;
}

DriverRoots::DriverRoots(const BaseInputs &inputs, std::string user, std::vector<std::string> file_names)
    : inputs_(inputs), user_(std::move(user)), file_names_(std::move(file_names)) {
	const auto candidate = [&](BaseKind kind, const std::wstring &path, std::string refusal) {
		if (refusal.empty() && !PathFormRule(path).empty()) {
			refusal = "the path " + PathFormRule(path);
		}
		candidates_.push_back({kind, Narrow(path), std::move(refusal)});
	};
	if (inputs.override_set) {
		// The only base when set, with no fallback.
		candidate(BaseKind::OVERRIDE, inputs.override_value, inputs.override_failure);
		return;
	}
	const auto &file = inputs.extension_file;
	std::string refusal = inputs.extension_failure;
	if (refusal.empty() && !PathFormRule(file).empty()) {
		refusal = "the path " + PathFormRule(file);
	}
	if (refusal.empty() && file.size() > windows_rules::MAX_PATH_CHARACTERS) {
		refusal = "the extension's path is longer than 259 characters";
	}
	candidate(BaseKind::EXTENSION, ParentOf(file), refusal);
	candidate(BaseKind::CACHE, inputs.per_user_base,
	          inputs.per_user_base.empty() ? "there is no known folder for local application data" : "");
}

bool DriverRoots::Next(std::string &root) {
	while (next_ < candidates_.size()) {
		const auto &candidate = candidates_[next_++];
		kind_ = candidate.kind;
		base_ = candidate.base;
		if (!candidate.refusal.empty()) {
			Fail(candidate.base, candidate.refusal);
			continue;
		}
		if (windows_rules::LongestPathUnder(base_, file_names_) > windows_rules::MAX_PATH_CHARACTERS) {
			Fail(base_, "a path the driver would use there is longer than 259 characters");
			continue;
		}
		std::string where, reason;
		if (!CheckBase(base_, kind_ == BaseKind::CACHE, inputs_, user_, where, reason)) {
			Fail(where, reason);
			continue;
		}
		root = windows_rules::JoinPath(base_, windows_rules::DRIVER_ROOT_NAME);
		const auto rule = MakeOwnDirectory(root, user_);
		if (!rule.empty()) {
			Fail(root, "the driver root " + rule);
			continue;
		}
		return true;
	}
	return false;
}

void DriverRoots::Fail(const std::string &where, const std::string &rule) {
	attempts.push_back({where, std::string(BaseRole(kind_)) + ": " + rule});
}

const char *DriverRoots::Role() const {
	return BaseRole(kind_);
}

std::string ChooseTrustDirectory(const std::string &base, const BaseInputs &inputs, const std::string &user) {
	const auto usable = [&](const std::string &dir) {
		return windows_rules::TrustDirectoryRule(dir).empty() && MakeOwnDirectory(dir, user).empty();
	};
	const auto in_base = windows_rules::JoinPath(base, windows_rules::TRUST_DIRECTORY_NAME);
	if (usable(in_base)) {
		return in_base;
	}
	// The per-user cache's, made and checked as a base, for a read-only base such as an administrator's.
	const auto cache = Narrow(inputs.per_user_base);
	const auto in_cache = windows_rules::JoinPath(cache, windows_rules::TRUST_DIRECTORY_NAME);
	std::string where, reason;
	if (!inputs.per_user_base.empty() && PathFormRule(inputs.per_user_base).empty() && !SamePath(cache, base) &&
	    CheckBase(cache, true, inputs, user, where, reason) && usable(in_cache)) {
		return in_cache;
	}
	return "";
}

} // namespace windows_driver
} // namespace redshift_driver
} // namespace duckdb
