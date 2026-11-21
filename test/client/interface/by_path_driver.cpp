//===----------------------------------------------------------------------===//
//                         DuckDB - Redshift
//
// test/client/interface/by_path_driver.cpp
//
// The pin catches a wrong build, not a hostile file: no shipped build contains this source, so the
// library is checked and loaded where it lies. The checks are the path, ownership and pin rules of
// the driver loader's directory and file rules, with its flags, without the noexec, ACL,
// file-system-type, driver-root, exact-file-set and prerequisite rules the header lists.
//===----------------------------------------------------------------------===//

#include "by_path_driver.hpp"

#include "duckdb/main/database.hpp"
#include "mbedtls_wrapper.hpp"

#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>

#include <algorithm>
#include <cctype>
#include <cerrno>
#include <cstring>
#include <mutex>
#include <string>
#include <utility>
#include <vector>

namespace duckdb {
namespace redshift_driver {
namespace testing {

namespace {

//! One open file descriptor, closed however its holder returns.
class ScopedFileDescriptor {
public:
	explicit ScopedFileDescriptor(int descriptor_p) : descriptor(descriptor_p) {
	}
	~ScopedFileDescriptor() {
		if (descriptor >= 0) {
			::close(descriptor);
		}
	}
	ScopedFileDescriptor(const ScopedFileDescriptor &) = delete;
	ScopedFileDescriptor &operator=(const ScopedFileDescriptor &) = delete;
	int Get() const {
		return descriptor;
	}

private:
	int descriptor;
};

//! "/opt/driver" for "/opt/driver/librsodbc64.so"; "/" for a file in the root directory.
std::string ExtractDirectory(const std::string &path) {
	return path.substr(0, std::max<std::string::size_type>(path.rfind('/'), 1));
}

std::string ExtractFileName(const std::string &path) {
	return path.substr(path.rfind('/') + 1);
}

//! "/", "/opt", "/opt/driver" for "/opt/driver", so each is checked in turn.
std::vector<std::string> ListDirectoriesFromRoot(const std::string &directory) {
	std::vector<std::string> directories;
	directories.emplace_back("/");
	for (std::string::size_type slash = directory.find('/', 1); slash != std::string::npos;
	     slash = directory.find('/', slash + 1)) {
		directories.push_back(directory.substr(0, slash));
	}
	if (directory != "/") {
		directories.push_back(directory);
	}
	return directories;
}

//! A directory no load may use, and the rule it breaks; no refusal when `rule` is empty.
struct DirectoryRefusal {
	std::string directory;
	bool holds_the_library;
	std::string rule;
};

//! Every directory from / down is checked and none may be a symbolic
//! link, so, with '$' and macOS's dyld search variables refused, the path checked is the
//! path the platform loader opens.
DirectoryRefusal FindDirectoryRefusal(const std::string &directory, uid_t trusted_uid) {
	const std::vector<std::string> directories = ListDirectoriesFromRoot(directory);
	for (idx_t i = 0; i < directories.size(); i++) {
		DirectoryRefusal refusal {directories[i], i + 1 == directories.size(), std::string()};
		struct stat status;
		if (::lstat(refusal.directory.c_str(), &status) != 0) {
			refusal.rule = "cannot be read (" + std::string(std::strerror(errno)) + ")";
		} else if (S_ISLNK(status.st_mode)) {
			refusal.rule = "is a symbolic link";
		} else if (!S_ISDIR(status.st_mode)) {
			refusal.rule = "is not a directory";
		} else if (!refusal.holds_the_library && IsTrustedStickyAncestor(status.st_uid, status.st_mode)) {
			continue;
		} else {
			refusal.rule = FindOwnershipRefusal(status.st_uid, status.st_mode, trusted_uid);
		}
		if (!refusal.rule.empty()) {
			return refusal;
		}
	}
	return DirectoryRefusal();
}

//! A descriptor's lowercase hex SHA-256, or why it could not be read.
struct DescriptorHash {
	std::string read_error;
	std::string sha256;
};

DescriptorHash HashDescriptor(int descriptor) {
	duckdb_mbedtls::MbedTlsWrapper::SHA256State state;
	char buffer[1 << 16];
	DescriptorHash hash;
	for (;;) {
		const ssize_t count = ::read(descriptor, buffer, sizeof(buffer));
		if (count == 0) {
			break;
		}
		if (count < 0) {
			hash.read_error = std::strerror(errno);
			return hash;
		}
		state.AddBytes(reinterpret_cast<const_data_ptr_t>(buffer), static_cast<idx_t>(count));
	}
	char hex[duckdb_mbedtls::MbedTlsWrapper::SHA256_HASH_LENGTH_TEXT];
	state.FinishHex(hex);
	hash.sha256.assign(hex, sizeof(hex));
	return hash;
}

//! Why the library must not be loaded, and its hash once it has been read. The attempt's reason
//! names the path component that broke a rule; the message's names a directory by its role, as no
//! message holds a path.
struct FileCheck {
	std::string attempt_reason;
	std::string message_reason;
	std::string sha256;
};

//! Why opening the file failed, given the open's errno; O_NOFOLLOW reports a link as ELOOP.
std::string DescribeOpenFailure(int error) {
	return error == ELOOP ? "is a symbolic link" : "cannot be opened (" + std::string(std::strerror(error)) + ")";
}

//! Why the open file may not be loaded, or an empty string: fstat fails, it is not a regular file,
//! or FindOwnershipRefusal refuses its owner or mode.
std::string FindOpenFileRefusal(int descriptor, uid_t trusted_uid) {
	struct stat status;
	if (::fstat(descriptor, &status) != 0) {
		return "cannot be read (" + std::string(std::strerror(errno)) + ")";
	}
	if (!S_ISREG(status.st_mode)) {
		return "is not a regular file";
	}
	return FindOwnershipRefusal(status.st_uid, status.st_mode, trusted_uid);
}

//! The file is opened once without following links and every check reads that one descriptor, so
//! the bytes hashed are the bytes checked. The platform
//! loader opens the path again, which is why the directories above it are checked too. O_NONBLOCK
//! lets a FIFO open at once, so the regular-file check refuses it rather than the open hanging.
FileCheck CheckFile(const std::string &path, const std::string &pin, uid_t trusted_uid) {
	const std::string name = ExtractFileName(path);
	const ScopedFileDescriptor file(::open(path.c_str(), O_RDONLY | O_NOFOLLOW | O_CLOEXEC | O_NONBLOCK));
	const std::string refusal =
	    file.Get() < 0 ? DescribeOpenFailure(errno) : FindOpenFileRefusal(file.Get(), trusted_uid);
	if (!refusal.empty()) {
		return FileCheck {name + " " + refusal, name + " " + refusal, std::string()};
	}
	const DescriptorHash hash = HashDescriptor(file.Get());
	if (!hash.read_error.empty()) {
		const std::string reason = name + " cannot be read (" + hash.read_error + ")";
		return FileCheck {reason, reason, std::string()};
	}
	FileCheck check {std::string(), std::string(), hash.sha256};
	if (check.sha256 != pin) {
		check.attempt_reason =
		    name + " has SHA-256 " + check.sha256 + ", not the one " + BY_PATH_DRIVER_SHA256_VARIABLE + " pins";
		check.message_reason = check.attempt_reason;
	}
	return check;
}

//! Directories first, from / down, so a refusal names the first component of the path that fails.
FileCheck CheckLibrary(const std::string &library_path, const std::string &pin, uid_t trusted_uid) {
	const DirectoryRefusal refused = FindDirectoryRefusal(ExtractDirectory(library_path), trusted_uid);
	if (!refused.rule.empty()) {
		const std::string role = refused.holds_the_library ? "its directory " : "a directory above it ";
		return FileCheck {refused.directory + " " + refused.rule, role + refused.rule, std::string()};
	}
	return CheckFile(library_path, pin, trusted_uid);
}

//! A refusal from before the platform loader, which loads no code, so the next call checks again.
//! The message names the variables and the rule broken, never a
//! variable's value; once a path was checked, its directory is in the one attempt.
DriverLoadResult MakeRefusal(const std::string &refusal, std::vector<DriverAttempt> attempts = {}) {
	DriverLoadResult result;
	result.error.kind = DriverErrorKind::NO_USABLE_DIRECTORY;
	result.error.message = refusal + "; set " + BY_PATH_DRIVER_PATH_VARIABLE +
	                       " to the driver library's absolute path and " + BY_PATH_DRIVER_SHA256_VARIABLE +
	                       " to its SHA-256";
	result.error.attempts = std::move(attempts);
	return result;
}

//! Why the variables' values cannot name a library to check, or an empty string.
std::string FindVariableRefusal(const char *path, const char *pin, const std::vector<std::string> &search_variables) {
	if (path == nullptr || pin == nullptr) {
		return "no test driver is named";
	}
	// The platform loader searches its library path for a name with no slash in it, and expands
	// $LIB, $ORIGIN and $PLATFORM in one with a slash, so either could load an unchecked file.
	if (path[0] != '/') {
		return std::string(BY_PATH_DRIVER_PATH_VARIABLE) + " is not an absolute path";
	}
	if (std::strchr(path, '$') != nullptr) {
		return std::string(BY_PATH_DRIVER_PATH_VARIABLE) + " contains a '$', which the platform loader expands";
	}
	// CutPathsToFileNames ends a path in the loader's text at a ':' or a quote, so either in the
	// path would let part of its directory into the message.
	if (std::strpbrk(path, ":'") != nullptr) {
		return std::string(BY_PATH_DRIVER_PATH_VARIABLE) + " contains a ':' or a quote";
	}
	if ((std::string(path) + "/").find("/../") != std::string::npos) {
		return std::string(BY_PATH_DRIVER_PATH_VARIABLE) + " has a '..' component";
	}
	for (const std::string &name : search_variables) {
		if (std::getenv(name.c_str()) != nullptr) {
			return name + " is set, so the platform loader may open another file; unset it";
		}
	}
	const std::string hex(pin);
	if (hex.size() != 64 || hex.find_first_not_of("0123456789abcdef") != std::string::npos) {
		return std::string(BY_PATH_DRIVER_SHA256_VARIABLE) + " is not 64 lowercase hex digits";
	}
	return std::string();
}

//! The version names the build by its pin, so two builds of the driver describe differently.
DriverIdentity MakeByPathIdentity(const std::string &path, const std::string &sha256) {
	DriverIdentity identity;
	identity.origin = DriverOrigin::TEST_BY_PATH;
	identity.version = "sha256:" + sha256.substr(0, 12);
	identity.platform = DuckDB::Platform();
	identity.files.push_back(DriverFile {ExtractFileName(path), sha256});
	identity.directory = ExtractDirectory(path);
	return identity;
}

OdbcApi ResolveEntryPoints(void *library) {
	OdbcApi api;
#define REDSHIFT_RESOLVE_ENTRY_POINT(name) api.name = reinterpret_cast<decltype(api.name)>(dlsym(library, "SQL" #name));
	REDSHIFT_ODBC_ENTRY_POINTS(REDSHIFT_RESOLVE_ENTRY_POINT)
#undef REDSHIFT_RESOLVE_ENTRY_POINT
	return api;
}

//! Whether a path in the platform loader's text may start after `c`: glibc and dyld put one first
//! or after a space, and dyld also after '(' or in quotes.
bool StartsLoaderPath(char c) {
	return std::isspace(static_cast<unsigned char>(c)) != 0 || c == '(' || c == '\'';
}

//! A failure from the platform loader on, which names the variable; the path and the detail,
//! which may contain the path, are in the one attempt.
DriverError MakeLoadError(DriverErrorKind kind, const std::string &message, const std::string &path,
                          const std::string &detail) {
	DriverError error;
	error.kind = kind;
	error.message = message;
	error.attempts.push_back(DriverAttempt {ExtractDirectory(path), ExtractFileName(path) + ": " + detail});
	return error;
}

} // namespace

int PlatformDlopenFlags() {
#ifdef REDSHIFT_BY_PATH_DEEPBIND
	return RTLD_NOW | RTLD_LOCAL | RTLD_DEEPBIND;
#else
	return RTLD_NOW | RTLD_LOCAL;
#endif
}

std::string FindOwnershipRefusal(uid_t owner, mode_t mode, uid_t trusted_uid) {
	if (owner != trusted_uid && owner != 0) {
		return "is owned by another user";
	}
	if ((mode & S_IWGRP) != 0) {
		return "is writable by its group";
	}
	if ((mode & S_IWOTH) != 0) {
		return "is writable by other users";
	}
	return std::string();
}

bool IsTrustedStickyAncestor(uid_t owner, mode_t mode) {
	return owner == 0 && (mode & S_ISVTX) != 0;
}

std::string CutPathsToFileNames(const std::string &text) {
	std::string result;
	std::string::size_type at = 0;
	while (at < text.size()) {
		const char before = at > 0 ? text[at - 1] : ' ';
		if (text[at] != '/' || !StartsLoaderPath(before)) {
			result += text[at++];
			continue;
		}
		// glibc and dyld end an unquoted path with ':'. The span may take in text after the path, such
		// as dyld's ", 0x0006)", which has no '/' and so stays.
		const auto end = std::min(text.find(before == '\'' ? '\'' : ':', at), text.size());
		result += ExtractFileName(text.substr(at, end - at));
		at = end;
	}
	return result;
}

std::vector<std::string> ListLoaderSearchVariables() {
#ifdef __APPLE__
	return {"DYLD_LIBRARY_PATH",
	        "DYLD_FALLBACK_LIBRARY_PATH",
	        "DYLD_IMAGE_SUFFIX",
	        "DYLD_VERSIONED_LIBRARY_PATH",
	        "DYLD_FRAMEWORK_PATH",
	        "DYLD_FALLBACK_FRAMEWORK_PATH",
	        "DYLD_VERSIONED_FRAMEWORK_PATH"};
#else
	return {};
#endif
}

ByPathDriverLoader::ByPathDriverLoader(uid_t trusted_uid_p, std::vector<std::string> search_variables_p)
    : trusted_uid(trusted_uid_p), search_variables(std::move(search_variables_p)) {
}

DriverLoadResult ByPathDriverLoader::Load() {
	std::lock_guard<std::mutex> guard(mutex);
	if (has_final_outcome) {
		return outcome;
	}
	const char *path = std::getenv(BY_PATH_DRIVER_PATH_VARIABLE);
	const char *pin = std::getenv(BY_PATH_DRIVER_SHA256_VARIABLE);
	const std::string variable_refusal = FindVariableRefusal(path, pin, search_variables);
	if (!variable_refusal.empty()) {
		return MakeRefusal(variable_refusal);
	}
	const FileCheck check = CheckLibrary(path, pin, trusted_uid);
	if (!check.attempt_reason.empty()) {
		return MakeRefusal(std::string("the library ") + BY_PATH_DRIVER_PATH_VARIABLE +
		                       " names was refused before loading: " + check.message_reason,
		                   {DriverAttempt {ExtractDirectory(path), check.attempt_reason}});
	}
	LoadMatchedFile(path, check.sha256);
	has_final_outcome = true;
	return outcome;
}

//! Loads a file that matched its pin. The outcome is final, as the driver's start-up code may have
//! run; a refusal is LOAD_FAILED, with no host-prerequisite list to
//! tell MISSING_PREREQUISITE apart. The library is never closed.
void ByPathDriverLoader::LoadMatchedFile(const std::string &path, const std::string &sha256) {
	void *library = dlopen(path.c_str(), PlatformDlopenFlags());
	if (library == nullptr) {
		const char *loader_message = dlerror();
		const std::string detail = loader_message != nullptr ? loader_message : "it gave no reason";
		outcome.error =
		    MakeLoadError(DriverErrorKind::LOAD_FAILED,
		                  std::string("the platform loader refused the library ") + BY_PATH_DRIVER_PATH_VARIABLE +
		                      " names: " + CutPathsToFileNames(detail) + "; fix that, then start a new process",
		                  path, detail);
		return;
	}
	driver.api = ResolveEntryPoints(library);
	if (!driver.api.Complete()) {
		const std::string missing = driver.api.FirstMissing();
		outcome.error =
		    MakeLoadError(DriverErrorKind::MISSING_ENTRY_POINT,
		                  std::string("the library ") + BY_PATH_DRIVER_PATH_VARIABLE + " names does not export " +
		                      missing + ", which the extension calls; point it at a Redshift ODBC driver library",
		                  path, "does not export " + missing);
		return;
	}
	driver.identity = MakeByPathIdentity(path, sha256);
	outcome.driver = &driver;
}

DriverLoadResult ByPathDriverSource() {
	// Never destroyed, as a success is one object for the process: a thread still holding the
	// LoadedDriver at exit must not read a destroyed one.
	static ByPathDriverLoader *const loader = new ByPathDriverLoader();
	return loader->Load();
}

} // namespace testing
} // namespace redshift_driver
} // namespace duckdb
