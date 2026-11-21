// A log sink the Catch2 tests own, for counting the mock's and the extension's lines, and
// reading them in order, without reading duckdb_logs: the counts outlive the database, and a test
// can read them while other threads log. Also the database fixture most cases share.

#pragma once

#include "catch.hpp"

#include <mutex>

#include "duckdb.hpp"
#include "duckdb/common/string_util.hpp"
#include "duckdb/common/unordered_map.hpp"
#include "duckdb/logging/log_manager.hpp"
#include "mock_transport.hpp"
#include "redshift_extension.hpp"
#include "redshift_transport.hpp"
#include "test_util.hpp"

namespace redshift_test {

//! Counts the lines starting with "redshift ", the mock's and the tests' own, and "redshift_query: ",
//! the extension's own, and keeps them in order; it leaves DuckDB's alone. With `refuse` set it
//! throws on each counted line, so a test reaches the sink's catch.
class RedshiftLineStorage : public duckdb::LogSink {
public:
	explicit RedshiftLineStorage(bool refuse_p = false) : refuse(refuse_p) {
	}

	const duckdb::string GetSinkName() override {
		return "redshift_test_lines";
	}

	void WriteLogEntry(duckdb::timestamp_t, duckdb::LogLevel, const duckdb::string &, const duckdb::string &log_message,
	                   const duckdb::RegisteredLoggingContext &) override {
		if (!duckdb::StringUtil::StartsWith(log_message, "redshift ") &&
		    !duckdb::StringUtil::StartsWith(log_message, "redshift_query: ")) {
			return;
		}
		{
			std::lock_guard<std::mutex> guard(lock);
			counts[log_message]++;
			lines.push_back(log_message);
		}
		if (refuse) {
			throw duckdb::IOException("log storage refused the line");
		}
	}

	void WriteLogEntries(duckdb::DataChunk &, const duckdb::RegisteredLoggingContext &) override {
	}

	void FlushAll() override {
	}

	void Flush(duckdb::LoggingTargetTable) override {
	}

	bool IsEnabled(duckdb::LoggingTargetTable) override {
		return true;
	}

	//! How many counted lines are exactly `message`.
	duckdb::idx_t Count(const duckdb::string &message) {
		std::lock_guard<std::mutex> guard(lock);
		auto entry = counts.find(message);
		return entry == counts.end() ? 0 : entry->second;
	}

	//! How many counted lines start with `prefix`.
	duckdb::idx_t CountStartingWith(const duckdb::string &prefix) {
		std::lock_guard<std::mutex> guard(lock);
		duckdb::idx_t total = 0;
		for (auto &entry : counts) {
			if (duckdb::StringUtil::StartsWith(entry.first, prefix)) {
				total += entry.second;
			}
		}
		return total;
	}

	//! The mock's open lines, for any configuration.
	duckdb::idx_t Opened() {
		return CountStartingWith("redshift mock: opened a connection to ");
	}

	//! The mock's close lines, for any configuration.
	duckdb::idx_t Closed() {
		return CountStartingWith("redshift mock: closed a connection to ");
	}

	//! The mock's Describe lines, for any configuration.
	duckdb::idx_t Described() {
		return CountStartingWith("redshift mock: described a statement on ");
	}

	//! The mock's execution lines, for any configuration.
	duckdb::idx_t Executed() {
		return CountStartingWith("redshift mock: executed a statement on ");
	}

	//! Every counted line, in the order the lines were written.
	duckdb::vector<duckdb::string> Lines() {
		std::lock_guard<std::mutex> guard(lock);
		return lines;
	}

	//! Forgets every line counted so far, so a fixture's own setup is not counted as the case's.
	void Forget() {
		std::lock_guard<std::mutex> guard(lock);
		counts.clear();
		lines.clear();
	}

private:
	const bool refuse;
	//! Guards `counts` and `lines`: tests read them while other threads log.
	std::mutex lock;
	duckdb::unordered_map<duckdb::string, duckdb::idx_t> counts;
	duckdb::vector<duckdb::string> lines;
};

//! Sends `db`'s debug lines to `storage`. The caller keeps its own reference, so it can read the
//! storage after `db` has closed.
inline void UseLogStorage(duckdb::DatabaseInstance &db, duckdb::shared_ptr<RedshiftLineStorage> storage) {
	auto &log_manager = db.GetLogManager();
	duckdb::shared_ptr<duckdb::LogSink> registered = std::move(storage); // RegisterLogSink moves from it
	REQUIRE(log_manager.RegisterLogSink("redshift_test_lines", registered));
	log_manager.SetLogSink(db, "redshift_test_lines");
	log_manager.SetLogLevel(duckdb::LogLevel::LOG_DEBUG);
	log_manager.SetEnableLogging(true);
}

//! A sink that drops every line, for a mock connection a test opens outside any database.
class DroppingLogSink : public duckdb::RedshiftLogSink {
public:
	void Debug(const duckdb::string &) noexcept override {
	}
};

//! An in-memory database with the extension loaded, its lines counted, and handle `rs`.
struct LoggedDatabase {
	//! Counts the lines. Shared with the database's log manager, which writes to it, so the counts can
	//! be read after the database closes.
	duckdb::shared_ptr<RedshiftLineStorage> lines;
	duckdb::DuckDB db {nullptr, NoPersistentSecrets().get()};

	explicit LoggedDatabase(bool refuse_lines = false)
	    : lines(duckdb::make_shared_ptr<RedshiftLineStorage>(refuse_lines)) {
		// A failure an earlier case armed and never used would fail this case's first open.
		duckdb::DisarmMockOpenFailure();
		db.LoadStaticExtension<duckdb::RedshiftExtension>();
		UseLogStorage(*db.instance, lines);
		{
			duckdb::Connection con(db);
			Run(con, Attach("rs"));
		}
		// The ATTACH opened its connection's session, which closed with that connection; the case counts
		// only what it does itself.
		lines->Forget();
	}
};

} // namespace redshift_test
