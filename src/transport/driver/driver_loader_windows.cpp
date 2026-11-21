#include "driver_loader_windows.hpp"

#include <psapi.h>
#include <winver.h>

#include <algorithm>
#include <cstdint>
#include <utility>

#pragma comment(lib, "advapi32.lib")
#pragma comment(lib, "version.lib")

namespace duckdb {
namespace redshift_driver {
namespace windows_driver {

using windows_rules::Narrow;
using windows_rules::Widen;

namespace {

// How often a base whose set something changed is installed again before the next base is tried.
constexpr int INSTALL_TRIES = 3;
constexpr DWORD MODULE_NAME_BUFFER = 32768;

DriverError Error(DriverErrorKind kind, std::string message, std::string where = "", std::string why = "") {
	DriverError error;
	error.kind = kind;
	error.message = std::move(message);
	if (!why.empty()) {
		error.attempts.push_back({std::move(where), std::move(why)});
	}
	return error;
}

DriverError LoadFailed(const char *role, const std::string &what, const std::string &where, const std::string &why) {
	return Error(DriverErrorKind::LOAD_FAILED,
	             std::string("the Redshift ODBC driver could not be loaded (") + role + "): " + what, where, why);
}

// The module's path, or empty when it cannot be read. File calls take it wide: Narrow turns each
// character that is not ASCII into '?', which names no file.
std::wstring ModuleWidePath(HMODULE module) {
	std::vector<wchar_t> name(MODULE_NAME_BUFFER);
	const DWORD length = GetModuleFileNameW(module, name.data(), MODULE_NAME_BUFFER);
	return length == 0 || length >= MODULE_NAME_BUFFER ? std::wstring() : std::wstring(name.data(), length);
}

std::string ModulePath(HMODULE module) {
	return Narrow(ModuleWidePath(module));
}

constexpr const char *MODULES_UNLISTED = "the process's modules could not be listed";

// The process's modules into `modules`, each identified by file when its name is one of `names`.
// False with the system's error in `why` when the list cannot be read.
bool LoadedModules(const std::vector<std::string> &names, std::vector<windows_rules::LoadedModule> &modules,
                   std::string &why) {
	std::vector<HMODULE> handles(1024);
	DWORD list_error = ERROR_SUCCESS;
	const auto list = [&](HMODULE *data, size_t capacity, size_t &needed) {
		DWORD bytes = 0;
		const bool listed = K32EnumProcessModules(GetCurrentProcess(), data,
		                                          static_cast<DWORD>(capacity * sizeof(HMODULE)), &bytes) != FALSE;
		list_error = listed ? ERROR_SUCCESS : GetLastError();
		needed = bytes / sizeof(HMODULE);
		return listed;
	};
	if (!windows_rules::ListModuleHandles(list, handles)) {
		why = ErrorText(list_error);
		return false;
	}
	for (HMODULE handle : handles) {
		windows_rules::LoadedModule module;
		module.path = ModulePath(handle);
		const auto name = windows_rules::BaseName(module.path);
		if (std::any_of(names.begin(), names.end(),
		                [&](const std::string &n) { return windows_rules::SamePath(n, name); })) {
			module.identified = FileIdOf(module.path, module.id);
		}
		modules.push_back(module);
	}
	return true;
}

std::vector<std::string> SetFileNames(const std::vector<SetFile> &files) {
	std::vector<std::string> names;
	for (const auto &file : files) {
		names.push_back(file.name);
	}
	return names;
}

// System32's path, or empty when it cannot be read.
std::string SystemFolder() {
	wchar_t system[MAX_PATH];
	const UINT length = GetSystemDirectoryW(system, MAX_PATH);
	return length == 0 || length >= MAX_PATH ? "" : Narrow(std::wstring(system, length));
}

// System32's DbgHelp.dll by full path; empty when the system folder cannot be read, so every copy
// is one from elsewhere.
std::string SystemDbgHelp() {
	return windows_rules::SystemFilePath(SystemFolder(), windows_rules::DBGHELP);
}

bool IsLibrary(const std::string &name) {
	return name.size() > 4 && windows_rules::SamePath(name.substr(name.size() - 4), ".dll");
}

enum class SetLoad : uint8_t { LOADED, FINAL, CHANGED, BASE_FAILED };

// The set check, the load and the post-load check for `set_dir`. FINAL once LoadLibraryExW was called.
SetLoad LoadSet(const LoaderConfig &config, const std::string &set_dir, const LoadInputs &inputs, const char *role,
                bool &platform_loader_called, LoadedDriver &out, DriverError &error, std::string &where,
                std::string &reason) {
	std::vector<HeldFile> held;
	const auto state = CheckSet(set_dir, config.files, inputs.user, where, reason, &held);
	if (state != SetState::VALID) {
		return state == SetState::UNCHECKED ? SetLoad::BASE_FAILED : SetLoad::CHANGED;
	}
	if (held.empty()) {
		reason = "the driver set lists no file";
		return SetLoad::BASE_FAILED;
	}
	// The libraries first, in the lock's order, and the driver last. The order does not matter: each
	// import of a set file resolves from the set's folder by LOAD_LIBRARY_SEARCH_DLL_LOAD_DIR, and the
	// post-load check proves it. The handles stay open across every load. held[i] is files[i]'s:
	// CheckSet hands the handles over, in the list's order, only once every file passed.
	std::vector<const HeldFile *> order;
	for (size_t i = 1; i < held.size(); i++) {
		if (IsLibrary(config.files[i].name)) {
			order.push_back(&held[i]);
		}
	}
	order.push_back(&held[0]);
	std::vector<std::string> paths;
	std::vector<windows_rules::CheckedFile> checked;
	for (const auto *file : order) {
		paths.push_back(file->path);
		checked.push_back({file->path, file->id});
	}
	if (inputs.before_platform_loader != nullptr) {
		inputs.before_platform_loader(paths);
	}
	platform_loader_called = true;
	const auto system_dbghelp = config.system_dbghelp ? SystemDbgHelp() : "";
	// System32's copy first, so the driver's bare-name load binds to it.
	if (config.system_dbghelp &&
	    LoadLibraryExW(Widen(system_dbghelp).c_str(), nullptr, LOAD_LIBRARY_SEARCH_SYSTEM32) == nullptr) {
		error = LoadFailed(role, "the system's loader refused System32's DbgHelp.dll", system_dbghelp,
		                   ErrorText(GetLastError()));
		return SetLoad::FINAL;
	}
	HMODULE driver = nullptr;
	for (const auto *file : order) {
		driver = LoadLibraryExW(Widen(file->path).c_str(), nullptr,
		                        LOAD_LIBRARY_SEARCH_DLL_LOAD_DIR | LOAD_LIBRARY_SEARCH_SYSTEM32);
		if (driver == nullptr) {
			error = LoadFailed(role,
			                   "the system's loader refused a file of the driver set: a library it needs is missing "
			                   "or incompatible",
			                   file->path, ErrorText(GetLastError()));
			return SetLoad::FINAL;
		}
	}
	// Final like every outcome from here: driver code has run, so a retry would load it again.
	std::vector<windows_rules::LoadedModule> modules;
	std::string unlisted;
	if (!LoadedModules(SetFileNames(config.files), modules, unlisted)) {
		error = LoadFailed(role,
		                   std::string(MODULES_UNLISTED) +
		                       ", so the loaded files cannot be checked; restart the program and connect again",
		                   set_dir, unlisted);
		return SetLoad::FINAL;
	}
	const auto rule = windows_rules::PostLoadRule(modules, checked);
	if (!rule.empty()) {
		error = LoadFailed(role,
		                   "another module answers to the name of a driver file, or the loaded file is not the "
		                   "one that was checked",
		                   set_dir, rule);
		return SetLoad::FINAL;
	}
	const std::string dbghelp_rule =
	    config.system_dbghelp ? windows_rules::SystemCopyRule(modules, windows_rules::DBGHELP, system_dbghelp) : "";
	if (!dbghelp_rule.empty()) {
		error = LoadFailed(role, "a DbgHelp.dll other than System32's is loaded, which the driver would use", set_dir,
		                   dbghelp_rule);
		return SetLoad::FINAL;
	}
	OdbcApi api;
#define REDSHIFT_RESOLVE(name) api.name = reinterpret_cast<decltype(api.name)>(GetProcAddress(driver, "SQL" #name));
	REDSHIFT_ODBC_ENTRY_POINTS(REDSHIFT_RESOLVE)
#undef REDSHIFT_RESOLVE
	if (!api.Complete()) {
		error = Error(DriverErrorKind::MISSING_ENTRY_POINT,
		              std::string("the Redshift ODBC driver does not export ") + api.FirstMissing(), set_dir,
		              "an entry point is missing");
		return SetLoad::FINAL;
	}
	out.api = api;
	out.identity = config.identity;
	out.identity.files.clear();
	for (const auto &file : config.files) {
		out.identity.files.push_back({file.name, file.sha256});
	}
	out.identity.directory = set_dir;
	out.refused_environment = config.refused_environment;
	out.overridden_environment = config.overridden_environment;
	return SetLoad::LOADED;
}

enum class VersionRead : uint8_t { MISSING, UNREADABLE, READ };

VersionRead FileVersion(const std::wstring &path, uint64_t &version) {
	DWORD ignored = 0;
	const DWORD size = GetFileVersionInfoSizeW(path.c_str(), &ignored);
	if (size == 0) {
		const DWORD error = GetLastError();
		return error == ERROR_FILE_NOT_FOUND || error == ERROR_PATH_NOT_FOUND ? VersionRead::MISSING
		                                                                      : VersionRead::UNREADABLE;
	}
	std::vector<unsigned char> data(size);
	VS_FIXEDFILEINFO *info = nullptr;
	UINT length = 0;
	if (!GetFileVersionInfoW(path.c_str(), 0, size, data.data()) ||
	    !VerQueryValueW(data.data(), L"\\", reinterpret_cast<void **>(&info), &length) || length < sizeof(*info)) {
		return VersionRead::UNREADABLE;
	}
	version = (static_cast<uint64_t>(info->dwFileVersionMS) << 32) | info->dwFileVersionLS;
	return VersionRead::READ;
}

} // namespace

LoadInputs CurrentLoadInputs() {
	LoadInputs inputs;
	inputs.bases = CurrentBaseInputs();
	inputs.user = CurrentUserSid();
	return inputs;
}

bool CheckRegistryTrace(DriverError &error) {
	// The driver reads these in DllMain, as raw bytes through atoi, before any connection string exists.
	const wchar_t *path = L"SOFTWARE\\Amazon\\Amazon Redshift ODBC Driver (x64)\\Driver";
	HKEY key = nullptr;
	LSTATUS status = RegOpenKeyExW(HKEY_LOCAL_MACHINE, path, 0, KEY_QUERY_VALUE | KEY_WOW64_64KEY, &key);
	if (status == ERROR_FILE_NOT_FOUND) {
		return true;
	}
	std::string why = status == ERROR_SUCCESS ? "" : "the key cannot be read: " + ErrorText(status);
	for (const wchar_t *name : {L"LogLevel", L"Trace"}) {
		if (!why.empty()) {
			break;
		}
		DWORD type = 0, size = 0;
		status = RegQueryValueExW(key, name, nullptr, &type, nullptr, &size);
		if (status == ERROR_FILE_NOT_FOUND) {
			continue;
		}
		std::string bytes(size, '\0');
		if (status == ERROR_SUCCESS && size > 0) {
			status = RegQueryValueExW(key, name, nullptr, &type, reinterpret_cast<BYTE *>(&bytes[0]), &size);
		}
		bytes.resize(std::min<size_t>(size, bytes.size()));
		if (status != ERROR_SUCCESS) {
			why = "a trace value cannot be read: " + ErrorText(status);
		} else if (!windows_rules::TraceValueIsOff(type, bytes)) {
			why = Narrow(name) + " counts as on";
		}
	}
	if (key != nullptr) {
		RegCloseKey(key);
	}
	if (why.empty()) {
		return true;
	}
	error = Error(DriverErrorKind::REFUSED_ENVIRONMENT, windows_rules::REGISTRY_TRACE_MESSAGE,
	              "HKEY_LOCAL_MACHINE\\" + Narrow(path), why);
	return false;
}

bool CheckDbgHelp(DriverError &error) {
	const auto system = SystemDbgHelp();
	std::vector<windows_rules::LoadedModule> modules;
	std::string unlisted;
	if (!LoadedModules({}, modules, unlisted)) {
		// Not final: nothing is loaded yet, so the next connection checks again.
		error = Error(DriverErrorKind::REFUSED_ENVIRONMENT,
		              std::string(MODULES_UNLISTED) +
		                  ", so a DbgHelp.dll from outside the Windows system folder cannot be ruled out; try again",
		              system, unlisted);
		return false;
	}
	if (!windows_rules::LoadedFromElsewhere(modules, windows_rules::DBGHELP, system)) {
		return true;
	}
	error = Error(DriverErrorKind::REFUSED_ENVIRONMENT, windows_rules::DBGHELP_MESSAGE, system,
	              "a DbgHelp.dll is loaded from outside System32");
	return false;
}

bool CheckPrerequisites(const std::vector<Prerequisite> &prerequisites, DriverError &error) {
	for (const auto &prerequisite : prerequisites) {
		uint64_t minimum = UINT64_MAX; // a minimum that cannot be read is met by no copy
		windows_rules::ParseFileVersion(prerequisite.minimum, minimum);
		// The copy the process will use: one it has loaded, else System32's, never one beside the program
		// that the program has not loaded.
		const HMODULE loaded = GetModuleHandleW(Widen(prerequisite.name).c_str());
		// With no System32 path the copy is unreadable, never one at the drive's root.
		const auto system_copy = windows_rules::SystemFilePath(SystemFolder(), prerequisite.name);
		const std::wstring path = loaded != nullptr ? ModuleWidePath(loaded) : Widen(system_copy);
		// GetModuleHandleW matches the name wherever the module came from, and a /MD host loads System32's
		// copy, which the redistributable does replace. Only a copy from elsewhere is the program's own.
		const bool own_copy = loaded != nullptr && !windows_rules::SamePath(Narrow(path), system_copy);
		uint64_t version = 0;
		const auto read = path.empty() ? VersionRead::UNREADABLE : FileVersion(path, version);
		if (read != VersionRead::READ || version < minimum) {
			const auto state = read == VersionRead::MISSING      ? windows_rules::RuntimeState::MISSING
			                   : read == VersionRead::UNREADABLE ? windows_rules::RuntimeState::UNREADABLE
			                                                     : windows_rules::RuntimeState::READ;
			error =
			    Error(DriverErrorKind::MISSING_PREREQUISITE,
			          windows_rules::RuntimeMessage(prerequisite.name, prerequisite.minimum, own_copy, state, version),
			          Narrow(path), "the runtime this process would use is missing or older than the minimum");
			return false;
		}
	}
	return true;
}

DriverLoader::DriverLoader(LoaderConfig config) : config_(std::move(config)) {
}

DriverLoadResult DriverLoader::Load(const std::function<LoadInputs()> &inputs) {
	const std::lock_guard<std::mutex> guard(mutex_);
	if (final_) {
		return final_result_;
	}
	bool platform_loader_called = false;
	DriverLoadResult result;
	try {
		result = Attempt(inputs(), platform_loader_called);
	} catch (...) {
		// Out of memory, most likely: final once driver code may have run, as any outcome from that call on.
		result = DriverLoadResult();
		result.error =
		    Error(platform_loader_called ? DriverErrorKind::LOAD_FAILED : DriverErrorKind::NO_USABLE_DIRECTORY,
		          std::string("the Redshift ODBC driver could not be prepared (out of memory or another "
		                      "internal error); ") +
		              (platform_loader_called ? "restart the program and connect again" : "try again"));
	}
	if (result.ok() || platform_loader_called || result.error.kind == DriverErrorKind::MISSING_PREREQUISITE) {
		final_ = true;
		final_result_ = result;
	}
	return result;
}

DriverLoadResult DriverLoader::Attempt(const LoadInputs &inputs, bool &platform_loader_called) {
	DriverLoadResult result;
	// Environment block, registry, runtime, DbgHelp.dll, each file.
	LoadedDriver probe;
	probe.refused_environment = config_.refused_environment;
	if (const char *name = FindRefusedEnvironmentVariable(probe)) {
		// The client's wording, as the POSIX loader's RefusedVariableError; never the value.
		result.error = Error(DriverErrorKind::REFUSED_ENVIRONMENT,
		                     std::string(name) + " is set; the Redshift driver would read it, and it can change the "
		                                         "driver's connection or security settings. Unset it to connect.");
		return result;
	}
	if (!CheckRegistryTrace(result.error) || !CheckPrerequisites(config_.prerequisites, result.error) ||
	    (config_.system_dbghelp && !CheckDbgHelp(result.error))) {
		return result;
	}
	DriverRoots roots(inputs.bases, inputs.user, SetFileNames(config_.files));
	if (inputs.user.empty()) {
		roots.attempts.push_back({"", "the current account cannot be identified"});
	}
	std::string root;
	while (!inputs.user.empty() && roots.Next(root)) {
		for (int tries = 0; tries < INSTALL_TRIES; tries++) {
			std::string set_dir, where, reason;
			const auto installed = InstallSet(root, config_.set_sha256, config_.files, inputs.user,
			                                  inputs.bases.lock_deadline_milliseconds, set_dir, where, reason);
			if (installed == InstallResult::BYTES_CORRUPT) {
				result.error = Error(DriverErrorKind::EMBEDDED_BYTES_CORRUPT, reason);
				return result;
			}
			auto outcome = SetLoad::BASE_FAILED;
			if (installed == InstallResult::INSTALLED) {
				LoadedDriver loaded;
				outcome = LoadSet(config_, set_dir, inputs, roots.Role(), platform_loader_called, loaded, result.error,
				                  where, reason);
				if (outcome == SetLoad::LOADED) {
					loaded.trust_directory = ChooseTrustDirectory(roots.Base(), inputs.bases, inputs.user);
					driver_ = std::move(loaded);
					result.driver = &driver_;
					return result;
				}
				if (outcome == SetLoad::FINAL) {
					return result; // no other base: every one holds the same bytes, and driver code may have run
				}
			}
			if (outcome != SetLoad::CHANGED || tries + 1 == INSTALL_TRIES) {
				roots.Fail(where, reason);
				break;
			}
		}
	}
	result.error = Error(DriverErrorKind::NO_USABLE_DIRECTORY, windows_rules::NoUsableDirectoryMessage(roots.attempts));
	for (const auto &attempt : roots.attempts) {
		result.error.attempts.push_back({attempt.directory, attempt.reason});
	}
	return result;
}

SQLRETURN AllocateEnvironment(const LoadedDriver &driver, SQLHENV *environment) {
	return driver.api.AllocHandle(SQL_HANDLE_ENV, SQL_NULL_HANDLE, environment);
}

} // namespace windows_driver
} // namespace redshift_driver
} // namespace duckdb
