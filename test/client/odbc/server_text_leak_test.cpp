// The server's own text carries host names, addresses, process IDs, URLs and markup, and none of
// them may reach what a user or a log sees: not an exception's message, not its extra_info, not a
// log line, in either phase and for any SQLSTATE class.

// catch.hpp comes first, as the extension's C++ tests include it: DuckDB's copy declares the
// duckdb_base_std aliases it needs only while no DuckDB header has opened that namespace.
#include "catch.hpp"

#include "diagnostic_fixtures.hpp"
#include "duckdb/common/string_util.hpp"
#include "extension_database.hpp"
#include "fake_driver_fixture.hpp"
#include "odbc/odbc_errors.hpp"
#include "thrown.hpp"

#include <string>
#include <utility>
#include <vector>

using duckdb::DiagnosticSet;
using duckdb::StatementPhase;
using duckdb::StringUtil;
using duckdb::redshift_driver::testing::FakeResultSet;
using redshift_test::CaptureError;
using redshift_test::ConnectRecords;
using redshift_test::DRIVER_FAILURE_TRAILER;
using redshift_test::RecordingSink;
using redshift_test::StatementRecords;
using redshift_test::StatementWarnings;

namespace {

using ExtraInfo = duckdb::unordered_map<duckdb::string, duckdb::string>;

const std::string HOSTILE = "ERROR:  failed at relay-7.us-east-1.internal:5471 (10.20.30.40) [fd00::1] pid 4242 via "
                            "https://relay.example/v1 <script>alert(1)</script> leader:5471 [face::beef]:5471 "
                            "[.fe80::1]:5471";

//! The pieces of HOSTILE that must not be seen. The words around them may be.
//! The port is one no message names: the client's own hints name 5439, Redshift's default port.
const std::vector<std::string> FRAGMENTS {
    "relay-7", "us-east-1.internal", "5471",   "10.20.30.40", "fd00", "4242", "https:", "relay.example",
    "<script", "</script",           "leader", "face::beef",  "fe80"};

//! Every SQLSTATE class the rule names, a generic driver state, one no rule knows, and the state of a
//! connect's database not found.
const std::vector<std::string> SQLSTATES {"42601", "22012", "23505", "0A000", "08001", "53300",
                                          "57P01", "58030", "XX000", "HY000", "ZZ999", "3D000"};

//! The first fragment `text` holds, or empty when it holds none.
std::string Leaked(const std::string &text) {
	for (const auto &fragment : FRAGMENTS) {
		if (text.find(fragment) != std::string::npos) {
			return fragment;
		}
	}
	return std::string();
}

//! The fragments the exception `call` throws shows, in its message or in any extra_info key or
//! value; empty when it shows none.
template <class CALL>
std::string LeakedThrough(CALL &&call) {
	const auto error = CaptureError(call);
	REQUIRE(error.Type() != duckdb::ExceptionType::INVALID);
	auto leaked = Leaked(error.RawMessage());
	for (const auto &entry : error.ExtraInfo()) {
		leaked += Leaked(entry.first) + Leaked(entry.second);
	}
	return leaked;
}

//! The fragments the lines logged for `records` show; empty when they show none.
std::string LeakedToLog(const DiagnosticSet &records) {
	RecordingSink sink;
	duckdb::LogInformationalDiagnostics(sink, records);
	REQUIRE(sink.Lines().size() == records.records.size());
	std::string leaked;
	for (const auto &line : sink.Lines()) {
		leaked += Leaked(line);
	}
	return leaked;
}

//! Checks that a class 42601 statement failure, and the log line of the same record as a warning,
//! show the server's `message` after `ERROR:  ` as `shown`.
void CheckShownAs(const std::string &message, const std::string &shown) {
	INFO("shown as " << shown);
	const auto text = "[Redshift][ODBC Driver][Server]42601:ERROR:  " + message;
	const auto error = CaptureError([&]() {
		duckdb::ThrowStatementFailure(StatementRecords({{"42601", 0, text}}), StatementPhase::EXECUTE);
	});
	CHECK(error.RawMessage() == "Redshift could not run the statement: ERROR:  " + shown + "\nSQLSTATE: 42601");
	RecordingSink sink;
	duckdb::LogInformationalDiagnostics(sink, StatementWarnings({{"42601", 0, text}}));
	CHECK(sink.Lines() == std::vector<std::string> {"Redshift driver diagnostic, SQLSTATE 42601: ERROR:  " + shown});
}

//! What a bound fetch's failed row, rather than a failed call, carries the hostile record on.
const char *const FAILED_ROW = "a failed row";

//! One VARCHAR row of `size` bytes declared, holding "x".
FakeResultSet TextRow(SQLULEN size) {
	FakeResultSet rows;
	rows.columns = {redshift_test::VarcharColumn("a", size)};
	rows.rows = {{"x"}};
	return rows;
}

//! The SQL that reaches `failure`: SELECT 1, whose INTEGER is bound; a long column, which SQLGetData
//! reads; or, for FAILED_ROW, a bound one whose row status fails the fetch.
std::string SqlFor(const std::string &failure) {
	if (failure == "SQLGetData") {
		return "SELECT a FROM t";
	}
	return failure == FAILED_ROW ? "SELECT b FROM t" : "SELECT 1";
}

//! The fragments shown when `entry_point` fails, or, for FAILED_ROW, when a bound fetch's row fails,
//! with HOSTILE under `sqlstate`, on a connection the driver reports `dead` or alive: in the
//! exception, its extra_info, or a log line.
std::string LeakedEndToEnd(const char *entry_point, const std::string &sqlstate, bool dead) {
	redshift_test::FakeDriverFixture fixture;
	const std::string failure = entry_point;
	const auto sql = SqlFor(failure);
	fixture.fake.AddResult("SELECT a FROM t", {TextRow(65535)});
	auto failed_row = TextRow(10);
	failed_row.row_status = {SQL_ROW_ERROR};
	failed_row.row_diags[0] = {{sqlstate, 0, HOSTILE, {}}};
	fixture.fake.AddResult("SELECT b FROM t", {failed_row});
	auto connection = fixture.OpenConnection();
	if (failure != FAILED_ROW) {
		fixture.FailNextCall(entry_point, SQL_ERROR, {{sqlstate, 0, HOSTILE, {}}});
	}
	fixture.fake.SetConnectionDead(dead);
	auto leaked = LeakedThrough([&]() {
		auto result = redshift_test::ExecuteStatement(*connection, sql, {});
		duckdb::RedshiftBatch batch;
		result->NextBatch(batch);
	});
	connection.reset();
	for (const auto &line : fixture.log_sink->Lines()) {
		leaked += Leaked(line);
	}
	return leaked;
}

} // namespace

