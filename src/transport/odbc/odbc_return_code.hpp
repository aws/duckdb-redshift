//===----------------------------------------------------------------------===//
//                         DuckDB - Redshift
//
// src/transport/odbc/odbc_return_code.hpp
//
// What an ODBC return code says about the call that returned it.
//===----------------------------------------------------------------------===//

#pragma once

#include "odbc_driver.hpp"

namespace duckdb {

//! Whether an ODBC call succeeded. SQL_SUCCESS_WITH_INFO is a success that also left
//! informational diagnostic records on the handle.
inline bool Succeeded(SQLRETURN rc) {
	return rc == SQL_SUCCESS || rc == SQL_SUCCESS_WITH_INFO;
}

} // namespace duckdb
