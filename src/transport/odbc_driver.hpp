// src/transport/odbc_driver.hpp
// The one contract between the ODBC client (ODBC behaviour, src/transport/odbc/) and the driver loader
// (shipping and loading the driver per platform, src/transport/driver/). The ODBC client calls driver code
// only through LoadedDriver::api. The driver loader knows nothing about connection strings, statements or
// diagnostics. Platform-neutral on purpose: no type, member or signature here names a loader handle,
// and a driver is a set of files with hashes, so another platform's loader can sit behind the same
// declarations.
#pragma once
// Before any include: a standard library parsed as 32-bit Windows can fail before the #error.
#if defined(_WIN32) && !defined(_WIN64)
#error "32-bit Windows is not a supported platform; build windows_amd64"
#endif
#include <cstdint>
#include <cstdlib>
#include <string>
#include <type_traits>
#include <vector>

// The vendored sqltypes.h needs the size of long and otherwise includes a configure-generated
// unixodbc.h, which is not vendored. Linux and macOS are LP64. Windows x64 is LLP64: long is 4
// bytes, and SQLLEN, SQLULEN and SQLBIGINT are long long. This block is the one place a platform
// chooses its ODBC headers and type sizes.
#if defined(_WIN64)
#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef _WINSOCKAPI_
#define _WINSOCKAPI_
#endif
#include <windows.h> // first, as DuckDB includes it, so sqltypes.h's stand-ins agree with it
#ifndef SIZEOF_LONG_INT
#define SIZEOF_LONG_INT 4
#endif
#ifndef HAVE_LONG_LONG
#define HAVE_LONG_LONG 1 // SQLBIGINT is long long, not a struct
#endif
#ifndef SQL_WCHART_CONVERT
#define SQL_WCHART_CONVERT // sqltypes.h's WCHAR is wchar_t, as windows.h's is
#endif
#endif
#ifndef SIZEOF_LONG_INT
#define SIZEOF_LONG_INT 8
#endif
#ifndef SQL_NOUNICODEMAP
#define SQL_NOUNICODEMAP // the table holds narrow entry points even where UNICODE is defined
#endif
#if defined(_MSC_VER)
// sqltypes.h redefines windows.h's FAR and CALLBACK as empty (C4005). x64 has one calling
// convention, so nothing changes, and a build that treats warnings as errors still compiles.
#pragma warning(push)
#pragma warning(disable : 4005)
#endif
#include "sql.h" // third_party/unixodbc/: unmodified unixODBC 2.3.12 headers
#include "sqlext.h"
#if defined(_MSC_VER)
#pragma warning(pop)
#endif
// sqltypes.h defines BOOL as a macro, which breaks DuckDB's PhysicalType::BOOL in any DuckDB
// header included after this one. sqlext.h has already used it, so nothing here needs it.
#undef BOOL
#if defined(_WIN32)
// The Win32 macros DuckDB removes before its own declarations (duckdb/common/windows_undefs.hpp),
// so a DuckDB header included after this one still compiles. Copied, because this header
// includes no DuckDB header; a test checks the copy against DuckDB's list.
#undef min
#undef max
#undef ERROR
#undef small
#undef CreateDirectory
#undef MoveFile
#undef RemoveDirectory
#undef UUID
#undef interface
#undef OPTIONAL
#undef LIST
#undef DELETE
#endif

// The fake, the ODBC client and the loaded driver must agree on the ABI the driver was built with.
static_assert(sizeof(SQLLEN) == sizeof(void *), "SQLLEN must be pointer-sized");
static_assert(sizeof(SQLULEN) == sizeof(void *), "SQLULEN must be pointer-sized");
static_assert(sizeof(SQLHANDLE) == sizeof(void *), "SQLHANDLE must be pointer-sized");
static_assert(sizeof(SQLINTEGER) == 4, "SQLINTEGER must be 4 bytes");
static_assert(sizeof(SQLBIGINT) == 8, "SQLBIGINT must be 8 bytes");
// Without HAVE_LONG_LONG where long is 4 bytes, sqltypes.h makes SQLBIGINT a struct of two ints,
// which is 8 bytes too.
static_assert(std::is_integral<SQLBIGINT>::value, "SQLBIGINT must be an integer type");

