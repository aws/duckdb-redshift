// The transport-class SQLSTATEs against the client's failure rules (odbc_errors.hpp), at execute
// and at fetch, on connections the driver reports alive and dead; the records driver 2.3.0.0 gave
// live for a session the server ended and for a fetch the server failed; and the client's latch.

// catch.hpp comes first, as in this directory's other tests.
#include "catch.hpp"

#include "call_trace.hpp"
#include "duckdb/common/string_util.hpp"
#include "fake_driver_fixture.hpp"
#include "recorded_failures.hpp"
#include "thrown.hpp"

#include <algorithm>
#include <functional>
#include <string>
#include <vector>

using namespace duckdb;
using redshift_driver::testing::FakeDiag;
using redshift_driver::testing::FakeResultSet;
using redshift_test::Capture;
using redshift_test::CaptureError;
using redshift_test::FakeDriverFixture;
using redshift_test::InfoOf;
using redshift_test::Thrown;

namespace {

//! The transport-class SQLSTATEs: class 08, which shows the loss itself, and the
//! others, which close the connection only when the driver reports it dead.
const std::vector<std::string> CLASS_08 {"08001", "08003", "08004", "08006", "08007", "08S01"};
const std::vector<std::string> DEAD_ONLY {"57P01", "57P02", "57P03", "58000", "58030", "HYT00", "HYT01"};

//! A record of `sqlstate` in the layout the driver writes a server's: stamped, then the severity.
FakeDiag ServerRecord(const std::string &sqlstate) {
	return {sqlstate, 0, "[Redshift][ODBC Driver][Server]" + sqlstate + ":FATAL:  x", {}};
}

//! What a failure did: the exception's type and `sqlstate`, and whether the connection then ran
//! another statement.
struct Outcome {
	ExceptionType type;
	std::string sqlstate;
	bool kept;
};

bool operator==(const Outcome &left, const Outcome &right) {
	return left.type == right.type && left.sqlstate == right.sqlstate && left.kept == right.kept;
}

std::ostream &operator<<(std::ostream &out, const Outcome &outcome) {
	return out << Exception::ExceptionTypeToString(outcome.type) << " " << outcome.sqlstate
	           << (outcome.kept ? ", kept" : ", closed");
}

//! SELECT 1 with `entry_point` failing with `records`, the driver answering `dead` after it.
Outcome FailSelectOne(const char *entry_point, const std::vector<FakeDiag> &records, bool dead) {
	FakeDriverFixture fixture;
	auto connection = fixture.OpenConnection();
	fixture.FailNextCall(entry_point, SQL_ERROR, records);
	fixture.fake.SetConnectionDead(dead);
	const auto error = CaptureError([&] {
		auto result = redshift_test::ExecuteStatement(*connection, "SELECT 1", {});
		RedshiftBatch batch;
		result->NextBatch(batch);
	});
	fixture.fake.SetConnectionDead(false);
	const auto next = CaptureError([&] { connection->Describe("SELECT 1"); });
	return {error.Type(), InfoOf(error, "sqlstate"), !next.HasError()};
}

} // namespace

TEST_CASE("A class 08 SQLSTATE closes the connection at execute or fetch, whatever the driver answers", "[odbc]") {
	for (const auto entry_point : {"SQLExecDirect", "SQLFetch"}) {
		for (const auto &sqlstate : CLASS_08) {
			for (const bool dead : {false, true}) {
				INFO(entry_point << " " << sqlstate << (dead ? " dead" : " alive"));
				CHECK(FailSelectOne(entry_point, {ServerRecord(sqlstate)}, dead) ==
				      Outcome {ExceptionType::IO, sqlstate, false});
			}
		}
	}
}

TEST_CASE("57P01 to 57P03, class 58, HYT00 and HYT01 close the connection only when the driver reports it dead",
          "[odbc]") {
	for (const auto entry_point : {"SQLExecDirect", "SQLFetch"}) {
		for (const auto &sqlstate : DEAD_ONLY) {
			INFO(entry_point << " " << sqlstate);
			CHECK(FailSelectOne(entry_point, {ServerRecord(sqlstate)}, false) ==
			      Outcome {ExceptionType::EXECUTOR, sqlstate, true});
			CHECK(FailSelectOne(entry_point, {ServerRecord(sqlstate)}, true) ==
			      Outcome {ExceptionType::IO, sqlstate, false});
		}
	}
}