TEST_CASE("The leak check finds each fragment the hostile message holds", "[odbc]") {
	for (const auto &fragment : FRAGMENTS) {
		CHECK(Leaked("seen " + fragment + " here") == fragment);
		CHECK(HOSTILE.find(fragment) != std::string::npos);
	}
	CHECK(Leaked("at <host> (<address>) pid <pid> via <url> (script)").empty());
}

TEST_CASE("No server host, address, PID, URL or markup reaches a connect exception, for any class", "[odbc]") {
	const duckdb::ConnectFailureContext context {
	    "wg.example", {"workgroup", true, "Grant it access.", ""}, duckdb::redshift_driver::DriverIdentity(), 1, 61};
	for (const auto &sqlstate : SQLSTATES) {
		INFO("SQLSTATE " << sqlstate);
		const auto records = ConnectRecords(SQL_ERROR, {{sqlstate, 0, HOSTILE}, {"HY000", 0, HOSTILE}});
		CHECK(LeakedThrough([&]() { duckdb::ThrowConnectFailure(records, context); }).empty());
	}
}

TEST_CASE("No server host, address, PID, URL or markup reaches a statement exception, in any phase or class",
          "[odbc]") {
	const std::vector<std::pair<StatementPhase, std::string>> phases {{StatementPhase::ALLOCATE, "allocate"},
	                                                                  {StatementPhase::PREPARE, "prepare"},
	                                                                  {StatementPhase::PARAMETERS, "parameters"},
	                                                                  {StatementPhase::DESCRIBE, "describe"},
	                                                                  {StatementPhase::BIND, "bind"},
	                                                                  {StatementPhase::EXECUTE, "execute"},
	                                                                  {StatementPhase::FETCH, "fetch"}};
	for (const auto &phase : phases) {
		for (const auto &sqlstate : SQLSTATES) {
			INFO("SQLSTATE " << sqlstate << ", phase " << phase.second);
			const auto records = StatementRecords({{"HY000", 0, HOSTILE}, {sqlstate, 0, HOSTILE}});
			CHECK(LeakedThrough([&]() { duckdb::ThrowStatementFailure(records, phase.first); }).empty());
		}
	}
}

