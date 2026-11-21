//===----------------------------------------------------------------------===//
//                         DuckDB - Redshift
//
// src/transport/odbc/accountless_settings.cpp
//
// The public build's AccountlessWorkgroupSettings: it refuses every Serverless workgroup named
// without an account. A tree that carries a preview implementation compiles that one instead
// (redshift_select_hook), never both.
//===----------------------------------------------------------------------===//

#include "odbc/connection_settings.hpp"

#include "duckdb/common/exception.hpp"

namespace duckdb {

TargetSettings AccountlessWorkgroupSettings(const RedshiftConfig &) {
	// Before any driver call, like every refused target, and naming no value.
	throw InvalidInputException("Invalid Redshift target: a Serverless workgroup needs the account that owns it");
}

} // namespace duckdb
