//===----------------------------------------------------------------------===//
//                         DuckDB - Redshift
//
// test/client/odbc/driver_source_hook.hpp
//
// The test-only hooks that replace the process's driver source, so an offline test runs the ODBC client's
// real code against the fake driver, the clock a connect reads, and how a result fetches numbers.
// Listed in this area's sources.cmake alone, so the shipped extension does not compile it.
//===----------------------------------------------------------------------===//

#pragma once

#include "odbc/odbc_process_state.hpp"
#include "odbc_driver.hpp"

namespace duckdb {

//! Makes every later connection obtain its driver from `source`, starting from a fresh process
//! state.
void SetDriverSourceForTesting(redshift_driver::DriverSource source);

//! Puts the process back in a fresh process's state: the shipped driver source, no driver and no
//! environment, which it frees through the driver. Call it before the fake driver that served the
//! test is destroyed.
void ResetProcessStateForTesting();

//! Makes every later connect read the time from `clock`, until the next reset restores the steady
//! clock.
void SetConnectClockForTesting(ConnectClock clock);

//! Makes every later connection's results fetch numbers as `binding` says, until the next reset
//! restores the shipped binding.
void SetNumberBindingForTesting(NumberBinding binding);

//! Makes every later connection's results fill their buffer sets on `thread`, until the next reset
//! restores the producer thread: FetchThread::CALLER runs the ring in each NextBatch instead.
void SetFetchThreadForTesting(FetchThread thread);

} // namespace duckdb
