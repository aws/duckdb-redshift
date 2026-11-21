//===----------------------------------------------------------------------===//
//                         DuckDB - Redshift
//
// redshift_context_log.hpp
//
// The extension's own log lines, written to a DuckDB connection's logger.
//===----------------------------------------------------------------------===//

#pragma once

#include "duckdb/common/common.hpp"
#include "duckdb/logging/logger.hpp"
#include "duckdb/main/client_context.hpp"

namespace duckdb {

//! Write `message` to `context`'s logger at info level. It does not throw: a log storage can refuse a
//! line, and a line that cannot be written is dropped, as the client's sink drops one
//! (redshift_log_sink.cpp), rather than fail the statement that logs it.
inline void LogInfo(ClientContext &context, const string &message) noexcept {
	try {
		DUCKDB_LOG_INFO(context, message);
	} catch (...) { // NOLINT
	}
}

//! Write `message` at warning level, dropping it if the log storage refuses it, as LogInfo does.
inline void LogWarning(ClientContext &context, const string &message) noexcept {
	try {
		DUCKDB_LOG_WARNING(context, message);
	} catch (...) { // NOLINT
	}
}

} // namespace duckdb
