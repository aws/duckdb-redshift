//===----------------------------------------------------------------------===//
// The run-time sequence behind LoadEmbeddedDriver. Common POSIX code; the platform files provide the hooks declared in
// driver_loader_core.hpp. It includes no driver_info.h: the embedded driver's data comes in through LoaderConfig
// (driver_embedded.cpp), so the tests give the same sequence a test payload.
//===----------------------------------------------------------------------===//

#include "driver_loader_core.hpp"
#include "sha256.hpp"

#include <algorithm>
#include <cerrno>
#include <cstdint>
#include <cstdlib>
#include <string>
#include <utility>
#include <vector>

#include <dlfcn.h>
#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>

namespace duckdb {
namespace redshift_driver {
namespace detail {

namespace {

DriverError Error(DriverErrorKind kind, std::string message) {
	DriverError error;
	error.kind = kind;
	error.message = std::move(message);
	return error;
}

} // namespace

bool PathIsLoaded(const std::string &name) {
	if (name.find('/') == std::string::npos) {
		return false; // never searched for: the header says why
	}
	void *handle = ::dlopen(name.c_str(), RTLD_LAZY | RTLD_NOLOAD);
	if (handle == nullptr) {
		return false;
	}
	::dlclose(handle);
	return true;
}

bool IsFinalKind(DriverErrorKind kind) {
	switch (kind) {
	case DriverErrorKind::NOT_BUILT:
		return true; // the same answer on every call
	case DriverErrorKind::EMBEDDED_BYTES_CORRUPT:
	case DriverErrorKind::NO_USABLE_DIRECTORY:
	case DriverErrorKind::REFUSED_ENVIRONMENT:
		return false;                           // before the platform loader call: nothing foreign ran
	case DriverErrorKind::MISSING_PREREQUISITE: // the platform loader was called, or no file was found for it
	case DriverErrorKind::LOAD_FAILED:
	case DriverErrorKind::MISSING_ENTRY_POINT:
		return true; // the platform loader was called
	}
	return true;
}

DriverError InternalError(DriverErrorKind kind) {
	return Error(
	    kind,
	    std::string("the Redshift ODBC driver could not be prepared (out of memory or another internal error); ") +
	        (IsFinalKind(kind) ? "restart the program and connect again" : "try again"));
}

const char *FindSetVariable(const std::vector<std::string> &names) {
	for (const auto &name : names) {
		// Whether it is set, never what it is set to.
		if (std::getenv(name.c_str()) != nullptr) {
			return name.c_str();
		}
	}
	return nullptr;
}

DriverError LoaderVariableError(const std::string &name) {
	return Error(DriverErrorKind::REFUSED_ENVIRONMENT,
	             name +
	                 " is set; the system's library loader reads it, and it can make the loader load another library "
	                 "in place of the Redshift driver. Unset it and start the program again to connect.");
}

DriverError RefusedVariableError(const std::string &name) {
	// The client's wording, so a variable refused
	// here and one refused there read the same: OdbcConnection::RefuseWeakeningEnvironment (odbc_connection.cpp)
	// writes the same sentence, and environment_allocation_test.cpp compares the two. Never the value.
	return Error(DriverErrorKind::REFUSED_ENVIRONMENT,
	             name + " is set; the Redshift driver would read it, and it can change the driver's connection or "
	                    "security settings. Unset it to connect.");
}

SQLRETURN AllocateWithoutOdbcIni(const OdbcApi &api, SQLHENV *environment, int (*failing_set)()) {
	// Whether it is set, never what it is set to: a set one is the caller's to refuse.
	if (std::getenv(ODBC_INI_VARIABLE) != nullptr ||
	    (failing_set != nullptr ? failing_set() : ::setenv(ODBC_INI_VARIABLE, NO_ODBC_INI, 1)) != 0) {
		return SQL_ERROR;
	}
	const SQLRETURN rc = api.AllocHandle(SQL_HANDLE_ENV, SQL_NULL_HANDLE, environment);
	::unsetenv(ODBC_INI_VARIABLE);
	return rc;
}

#if defined(__linux__)
DriverError AllocatorReplacementError(const AllocatorReplacement &replacement) {
	const auto slash = replacement.file.rfind('/');
	const auto name = slash == std::string::npos ? replacement.file : replacement.file.substr(slash + 1);
	const auto who = name.empty() ? std::string("another allocator") : name;
	auto error = Error(DriverErrorKind::REFUSED_ENVIRONMENT,
	                   who +
	                       " has replaced the C library's malloc in this process, and the Redshift ODBC driver uses "
	                       "the C library's own allocator, so memory can pass between the two and crash the "
	                       "process. To connect, start the program without " +
	                       who +
	                       " in LD_PRELOAD; a program that links or defines its own malloc cannot use the Redshift "
	                       "driver yet.");
	error.attempts.push_back(
	    {replacement.file, "replaces the C library's malloc, free, calloc or realloc for the C library's own calls"});
	return error;
}
#endif

LoadInputs CurrentLoadInputs() {
	LoadInputs inputs;
	inputs.bases = CurrentBaseInputs();
	return inputs;
}

namespace {

bool IsPrerequisite(const std::string &name, const LoadOptions &options) {
	return std::find(options.prerequisites.begin(), options.prerequisites.end(), name) != options.prerequisites.end();
}

// MISSING_PREREQUISITE for the library `name`, final like every outcome of the platform loader call: the
// process must start again once it is installed. `unlisted`: neither the library cache nor a system directory
// has a file for it, which is also what a cache not rebuilt since the library was installed elsewhere says.
DriverError MissingLibraryError(const std::string &name, const LoadOptions &options, bool unlisted) {
	return Error(
	    DriverErrorKind::MISSING_PREREQUISITE,
	    "the Redshift ODBC driver needs " + name + ", which this host does not have; " +
	        (IsPrerequisite(name, options) && !options.prerequisite_hint.empty() ? options.prerequisite_hint + "; "
	                                                                             : "") +
	        (unlisted ? "install it (or, if it is installed, have an administrator run ldconfig)" : "install it") +
	        ", then restart the program and connect again");
}

// A LOAD_FAILED for the base by its role, with `what` after the fixed start.
DriverError LoadFailed(const LoadOptions &options, const std::string &what) {
	return Error(DriverErrorKind::LOAD_FAILED,
	             std::string("the Redshift ODBC driver could not be loaded (") + options.role + "): " + what);
}

#if defined(__linux__)
DriverError SearchError(const SearchRefusal &refusal, const LoadOptions &options) {
	auto error = refusal.cause == SearchCause::MISSING
	                 ? MissingLibraryError(refusal.library, options, true)
	                 : Error(DriverErrorKind::REFUSED_ENVIRONMENT, SearchRefusalMessage(refusal));
	if (!refusal.where.empty()) {
		error.attempts.push_back({refusal.where, refusal.rule});
	}
	return error;
}

#endif

// The refusals step 7 describes, from the load-phase names, the platform loader's own variables, on Linux an
// allocator that has replaced the C library's, the search list and the libraries the driver needs; false with
// `error` set when the load is refused, else true with `preload` what to load first.
bool CheckLoadEnvironment(const LoadOptions &options, DriverError &error, std::vector<NeededLibrary> &preload) {
	if (const char *name = FindSetVariable(options.load_refused_environment)) {
		error = RefusedVariableError(name);
		return false;
	}
	const auto loader_variables = PlatformLoaderVariables();
	if (const char *name = FindSetVariable(loader_variables)) {
		error = LoaderVariableError(name);
		return false;
	}
#if defined(__linux__)
	const auto replacement = FindAllocatorReplacement();
	if (replacement.replaced) {
		error = AllocatorReplacementError(replacement);
		return false;
	}
	SearchRefusal refusal;
	if (!CheckSearchList(options.search, refusal, preload)) {
		error = SearchError(refusal, options);
		return false;
	}
#else
	preload.clear(); // dyld searches nothing for the driver's path, and its libraries are system paths
#endif
	return true;
}

// How many descriptor numbers are tried for a name nothing is loaded under.
constexpr int DESCRIPTOR_NAME_TRIES = 64;

bool SameFile(const struct stat &a, const struct stat &b) {
	return a.st_dev == b.st_dev && a.st_ino == b.st_ino;
}

// The /proc name of a descriptor of the checked driver file that nothing is loaded under yet, in `load_path`;
// the checked path stays where descriptors have no working name (no /proc, and macOS). False, with
// `reason`, when they do and every number tried is taken (dlopen would reuse what is loaded under it) or cannot
// be duplicated. `load_path` may name a descriptor held in `spare`, which must outlive the dlopen.
bool FindUnusedDescriptorName(int driver_fd, const struct stat &checked, const LoadHooks &hooks,
                              std::vector<UniqueFd> &spare, std::string &load_path, std::string &reason) {
	bool descriptor_names_work = false;
	int load_fd = driver_fd;
	int duplicate_error = 0;
	for (int tries = 0; tries < DESCRIPTOR_NAME_TRIES; tries++) {
		const auto named = hooks.descriptor_path(load_fd);
		struct stat leads_to;
		if (named.empty() || ::stat(named.c_str(), &leads_to) != 0 || !SameFile(leads_to, checked)) {
			break;
		}
		descriptor_names_work = true;
		if (!hooks.name_in_use(named)) {
			load_path = named;
			return true;
		}
		// An object is loaded under this name already, from an earlier descriptor with this number:
		// dlopen would reuse it in place of this file. Use a number nothing is named by.
		const int next = ::fcntl(load_fd, F_DUPFD_CLOEXEC, load_fd + 1);
		if (next < 0) {
			duplicate_error = errno;
			break;
		}
		UniqueFd held(next); // closed here if the push throws
		spare.push_back(std::move(held));
		load_fd = next;
	}
	if (!descriptor_names_work) {
		return true;
	}
	reason = duplicate_error != 0 ? "the driver file's descriptor cannot be duplicated to look for an unused name: " +
	                                    ErrnoText(duplicate_error)
	                              : "no unused name was found for the driver file's descriptor";
	return false;
}

// Loads the libraries the driver needs, each by the path that was checked, so that the platform loader finds
// them loaded when it loads the driver, and searches no directory for them. False, with `error` (final), when
// one is refused or does not answer to its name.
bool PreloadNeededLibraries(const std::vector<NeededLibrary> &preload, const LoadOptions &options, DriverError &error) {
	for (const auto &library : preload) {
		if (::dlopen(library.path.c_str(), RTLD_NOW | RTLD_LOCAL) == nullptr) {
			// Decided by the library, not by the platform loader's text, which names the real file
			// (libkeyutils.so.1.10, say).
			const char *text = ::dlerror();
			error = IsPrerequisite(library.name, options)
			            ? MissingLibraryError(library.name, options, false)
			            : LoadFailed(options,
			                         "the platform loader refused " + library.name + ", a library the driver needs");
			error.attempts.push_back({library.path, text != nullptr ? text : "unknown error"});
			return false;
		}
#if defined(__linux__)
		// glibc reuses a loaded object for a name only when the name is its soname (or its path).
		if (options.search.loaded != nullptr && !options.search.loaded(library.name)) {
			error = LoadFailed(options, "the file found for " + library.name + " does not answer to that name");
			error.attempts.push_back({library.path, "the loaded library's soname is not " + library.name});
			return false;
		}
#endif
	}
	return true;
}

} // namespace

LoadResult LoadDriverFileSet(const std::string &dir, const std::vector<DriverFile> &files, const LoadOptions &options,
                             LoadedDriver &out, DriverError &error, std::string &where, std::string &reason) {
	error = DriverError();
	where.clear();
	reason.clear();
	if (files.empty()) {
		where = dir;
		reason = "the driver set lists no file";
		return LoadResult::BASE_FAILED;
	}
	const uid_t user = options.search.user;
	// The directory the set is in must allow executing code (a statvfs that fails is a failure of
	// this base, not "exec allowed"); then every file's type, owner, mode and hash, the driver's
	// through the descriptor it is loaded from.
	const int noexec = options.hooks.no_exec_state(dir);
	if (noexec != 0) {
		where = dir;
		reason = noexec < 0 ? "the driver set's file system cannot be examined for the noexec option"
		                    : "the driver set is on a file system mounted noexec";
		return LoadResult::BASE_FAILED;
	}
	int raw_fd = -1;
	const auto state = CheckInstalledSet(dir, files, user, where, reason, &raw_fd, options.hooks.after_hash);
	UniqueFd driver_fd(raw_fd);
	// No foreign code has run whatever the state was. `where` and `reason` are what CheckInstalledSet found:
	// it sets both for every state but VALID.
	if (state == SetState::UNCHECKED) {
		// The set could not be judged, so it is no evidence: a fact about the base that the same set would
		// meet again (step 6), which the install reads the same way (InstallDriverSet's step 3). A retry
		// would repair nothing, so the base ends here.
		return LoadResult::BASE_FAILED;
	}
	if (state != SetState::VALID) {
		// MISSING or INVALID: another process may be repairing the same set, so the next try repairs it.
		return LoadResult::CHANGED;
	}
	// The environment and the libraries the driver needs again, as close to the load as the hashing allows.
	std::vector<NeededLibrary> preload;
	if (!CheckLoadEnvironment(options, error, preload)) {
		return LoadResult::REFUSED_ENVIRONMENT;
	}
	struct stat checked;
	if (::fstat(driver_fd.get(), &checked) != 0) {
		const int fstat_error = errno;
		where = dir;
		reason = "the driver file cannot be examined: " + ErrnoText(fstat_error);
		return LoadResult::BASE_FAILED;
	}
	// Load the very file that was hashed: its descriptor, named in /proc, so nothing renamed in
	// after the check is what loads. Where /proc is missing, the checked path.
	std::string load_path = dir + "/" + files[0].name;
	// The numbers left behind while looking for a name nothing is loaded under, closed when the load is over.
	std::vector<UniqueFd> spare;
	if (!FindUnusedDescriptorName(driver_fd.get(), checked, options.hooks, spare, load_path, reason)) {
		where = dir;
		return LoadResult::BASE_FAILED;
	}
	// From here on the platform loader has been called, and the outcome is final: a library loaded first
	// runs its own start-up code as the driver does.
	if (options.platform_loader_called != nullptr) {
		*options.platform_loader_called = true;
	}
	if (options.hooks.on_platform_loader_call != nullptr) {
		options.hooks.on_platform_loader_call();
	}
	if (!PreloadNeededLibraries(preload, options, error)) {
		return LoadResult::FINAL;
	}
	void *handle = ::dlopen(load_path.c_str(), PlatformDlopenFlags());
	if (handle == nullptr) {
		// Its own text names paths, so it goes into the attempts only, never into the message.
		const char *text = ::dlerror();
		const std::string loader_text = text != nullptr ? text : "unknown error";
		if (const auto *missing = MissingPrerequisite(text, options.prerequisites)) {
			error = MissingLibraryError(*missing, options, false);
		} else {
			error = LoadFailed(options, "the platform loader refused the driver: a library it needs is missing, "
			                            "unreadable or incompatible");
		}
		error.attempts.push_back({dir, loader_text});
		return LoadResult::FINAL;
	}
	// The handle is kept: driver code may already have run, and the driver is never unloaded.
	OdbcApi api;
#define REDSHIFT_RESOLVE(name) api.name = reinterpret_cast<decltype(api.name)>(::dlsym(handle, "SQL" #name));
	REDSHIFT_ODBC_ENTRY_POINTS(REDSHIFT_RESOLVE)
#undef REDSHIFT_RESOLVE
	// What was loaded must be what was hashed: a platform check (macOS, which loads the checked path, and where
	// only a DYLD_ variable dyld read at start-up can put another file in its place), on the first entry point
	// resolved, so a file in the driver's place that lacks some is still caught before Complete() is asked.
	const void *entry = nullptr;
#define REDSHIFT_FIRST_RESOLVED(name)                                                                                  \
	if (entry == nullptr) {                                                                                            \
		entry = reinterpret_cast<const void *>(api.name);                                                              \
	}
	REDSHIFT_ODBC_ENTRY_POINTS(REDSHIFT_FIRST_RESOLVED)
#undef REDSHIFT_FIRST_RESOLVED
	if (entry != nullptr && !options.hooks.loaded_file_is_checked(entry, checked)) {
		error = LoadFailed(options, "the platform loader loaded a file other than the one that was checked (start the "
		                            "program without DYLD_ variables set to connect)");
		error.attempts.push_back({dir, "the loaded file is not the checked one (no absolute name, or another inode)"});
		return LoadResult::FINAL;
	}
	if (!api.Complete()) {
		error = Error(DriverErrorKind::MISSING_ENTRY_POINT,
		              std::string("the Redshift ODBC driver does not export ") + api.FirstMissing());
		error.attempts.push_back({dir, ""});
		return LoadResult::FINAL;
	}
	out.api = api;
	out.identity.files = files;
	out.identity.directory = dir;
	return LoadResult::LOADED;
}

DriverLoader::DriverLoader(LoaderConfig config) : config_(std::move(config)) {
}

LoadOptions DriverLoader::Options(const LoadInputs &inputs, const char *role) const {
	LoadOptions options;
	options.prerequisites = config_.prerequisites;
	options.prerequisite_hint = config_.prerequisite_hint;
	options.load_refused_environment = config_.load_refused_environment;
	options.role = role;
	options.search = inputs.search;
	options.search.needed = config_.needed; // the driver's own, whatever the inputs say
	options.hooks = inputs.hooks;
	return options;
}

DriverLoadResult DriverLoader::Load(const LoadInputs &inputs) {
	return Load([&] { return inputs; });
}

DriverLoadResult DriverLoader::Load(const std::function<LoadInputs()> &inputs) {
	const std::lock_guard<std::mutex> guard(mutex_);
	DriverLoadResult result;
	// A success is final like any outcome of the platform loader: it is returned at once.
	if (final_) {
		if (loaded_) {
			result.driver = &driver_;
		} else {
			result.error = final_error_;
		}
		return result;
	}
	bool platform_loader_called = false;
	try {
		result = Attempt(inputs(), platform_loader_called);
	} catch (...) {
		// Out of memory, most likely. Once the platform loader has run, driver code may have run too,
		// so the outcome is final like any other from that call on; before it, the next call retries.
		result = DriverLoadResult();
		result.error =
		    InternalError(platform_loader_called ? DriverErrorKind::LOAD_FAILED : DriverErrorKind::NO_USABLE_DIRECTORY);
	}
	// Stored exactly when IsFinalKind says so, a success included: every outcome from the first platform loader
	// call on, and MISSING_PREREQUISITE.
	if (result.ok()) {
		final_ = true;
		loaded_ = true;
	} else if (IsFinalKind(result.error.kind)) {
		final_ = true;
		final_error_ = result.error;
	}
	return result;
}

DriverLoadResult DriverLoader::Attempt(const LoadInputs &inputs, bool &platform_loader_called) {
	DriverLoadResult result;
	const uid_t user = inputs.search.user;
	// Step 7's refusals first, before any base is chosen, so a refused environment costs no hashing.
	auto options = Options(inputs, "the base");
	options.platform_loader_called = &platform_loader_called;
	std::vector<NeededLibrary> preload;
	if (!CheckLoadEnvironment(options, result.error, preload)) {
		return result;
	}
	// What ended the search, when a set was used and the outcome was not a success.
	bool loaded_here = false;
	DriverError ended;
	std::string set_dir;
	DriverError not_prepared;
	const UseSet use = [&](const std::string &dir, const DriverRoots &roots, std::string &where, std::string &reason) {
		options.role = roots.Role();
		LoadedDriver loaded;
		loaded.identity = config_.identity;
		loaded.refused_environment = config_.refused_environment;
		loaded.overridden_environment = config_.overridden_environment;
		DriverError error;
		std::string load_where, load_reason;
		const auto outcome =
		    LoadDriverFileSet(dir, config_.set.Manifest(), options, loaded, error, load_where, load_reason);
		switch (outcome) {
		case LoadResult::LOADED:
			driver_ = std::move(loaded);
			loaded_here = true;
			return SetUse::DONE;
		case LoadResult::FINAL:
			// No other directory or copy: every one holds the same bytes, and driver code may
			// already have run. The bases tried before are named, as for any failure.
			if (!roots.attempts.empty()) {
				error.message += " (tried before: " + DescribeAttempts(roots.attempts) + ")";
			}
			ended = error;
			return SetUse::DONE;
		case LoadResult::REFUSED_ENVIRONMENT:
			// About the environment, not this base: another base would meet the same.
			ended = error;
			return SetUse::DONE;
		case LoadResult::CHANGED:
		case LoadResult::BASE_FAILED:
			where = load_where;
			reason = load_reason;
			return outcome == LoadResult::CHANGED ? SetUse::CHANGED : SetUse::BASE_FAILED;
		}
		return SetUse::BASE_FAILED;
	};
	if (!PrepareWith(inputs.bases, config_.set, user, use, set_dir, not_prepared)) {
		result.error = not_prepared;
	} else if (loaded_here) {
		result.driver = &driver_;
	} else {
		result.error = ended;
	}
	return result;
}

SQLRETURN DriverLoader::AllocateEnvironment(const OdbcApi &api, SQLHENV *environment) {
	const std::lock_guard<std::mutex> guard(mutex_);
	return AllocateWithoutOdbcIni(api, environment);
}

} // namespace detail
} // namespace redshift_driver
} // namespace duckdb
