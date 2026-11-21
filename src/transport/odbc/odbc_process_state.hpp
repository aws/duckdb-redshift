//===----------------------------------------------------------------------===//
//                         DuckDB - Redshift
//
// src/transport/odbc/odbc_process_state.hpp
//
// What the process keeps about reaching the driver: where the driver comes from, the driver once
// it has loaded, the one ODBC environment, the lock that serialises every connect, the clock a
// connect reads, and the driver's SQL_GETDATA_EXTENSIONS once a result has needed it.
//===----------------------------------------------------------------------===//

#pragma once

#include "duckdb/common/mutex.hpp"
#include "duckdb/common/optional.hpp"
#include "duckdb/common/unique_ptr.hpp"
#include "odbc/bulk_fetch.hpp"
#include "odbc/fetch_producer.hpp"
#include "odbc_driver.hpp"
#include "redshift_transport.hpp"

#include <chrono>
#include <functional>

namespace duckdb {

//! What a connect reads the time from, to measure its attempts and the resume budget, and what a
//! connection reads how long ago Describe prepared a statement from.
using ConnectClock = std::function<std::chrono::steady_clock::time_point()>;

//! Process-wide state for reaching the driver.
struct OdbcProcessState {
	//! Where a connection obtains the driver. The test-only setter replaces it.
	redshift_driver::DriverSource source = &redshift_driver::LoadEmbeddedDriver;
	//! Guards `driver`, `environment` and `getdata_extensions`, and serialises the calls to `source`:
	//! one lock covers them, and a null member after a failure is simply set up again. Also
	//! orders the client's environment reads (ReadEnvironmentVariable, the refused-variable check)
	//! against the environment allocation's setenv of ODBCINI on Linux and macOS; Windows has no
	//! setenv there, and its reads take the lock all the same.
	mutex lock;
	//! The driver, once a call to `source` has succeeded; null until then. A failure is not kept,
	//! so the next connection calls `source` again.
	const redshift_driver::LoadedDriver *driver = nullptr;
	//! The one ODBC environment, with ODBC 3 selected, allocated by the first connection that needs
	//! it; null until then and after a failed allocation, so the next connection tries again.
	//! Not freed in a shipped build, so no cleanup can invalidate a connection
	//! open on another thread; the test-only reset frees it.
	SQLHANDLE environment = SQL_NULL_HANDLE;
	//! Held around every connect. Driver 2.3.0.0 parsed each connection string through a
	//! process-wide cache with no lock and kept its TLS settings process-wide; 2.3.1.0 drops the
	//! cache and keeps them per connection, and connects stay one at a time as before.
	mutex connect_lock;
	//! Read, and copied by each connection as it opens, under `connect_lock`. The test-only setter
	//! replaces it, so a case can pass the resume budget, or the bound on Describe's statement, without
	//! waiting for it.
	ConnectClock connect_clock = [] {
		return std::chrono::steady_clock::now();
	};
	//! How a connection's results fetch numbers, read under `connect_lock` as it opens. The test-only
	//! setter replaces it, so a case can run the same script over both bindings.
	NumberBinding number_binding = DEFAULT_NUMBER_BINDING;
	//! Which thread fills a connection's results' buffer sets, read under `connect_lock` as it opens.
	//! The test-only setter replaces it, so a case can run the ring on the caller's thread.
	FetchThread fetch_thread = FetchThread::PRODUCER;
	//! The driver's SQL_GETDATA_EXTENSIONS, read by the first result with a long column; unset until
	//! a read succeeds. Only logged, so a driver change shows: a long column is read over a row array
	//! of 1 whatever it reports.
	optional<SQLUINTEGER> getdata_extensions;
};

//! The one state object, never destroyed, so a connection still open at exit keeps its locks. The
//! test-only reset replaces it whole, so no test inherits another's.
unique_ptr<OdbcProcessState> &CurrentOdbcProcessState();

//! The loaded driver, calling the driver source when no call has succeeded in this process yet,
//! and logging the driver's identity on the call that loads it. Throws
//! IOException with the source's message unchanged after a fixed prefix when the source fails.
const redshift_driver::LoadedDriver &ObtainDriver(RedshiftLogSink &log_sink);

//! Reads SQL_GETDATA_EXTENSIONS on `connection` unless the process holds it, and logs what the read
//! gave. A failed read is logged and left for the next result with a long column to try again.
void LogGetDataExtensions(const redshift_driver::OdbcApi &api, SQLHANDLE connection, RedshiftLogSink &log_sink);

//! The process's ODBC environment, allocated through `driver`'s api with ODBC 3 selected when no
//! connection has allocated it yet. Throws IOException naming the driver under `redshift_driver`
//! when either step fails, leaving no environment.
SQLHANDLE ObtainEnvironment(const redshift_driver::LoadedDriver &driver);

} // namespace duckdb