TEST_CASE("No server host, address or PID in Redshift's box reaches a statement exception or a log line", "[odbc]") {
	// The error line is shown and sanitized; the box's other lines are never read.
	const std::string boxed = "ERROR:  \nDETAIL:  \n  ------\n  error:  failed at relay-7.us-east-1.internal:5471 "
	                          "(10.20.30.40) pid 4242\n  code:  1207\n  location:  leader:5471\n  process:  relay-7 "
	                          "[pid=4242]\n";
	for (const auto *sqlstate : {"XX000", "42601", "22012"}) {
		INFO("SQLSTATE " << sqlstate);
		const auto records = StatementRecords({{sqlstate, 0, boxed}});
		CHECK(LeakedThrough([&]() { duckdb::ThrowStatementFailure(records, StatementPhase::EXECUTE); }).empty());
		CHECK(LeakedToLog(StatementWarnings({{sqlstate, 0, boxed}})).empty());
		// The error line itself is shown, so the box is read and the check is not met by showing nothing.
		CHECK(redshift_test::CaptureError([&]() { duckdb::ThrowStatementFailure(records, StatementPhase::EXECUTE); })
		          .RawMessage()
		          .find("ERROR:  failed at") != std::string::npos);
	}
}

TEST_CASE("No server host, address, PID, URL or markup reaches a transport exception, for any class", "[odbc]") {
	for (const auto &sqlstate : SQLSTATES) {
		INFO("SQLSTATE " << sqlstate);
		const auto records = StatementRecords({{sqlstate, 0, HOSTILE}});
		CHECK(LeakedThrough([&]() {
			      duckdb::ThrowTransportFailure(records, "wg.example", StatementPhase::EXECUTE);
		      }).empty());
	}
}

TEST_CASE("No server host, address, PID, URL or markup reaches a log line, in either phase or any class", "[odbc]") {
	for (const auto &sqlstate : SQLSTATES) {
		INFO("SQLSTATE " << sqlstate);
		CHECK(LeakedToLog(ConnectRecords(SQL_SUCCESS_WITH_INFO, {{sqlstate, 0, HOSTILE}})).empty());
		CHECK(LeakedToLog(StatementWarnings({{sqlstate, 0, HOSTILE}})).empty());
	}
}

