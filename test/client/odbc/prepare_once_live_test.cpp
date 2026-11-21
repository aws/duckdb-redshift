// With the live variables, the statement Describe keeps, on driver 2.3.0.0: a redshift_query scan with
// no parameters runs the statement its bind prepared, and one with parameters or a DOUBLE column is
// prepared again; a kept statement whose table changes while it is kept runs its text again, so the
// check after the run refuses the change, where one prepared again is refused before it runs. The
// [writes] cases write only a temporary table in their own session.

// catch.hpp comes first, as in this directory's other tests.
#include "catch.hpp"

#include "call_trace.hpp"
#include "fake_driver_fixture.hpp"
#include "live_workgroup.hpp"
#include "odbc/odbc_connection.hpp"
#include "session_probe.hpp"
#include "thrown.hpp"

#include <algorithm>
#include <cstddef>
#include <string>
#include <vector>

using namespace duckdb;
using redshift_test::CallTrace;
using redshift_test::ExtensionDatabase;
using redshift_test::SessionProbe;
using redshift_test::Thrown;

namespace {

//! What a scan returned, and the driver calls it made.
struct Scanned {
	//! How many calls of each kind: "<n> SQLPrepare, <n> SQLExecDirect, <n> SQLExecute, <n> SQLAllocHandle".
	std::string calls;
	//! The first column's values.
	std::vector<std::string> values;
};

//! How many of `calls` are to `entry_point`, whatever each returned.
size_t CallsTo(const std::vector<std::string> &calls, const std::string &entry_point) {
	const auto prefix = entry_point + " ";
	return NumericCast<size_t>(std::count_if(calls.begin(), calls.end(), [&](const std::string &call) {
		return call.compare(0, prefix.size(), prefix) == 0;
	}));
}

//! The live session a DuckDB database opens through `rs`, every driver call recorded.
struct TracedDatabase {
	TracedDatabase() {
		SetDriverSourceForTesting(&CallTrace::Source);
		CallTrace::Start(&redshift_driver::LoadEmbeddedDriver);
		database = make_uniq<ExtensionDatabase>();
		database->Run(redshift_test::LiveWorkgroupAttach());
		// Opens the session, so the calls a case counts are the scan's alone.
		database->Run("SELECT * FROM redshift_query('rs', 'SELECT 1')");
	}

	Scanned Scan(const std::string &scan) {
		const auto before = CallTrace::Calls().size();
		Scanned scanned;
		scanned.values = redshift_test::FirstColumn(*database->Run(scan));
		const auto all = CallTrace::Calls();
		const std::vector<std::string> calls(all.begin() + NumericCast<std::ptrdiff_t>(before), all.end());
		for (const char *entry_point : {"SQLPrepare", "SQLExecDirect", "SQLExecute", "SQLAllocHandle"}) {
			scanned.calls +=
			    (scanned.calls.empty() ? "" : ", ") + std::to_string(CallsTo(calls, entry_point)) + " " + entry_point;
		}
		return scanned;
	}

	// Destroyed in reverse, so the database closes its session before the process state is reset.
	redshift_test::ResetOnExit reset;
	redshift_test::LiveEndpoints endpoints;
	unique_ptr<ExtensionDatabase> database;
};

const std::string READ_ROWS = "SELECT * FROM prepare_once_rows";
const std::string ADD_COLUMN = "ALTER TABLE prepare_once_rows ADD COLUMN b INTEGER DEFAULT 8";
//! The client's refusal of a kept statement whose table gained a column after Describe.
const std::string MAY_HAVE_RUN = "the statement's result shape changed on the server after it was checked, so its "
                                 "rows were not read; it may have run, so check before running it again: it was bound "
                                 "with 1 column and now has 2";

//! A client connection to the live workgroup, whose handle SessionProbe keeps, so a case can change
//! the session's table on a handle of its own while the client keeps a statement. The connection's
//! clock stands still, so the kept statement is within its second however long the change takes.
struct KeptStatementSession {
	KeptStatementSession() {
		SetDriverSourceForTesting(&SessionProbe::Source);
		SessionProbe::Start(&redshift_driver::LoadEmbeddedDriver);
		// After the source, whose reset would drop it.
		clock = make_uniq<redshift_test::ManualClock>();
		connection = OpenRedshiftConnection(redshift_test::LiveWorkgroupConfig(),
		                                    make_shared_ptr<redshift_test::CapturingLogSink>());
		Run("CREATE TEMP TABLE prepare_once_rows (a INTEGER)");
		Run("INSERT INTO prepare_once_rows VALUES (7)");
	}

	//! `sql` described, run and read to the end.
	redshift_test::ReadBatches Run(const std::string &sql) {
		auto result = connection->Execute(sql, {}, connection->Describe(sql));
		return redshift_test::ReadBatches(*result);
	}

	// Destroyed in reverse, so the connection closes before the process state is reset.
	redshift_test::ResetOnExit reset;
	redshift_test::LiveEndpoints endpoints;
	unique_ptr<redshift_test::ManualClock> clock;
	unique_ptr<RedshiftConnection> connection;
};

//! The rows of `read` as text, one line a row, its values separated by spaces.
std::vector<std::string> RowTexts(const redshift_test::ReadBatches &read) {
	std::vector<std::string> texts;
	for (const auto &row : read.rows) {
		std::string text;
		for (const auto &value : row) {
			text += (text.empty() ? "" : " ") + std::to_string(value.integer);
		}
		texts.push_back(text);
	}
	return texts;
}

} // namespace

