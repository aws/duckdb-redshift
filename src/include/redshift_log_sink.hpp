//===----------------------------------------------------------------------===//
//                         DuckDB - Redshift
//
// redshift_log_sink.hpp
//
// The client's log lines, written to DuckDB's logger.
//===----------------------------------------------------------------------===//

#pragma once

#include "redshift_transport.hpp"

namespace duckdb {

class DatabaseInstance;

//! The sink passed to OpenRedshiftConnection: each line goes to `db`'s logger at debug level.
//! It reaches the logger through the database rather than a ClientContext, because the client
//! logs from destructors that can run after the DuckDB connection that opened it has closed. It
//! holds `db` only while it writes a line; if the last other owner lets go meanwhile, `db` closes
//! on that thread when the line returns. Lines are dropped once `db`'s last owner has let go,
//! including lines logged while `db` closes its attached databases; the sessions redshift_query
//! opens have all closed before then.
shared_ptr<RedshiftLogSink> CreateRedshiftLogSink(DatabaseInstance &db);

} // namespace duckdb
