// How the client classifies failures (odbc_errors.hpp) on driver 2.3.0.0, with the live variables,
// on the client's own sessions only: one the server ends (pg_terminate_backend on its own pid), a
// fetch the server fails, a transaction a failure aborted, the settings a caller's SET pins to the
// session, and Describe of a CREATE TEMP TABLE. Nothing is written; recorded_failures.hpp keeps the
// records as the fake's.

// catch.hpp comes first, as in this directory's other tests.
#include "catch.hpp"

#include "call_trace.hpp"
#include "live_workgroup.hpp"
#include "recorded_failures.hpp"
#include "thrown.hpp"

#include <cstdlib>
#include <string>
#include <vector>

using namespace duckdb;
using redshift_test::CallTrace;
using redshift_test::Capture;
using redshift_test::CaptureError;
using redshift_test::InfoOf;
using redshift_test::Thrown;

namespace {

//! The live provisioned cluster's config. Call it only once LiveSkipReason, for the cluster's variables, is empty.
RedshiftConfig LiveClusterConfig() {
	RedshiftConfig config;
	config.region = std::getenv(redshift_test::LIVE_REGION_VARIABLE);
	config.target = RedshiftTarget::PROVISIONED;
	config.cluster = std::getenv(redshift_test::LIVE_CLUSTER_VARIABLE);
	return config;
}

//! A traced connection to a live target (the workgroup unless given) on the embedded driver, its process
//! state reset after.
struct LiveConnection {
	explicit LiveConnection(const RedshiftConfig &config = redshift_test::LiveWorkgroupConfig()) {
		SetDriverSourceForTesting(&CallTrace::Source);
		CallTrace::Start(&redshift_driver::LoadEmbeddedDriver);
		connection = OpenRedshiftConnection(config, make_shared_ptr<redshift_test::CapturingLogSink>());
	}

	//! `sql` described, run and read to the end: the first column's values, as text.
	std::vector<std::string> Run(const std::string &sql) {
		auto result = connection->Execute(sql, {}, connection->Describe(sql));
		std::vector<std::string> cells;
		RedshiftBatch batch;
		while (result->NextBatch(batch)) {
			for (const auto &row : batch.rows) {
				cells.push_back(row[0].kind == RedshiftValueKind::INTEGER ? std::to_string(row[0].integer)
				                                                          : row[0].text);
			}
		}
		return cells;
	}

	redshift_test::ResetOnExit reset;
	redshift_test::LiveEndpoints endpoints;
	unique_ptr<RedshiftConnection> connection;
};

//! The connection-dead answers in the trace since `from`.
std::vector<std::string> DeadAnswersSince(size_t from) {
	const auto calls = CallTrace::Calls();
	std::vector<std::string> answers;
	for (size_t index = from; index < calls.size(); index++) {
		if (calls[index].find(" dead=") != std::string::npos) {
			answers.push_back(calls[index]);
		}
	}
	return answers;
}

//! The records the driver gave since `from`, each as "<SQLSTATE> <message>".
std::vector<std::string> RecordsSince(size_t from) {
	const std::string read = "SQLGetDiagRec 0 ";
	const auto calls = CallTrace::Calls();
	std::vector<std::string> records;
	for (size_t index = from; index < calls.size(); index++) {
		if (calls[index].compare(0, read.size(), read) == 0 && calls[index].size() > read.size()) {
			records.push_back(calls[index].substr(read.size()));
		}
	}
	return records;
}

//! Each of `diags` as RecordsSince writes one.
std::vector<std::string> AsRecords(const std::vector<redshift_driver::testing::FakeDiag> &diags) {
	std::vector<std::string> records;
	for (const auto &diag : diags) {
		records.push_back(diag.sqlstate + " " + diag.message);
	}
	return records;
}

} // namespace

TEST_CASE("Live: a statement whose session the server ended is a transport failure, and nothing runs after",
          "[odbc][live]") {
	// On the provisioned cluster, where the session's end reaches the client at once. On a Serverless
	// workgroup the endpoint kept the TLS connection open after the backend's FATAL, and the driver's
	// libpq reads until it closes: SQLExecute returned after 341 to 783 seconds there, and after 0.12
	// seconds on the cluster, with the same records and dead answer. The rule is the client's, the same
	// on either target.
	SKIP_UNLESS_LIVE_ON(redshift_test::LIVE_CLUSTER_VARIABLES, redshift_driver::LoadEmbeddedDriver());
	LiveConnection live(LiveClusterConfig());
	const auto before = CallTrace::Calls().size();
	const auto error = CaptureError([&] { live.Run("SELECT pg_terminate_backend(pg_backend_pid())"); });

	CHECK(error.Type() == ExceptionType::IO);
	CHECK(DeadAnswersSince(before) == std::vector<std::string> {"SQLGetConnectAttr 0 dead=1"});
	// The records depend on the target, so the case checks the dead answer and the records' kind, not
	// one shape: recorded_failures.hpp keeps both shapes, one of three records (08006, 08006, 57P01)
	// and one of two (57P01, 57P01, seen on both targets), each then the teardown's. Every record is
	// the session's end (08006 or 57P01), the server's FATAL is among them, and the last is the
	// teardown's rollback, which the connection no longer answers.
	const auto sqlstate = InfoOf(error, "sqlstate");
	CHECK((sqlstate == "08006" || sqlstate == "57P01"));
	CHECK(InfoOf(error, "sqlstates").find("57P01") != std::string::npos);
	// Either shape names the same cause, since a 57P01 record is among them.
	CHECK(error.RawMessage().find("was lost (Redshift ended the session). ") != std::string::npos);
	const auto records = RecordsSince(before);
	REQUIRE(records.size() >= 2);
	bool fatal = false;
	for (const auto &record : records) {
		CHECK((record.compare(0, 6, "08006 ") == 0 || record.compare(0, 6, "57P01 ") == 0));
		fatal = fatal ||
		        record.find("57P01:FATAL:  terminating connection due to administrator command") != std::string::npos;
	}
	CHECK(fatal);
	const std::string teardown = ":no connection to the server\n";
	CHECK(records.back().size() > teardown.size());
	CHECK(records.back().compare(records.back().size() - teardown.size(), teardown.size(), teardown) == 0);
	const auto closed = CallTrace::Calls().size();
	CHECK(Capture([&] { live.Run("SELECT 1"); }).type == ExceptionType::IO);
	CHECK(live.connection->SessionHoldsState());
	CHECK(CallTrace::Calls().size() == closed);
}