// On the real clock, as a scan runs: its bind and its run are one prepare round trip apart, well inside
// the second, so a failure with two prepares means either the reuse is lost or the server took that long.
TEST_CASE("Live: a redshift_query with no parameters runs the statement its bind prepared", "[odbc][live]") {
	SKIP_UNLESS_LIVE(redshift_driver::LoadEmbeddedDriver());
	TracedDatabase traced;

	// An INTEGER and a VARCHAR, which the simple protocol keeps, so the bind's statement is kept.
	const auto scanned = traced.Scan("SELECT a FROM redshift_query('rs', 'SELECT 7 AS a, ''seven''::varchar(8) AS b')");
	CHECK(scanned.calls == "1 SQLPrepare, 1 SQLExecDirect, 0 SQLExecute, 1 SQLAllocHandle");
	CHECK(scanned.values == std::vector<std::string> {"7"});
}

TEST_CASE("Live: a redshift_query with parameters, or with a DOUBLE column, prepares its statement again",
          "[odbc][live]") {
	SKIP_UNLESS_LIVE(redshift_driver::LoadEmbeddedDriver());
	TracedDatabase traced;
	const std::string prepared_twice = "2 SQLPrepare, 0 SQLExecDirect, 1 SQLExecute, 2 SQLAllocHandle";

	const auto with_params =
	    traced.Scan("SELECT * FROM redshift_query('rs', 'SELECT $1::integer AS a', params := ['7'])");
	CHECK(with_params.calls == prepared_twice);
	CHECK(with_params.values == std::vector<std::string> {"7"});
	const auto with_double = traced.Scan("SELECT * FROM redshift_query('rs', 'SELECT 0.5::float8 AS a')");
	CHECK(with_double.calls == prepared_twice);
	CHECK(with_double.values == std::vector<std::string> {"0.5"});
}

// The kept statement's description still has one column, so the check before the run passes; its run
// sends the text again, and the server reads the table as it now stands. A statement prepared again
// would have been refused before it ran (the next case), so the may-have-run refusal shows the kept
// statement ran.
TEST_CASE("Live: a kept statement whose table changes while it is kept is not read, and may have run",
          "[odbc][live][writes]") {
	SKIP_UNLESS_LIVE(redshift_driver::LoadEmbeddedDriver());
	KeptStatementSession session;
	const auto bound = session.connection->Describe(READ_ROWS);
	SessionProbe::RunStatement(ADD_COLUMN);

	const auto thrown = redshift_test::Capture([&] { session.connection->Execute(READ_ROWS, {}, bound); });

	CHECK(thrown == Thrown {ExceptionType::EXECUTOR, MAY_HAVE_RUN});
	CHECK(RowTexts(session.Run(READ_ROWS)) == std::vector<std::string> {"7 8"});
}

// Moved on by the bound itself, so an age of exactly one second is the first that prepares again.
TEST_CASE("Live: a statement run a second after Describe's prepare is prepared again, and a changed one not run",
          "[odbc][live][writes]") {
	SKIP_UNLESS_LIVE(redshift_driver::LoadEmbeddedDriver());
	KeptStatementSession session;
	const auto bound = session.connection->Describe(READ_ROWS);
	SessionProbe::RunStatement(ADD_COLUMN);
	session.clock->Advance(DESCRIBED_STATEMENT_REUSE);

	const auto error = redshift_test::CaptureError([&] { session.connection->Execute(READ_ROWS, {}, bound); });

	CHECK(Thrown {error.Type(), error.RawMessage()} ==
	      Thrown {ExceptionType::BINDER, "the statement's result shape is not the one it was bound with, so it was "
	                                     "not run: it was bound with 1 column and now has 2"});
	CHECK(redshift_test::InfoOf(error, "redshift_error") == "shape_changed");
	CHECK(RowTexts(session.Run(READ_ROWS)) == std::vector<std::string> {"7 8"});
}

// As a caller meets it: a DuckDB PREPARE binds the scan and keeps its statement, and the EXECUTE that
// follows inside the second runs it. The clock stands still, as above.
TEST_CASE("Live: an EXECUTE whose table changed after its PREPARE is not read, and may have run",
          "[odbc][live][writes]") {
	SKIP_UNLESS_LIVE(redshift_driver::LoadEmbeddedDriver());
	redshift_test::ResetOnExit reset;
	redshift_test::LiveEndpoints endpoints;
	SetDriverSourceForTesting(&SessionProbe::Source);
	SessionProbe::Start(&redshift_driver::LoadEmbeddedDriver);
	redshift_test::ManualClock clock;
	ExtensionDatabase database;
	database.Run(redshift_test::LiveWorkgroupAttach());
	database.Run("SELECT * FROM redshift_query('rs', 'CREATE TEMP TABLE prepare_once_rows (a INTEGER)')");
	database.Run("SELECT * FROM redshift_query('rs', 'INSERT INTO prepare_once_rows VALUES (7)')");
	database.Run("PREPARE read_rows AS SELECT * FROM redshift_query('rs', '" + READ_ROWS + "')");
	SessionProbe::RunStatement(ADD_COLUMN);

	auto refused = database.con.Query("EXECUTE read_rows");
	REQUIRE(refused->HasError());
	CHECK(refused->GetErrorObject().Type() == ExceptionType::EXECUTOR);
	CHECK_THAT(refused->GetErrorObject().RawMessage(), Catch::Contains(MAY_HAVE_RUN));
	CHECK(redshift_test::FirstColumn(*database.Run("SELECT b FROM redshift_query('rs', '" + READ_ROWS + "')")) ==
	      std::vector<std::string> {"8"});
}