TEST_CASE("No SQLSTATE that is not five characters from [0-9A-Z] reaches a message, extra_info or log line", "[odbc]") {
	// In driver 2.3.0.0 the SQLSTATE is often libpq's, so a hostile server picks its bytes: an escape
	// sequence, or a line break and markup, one of them behind a class the message would show.
	const duckdb::ConnectFailureContext context {
	    "wg.example", {"workgroup", true, "Grant it access.", ""}, duckdb::redshift_driver::DriverIdentity(), 1, 61};
	const std::vector<std::pair<StatementPhase, std::string>> phases {
	    {StatementPhase::PREPARE, "prepare"}, {StatementPhase::EXECUTE, "execute"}, {StatementPhase::FETCH, "fetch"}};
	for (const std::string sqlstate : {"\x1b[2J", "4\n<b", "42\n<b"}) {
		INFO("SQLSTATE of " << sqlstate.size() << " bytes");
		const ExtraInfo expected_info {{"sqlstate", "HY000"}, {"sqlstates", "?????"}};
		const auto connect_records = ConnectRecords(SQL_ERROR, {{sqlstate, 0, "x"}});
		const auto connect = CaptureError([&]() { duckdb::ThrowConnectFailure(connect_records, context); });
		CHECK(connect.RawMessage().find(sqlstate) == std::string::npos);
		CHECK(StringUtil::EndsWith(connect.RawMessage(), "\nSQLSTATE: HY000"));
		CHECK(connect.ExtraInfo().at("sqlstate") == "HY000");
		CHECK(connect.ExtraInfo().at("sqlstates") == "?????");
		for (const auto &phase : phases) {
			INFO("phase " << phase.second);
			const auto records = StatementRecords({{sqlstate, 0, "ERROR:  x"}});
			const auto statement = CaptureError([&]() { duckdb::ThrowStatementFailure(records, phase.first); });
			CHECK(statement.RawMessage().find(sqlstate) == std::string::npos);
			CHECK(statement.RawMessage().find("(SQLSTATE class HY, driver error), and its message is not shown.") !=
			      std::string::npos);
			CHECK(StringUtil::EndsWith(statement.RawMessage(), DRIVER_FAILURE_TRAILER + "\nSQLSTATE: HY000"));
			CHECK(statement.ExtraInfo() == expected_info);
		}
		const std::vector<std::string> logged {"Redshift driver diagnostic, SQLSTATE ?????"};
		RecordingSink connect_sink;
		duckdb::LogInformationalDiagnostics(connect_sink, ConnectRecords(SQL_SUCCESS_WITH_INFO, {{sqlstate, 0, "x"}}));
		CHECK(connect_sink.Lines() == logged);
		RecordingSink statement_sink;
		duckdb::LogInformationalDiagnostics(statement_sink, StatementWarnings({{sqlstate, 0, "ERROR:  x"}}));
		CHECK(statement_sink.Lines() == logged);
	}
}

TEST_CASE("A credential a control or other space hides reaches neither a statement exception nor a log line",
          "[odbc]") {
	const std::string rlo = "\xE2\x80\xAE";
	const std::vector<std::pair<std::string, std::string>> messages {
	    {"key\x01"
	     "AKIA" +
	         rlo + "IOSFODNN7EXAMPLE",
	     "key <redacted>"},
	    {"key\xC2\xA0"
	     "AKIA" +
	         rlo + "IOSFODNN7EXAMPLE",
	     "key <redacted>"},
	    {"key AKIA" + rlo + "IOSFODNN7EXAMPLE\x01x", "key <redacted> x"},
	    {"x\x01PWD" + rlo + "=hunter22", "x PWD=<redacted>"},
	    {"x\xC2\xA0PWD" + rlo + "=hunter22", "x PWD=<redacted>"}};
	for (const auto &message : messages) {
		CheckShownAs(message.first, message.second);
	}
}

TEST_CASE("A host, address or pid that a control or other space splits, with another beside it, reaches neither a "
          "statement exception nor a log line",
          "[odbc]") {
	const std::string nbsp = "\xC2\xA0";
	const std::string shy = "\xC2\xAD";
	const std::vector<std::pair<std::string, std::string>> messages {
	    {"host x\x01ip-10-0\x01-0-1 x", "host x <host> x"},
	    {"host x" + nbsp + "ip-10-0" + nbsp + "-0-1 x", "host x <host> x"},
	    {"host 7\x01ip\x01-10-0-0-1 x", "host 7 <host> x"},
	    {"at 1234\x01"
	     "10.0\x01.0.1 x",
	     "at 1234 <address> x"},
	    {"at 1234\xE3\x80\x80"
	     "10.0\xE2\x80\xA8.0.1 x",
	     "at 1234 <address> x"},
	    {"at 1.\x01"
	     "10.0\x01.0.1 x",
	     "at 1. <address> x"},
	    {"x\x01pi\x01"
	     "d 4242",
	     "x pid <pid>"},
	    {"key" + shy + "ip-10-0-0-1 ok", "key<host> ok"},
	    {"key\xE2\x80\x8B"
	     "fe80::1%eth0:54321 ok",
	     "key<address> ok"},
	    {"at db-1.abc.us-east-1.redshift.amazonaws.com" + shy + "w", "at <host>w"},
	    {"at 2001:db8:85a3:0:0:8a2e:370:7334" + shy + "w", "at <address>w"},
	    {"key" + shy + "pid 4242 ok", "keypid <pid> ok"}};
	for (const auto &message : messages) {
		CheckShownAs(message.first, message.second);
	}
}

