#include "odbc/odbc_process_state.hpp"

#include "duckdb/common/exception.hpp"
#include "duckdb/common/numeric_utils.hpp"
#include "duckdb/common/string_util.hpp"
#include "odbc/odbc_diagnostics.hpp"
#include "odbc/odbc_errors.hpp"
#include "odbc/odbc_return_code.hpp"
#include "odbc/process_environment.hpp"

#include <cstdint>
#include <cstdlib>

#ifdef _WIN32
#include "odbc/windows_text.hpp"
#endif

namespace duckdb {

unique_ptr<OdbcProcessState> &CurrentOdbcProcessState() {
	// Never destroyed, like the driver's loader: exit's static destructors would otherwise free the
	// locks and the environment while a connection on another thread is still using them.
	// NOLINTNEXTLINE(cppcoreguidelines-avoid-non-const-global-variables): never destroyed, on purpose
	static auto *state = new unique_ptr<OdbcProcessState>(make_uniq<OdbcProcessState>());
	return *state;
}

optional<string> ReadEnvironmentVariable(const char *name) {
	lock_guard<mutex> guard(CurrentOdbcProcessState()->lock);
#ifdef _WIN32
	// DuckDB links the C runtime statically, so getenv would read this module's copy, fixed when the
	// extension loaded, while the driver's libraries see the process's block, which a host can still
	// change.
	const auto wide_name = Utf8ToWide(name);
	const auto read = ReadWholeValue<wchar_t>([&](wchar_t *buffer, uint32_t capacity) {
		SetLastError(ERROR_SUCCESS);
		const DWORD length = GetEnvironmentVariableW(wide_name.c_str(), buffer, capacity);
		const bool not_found = length == 0 && GetLastError() == ERROR_ENVVAR_NOT_FOUND;
		return EnvironmentCall {static_cast<uint32_t>(length), not_found};
	});
	return EnvironmentValue(read, WideToUtf8);
#else
	const char *value = std::getenv(name);
	if (value == nullptr) {
		return nullopt;
	}
	return string(value);
#endif
}

namespace {

//! The driver, and whether the call that found it is the one that loaded it.
struct DriverLookup {
	const redshift_driver::LoadedDriver *driver;
	bool loaded_now;
};

//! The driver, calling the source while none has loaded.
DriverLookup FindOrLoadDriver(OdbcProcessState &state) {
	lock_guard<mutex> guard(state.lock);
	if (state.driver != nullptr) {
		return DriverLookup {state.driver, false};
	}
	const auto result = state.source();
	if (!result.ok()) {
		// Concatenated, not formatted: the message is the driver loader's data, and a '%' in it must reach the
		// user as written. It already says what to fix, so nothing is added.
		throw IOException("the Redshift driver could not be loaded: " + result.error.message);
	}
	state.driver = result.driver;
	return DriverLookup {result.driver, true};
}

//! A new environment with ODBC 3 selected, or IOException with no environment left allocated.
SQLHANDLE AllocateEnvironment(const redshift_driver::LoadedDriver &driver) {
	const auto &api = driver.api;
	const auto info = DriverInfo(driver.identity);
	SQLHANDLE environment = SQL_NULL_HANDLE;
	// Through the driver loader's wrapper, never api.AllocHandle: it keeps the driver from reading an odbc.ini in the
	// current directory, which it does at an environment allocation while it holds none.
	if (!Succeeded(redshift_driver::AllocateEnvironmentHandle(driver, &environment))) {
		throw IOException(info, "the Redshift driver could not allocate an ODBC environment; run the statement again");
	}
	// ODBC 3 is selected before the environment is used for anything else.
	if (!Succeeded(api.SetEnvAttr(environment, SQL_ATTR_ODBC_VERSION,
	                              reinterpret_cast<SQLPOINTER>(static_cast<uintptr_t>(SQL_OV_ODBC3)), 0))) {
		// Read before the free, which takes the records with the handle.
		const auto records = ReadConnectDiagnostics(api, {SQL_HANDLE_ENV, environment});
		api.FreeHandle(SQL_HANDLE_ENV, environment);
		ThrowPreConnectFailure(records, driver.identity,
		                       "the Redshift driver refused ODBC version 3, which this extension needs",
		                       "install a published release of the extension");
	}
	return environment;
}

} // namespace

const redshift_driver::LoadedDriver &ObtainDriver(RedshiftLogSink &log_sink) {
	const auto lookup = FindOrLoadDriver(*CurrentOdbcProcessState());
	// Logged after the lock is released: a sink call can close the DuckDB database on this thread
	// (RedshiftLogSink::Debug in redshift_transport.hpp).
	if (lookup.loaded_now) {
		log_sink.Debug("Redshift driver loaded: " + lookup.driver->identity.Describe());
	}
	return *lookup.driver;
}

void LogGetDataExtensions(const redshift_driver::OdbcApi &api, SQLHANDLE connection, RedshiftLogSink &log_sink) {
	auto &state = *CurrentOdbcProcessState();
	SQLUINTEGER extensions = 0;
	bool read = false;
	{
		lock_guard<mutex> guard(state.lock);
		if (state.getdata_extensions) {
			return;
		}
		read = Succeeded(api.GetInfo(connection, SQL_GETDATA_EXTENSIONS, &extensions,
		                             NumericCast<SQLSMALLINT>(sizeof(extensions)), nullptr));
		if (read) {
			state.getdata_extensions = extensions;
		}
	}
	// Logged after the lock is released, as ObtainDriver's line is.
	if (!read) {
		log_sink.Debug("Redshift driver's SQL_GETDATA_EXTENSIONS could not be read; a long column is read one row "
		               "a fetch either way");
		return;
	}
	log_sink.Debug(StringUtil::Format("Redshift driver's SQL_GETDATA_EXTENSIONS: 0x%x", extensions));
}

SQLHANDLE ObtainEnvironment(const redshift_driver::LoadedDriver &driver) {
	auto &state = *CurrentOdbcProcessState();
	lock_guard<mutex> guard(state.lock);
	if (state.environment == SQL_NULL_HANDLE) {
		state.environment = AllocateEnvironment(driver);
	}
	return state.environment;
}

} // namespace duckdb