TEST_CASE("The records of a session the server ended give a transport failure carrying 08006, alive or dead",
          "[odbc]") {
	// The first record's 08006 is libpq's own failure, stamped with the state of the server's FATAL
	// beside it; either record alone closes the connection. Both carry 08006, so which one the state
	// is taken from is statement_error_test.cpp's to check. The third record's 57P01 says Redshift
	// ended the session, though the state acted on is 08006.
	for (const bool dead : {false, true}) {
		INFO((dead ? "dead" : "alive"));
		FakeDriverFixture fixture;
		auto connection = fixture.OpenConnection();
		fixture.FailNextCall("SQLExecDirect", SQL_ERROR, redshift_test::SESSION_ENDED_RECORDS);
		fixture.fake.SetConnectionDead(dead);
		const auto error = CaptureError([&] { redshift_test::ExecuteStatement(*connection, "SELECT 1", {}); });

		CHECK(Thrown {error.Type(), error.RawMessage()} ==
		      Thrown {ExceptionType::IO,
		              "The connection to Redshift at " + redshift_test::ServerlessTarget("us-east-1") +
		                  " was lost (Redshift ended the session). The statement was not replayed, "
		                  "because a write it carried may already have taken effect; check before running it "
		                  "again.\nSQLSTATE: 08006"});
		CHECK(error.ExtraInfo() ==
		      unordered_map<string, string> {{"sqlstate", "08006"}, {"sqlstates", "08006,08006,57P01"}});
		CHECK(fixture.fake.AllocatedHandles() == "environment");
	}
}

TEST_CASE("The 57P01 records of a session the server ended give a transport failure, alive or dead", "[odbc]") {
	// Neither record is class 08, but the first holds libpq's "SSL connection has been closed unexpectedly", a
	// dropped-connection text, so the records alone close the connection whatever the driver answers.
	for (const bool dead : {false, true}) {
		INFO((dead ? "dead" : "alive"));
		FakeDriverFixture fixture;
		auto connection = fixture.OpenConnection();
		fixture.FailNextCall("SQLExecDirect", SQL_ERROR, redshift_test::SESSION_ENDED_57P01_RECORDS);
		fixture.fake.SetConnectionDead(dead);
		const auto error = CaptureError([&] { redshift_test::ExecuteStatement(*connection, "SELECT 1", {}); });

		// The measured shape names its cause in the client's words.
		CHECK(Thrown {error.Type(), error.RawMessage()} ==
		      Thrown {ExceptionType::IO,
		              "The connection to Redshift at " + redshift_test::ServerlessTarget("us-east-1") +
		                  " was lost (Redshift ended the session). The statement was not replayed, "
		                  "because a write it carried may already have taken effect; check before running it "
		                  "again.\nSQLSTATE: 57P01"});
		CHECK(error.ExtraInfo() == unordered_map<string, string> {{"sqlstate", "57P01"}, {"sqlstates", "57P01,57P01"}});
		CHECK(fixture.fake.AllocatedHandles() == "environment");
	}
}

TEST_CASE("22012 in a streaming read is an executor error carrying it, and the connection runs on", "[odbc]") {
	FakeDriverFixture fixture;
	auto connection = fixture.OpenConnection();
	auto result = redshift_test::ExecuteStatement(*connection, "SELECT 1", {});
	fixture.FailNextCall("SQLFetch", SQL_ERROR, {redshift_test::DIVISION_BY_ZERO_RECORD});

	RedshiftBatch batch;
	const auto error = CaptureError([&] { result->NextBatch(batch); });
	CHECK(Thrown {error.Type(), error.RawMessage()} ==
	      Thrown {ExceptionType::EXECUTOR,
	              "Redshift could not return the rest of the result: ERROR:  division by zero\nSQLSTATE: 22012"});
	CHECK(error.ExtraInfo().at("sqlstate") == "22012");
	result.reset();
	CHECK(connection->Describe("SELECT 1").returns_rows);
}