TEST_CASE("A credential that a character left out splits shows as it would whole in an exception and a log line",
          "[odbc]") {
	CheckShownAs("x P\xE2\x80\x8BWD=10.0.0.1 y", "x PWD=<redacted>");
	CheckShownAs("a.b.P\xE2\x80\x8BWD:5439 y", "a.b.PWD=<redacted>");
}

TEST_CASE("A bound value that matches the driver's framing leaves it out of a failed fetch's message", "[odbc]") {
	redshift_test::FakeDriverFixture fixture;
	fixture.fake.AddResult("SELECT a FROM t WHERE b = $1",
	                       {redshift_test::OneColumn(redshift_test::VarcharColumn("a", 16), {"x"})}, 1);
	auto connection = fixture.OpenConnection();
	auto result = redshift_test::ExecuteStatement(*connection, "SELECT a FROM t WHERE b = $1", {"ODBC Driver"});
	fixture.FailNextCall(
	    "SQLFetch", SQL_ERROR,
	    {{"22P02", 0, "[Redshift][ODBC Driver][Server]22P02:ERROR:  invalid input syntax: ODBC Driver", {}}});
	duckdb::RedshiftBatch batch;
	CHECK(redshift_test::Capture([&] { result->NextBatch(batch); }) ==
	      redshift_test::Thrown {duckdb::ExceptionType::EXECUTOR,
	                             "Redshift could not return the rest of the result: ERROR:  invalid "
	                             "input syntax: <param 1>\nSQLSTATE: 22P02"});
}

TEST_CASE("A credential value that a marker only starts reaches neither a statement exception nor a log line",
          "[odbc]") {
	const std::string rlo = "\xE2\x80\xAE";
	CheckShownAs("PWD\x01=AKIAIOSFODNN7EXAMPLE-rest-of-pw", "PWD=<redacted>");
	CheckShownAs("PWD" + rlo + "=AKIAIOSFODNN7EXAMPLE hunter22", "PWD=<redacted>");
	CheckShownAs("aws_secret_access_key\xE2\x80\x8B=ASIAIOSFODNN7EXAMPLE/wJalrXUtnFEMI",
	             "aws_secret_access_key=<redacted>");
	CheckShownAs("PWD" + rlo + "=<redacted>hunter22", "PWD=<redacted>");
}

TEST_CASE("A credential that a combining mark splits reaches neither a statement exception nor a log line", "[odbc]") {
	// An acute accent (Mn) and an enclosing circle (Me).
	CheckShownAs("key AKIA\xCC\x81IOSFODNN7EXAMPLE", "key <redacted>");
	CheckShownAs("key AKIA\xE2\x83\x9DIOSFODNN7EXAMPLE", "key <redacted>");
}

TEST_CASE("A credential that a private-use character, U+FFFC or U+FFFD splits reaches neither a statement exception "
          "nor a log line",
          "[odbc]") {
	CheckShownAs("at AKIA\xEF\xBF\xBCIOSFODNN7EXAMPLE ok", "at <redacted> ok");
	CheckShownAs("at PWD\xEE\x80\x80=hunter22 ok", "at PWD=<redacted>");
	CheckShownAs("at aws_secret_access_key\xEF\xBF\xBD=wJalrXUtnFEMI ok", "at aws_secret_access_key=<redacted>");
}

TEST_CASE("An assignment whose value is white space alone reaches a statement exception and a log line redacted",
          "[odbc]") {
	// The reader trims a message's trailing white space, so the value ends the first of two lines.
	for (const std::string hidden : {"\x01", "\xE2\x80\xAE"}) {
		INFO("hidden by " << hidden.size() << " bytes");
		const auto text = "[Redshift][ODBC Driver][Server]42601:ERROR:  PWD" + hidden + "= \nsecond line";
		const auto records = StatementRecords({{"42601", 0, text}});
		const auto error = CaptureError([&]() { duckdb::ThrowStatementFailure(records, StatementPhase::EXECUTE); });
		CHECK(error.Type() == duckdb::ExceptionType::EXECUTOR);
		CHECK(error.RawMessage() == "Redshift could not run the statement: ERROR:  PWD=<redacted>\nSQLSTATE: 42601");
		RecordingSink sink;
		duckdb::LogInformationalDiagnostics(sink, StatementWarnings({{"42601", 0, text}}));
		CHECK(sink.Lines() ==
		      std::vector<std::string> {"Redshift driver diagnostic, SQLSTATE 42601: ERROR:  PWD=<redacted>"});
	}
}

