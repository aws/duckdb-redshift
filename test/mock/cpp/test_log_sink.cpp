// The log sink the SQL surface passes to the client, observed through the mock's open and close
// lines. The close line comes from a destructor, so two cases destroy a connection at the late
// points it can reach: after the DuckDB connection that opened it has closed, and after the
// database has. The sink holds the database only while it writes a line, so it never keeps an idle
// database open; threads logging through it while the database closes write only the earlier
// lines; and a line the logger refuses is dropped.

#include "catch.hpp"

#include <atomic>
#include <chrono>
#include <thread>

#include "duckdb.hpp"
#include "duckdb/common/string_util.hpp"
#include "redshift_log_sink.hpp"
#include "redshift_transport.hpp"
#include "test_log_storage.hpp"
#include "test_util.hpp"

using namespace duckdb;

namespace {

//! The number of debug lines the database logged with exactly `message`, as a BIGINT Value.
//! Fails the test unless the query itself succeeded.
Value DebugLinesLogged(Connection &reader, const string &message) {
	auto logged =
	    reader.Query("SELECT count(*) FROM duckdb_logs WHERE log_level = 'DEBUG' AND message = '" + message + "'");
	REQUIRE_FALSE(logged->HasError());
	auto chunk = logged->Fetch();
	REQUIRE(chunk);
	return chunk->GetValue(0, 0);
}

const string OPENED_LINE = "redshift mock: opened a connection to " + redshift_test::MockTarget();
const string CLOSED_LINE = "redshift mock: closed a connection to " + redshift_test::MockTarget();

} // namespace

TEST_CASE("a mock connection destroyed after its DuckDB connection closed logs its close", "[log_sink]") {
	DuckDB db(nullptr);
	Connection reader(db);
	REQUIRE_FALSE(reader.Query("CALL enable_logging(level = 'debug')")->HasError());
	unique_ptr<RedshiftConnection> redshift;
	{
		Connection opener(db);
		redshift = OpenRedshiftConnection(redshift_test::MockConfig(),
		                                  CreateRedshiftLogSink(DatabaseInstance::GetDatabase(*opener.context)));
	}
	redshift.reset();

	CHECK(DebugLinesLogged(reader, CLOSED_LINE) == Value::BIGINT(1));
}

TEST_CASE("an idle log sink does not keep its database open", "[log_sink]") {
	auto db = make_uniq<DuckDB>(nullptr);
	weak_ptr<DatabaseInstance> instance = db->instance;
	auto log_sink = CreateRedshiftLogSink(*db->instance);

	db.reset();

	CHECK(instance.expired());
}

TEST_CASE("a line the mock logs after its database closed is dropped", "[log_sink]") {
	auto db = make_uniq<DuckDB>(nullptr);
	weak_ptr<DatabaseInstance> instance = db->instance;
	auto storage = make_shared_ptr<redshift_test::RedshiftLineStorage>(false);
	redshift_test::UseLogStorage(*db->instance, storage);
	auto redshift = OpenRedshiftConnection(redshift_test::MockConfig(), CreateRedshiftLogSink(*db->instance));
	db.reset();
	// The open connection's sink did not keep the database from closing, so its close line has
	// nowhere to go.
	REQUIRE(instance.expired());

	// The mock logs its close from its destructor after the database has closed. Debug is
	// noexcept, so a throw that escaped it would terminate the run.
	redshift.reset();

	CHECK(storage->Count(OPENED_LINE) == 1);
	CHECK(storage->Count(CLOSED_LINE) == 0);
}

TEST_CASE("threads logging through one sink while its database closes drop only later lines", "[log_sink]") {
	auto db = make_uniq<DuckDB>(nullptr);
	weak_ptr<DatabaseInstance> instance = db->instance;
	auto storage = make_shared_ptr<redshift_test::RedshiftLineStorage>(false);
	redshift_test::UseLogStorage(*db->instance, storage);
	auto log_sink = CreateRedshiftLogSink(*db->instance);

	// Each thread logs one line before the database closes, then keeps logging while it closes, so
	// a lock() races the last release and the database may close on that thread, as it can on the
	// client's fetch thread. The loop has a deadline, so a sink that kept the database open fails
	// the checks below instead of hanging. A line count would not do: the log manager's mutex is
	// not fair, so early threads could use up a count before the last thread logged its first line.
	const idx_t thread_count = 4;
	const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(30);
	std::atomic<idx_t> started(0);
	vector<std::thread> threads;
	for (idx_t i = 0; i < thread_count; i++) {
		threads.emplace_back([&]() {
			log_sink->Debug("redshift test: before close");
			started++;
			while (!instance.expired() && std::chrono::steady_clock::now() < deadline) {
				log_sink->Debug("redshift test: while closing");
			}
			// Only once this thread has seen the database gone, so the count below does not depend
			// on the deadline.
			if (instance.expired()) {
				log_sink->Debug("redshift test: after close");
			}
		});
	}
	while (started < thread_count) {
		std::this_thread::yield();
	}
	db.reset();
	for (auto &thread : threads) {
		thread.join();
	}

	CHECK(instance.expired());
	CHECK(storage->Count("redshift test: before close") == thread_count);
	CHECK(storage->Count("redshift test: after close") == 0);
}

TEST_CASE("a line the logger cannot write is dropped", "[log_sink]") {
	DuckDB db(nullptr);
	auto refusing = make_shared_ptr<redshift_test::RedshiftLineStorage>(true);
	redshift_test::UseLogStorage(*db.instance, refusing);

	// Debug is noexcept, so a refusal that escaped it would terminate the run. The counts prove
	// both lines reached the storage, the close line from a destructor.
	auto redshift = OpenRedshiftConnection(redshift_test::MockConfig(), CreateRedshiftLogSink(*db.instance));
	redshift.reset();

	CHECK(refusing->Count(OPENED_LINE) == 1);
	CHECK(refusing->Count(CLOSED_LINE) == 1);
}