namespace {

//! A step a latch case takes on an open connection, which may throw.
using Step = std::function<void(FakeDriverFixture &, RedshiftConnection &)>;

//! Runs `sql` through Execute and reads its result to the end.
void RunToEnd(RedshiftConnection &connection, const std::string &sql) {
	auto result = redshift_test::ExecuteStatement(connection, sql, {});
	RedshiftBatch batch;
	while (result->NextBatch(batch)) {
	}
}

//! Whether the latch is set once `step` ran, on a new connection, whatever `step` threw.
bool HoldsStateAfter(const Step &step) {
	FakeDriverFixture fixture;
	FakeResultSet no_columns;
	no_columns.row_count = 0;
	fixture.fake.AddResult("SET search_path TO public", {no_columns});
	fixture.fake.AddResult("SELECT $1", {redshift_test::OneColumn(redshift_test::VarcharColumn("a", 10), {"x"})}, 1);
	auto connection = fixture.OpenConnection();
	REQUIRE_FALSE(connection->SessionHoldsState());
	CaptureError([&] { step(fixture, *connection); });
	return connection->SessionHoldsState();
}

} // namespace

TEST_CASE("The latch is set by each statement that reaches the driver to run, and by nothing before it", "[odbc]") {
	const std::vector<std::pair<std::string, Step>> setting {
	    {"a row-returning statement",
	     [](FakeDriverFixture &, RedshiftConnection &connection) {
		     RunToEnd(connection, "SELECT 1");
	     }},
	    {"a failed row-returning statement",
	     [](FakeDriverFixture &fixture, RedshiftConnection &connection) {
		     fixture.FailNextCall("SQLExecDirect", SQL_ERROR, {});
		     RunToEnd(connection, "SELECT 1");
	     }},
	    {"a caller's SET",
	     [](FakeDriverFixture &, RedshiftConnection &connection) {
		     RunToEnd(connection, "SET search_path TO public");
	     }},
	    {"a failed statement with no result columns",
	     [](FakeDriverFixture &fixture, RedshiftConnection &connection) {
		     fixture.FailNextCall("SQLExecDirect", SQL_ERROR, {});
		     RunToEnd(connection, "SET search_path TO public");
	     }},
	    {"a failed statement with parameters", [](FakeDriverFixture &fixture, RedshiftConnection &connection) {
		     fixture.FailNextCall("SQLExecute", SQL_ERROR, {});
		     redshift_test::ExecuteStatement(connection, "SELECT $1", {"x"});
	     }}};
	for (const auto &step : setting) {
		INFO(step.first);
		CHECK(HoldsStateAfter(step.second));
	}
	const std::vector<std::pair<std::string, Step>> leaving {
	    {"Describe",
	     [](FakeDriverFixture &, RedshiftConnection &connection) {
		     connection.Describe("SELECT 1");
	     }},
	    {"a prepare refusal inside Execute",
	     [](FakeDriverFixture &fixture, RedshiftConnection &connection) {
		     fixture.FailNextCall("SQLPrepare", SQL_ERROR, {{"42601", 0, "ERROR:  syntax error", {}}});
		     RunToEnd(connection, "SELECT 1");
	     }},
	    {"a parameter-count refusal",
	     [](FakeDriverFixture &, RedshiftConnection &connection) {
		     redshift_test::ExecuteStatement(connection, "SELECT $1", {});
	     }},
	    {"a transport failure before SQLExecute", [](FakeDriverFixture &fixture, RedshiftConnection &connection) {
		     fixture.FailNextCall("SQLPrepare", SQL_ERROR, {ServerRecord("08006")});
		     RunToEnd(connection, "SELECT 1");
	     }}};
	for (const auto &step : leaving) {
		INFO(step.first);
		CHECK_FALSE(HoldsStateAfter(step.second));
	}
}