TEST_CASE("A form that U+1D159 splits reaches neither a statement exception nor a log line", "[odbc]") {
	// U+1D159, MUSICAL SYMBOL NULL NOTEHEAD, which many fonts draw as nothing.
	const std::string notehead = "\xF0\x9D\x85\x99";
	const std::vector<std::pair<std::string, std::string>> messages {
	    {"at A" + notehead + "KIAIOSFODNN7EXAMPLE ok", "at <redacted> ok"},
	    {"at PWD" + notehead + "=hunter22xyz ok", "at PWD=<redacted>"},
	    {"at aws" + notehead + "_secret_access_key=wJalrXUtnFEMI ok", "at aws_secret_access_key=<redacted>"},
	    {"at https" + notehead + "://relay9.example.net/v1 ok", "at <url> ok"},
	    {"at 172" + notehead + ".31.45.67 ok", "at <address> ok"},
	    {"at i" + notehead + "p-172-31-45-67 ok", "at <host> ok"},
	    {"at p" + notehead + "id 48213 ok", "at pid <pid> ok"},
	    {"at wgx9" + notehead + ".123456789012.us-east-1.redshift-serverless.amazonaws.com ok", "at <host> ok"}};
	for (const auto &message : messages) {
		CheckShownAs(message.first, message.second);
	}
}

TEST_CASE("A braced credential after a form feed or vertical tab reaches neither a statement exception nor a log "
          "line",
          "[odbc]") {
	CheckShownAs("at PWD=\v{hun;ter22} ok", "at PWD=<redacted>");
	CheckShownAs("at PWD=\f{hun;ter22} ok", "at PWD=<redacted>");
}

TEST_CASE("No server host, address, PID, URL or markup reaches a failed execute or fetch, alive or lost, for any class",
          "[odbc]") {
	for (const auto entry_point : {"SQLExecDirect", "SQLBindCol", "SQLFetch", "SQLGetData", FAILED_ROW}) {
		for (const auto &sqlstate : SQLSTATES) {
			INFO("SQLSTATE " << sqlstate << ", " << entry_point);
			CHECK(LeakedEndToEnd(entry_point, sqlstate, false).empty());
			CHECK(LeakedEndToEnd(entry_point, sqlstate, true).empty());
		}
	}
}

namespace {

//! The statement redshift_query runs after its first: one marker and one VARCHAR row.
const char *const ONE_MARKER = "SELECT $1";

//! The fragments shown when `entry_point` fails with HOSTILE under `sqlstate` in redshift_query's
//! second statement, on a connection the driver then reports `dead` or alive: in DuckDB's error, its
//! extra_info, or a line of duckdb_logs. The first statement marks the session, so no loss is retried.
std::string LeakedThroughRedshiftQuery(const char *entry_point, const std::string &sqlstate, bool dead) {
	redshift_test::FakeDriverFixture fixture;
	fixture.fake.AddResult(ONE_MARKER, {TextRow(10)}, 1);
	redshift_test::ExtensionDatabase database;
	database.Run("CALL enable_logging(level = 'debug')");
	database.Run(redshift_test::ATTACH_RS);
	database.Run("SELECT * FROM redshift_query('rs', 'SELECT 1')");
	fixture.FailNextCall(entry_point, SQL_ERROR, {{sqlstate, 0, HOSTILE, {}}});
	fixture.fake.SetConnectionDead(dead);
	auto failed =
	    database.con.Query(std::string("SELECT * FROM redshift_query('rs', '") + ONE_MARKER + "', params := ['v'])");
	REQUIRE(failed->HasError());
	const auto &error = failed->GetErrorObject();
	auto leaked = Leaked(error.RawMessage());
	for (const auto &entry : error.ExtraInfo()) {
		leaked += Leaked(entry.first) + Leaked(entry.second);
	}
	for (const auto &line : redshift_test::FirstColumn(*database.Run("SELECT message FROM duckdb_logs"))) {
		leaked += Leaked(line);
	}
	return leaked;
}

} // namespace