namespace duckdb {
namespace redshift_driver {

// Every driver entry point the ODBC client calls (narrow ANSI), 28 in all. Adding one is an interface
// change that updates the fake driver and must pass the driver loader's expected-exports check
// unchanged. SQLCancel is not among them. ExecDirect runs a statement with no parameters after
// SQLPrepare has checked its shape: the server streams a simple-protocol result about twice as fast.
// GetConnectAttr reads SQL_ATTR_CONNECTION_DEAD. NumParams lets Execute
// refuse a parameter-count mismatch after prepare, before binding.
#define REDSHIFT_ODBC_ENTRY_POINTS(X)                                                                                  \
	X(AllocHandle)                                                                                                     \
	X(FreeHandle)                                                                                                      \
	X(SetEnvAttr)                                                                                                      \
	X(SetConnectAttr)                                                                                                  \
	X(GetConnectAttr)                                                                                                  \
	X(DriverConnect)                                                                                                   \
	X(Disconnect)                                                                                                      \
	X(EndTran)                                                                                                         \
	X(GetInfo)                                                                                                         \
	X(Prepare)                                                                                                         \
	X(NumParams)                                                                                                       \
	X(NumResultCols)                                                                                                   \
	X(DescribeCol)                                                                                                     \
	X(ColAttribute)                                                                                                    \
	X(BindParameter)                                                                                                   \
	X(Execute)                                                                                                         \
	X(ExecDirect)                                                                                                      \
	X(RowCount)                                                                                                        \
	X(MoreResults)                                                                                                     \
	X(SetStmtAttr)                                                                                                     \
	X(BindCol)                                                                                                         \
	X(Fetch)                                                                                                           \
	X(SetPos)                                                                                                          \
	X(GetData)                                                                                                         \
	X(FreeStmt)                                                                                                        \
	X(CloseCursor)                                                                                                     \
	X(GetDiagRec)                                                                                                      \
	X(GetDiagField)

struct OdbcApi {
#define REDSHIFT_ODBC_MEMBER(name) decltype(&::SQL##name) name = nullptr;
	REDSHIFT_ODBC_ENTRY_POINTS(REDSHIFT_ODBC_MEMBER)
#undef REDSHIFT_ODBC_MEMBER

	// Every member non-null.
	bool Complete() const {
		return FirstMissing() == nullptr;
	}
	// The first null member's driver name, e.g. "SQLGetDiagField"; nullptr when Complete().
	const char *FirstMissing() const {
#define REDSHIFT_ODBC_CHECK(name)                                                                                      \
	if (name == nullptr) {                                                                                             \
		return "SQL" #name;                                                                                            \
	}
		REDSHIFT_ODBC_ENTRY_POINTS(REDSHIFT_ODBC_CHECK)
#undef REDSHIFT_ODBC_CHECK
		return nullptr;
	}
};

enum class DriverOrigin : uint8_t {
	EMBEDDED = 0,     // the only origin a shipped build can produce
	TEST_FAKE = 1,    // test/client/fake_driver only
	TEST_BY_PATH = 2, // test/client/interface/by_path_driver only
};

struct DriverFile {
	std::string name;   // file name inside the driver directory, e.g. "librsodbc64.so"
	std::string sha256; // lowercase hex of the bytes checked before loading
};

struct DriverIdentity {
	DriverOrigin origin = DriverOrigin::EMBEDDED;
	std::string version;           // from the lock, e.g. "2.3.0.0"
	std::string platform;          // DuckDB platform, e.g. "linux_amd64"
	bool development = false;      // true when the lock's status is development; never published
	std::string set_sha256;        // names .redshift-driver/<set_sha256>/
	std::vector<DriverFile> files; // the driver first, then the licence notices
	std::string directory;         // absolute directory the set was loaded from