TEST_CASE("Once set, the latch stays set through refusals, a closed connection and an open result", "[odbc]") {
	FakeDriverFixture fixture;
	auto connection = fixture.OpenConnection();
	auto result = redshift_test::ExecuteStatement(*connection, "SELECT 1", {});
	REQUIRE(connection->SessionHoldsState());
	// The in-flight refusal, with the result still open.
	REQUIRE(Capture([&] { redshift_test::ExecuteStatement(*connection, "SELECT 1", {}); }).type ==
	        ExceptionType::INVALID_INPUT);
	CHECK(connection->SessionHoldsState());
	result.reset();
	fixture.FailNextCall("SQLPrepare", SQL_ERROR, {ServerRecord("08006")});
	REQUIRE(Capture([&] { connection->Describe("SELECT 1"); }).type == ExceptionType::IO);
	CHECK(connection->SessionHoldsState());
}

TEST_CASE("SessionHoldsState makes no driver call on an open connection or one a transport failure closed", "[odbc]") {
	FakeDriverFixture fixture;
	auto connection = fixture.OpenConnection();
	auto calls = fixture.fake.Calls().size();
	CHECK_NOTHROW(connection->SessionHoldsState());
	CHECK(fixture.fake.Calls().size() == calls);
	fixture.FailNextCall("SQLExecDirect", SQL_ERROR, {ServerRecord("08006")});
	REQUIRE(Capture([&] { redshift_test::ExecuteStatement(*connection, "SELECT 1", {}); }).type == ExceptionType::IO);
	calls = fixture.fake.Calls().size();
	CHECK(connection->SessionHoldsState());
	CHECK(fixture.fake.Calls().size() == calls);
}

TEST_CASE("The call trace records the connection-dead answer and each record the client acts on", "[odbc]") {
	for (const bool dead : {false, true}) {
		INFO((dead ? "dead" : "alive"));
		FakeDriverFixture fixture;
		SetDriverSourceForTesting(&redshift_test::CallTrace::Source);
		redshift_test::CallTrace::Start(&redshift_driver::testing::FakeOdbcDriver::Source);
		auto connection = fixture.OpenConnection();
		fixture.FailNextCall("SQLPrepare", SQL_ERROR, {ServerRecord("53200")});
		fixture.fake.SetConnectionDead(dead);
		CaptureError([&] { connection->Describe("SELECT 1"); });

		const auto calls = redshift_test::CallTrace::Calls();
		const auto answer = std::string("SQLGetConnectAttr 0 dead=") + (dead ? "1" : "0");
		CHECK(std::count(calls.begin(), calls.end(), answer) == 1);
		const auto record = "SQLGetDiagRec 0 53200 " + ServerRecord("53200").message;
		CHECK(std::count(calls.begin(), calls.end(), record) == 1);
	}
}

namespace {

//! What driver 2.3.0.0 writes for a server message, stamped, for the two answers that leave a session unusable, as
//! a Serverless workgroup gave them (a writable session's ID and the consumer transaction's ID in the second).
const std::string INTERNAL_SERVICE_ERROR = "[Redshift][ODBC Driver][Server]XX000:ERROR:  Internal Service Error";
const std::string WRITABLE_SESSION_OPEN =
    "[Redshift][ODBC Driver][Server]XX000:ERROR:  Existing writable session '3c70b1ce-0f2a-4c1e-9b7d-5e8a6f4d2c10' is "
    "still open and is created by transaction ID '1965'";

//! The failure of `call` after `entry_point` fails with `records` on a live connection, and whether the connection is
//! then closed: the next Describe fails as on a closed connection, and only the environment handle is left.
struct UnusableOutcome {
	duckdb::ErrorData error;
	bool closed;
};

template <class CALL>
UnusableOutcome FailWith(const char *entry_point, std::vector<FakeDiag> records, CALL &&call) {
	FakeDriverFixture fixture;
	auto connection = fixture.OpenConnection();
	fixture.FailNextCall(entry_point, SQL_ERROR, std::move(records));
	auto error = CaptureError([&] { call(*connection); });
	const auto next = CaptureError([&] { connection->Describe("SELECT 1"); });
	return {error,
	        next.HasError() && next.Type() == ExceptionType::IO && fixture.fake.AllocatedHandles() == "environment"};
}

//! The same with one `sqlstate` record holding `text`.
template <class CALL>
UnusableOutcome FailWith(const char *entry_point, const std::string &sqlstate, const std::string &text, CALL &&call) {
	return FailWith(entry_point, std::vector<FakeDiag> {{sqlstate, 0, text, {}}}, std::forward<CALL>(call));
}

void DescribeOne(RedshiftConnection &connection) {
	connection.Describe("SELECT 1");
}

//! The extra_info of a failure that left the session unusable, from one XX000 record.
const unordered_map<string, string> UNUSABLE {
    {"sqlstate", "XX000"}, {"sqlstates", "XX000"}, {"redshift_error", "session_unusable"}};

//! The server's line in a stamped record's `text`, from its severity word on.
std::string ServerLine(const std::string &text) {
	return text.substr(text.find("ERROR:  "));
}

void ExecuteOne(RedshiftConnection &connection) {
	auto result = redshift_test::ExecuteStatement(connection, "SELECT 1", {});
	RedshiftBatch batch;
	result->NextBatch(batch);
}

} // namespace