TEST_CASE("No server host, address, PID, URL or markup reaches redshift_query's error or log, in any phase", "[odbc]") {
	// Allocation, prepare, parameter count, describe, bind, execute and fetch, in that order.
	// The classes whose text a statement failure shows, and two transport classes: each case opens a
	// database, so the whole list would take minutes under TSan.
	for (const auto entry_point : {"SQLAllocHandle", "SQLPrepare", "SQLNumParams", "SQLDescribeCol", "SQLBindParameter",
	                               "SQLExecute", "SQLFetch"}) {
		for (const std::string sqlstate : {"42601", "22012", "23505", "0A000", "XX000", "08001", "57P01"}) {
			INFO("SQLSTATE " << sqlstate << ", " << entry_point);
			CHECK(LeakedThroughRedshiftQuery(entry_point, sqlstate, false).empty());
			CHECK(LeakedThroughRedshiftQuery(entry_point, sqlstate, true).empty());
		}
	}
}

TEST_CASE("A colon or quoted-key credential reaches neither a statement exception nor a log line", "[odbc]") {
	CheckShownAs("bad aws_session_token: TOKENcanary", "bad aws_session_token=<redacted>");
	// The driver's own keys, as the server echoes them in a cast's error.
	CheckShownAs("invalid input syntax for integer: \"SessionToken=IQoJcanary\"",
	             "invalid input syntax for integer: \"SessionToken=<redacted>\"");
	CheckShownAs("invalid input syntax for integer: \"Password={hunter2};UID=admin\"",
	             "invalid input syntax for integer: \"Password=<redacted>;UID=admin\"");
	CheckShownAs("bad {\"aws_secret_access_key\": \"SECRET;canary\"} next", "bad {aws_secret_access_key=<redacted>");
	// A URL in the value, whose tail can carry a session identifier, goes with it.
	CheckShownAs("bad PWD: see https://h.example/doc;jsessionid=SESSIONcanary", "bad PWD=<redacted>");
	// A key whose own quotes are escaped, once or twice, or doubled, inside a string literal.
	CheckShownAs("bad '{\\\"pwd\\\": \\\"SE;canary\\\"}' next", "bad '{pwd=<redacted>");
	CheckShownAs("bad '{\\\\\\\"pwd\\\\\\\": \\\\\\\"SE;canary\\\\\\\"}' next", "bad '{pwd=<redacted>");
	CheckShownAs("bad '{''pwd'': ''SE;canary''}' next", "bad '{pwd=<redacted>");
	// A keyword whose colon starts a URL's `://` is left to the URL rule, which replaces it whole.
	CheckShownAs("bad pwd://h.example/doc;jsessionid=SESSIONcanary ok", "bad <url> ok");
	// The fake driver keeps 326 bytes of the record, so the cut falls inside the quoted value.
	const std::string pad(100, 'x');
	CheckShownAs(pad + " aws_session_token: \"tok;" + std::string(300, 'C') + "\"",
	             pad + " aws_session_token=<redacted>...");
}