TEST_CASE("Live: a division by zero in a later row is an executor error, and the connection runs on", "[odbc][live]") {
	SKIP_UNLESS_LIVE(redshift_driver::LoadEmbeddedDriver());
	LiveConnection live;
	const auto before = CallTrace::Calls().size();
	const auto error = CaptureError([&] { live.Run("SELECT 1 / (g - 5000) FROM generate_series(1, 10000) g"); });

	CHECK(Thrown {error.Type(), error.RawMessage()} ==
	      Thrown {ExceptionType::EXECUTOR,
	              "Redshift could not return the rest of the result: ERROR:  division by zero\nSQLSTATE: 22012"});
	CHECK(DeadAnswersSince(before) == std::vector<std::string> {"SQLGetConnectAttr 0 dead=0"});
	CHECK(RecordsSince(before) == AsRecords({redshift_test::DIVISION_BY_ZERO_RECORD}));
	CHECK(live.Run("SELECT 1") == std::vector<std::string> {"1"});
}

TEST_CASE("Live: a statement in an aborted transaction is refused at prepare with 25P02 until ROLLBACK",
          "[odbc][live]") {
	SKIP_UNLESS_LIVE(redshift_driver::LoadEmbeddedDriver());
	LiveConnection live;
	live.Run("BEGIN");
	auto before = CallTrace::Calls().size();
	const auto failed = CaptureError([&] { live.Run("SELECT CAST(v AS INTEGER) FROM (SELECT 'x'::varchar AS v) t"); });
	REQUIRE(failed.Type() == ExceptionType::EXECUTOR);
	CHECK(InfoOf(failed, "sqlstate") == "22P02");
	CHECK(DeadAnswersSince(before) == std::vector<std::string> {"SQLGetConnectAttr 0 dead=0"});
	before = CallTrace::Calls().size();
	const auto error = CaptureError([&] { live.Run("SELECT 1"); });

	CHECK(error.Type() == ExceptionType::BINDER);
	CHECK(InfoOf(error, "sqlstate") == "25P02");
	CHECK(RecordsSince(before) == AsRecords({redshift_test::ABORTED_TRANSACTION_RECORD}));
	live.Run("ROLLBACK");
	CHECK(live.Run("SELECT 1") == std::vector<std::string> {"1"});
}

TEST_CASE("Live: a caller's SET search_path and SET timezone hold for the next statement on the connection",
          "[odbc][live]") {
	SKIP_UNLESS_LIVE(redshift_driver::LoadEmbeddedDriver());
	LiveConnection live;
	REQUIRE_FALSE(live.connection->SessionHoldsState());
	live.Run("SET search_path TO public");
	CHECK(live.connection->SessionHoldsState());
	live.Run("SET timezone TO 'Asia/Kolkata'");

	CHECK(live.Run("SHOW search_path") == std::vector<std::string> {"public"});
	CHECK(live.Run("SHOW timezone") == std::vector<std::string> {"Asia/Kolkata"});
	CHECK(live.Run("SELECT '2020-01-01 00:00:00+00'::timestamptz") ==
	      std::vector<std::string> {"2020-01-01 05:30:00+05:30"});
}

// Below the scan, where SessionHoldsState() can be read, which describe_no_side_effects.test cannot: a
// Describe leaves a fresh session holding no state, and creates nothing on the server.
TEST_CASE("Live: Describe of a CREATE TEMP TABLE creates nothing and leaves a fresh session unmarked", "[odbc][live]") {
	SKIP_UNLESS_LIVE(redshift_driver::LoadEmbeddedDriver());
	LiveConnection live;
	REQUIRE_FALSE(live.connection->SessionHoldsState());
	CHECK_FALSE(live.connection->Describe("CREATE TEMP TABLE describe_gate (a INTEGER)").returns_rows);
	CHECK_FALSE(live.connection->SessionHoldsState());
	const auto absent = CaptureError([&] { live.Run("SELECT count(*) FROM describe_gate"); });
	CHECK(InfoOf(absent, "sqlstate") == "42P01");
}
