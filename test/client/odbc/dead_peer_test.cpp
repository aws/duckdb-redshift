// The dead-peer bound: when the server's packets stop arriving mid-result, with
// no FIN or RST, the keepalive settings fail NextBatch with a transport IOException within 120
// seconds, and the connection closes. Live, and only under test/client/odbc/run-dead-peer.sh,
// which routes the connection through a relay it cuts off the network when this case asks.
// The bound is not a time limit: keepalive probes go only on an idle connection, so a result still
// streaming after 120 seconds is not cut. Live too, and only with REDSHIFT_TEST_LIVE_LONG=1.

// catch.hpp comes first, as in this directory's other tests.
#include "catch.hpp"

#include "duckdb/common/exception.hpp"
#include "fake_driver_fixture.hpp"
#include "live_target.hpp"
#include "live_workgroup.hpp"
#include "thrown.hpp"

#include <chrono>
#include <cstdlib>
#include <fstream>
#include <string>
#include <thread>

#include <dirent.h>

using namespace duckdb;

namespace {

using Clock = std::chrono::steady_clock;

//! The environment variable `name`, or empty when it is unset.
std::string Setting(const char *name) {
	const char *value = std::getenv(name);
	return value ? value : "";
}

//! The workgroup the live variables name, with the default credential chain.
RedshiftConfig LiveConfig() {
	RedshiftConfig config;
	config.region = Setting("REDSHIFT_TEST_LIVE_REGION");
	config.target = RedshiftTarget::SERVERLESS;
	config.workgroup = Setting("REDSHIFT_TEST_LIVE_WORKGROUP");
	config.account_id = Setting("REDSHIFT_TEST_LIVE_ACCOUNT_ID");
	return config;
}

//! The threads this process runs now.
size_t ThreadCount() {
	size_t threads = 0;
	if (DIR *tasks = opendir("/proc/self/task")) {
		while (auto *entry = readdir(tasks)) {
			threads += entry->d_name[0] != '.';
		}
		closedir(tasks);
	}
	return threads;
}

//! Asks the harness to cut the relay, through the signal directory, and waits up to a minute for
//! it to say it has. Returns whether it did.
bool CutRelay(const std::string &signals) {
	std::ofstream(signals + "/cut").put('\n');
	const auto deadline = Clock::now() + std::chrono::seconds(60);
	while (Clock::now() < deadline) {
		if (std::ifstream(signals + "/cut.done")) {
			return true;
		}
		std::this_thread::sleep_for(std::chrono::milliseconds(100));
	}
	return false;
}

//! 10,000,000 generated rows, which the server is still sending when the relay is cut.
const char *const STREAMING_SELECT =
    "SELECT g::bigint AS a, (g * 7)::bigint AS b, (g * 0.25)::float8 AS c FROM generate_series(1, 10000000) AS t(g)";

//! Generated rows, which the server takes more than 120 seconds to send (189 s on 2.3.0.0); a faster
//! server that sends them sooner fails the case's time check, and needs more.
const uint64_t LONG_STREAM_ROWS = 100000000;
//! The product is taken as BIGINT, so a larger count stays in range.
const std::string LONG_STREAM_SELECT =
    "SELECT g::bigint AS a, (g::bigint * 7) AS b, (g * 0.25)::float8 AS c FROM generate_series(1, " +
    std::to_string(LONG_STREAM_ROWS) + ") AS t(g)";

//! Why a case that takes minutes cannot run here, or empty when it can: LiveSkipReason, or
//! REDSHIFT_TEST_LIVE_LONG is not 1.
std::string LongCaseSkipReason() {
	const auto reason = redshift_test::LiveSkipReason();
	if (!reason.empty() || Setting("REDSHIFT_TEST_LIVE_LONG") == "1") {
		return reason;
	}
	return "skipped: REDSHIFT_TEST_LIVE_LONG is not 1 (the case takes minutes)";
}

} // namespace