TEST_CASE("A SQL credential literal or a driver key reaches neither a statement exception nor a log line", "[odbc]") {
	CheckShownAs("syntax error at or near \"x\" in COPY t FROM 's3://b/k' SECRET_ACCESS_KEY 'wJalcanary' "
	             "SESSION_TOKEN 'IQoJcanary' x",
	             "syntax error at or near \"x\" in COPY t FROM '<url>' SECRET_ACCESS_KEY '<redacted>' "
	             "SESSION_TOKEN '<redacted>' x");
	CheckShownAs("bad UNLOAD ('SELECT 1') TO 's3://b/p' MASTER_SYMMETRIC_KEY 'ROOTcanary' ENCRYPTED",
	             "bad UNLOAD ('SELECT 1') TO '<url>' MASTER_SYMMETRIC_KEY '<redacted>' ENCRYPTED");
	CheckShownAs("bad CREATE USER u PASSWORD 'PWcanary' x", "bad CREATE USER u PASSWORD '<redacted>' x");
	CheckShownAs("bad 'PASSWORD' 'PWcanary' x", "bad PASSWORD '<redacted>' x");
	CheckShownAs("bad \"SESSION_TOKEN\" 'TOKcanary' x", "bad SESSION_TOKEN '<redacted>' x");
	CheckShownAs("bad CREDENTIALS 'aws_iam_role=r;token=TOKcanary;master_symmetric_key=KEYcanary'",
	             "bad CREDENTIALS 'aws_iam_role=r;token=<redacted>;master_symmetric_key=<redacted>");
	for (const std::string key : {"web_identity_token", "token", "client_secret", "ProxyPwd", "https_proxy_password"}) {
		CheckShownAs("invalid input syntax for integer: \"" + key + "={se;cret}}canary}\"",
		             "invalid input syntax for integer: \"" + key + "=<redacted>\"");
	}
	// Near-misses, each shown as the server wrote it.
	CheckShownAs("bad ALTER USER u PASSWORD DISABLE x", "bad ALTER USER u PASSWORD DISABLE x");
	CheckShownAs("bad user_password 'x' and tokenizer: y", "bad user_password 'x' and tokenizer: y");
}

TEST_CASE("A credential literal a refused statement echoes reaches redshift_query's error redacted", "[odbc]") {
	redshift_test::FakeDriverFixture fixture;
	// The setup's statements are prepared first; then the Describe the extension binds with.
	const auto setup = static_cast<int>(redshift_test::SessionSetupStatements().size());
	fixture.fake.FailCall("SQLPrepare", setup + 1, SQL_ERROR,
	                      {{"42601",
	                        0,
	                        "[Redshift][ODBC Driver][Server]42601:ERROR:  syntax error at or near \"x\" in COPY t FROM "
	                        "'s3://b/k' SECRET_ACCESS_KEY 'c4nary' x",
	                        {}}});
	redshift_test::ExtensionDatabase database;
	database.Run(redshift_test::ATTACH_RS);

	const auto result = database.con.Query("SELECT * FROM redshift_query('rs', 'SELECT 1')");
	REQUIRE(result->HasError());
	const auto &error = result->GetErrorObject();
	CHECK(error.RawMessage().find("SECRET_ACCESS_KEY '<redacted>' x") != std::string::npos);
	CHECK(error.RawMessage().find("c4") == std::string::npos);
	for (const auto &entry : error.ExtraInfo()) {
		CHECK(entry.second.find("c4") == std::string::npos);
	}
}

TEST_CASE("No server host, address, PID, URL or markup reaches the error of ATTACH's connect or redshift_query's "
          "prepare",
          "[odbc]") {
	for (const bool at_connect : {true, false}) {
		INFO((at_connect ? "a failed connect" : "a refused prepare"));
		redshift_test::FakeDriverFixture fixture;
		if (at_connect) {
			fixture.fake.SetConnectOutcome(SQL_ERROR, {{"08001", 0, HOSTILE, {}}, {"HY000", 0, HOSTILE, {}}});
		} else {
			// The setup's statements are prepared first; then the Describe the extension binds with.
			const auto setup = static_cast<int>(redshift_test::SessionSetupStatements().size());
			fixture.fake.FailCall("SQLPrepare", setup + 1, SQL_ERROR, {{"42601", 0, HOSTILE, {}}});
		}
		redshift_test::ExtensionDatabase database;
		// The connect is the ATTACH's, so a failed one fails the ATTACH.
		auto result = database.con.Query(redshift_test::ATTACH_RS);
		if (!at_connect) {
			REQUIRE_FALSE(result->HasError());
			result = database.con.Query("SELECT * FROM redshift_query('rs', 'SELECT 1')");
		}
		REQUIRE(result->HasError());
		const auto &error = result->GetErrorObject();
		const auto lead =
		    at_connect ? "Cannot attach rs: Could not connect to Redshift" : "Redshift refused the statement";
		CHECK(error.RawMessage().find(lead) != std::string::npos);
		auto leaked = Leaked(error.RawMessage());
		for (const auto &entry : error.ExtraInfo()) {
			leaked += Leaked(entry.first) + Leaked(entry.second);
		}
		CHECK(leaked.empty());
	}
}