TEST_CASE("Redshift's internal service error, or a writable session still open, closes a live connection and marks "
          "the failure session_unusable, keeping its type and the server's line",
          "[odbc]") {
	for (const auto &text : {INTERNAL_SERVICE_ERROR, WRITABLE_SESSION_OPEN}) {
		INFO(text);
		// At prepare, which a bind's Describe makes: a refusal.
		auto prepared = FailWith("SQLPrepare", "XX000", text, DescribeOne);
		CHECK(Thrown {prepared.error.Type(), prepared.error.RawMessage()} ==
		      Thrown {ExceptionType::BINDER,
		              "Redshift refused the statement: " + ServerLine(text) + "\nSQLSTATE: XX000"});
		CHECK(prepared.error.ExtraInfo() == UNUSABLE);
		CHECK(prepared.closed);
		// At execution and at fetch: the executor's type.
		for (const auto entry_point : {"SQLExecDirect", "SQLFetch"}) {
			INFO(entry_point);
			auto outcome = FailWith(entry_point, "XX000", text, ExecuteOne);
			CHECK(outcome.error.Type() == ExceptionType::EXECUTOR);
			CHECK(outcome.error.ExtraInfo() == UNUSABLE);
			CHECK(outcome.error.RawMessage().find(ServerLine(text)) != string::npos);
			CHECK(StringUtil::EndsWith(outcome.error.RawMessage(), "\nSQLSTATE: XX000"));
			CHECK(outcome.closed);
		}
	}
	// Only the first line counts: a DETAIL line after it does not hide the answer.
	auto with_detail =
	    FailWith("SQLExecDirect", "XX000", INTERNAL_SERVICE_ERROR + "\nDETAIL:  placeholder detail", ExecuteOne);
	CHECK(with_detail.error.ExtraInfo() == UNUSABLE);
	CHECK(with_detail.closed);
	// Any record counts, not only the first: here the driver's own record comes before the server's.
	auto second_record = FailWith(
	    "SQLExecDirect",
	    {{"HY000", 0, "[Redshift][ODBC Driver]placeholder driver line", {}}, {"XX000", 0, INTERNAL_SERVICE_ERROR, {}}},
	    ExecuteOne);
	CHECK(second_record.error.Type() == ExceptionType::EXECUTOR);
	CHECK(second_record.error.ExtraInfo() == unordered_map<string, string> {{"sqlstate", "XX000"},
	                                                                        {"sqlstates", "HY000,XX000"},
	                                                                        {"redshift_error", "session_unusable"}});
	CHECK(second_record.closed);
}

