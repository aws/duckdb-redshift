//===----------------------------------------------------------------------===//
// The log sink the SQL surface passes to the client: DuckDB's logger, reached through
// the database. See redshift_log_sink.hpp.
//===----------------------------------------------------------------------===//

#include "redshift_log_sink.hpp"

#include "duckdb/logging/logger.hpp"
#include "duckdb/main/database.hpp"

namespace duckdb {

namespace {

class DuckDBLogSink : public RedshiftLogSink {
public:
	explicit DuckDBLogSink(DatabaseInstance &db_p) : db(db_p.shared_from_this()) {
	}

	void Debug(const string &message) noexcept override {
		// Locked per line rather than held, so the sink holds the database only while it writes a
		// line. If the last other owner lets go meanwhile, the database closes on this thread when
		// the line returns. Lines are dropped once the last owner has let go, including lines
		// logged while the database closes its attached databases. No session redshift_query opens
		// closes that late: each closes with its DuckDB connection, which holds the database until
		// then, with its handle while that connection still holds it, or, once lost, from the query
		// that met the loss (storage/redshift_connection_ownership.cpp).
		auto instance = db.lock();
		if (!instance) {
			return;
		}
		// The caller may be a destructor, so a line that cannot be written is dropped.
		try {
			DUCKDB_LOG_DEBUG(*instance, message);
		} catch (...) { // NOLINT
		}
	}

private:
	weak_ptr<DatabaseInstance> db;
};

} // namespace

shared_ptr<RedshiftLogSink> CreateRedshiftLogSink(DatabaseInstance &db) {
	return make_shared_ptr<DuckDBLogSink>(db);
}

} // namespace duckdb
