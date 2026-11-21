//===----------------------------------------------------------------------===//
//                         DuckDB - Redshift
//
// test/client/driver/windows_temp_dir.hpp
//
// The Windows loader tests' folders: a private folder under the final path of the temporary folder,
// never a raw TEMP, which can hold an 8.3 name or a link, and the access-list and
// junction fixtures the tests build there without elevation.
//===----------------------------------------------------------------------===//

#pragma once

#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>

#include <aclapi.h>
#include <sddl.h>

#include "catch.hpp"
#include "driver_windows.hpp"

#include <cstdlib>
#include <filesystem>
#include <random>
#include <string>
#include <vector>

namespace redshift_driver_test {

using duckdb::redshift_driver::windows_driver::CurrentUserSid;
using duckdb::redshift_driver::windows_driver::MakeOwnDirectory;
using duckdb::redshift_driver::windows_rules::Narrow;
using duckdb::redshift_driver::windows_rules::Widen;

inline std::string RandomName() {
	std::random_device random;
	return std::to_string(random()) + std::to_string(random());
}

// The temporary folder as the loader's final-path check reads it: long names, with any link or
// junction on the way resolved, so a host whose TEMP is under one still passes the base check.
inline std::string FinalTempFolder() {
	wchar_t temp[MAX_PATH + 1];
	const DWORD length = GetTempPathW(MAX_PATH + 1, temp);
	REQUIRE(length > 0);
	const DWORD share = FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE;
	const HANDLE folder =
	    CreateFileW(temp, FILE_READ_ATTRIBUTES, share, nullptr, OPEN_EXISTING, FILE_FLAG_BACKUP_SEMANTICS, nullptr);
	REQUIRE(folder != INVALID_HANDLE_VALUE);
	std::vector<wchar_t> final_path(32768);
	const DWORD final_length =
	    GetFinalPathNameByHandleW(folder, final_path.data(), 32768, FILE_NAME_NORMALIZED | VOLUME_NAME_DOS);
	CloseHandle(folder);
	REQUIRE(final_length > 0);
	REQUIRE(final_length < 32768);
	std::wstring resolved(final_path.data(), final_length);
	const std::wstring prefix = L"\\\\?\\";
	if (resolved.compare(0, prefix.size(), prefix) == 0) {
		resolved = resolved.substr(prefix.size());
	}
	std::string base = Narrow(resolved);
	if (base.back() == '\\') {
		base.pop_back();
	}
	return base;
}

class WindowsTempDir {
public:
	WindowsTempDir() {
		path_ = FinalTempFolder() + "\\redshift-driver-test-" + RandomName();
		REQUIRE(MakeOwnDirectory(path_, CurrentUserSid()).empty());
	}
	~WindowsTempDir() {
		std::error_code ignored;
		std::filesystem::remove_all(std::filesystem::path(Widen(path_)), ignored);
	}
	WindowsTempDir(const WindowsTempDir &) = delete;
	WindowsTempDir &operator=(const WindowsTempDir &) = delete;
	const std::string &path() const {
		return path_;
	}
	// A new owner-only folder `name` inside this one.
	std::string Make(const std::string &name) const {
		const auto dir = path_ + "\\" + name;
		REQUIRE(MakeOwnDirectory(dir, CurrentUserSid()).empty());
		return dir;
	}

private:
	std::string path_;
};

// Replaces the access list of `path` with the protected one `sddl` gives (`<user>` is the account's
// SID), as an administrator or another account could have set it.
inline void SetAccessList(const std::string &path, std::string sddl) {
	const auto user = CurrentUserSid();
	for (size_t at = sddl.find("<user>"); at != std::string::npos; at = sddl.find("<user>")) {
		sddl.replace(at, 6, user);
	}
	PSECURITY_DESCRIPTOR descriptor = nullptr;
	REQUIRE(ConvertStringSecurityDescriptorToSecurityDescriptorA(sddl.c_str(), SDDL_REVISION_1, &descriptor, nullptr));
	BOOL present = FALSE, defaulted = FALSE;
	PACL dacl = nullptr;
	REQUIRE(GetSecurityDescriptorDacl(descriptor, &present, &dacl, &defaulted));
	const DWORD status = SetNamedSecurityInfoA(const_cast<char *>(path.c_str()), SE_FILE_OBJECT,
	                                           DACL_SECURITY_INFORMATION | PROTECTED_DACL_SECURITY_INFORMATION, nullptr,
	                                           nullptr, dacl, nullptr);
	LocalFree(descriptor);
	REQUIRE(status == ERROR_SUCCESS);
}

// Writes `sddl`'s access list without reading the one in place, which a deny entry that takes
// READ_CONTROL from the owner would refuse.
inline void ReplaceAccessList(const std::string &path, std::string sddl) {
	const auto user = CurrentUserSid();
	for (size_t at = sddl.find("<user>"); at != std::string::npos; at = sddl.find("<user>")) {
		sddl.replace(at, 6, user);
	}
	PSECURITY_DESCRIPTOR descriptor = nullptr;
	REQUIRE(ConvertStringSecurityDescriptorToSecurityDescriptorA(sddl.c_str(), SDDL_REVISION_1, &descriptor, nullptr));
	const BOOL replaced = SetFileSecurityA(path.c_str(), DACL_SECURITY_INFORMATION, descriptor);
	LocalFree(descriptor);
	REQUIRE(replaced);
}

// Whether this process holds SeBackupPrivilege enabled, as SYSTEM does under a shell that enables
// it (MSYS bash): a backup-semantics open then reads past any deny entry.
inline bool BackupPrivilegeEnabled() {
	HANDLE token = nullptr;
	if (!OpenProcessToken(GetCurrentProcess(), TOKEN_QUERY, &token)) {
		return false;
	}
	DWORD size = 0;
	GetTokenInformation(token, TokenPrivileges, nullptr, 0, &size);
	std::vector<unsigned char> buffer(size);
	LUID backup;
	bool enabled = false;
	if (size > 0 && LookupPrivilegeValueW(nullptr, L"SeBackupPrivilege", &backup) &&
	    GetTokenInformation(token, TokenPrivileges, buffer.data(), size, &size)) {
		const auto privileges = reinterpret_cast<const TOKEN_PRIVILEGES *>(buffer.data());
		for (DWORD index = 0; index < privileges->PrivilegeCount; index++) {
			const auto &entry = privileges->Privileges[index];
			if (entry.Luid.LowPart == backup.LowPart && entry.Luid.HighPart == backup.HighPart) {
				enabled = (entry.Attributes & SE_PRIVILEGE_ENABLED) != 0;
			}
		}
	}
	CloseHandle(token);
	return enabled;
}

// A directory symbolic link at `link` to `target`. The account the tests run as needs
// SeCreateSymbolicLinkPrivilege for it; without it the case fails.
inline void MakeDirectoryLink(const std::string &link, const std::string &target) {
	REQUIRE(CreateSymbolicLinkW(Widen(link).c_str(), Widen(target).c_str(), SYMBOLIC_LINK_FLAG_DIRECTORY) != 0);
}

// A directory junction at `link` to `target`, which a standard account may make.
inline void MakeJunction(const std::string &link, const std::string &target) {
	const auto command = "mklink /J \"" + link + "\" \"" + target + "\" >NUL";
	REQUIRE(std::system(command.c_str()) == 0);
}

// That `dir`'s access list is protected and has one entry, which grants the current account.
inline void CheckOwnerOnlyList(const std::string &dir) {
	INFO(dir);
	PACL dacl = nullptr;
	PSECURITY_DESCRIPTOR descriptor = nullptr;
	REQUIRE(GetNamedSecurityInfoA(dir.c_str(), SE_FILE_OBJECT, DACL_SECURITY_INFORMATION, nullptr, nullptr, &dacl,
	                              nullptr, &descriptor) == ERROR_SUCCESS);
	SECURITY_DESCRIPTOR_CONTROL control = 0;
	DWORD revision = 0;
	REQUIRE(GetSecurityDescriptorControl(descriptor, &control, &revision));
	CHECK((control & SE_DACL_PROTECTED) != 0);
	REQUIRE(dacl->AceCount == 1);
	ACCESS_ALLOWED_ACE *entry = nullptr;
	REQUIRE(GetAce(dacl, 0, reinterpret_cast<void **>(&entry)));
	LPSTR sid = nullptr;
	REQUIRE(ConvertSidToStringSidA(&entry->SidStart, &sid));
	CHECK(std::string(sid) == CurrentUserSid());
	LocalFree(sid);
	LocalFree(descriptor);
}

} // namespace redshift_driver_test