TEST_CASE("A bound value that matches part of the server's line does not hide a session left unusable", "[odbc]") {
	// Statement redaction replaces each bound value of 8 bytes or more wherever it occurs in a record, so the answer is
	// judged on the record as the driver wrote it; the message still shows the value only as its position.
	const char *const sql = "SELECT a FROM t WHERE b = $1";
	const std::vector<std::pair<std::string, std::string>> cases {{"Internal", INTERNAL_SERVICE_ERROR},
	                                                              {"Existing", WRITABLE_SESSION_OPEN}};
	for (const auto &bound : cases) {
		INFO(bound.first);
		FakeDriverFixture fixture;
		fixture.fake.AddResult(sql, {redshift_test::OneColumn(redshift_test::VarcharColumn("a", 16), {"x"})}, 1);
		auto connection = fixture.OpenConnection();
		fixture.FailNextCall("SQLExecute", SQL_ERROR, {{"XX000", 0, bound.second, {}}});
		const auto error = CaptureError([&] { redshift_test::ExecuteStatement(*connection, sql, {bound.first}); });
		CHECK(error.Type() == ExceptionType::EXECUTOR);
		CHECK(error.ExtraInfo() == UNUSABLE);
		CHECK(error.RawMessage().find("ERROR:  <param 1> ") != string::npos);
		CHECK(error.RawMessage().find(bound.first) == string::npos);
		CHECK(CaptureError([&] { connection->Describe("SELECT 1"); }).Type() == ExceptionType::IO);
		CHECK(fixture.fake.AllocatedHandles() == "environment");
	}
}

TEST_CASE("A cell read that fails with Redshift's internal service error marks the failure session_unusable and "
          "closes the connection",
          "[odbc]") {
	FakeDriverFixture fixture;
	FakeResultSet rows;
	rows.columns.push_back(redshift_test::LongVarcharColumn("a"));
	rows.rows.push_back({"1"});
	// A long column, so its cell is read with SQLGetData.
	fixture.fake.AddResult("SELECT a FROM t", {rows});
	auto connection = fixture.OpenConnection();
	auto result = redshift_test::ExecuteStatement(*connection, "SELECT a FROM t", {});
	fixture.FailNextCall("SQLGetData", SQL_ERROR, {{"XX000", 0, INTERNAL_SERVICE_ERROR, {}}});
	RedshiftBatch batch;
	const auto error = CaptureError([&] { result->NextBatch(batch); });
	CHECK(error.Type() == ExceptionType::EXECUTOR);
	CHECK(error.ExtraInfo() == UNUSABLE);
	result.reset();
	CHECK(CaptureError([&] { connection->Describe("SELECT 1"); }).Type() == ExceptionType::IO);
	CHECK(fixture.fake.AllocatedHandles() == "environment");
}

TEST_CASE("Any other XX000 text, and the same text under another state, is an ordinary failure that keeps the "
          "connection",
          "[odbc]") {
	const std::vector<std::pair<std::string, std::string>> ordinary {
	    {"XX000", "[Redshift][ODBC Driver][Server]XX000:ERROR:  Invalid digit, Value 'a', Pos 0, Type: Integer"},
	    {"XX000", "[Redshift][ODBC Driver][Server]XX000:ERROR:  Internal Service Error while reading t"},
	    {"XX000", "[Redshift][ODBC Driver][Server]XX000:ERROR:  writable session 'x' is still open"},
	    {"XX000", "[Redshift][ODBC Driver][Server]XX000:ERROR:  Existing writable session 'x' was closed"},
	    // The writable-session text counts only at the start of the line.
	    {"XX000", "[Redshift][ODBC Driver][Server]XX000:ERROR:  could not write: existing writable session 'x' is "
	              "still open"},
	    {"42601", "[Redshift][ODBC Driver][Server]42601:ERROR:  Internal Service Error"},
	    // A state the driver carried over from an earlier server message: no severity follows its stamp.
	    {"XX000", "[Redshift][ODBC Driver][Server]XX000:Internal Service Error"}};
	for (const auto &record : ordinary) {
		INFO(record.second);
		auto outcome = FailWith("SQLExecDirect", record.first, record.second, ExecuteOne);
		CHECK(outcome.error.Type() == ExceptionType::EXECUTOR);
		CHECK(outcome.error.ExtraInfo().count("redshift_error") == 0);
		CHECK_FALSE(outcome.closed);
	}
}