TEST_CASE("Live: a peer gone silent mid-result fails NextBatch as a lost connection within 120 seconds",
          "[odbc][live]") {
	const auto signals = Setting("REDSHIFT_TEST_DEAD_PEER_SIGNALS");
	if (Setting("REDSHIFT_TEST_LIVE_REGION").empty() || signals.empty()) {
		SKIP_TEST("skipped: unset: REDSHIFT_TEST_LIVE_REGION or REDSHIFT_TEST_DEAD_PEER_SIGNALS "
		          "(run it with test/client/odbc/run-dead-peer.sh)");
		return;
	}
	auto log_sink = make_shared_ptr<redshift_test::CapturingLogSink>();
	auto connection = OpenRedshiftConnection(LiveConfig(), log_sink);
	const auto threads_before = ThreadCount();
	REQUIRE(threads_before > 0);
	auto result = connection->Execute(STREAMING_SELECT, {}, connection->Describe(STREAMING_SELECT));
	RedshiftBatch batch;
	REQUIRE(result->NextBatch(batch));
	// The producer thread, the default fetch: the first NextBatch starts one thread for the result,
	// which makes its fetches; the failure below is stored there and raised by NextBatch here.
	CHECK(ThreadCount() == threads_before + 1);
	REQUIRE(CutRelay(signals));

	const auto cut = Clock::now();
	const auto error = redshift_test::CaptureError([&] {
		while (result->NextBatch(batch)) {
		}
	});
	const auto seconds = std::chrono::duration<double>(Clock::now() - cut).count();
	WARN("NextBatch failed " << seconds << " s after the cut: " << error.RawMessage());
	CHECK(error.Type() == ExceptionType::IO);
	CHECK(seconds < 120);
	// The producer's thread has ended with the failure; one that has exited can stay listed a moment.
	const auto joined_by = Clock::now() + std::chrono::seconds(5);
	while (ThreadCount() != threads_before && Clock::now() < joined_by) {
		std::this_thread::sleep_for(std::chrono::milliseconds(50));
	}
	CHECK(ThreadCount() == threads_before);
	// Closed: the next statement is refused before it reaches the driver.
	const auto next = redshift_test::CaptureError([&] { connection->Describe("SELECT 1"); });
	CHECK(next.Type() == ExceptionType::IO);
	CHECK_THAT(next.RawMessage(), Catch::Contains("was lost and is closed"));
}

TEST_CASE("Live: a result still streaming after 120 seconds is not cut", "[odbc][live]") {
	if (redshift_test::SkippedFor(LongCaseSkipReason())) {
		return;
	}
	redshift_test::ResetOnExit reset;
	redshift_test::LiveEndpoints endpoints;
	auto log_sink = make_shared_ptr<redshift_test::CapturingLogSink>();
	auto connection = OpenRedshiftConnection(LiveConfig(), log_sink);
	auto result = connection->Execute(LONG_STREAM_SELECT, {}, connection->Describe(LONG_STREAM_SELECT));
	RedshiftBatch batch;
	REQUIRE(result->NextBatch(batch));
	uint64_t rows = batch.rows.size();
	const auto started = Clock::now();
	while (result->NextBatch(batch)) {
		rows += batch.rows.size();
	}
	const auto seconds = std::chrono::duration<double>(Clock::now() - started).count();
	WARN("the stream ran " << seconds << " s after its first batch");
	CHECK(rows == LONG_STREAM_ROWS);
	// Longer than the bound, so a limit on the result's time would have cut it.
	CHECK(seconds > 120);
	// The same session answers the next statement.
	const std::string next_sql = "SELECT 1 AS one";
	const redshift_test::ReadBatches next(*connection->Execute(next_sql, {}, connection->Describe(next_sql)));
	REQUIRE(next.rows.size() == 1);
	CHECK(next.rows[0][0].integer == 1);
}