	// "embedded 2.3.0.0 linux_amd64", or "test-fake ..." or "test-by-path ..." for a test origin,
	// with " (development)" appended for a development lock.
	std::string Describe() const {
		// Every origin and no default, so -Wswitch names one left out: make tidy_client, a Debug
		// build (DuckDB's -Wall) and Clang, which enables it by default, do; a GCC Release build does
		// not. The fallback keeps an out-of-range value from building a string from a null pointer.
		const char *origin_text = "unknown-origin ";
		switch (origin) {
		case DriverOrigin::EMBEDDED:
			origin_text = "embedded ";
			break;
		case DriverOrigin::TEST_FAKE:
			origin_text = "test-fake ";
			break;
		case DriverOrigin::TEST_BY_PATH:
			origin_text = "test-by-path ";
			break;
		}
		std::string text = origin_text + version + " " + platform;
		return development ? text + " (development)" : text;
	}
};

struct OverriddenVariable {
	std::string name;    // e.g. "PGHOST"
	std::string keyword; // the connection-string keyword that overrides it, e.g. "Host"
};

struct LoadedDriver {
	OdbcApi api;
	DriverIdentity identity;
	// Variables this driver would apply to options the extension does not set. The driver loader fills it
	// from policy/refused-environment.txt, reviewed on every driver update. At least:
	// PGGSSLIB PGHOSTADDR PGKRBSRVNAME PGOPTIONS PGREQUIRESSL PGSERVICE PGSERVICEFILE PGSSLCERT
	// PGSSLCRL PGSSLKEY PGSSLROOTCERT PGSYSCONFDIR, and non-PG names such as SSLKEYLOGFILE, KRB5CCNAME and
	// ODBCINI (the policy file lists them all). Every refused name is here, whatever the earliest phase in which the
	// driver or a bundled library reads it (load or initialisation, handle allocation, connect), which the policy file
	// records per name. Names read at load or initialisation are also checked by the loader before the platform loader
	// call, from data generated at build time, because a check on this list could only run after they were read
	// (REFUSED_ENVIRONMENT below).
	std::vector<std::string> refused_environment;
	// Variables the driver reads that are harmless only because the connection string always
	// sets `keyword`. The driver loader fills it from policy/known-environment.txt; the ODBC client refuses to connect
	// when its connection string lacks one of these keywords.
	std::vector<OverriddenVariable> overridden_environment;
	// Windows: the folder the ODBC client writes its exported trust file in, made and checked by the loader
	// beside the driver root or in the per-user cache. Empty where neither
	// is usable, and on Linux and macOS.
	std::string trust_directory;
};

enum class DriverErrorKind : uint8_t {
	NOT_BUILT,              // driver_not_built.cpp placeholder, for a build made without a driver;
	                        // never published
	EMBEDDED_BYTES_CORRUPT, // embedded bytes differ from the hash recorded at build time
	NO_USABLE_DIRECTORY,    // every candidate directory was refused or unusable (see attempts), or the
	                        // loader failed inside itself before trying one (no attempts); not final
	MISSING_PREREQUISITE,   // a host library is absent, e.g. libkeyutils.so.1; named in message;
	                        // final, also when found before the platform loader call: install, restart
	LOAD_FAILED,            // the platform loader refused the driver or a library it needs, which the
	                        // loader loads first; no other directory is tried
	MISSING_ENTRY_POINT,    // message names OdbcApi::FirstMissing()
	REFUSED_ENVIRONMENT     // checked before the platform loader, so not final (the next call looks
	                        // again). A refused variable is set: on Linux and macOS a load-phase one, on
	                        // Windows any (the message names it, never its value). On macOS, a DYLD_
	                        // variable is set that could make the platform loader load another file. On
	                        // Linux, the platform loader's search list has an empty or relative element,
	                        // or cannot be read; the library cache cannot be read, or it or a system
	                        // directory has a library the driver needs where another user could change it;
	                        // or another library, or the program itself, has replaced the C library's
	                        // malloc, free, calloc or realloc (the message names its file name, never its
	                        // path, or says "another allocator" when no loaded object holds the
	                        // replacement). On Windows, the installed Redshift driver's LogLevel or Trace
	                        // registry value counts as on, or its key cannot be read; or a DbgHelp.dll
	                        // from outside System32 is loaded, or the process's modules cannot be listed.
	                        // Each has its own sentence, which says what to change.
};

struct DriverAttempt {
	std::string directory;
	std::string reason; // e.g. "parent writable by another user", "mounted noexec"
};

struct DriverError {
	DriverErrorKind kind = DriverErrorKind::NOT_BUILT;
	// Actionable: names each base directory tried by its role (REDSHIFT_DRIVER_DIR, the extension's
	// directory, the per-user cache) and the rule it broke, and points to REDSHIFT_DRIVER_DIR where
	// relevant. Never contains a path, a credential or the value of an environment variable: it can
	// reach the SQL caller. A loaded object's file name as the platform loader reports it is not an
	// environment variable's value, even where it is the whole of LD_PRELOAD's. The directories are in
	// `attempts`, which are never logged where SQL can read them. For a user namespace's overflow user
	// the message is one attempt and the advice to run as a mapped user.
	std::string message;
	std::vector<DriverAttempt> attempts;
};

struct DriverLoadResult {
	const LoadedDriver *driver = nullptr; // non-null on success; one object for the process
	DriverError error;                    // meaningful only when driver == nullptr; a copy
	bool ok() const {
		return driver != nullptr;
	}
};

// Implemented by the driver loader. Installs or reuses the private copy, re-hashes it, loads it with the
// platform's flags and fills `api`. Thread-safe; calls are serialised. The platform loader is
// called for the driver at most once per process, and every outcome from its first call on (for a
// library the driver needs, which the loader loads first, or for the driver), success or failure, is
// final and returned by every later call, because their code may already have run; so is
// MISSING_PREREQUISITE, wherever it is found. Any other failure before that call (no usable or safe
// directory, a failed install or repair, corrupt embedded bytes, a REFUSED_ENVIRONMENT) is returned
// and the next call tries again, so a user can fix the cause, or set REDSHIFT_DRIVER_DIR, without
// restarting. A success names the same
// LoadedDriver every time; a failure is returned by value. The driver is never unloaded. noexcept:
// The driver loader never throws, and the ODBC client turns a failure into IOException. The ODBC client calls it at a
// connection, never at extension LOAD.
DriverLoadResult LoadEmbeddedDriver() noexcept;

// How the ODBC client obtains a driver: the connection factory is given one. Shipped builds pass
// &LoadEmbeddedDriver; tests pass testing::FakeOdbcDriver::Source or testing::ByPathDriverSource.
using DriverSource = DriverLoadResult (*)();

// Implemented by the driver loader. The ODBC client allocates every environment handle through this, never with
// api.AllocHandle directly: it is driver.api.AllocHandle(SQL_HANDLE_ENV, SQL_NULL_HANDLE, environment).
// The driver reads $ODBCINI, or else ./odbc.ini, whenever an environment handle is allocated while it
// holds none (the first, or one after the last was freed), so this sets ODBCINI to /dev/null for that one
// call, under the loader's lock, and then unsets it again: the file in the current
// directory is not read, and FindRefusedEnvironmentVariable and other ODBC users in the process see ODBCINI
// unset as before (the ODBC client's refused-variable check must not run during the call). Returns the allocation's
// result, or SQL_ERROR with nothing allocated when ODBCINI is set already (the ODBC client's
// FindRefusedEnvironmentVariable check refuses it first) or cannot be set. noexcept like LoadEmbeddedDriver. On Windows
// the driver reads no ODBCINI, so the definition there is the plain allocation.
SQLRETURN AllocateEnvironmentHandle(const LoadedDriver &driver, SQLHENV *environment) noexcept;

// The ODBC client calls this before allocating the environment or a connection handle on every connect,
// because the environment can change between connects. Returns the first refused variable that
// is set (an empty value counts as set), or nullptr. Its value is never read into a message. The
// check assumes the environment does not change between it and the driver call it guards;
// same-process code is trusted. On Windows it reads the process's environment block, as the
// extension's static C runtime copy is fixed when the extension loads. Once the process has loaded
// ucrtbase.dll, it also reads that shared C runtime's copy, which the driver reads (rsodbc.dll imports
// getenv and _wdupenv_s) and which keeps a name SetEnvironmentVariableW removed from the block
// alone; this keeps the check right, and is not a trust boundary. The copy has a narrow table, made
// from the block when ucrtbase.dll loads, and a wide one, made from the block when a wmain program
// built on that runtime starts or a wide reader or writer first asks for it, so either can hold a
// name the other lacks. The wide one is read only where it exists: asking for it would make it here,
// holding whatever the block holds now after the block loses it, while one made later is made from
// the block this check read.
inline const char *FindRefusedEnvironmentVariable(const LoadedDriver &driver) {
#if defined(_WIN32)
	// ucrtbase.dll's readers and its wide table, looked up on every call, as the driver's load can bring
	// the DLL in after an earlier call found none. GetModuleHandleW takes no reference, so nothing is
	// freed. A loaded DLL that lacks any of the three exports cannot be read, so every name counts as
	// set (fail closed).
	using NarrowGetenv = char *(__cdecl *)(const char *);
	using WideGetenv = wchar_t *(__cdecl *)(const wchar_t *);
	using WideTable = wchar_t ***(__cdecl *)();
	NarrowGetenv narrow_getenv = nullptr;
	WideGetenv wide_getenv = nullptr;
	WideTable wide_table = nullptr;
	if (const HMODULE ucrt = GetModuleHandleW(L"ucrtbase.dll")) {
		narrow_getenv = reinterpret_cast<NarrowGetenv>(GetProcAddress(ucrt, "getenv"));
		wide_getenv = reinterpret_cast<WideGetenv>(GetProcAddress(ucrt, "_wgetenv"));
		wide_table = reinterpret_cast<WideTable>(GetProcAddress(ucrt, "__p__wenviron"));
		if ((!narrow_getenv || !wide_getenv || !wide_table) && !driver.refused_environment.empty()) {
			return driver.refused_environment.front().c_str();
		}
	}
#endif
	for (const auto &name : driver.refused_environment) {
#if defined(_WIN32)
		// Names are ASCII; any other counts as set. No buffer, so no value is read, and the error
		// is cleared first, so a stale one cannot read as absent.
		std::wstring wide;
		for (unsigned char c : name) {
			if (c >= 0x80) {
				return name.c_str();
			}
			wide.push_back(c);
		}
		SetLastError(ERROR_SUCCESS);
		if (GetEnvironmentVariableW(wide.c_str(), nullptr, 0) != 0 || GetLastError() != ERROR_ENVVAR_NOT_FOUND) {
			return name.c_str(); // set, empty or unreadable: refuse
		}
		// Still in a table of the copy the driver reads. narrow_getenv is set only with the other two.
		if (narrow_getenv != nullptr && (narrow_getenv(name.c_str()) != nullptr ||
		                                 (*wide_table() != nullptr && wide_getenv(wide.c_str()) != nullptr))) {
			return name.c_str();
		}
#else
		if (std::getenv(name.c_str()) != nullptr) {
			return name.c_str();
		}
#endif
	}
	return nullptr;
}

} // namespace redshift_driver
} // namespace duckdb
