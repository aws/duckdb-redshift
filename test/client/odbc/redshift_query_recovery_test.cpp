// redshift_query's handling of a refused or lost call on the real client, against the fake driver:
// an aborted transaction at bind keeps the connection and says how to end it, a Describe that loses
// a connection whose session held no state is described again on one replacement, an Execute that
// loses its connection is not run again, a session Redshift's answer left unusable is discarded and
// nothing described again, and DETACH
// closes another DuckDB connection's idle session, which is told at its next statement. The extension's tests in
// test/mock/ cover these paths on the mock client; these check them through the client's own classification of what the
// driver returns.

// catch.hpp comes first, as in this directory's other tests.
#include "catch.hpp"

#include "extension_database.hpp"
#include "fake_driver_fixture.hpp"
#include "recorded_failures.hpp"

#include <string>
#include <vector>

using namespace duckdb;
using redshift_driver::testing::FakeColumn;
using redshift_driver::testing::FakeDiag;
using redshift_driver::testing::FakeResultSet;
using redshift_test::ATTACH_RS;
using redshift_test::ExtensionDatabase;
using redshift_test::FakeDriverFixture;
using redshift_test::FirstColumn;

namespace {

//! A query whose one result set holds the rows "x" and "y".
const char *const QUERY = "SELECT a FROM t";
//! redshift_query running QUERY on the handle ATTACH_RS attaches.
const char *const USE_QUERY = "SELECT * FROM redshift_query('rs', 'SELECT a FROM t') ORDER BY a";

//! A record the client classifies as a lost connection (class 08).
const FakeDiag LOST {"08S01", 0, "SSL error: unexpected eof from relay-1.internal (10.1.2.3)", {}};
//! The record Redshift gives a statement in a transaction an earlier error aborted, as recorded live.
const FakeDiag ABORTED = redshift_test::ABORTED_TRANSACTION_RECORD;
//! The record of an answer that leaves the session unusable, as driver 2.3.0.0 stamps Redshift's.
const FakeDiag INTERNAL_SERVICE_ERROR {
    "XX000", 0, "[Redshift][ODBC Driver][Server]XX000:ERROR:  Internal Service Error", {}};

//! A fixture with QUERY scripted.
class RecoveryFixture : public FakeDriverFixture {
public:
	RecoveryFixture() {
		FakeColumn column;
		column.name = "a";
		column.type_name = "CHARACTER VARYING";
		column.sql_type = SQL_VARCHAR;
		column.column_size = 256;
		FakeResultSet result;
		result.columns.push_back(column);
		result.rows.push_back({"x"});
		result.rows.push_back({"y"});
		fake.AddResult(QUERY, {result});
	}

	//! Makes the `nth` call to `entry_point`, SQLPrepare or SQLExecute, that is not part of a session
	//! setup fail, counting from the fixture's construction, where `connections` connections, that
	//! call's own included, have opened by then. Each setup runs one SQLPrepare and one SQLExecute per
	//! statement, before the connection is returned. Call it before any connection opens.
	void FailAfterSetup(const char *entry_point, size_t connections, size_t nth, std::vector<FakeDiag> diags) {
		const auto setup = redshift_test::SessionSetupStatements().size();
		fake.FailCall(entry_point, NumericCast<int>(connections * setup + nth), SQL_ERROR, std::move(diags));
	}
};

//! Whether `text` holds `part`.
bool Holds(const std::string &text, const std::string &part) {
	return text.find(part) != std::string::npos;
}

} // namespace

TEST_CASE("redshift_query in an aborted Redshift transaction says how to end it, and keeps the connection", "[odbc]") {
	RecoveryFixture fixture;
	// The bind's Describe, the first SQLPrepare after the session setup.
	fixture.FailAfterSetup("SQLPrepare", 1, 1, {ABORTED});
	ExtensionDatabase database;
	database.Run(ATTACH_RS);

	const auto refused = database.con.Query(USE_QUERY);
	REQUIRE(refused->HasError());
	const auto &error = refused->GetErrorObject();
	CHECK(error.Type() == ExceptionType::BINDER);
	CHECK(Holds(error.RawMessage(), "SQLSTATE: 25P02"));
	CHECK(Holds(error.RawMessage(), "The Redshift transaction on attach handle \"rs\" is aborted, so Redshift refuses "
	                                "every statement until it ends. Run redshift_query('rs', 'ROLLBACK') to end it."));
	CHECK(error.ExtraInfo().at("sqlstate") == "25P02");

	// A refusal leaves the connection, and with it the Redshift transaction to end, so the next
	// statement runs on the same one.
	auto next = database.Run(USE_QUERY);
	CHECK(FirstColumn(*next) == std::vector<std::string> {"x", "y"});
	CHECK(fixture.CallsTo("SQLDriverConnect") == 1);
	CHECK(fixture.fake.AllocatedHandles() == "environment, connection");
}

TEST_CASE("redshift_query describes again on one replacement when Describe loses a stateless session", "[odbc]") {
	RecoveryFixture fixture;
	fixture.FailAfterSetup("SQLPrepare", 1, 1, {LOST});
	ExtensionDatabase database;
	database.Run(ATTACH_RS);

	// The session ran nothing, so it held no state: the bind opens a new connection and describes
	// the statement on it, and the statement runs there.
	auto result = database.Run(USE_QUERY);
	CHECK(FirstColumn(*result) == std::vector<std::string> {"x", "y"});
	CHECK(fixture.CallsTo("SQLDriverConnect") == 2);
	// The lost connection is freed; the replacement stays for the pair's next statement.
	CHECK(fixture.fake.AllocatedHandles() == "environment, connection");
}

TEST_CASE("redshift_query fails a bind whose replacement connection is lost too, and opens no third", "[odbc]") {
	RecoveryFixture fixture;
	fixture.FailAfterSetup("SQLPrepare", 1, 1, {LOST});
	// The replacement's Describe: the second SQLPrepare outside a setup, after two connections'.
	fixture.FailAfterSetup("SQLPrepare", 2, 2, {LOST});
	ExtensionDatabase database;
	database.Run(ATTACH_RS);

	const auto lost = database.con.Query(USE_QUERY);
	REQUIRE(lost->HasError());
	const auto &error = lost->GetErrorObject();
	CHECK(error.Type() == ExceptionType::IO);
	CHECK(Holds(error.RawMessage(), "redshift_query: the Redshift connection of attach handle \"rs\" was lost while "
	                                "describing the statement, and so was the replacement connection opened to "
	                                "describe it again, so the statement was not run on Redshift."));
	// The replacement only described, so its session held no state either.
	CHECK_FALSE(Holds(error.RawMessage(), "may have held state"));
	CHECK(fixture.CallsTo("SQLDriverConnect") == 2);
	CHECK(fixture.CallsTo("SQLExecute") == 2 * redshift_test::SessionSetupStatements().size());
	CHECK(fixture.fake.AllocatedHandles() == "environment");

	// Neither session held state, so the next statement opens a new one and runs, with no report.
	auto next = database.Run(USE_QUERY);
	CHECK(FirstColumn(*next) == std::vector<std::string> {"x", "y"});
	CHECK(fixture.CallsTo("SQLDriverConnect") == 3);
}

TEST_CASE("redshift_query fails a bind whose Describe loses a session that may hold state, opening no replacement",
          "[odbc]") {
	RecoveryFixture fixture;
	ExtensionDatabase database;
	database.Run(ATTACH_RS);
	// A statement that ran marks the session as one that may hold state.
	database.Run(USE_QUERY);
	fixture.FailNextCall("SQLPrepare", SQL_ERROR, {LOST});

	const auto lost = database.con.Query(USE_QUERY);
	REQUIRE(lost->HasError());
	const auto &error = lost->GetErrorObject();
	CHECK(error.Type() == ExceptionType::IO);
	CHECK(Holds(error.RawMessage(), "redshift_query: the Redshift connection of attach handle \"rs\" was lost while "
	                                "describing the statement, so the statement was not run on Redshift. The session "
	                                "it carried may have held state, which may now be lost"));
	CHECK(fixture.CallsTo("SQLDriverConnect") == 1);
	CHECK(fixture.fake.AllocatedHandles() == "environment");

	// The user has seen the report, so the next statement opens a new session and runs.
	auto next = database.Run(USE_QUERY);
	CHECK(FirstColumn(*next) == std::vector<std::string> {"x", "y"});
	CHECK(fixture.CallsTo("SQLDriverConnect") == 2);
}

TEST_CASE("redshift_query does not run a statement again when Execute loses its connection", "[odbc]") {
	RecoveryFixture fixture;
	// The scan's statement has no parameters, so it runs with SQLExecDirect; the setup's do not.
	fixture.fake.FailCall("SQLExecDirect", 1, SQL_ERROR, {LOST});
	ExtensionDatabase database;
	database.Run(ATTACH_RS);

	const auto lost = database.con.Query(USE_QUERY);
	REQUIRE(lost->HasError());
	const auto &error = lost->GetErrorObject();
	CHECK(error.Type() == ExceptionType::IO);
	CHECK(Holds(error.RawMessage(), "redshift_query: the Redshift connection of attach handle \"rs\" was lost while "
	                                "executing the statement, and the statement was not retried. The statement may "
	                                "already have applied its effects on Redshift; check them before re-running it. "
	                                "The session it carried may have held state, which may now be lost"));
	// One run of the statement: the failed one, never a second.
	CHECK(fixture.CallsTo("SQLExecDirect") == 1);
	CHECK(fixture.CallsTo("SQLDriverConnect") == 1);
	CHECK(fixture.fake.AllocatedHandles() == "environment");

	// The next statement opens a new connection and runs on it.
	auto next = database.Run(USE_QUERY);
	CHECK(FirstColumn(*next) == std::vector<std::string> {"x", "y"});
	CHECK(fixture.CallsTo("SQLDriverConnect") == 2);
}

TEST_CASE("redshift_query discards a session the client reports unusable at bind, describes nothing again, and the "
          "next statement opens a new one",
          "[odbc]") {
	RecoveryFixture fixture;
	// The bind's Describe, the first SQLPrepare after the session setup, on a session that ran nothing.
	fixture.FailAfterSetup("SQLPrepare", 1, 1, {INTERNAL_SERVICE_ERROR});
	ExtensionDatabase database;
	database.Run(ATTACH_RS);

	const auto refused = database.con.Query(USE_QUERY);
	REQUIRE(refused->HasError());
	const auto &error = refused->GetErrorObject();
	CHECK(error.Type() == ExceptionType::BINDER);
	CHECK(Holds(error.RawMessage(),
	            "redshift_query: Redshift session error on attach handle \"rs\": the session cannot be used any more. "
	            "The extension discarded it; the next redshift_query on \"rs\" opens a new session. The statement "
	            "was not run on Redshift. The client reported: Redshift refused the statement: ERROR:  Internal "
	            "Service Error\nSQLSTATE: XX000"));
	// The session ran nothing, so the message does not say what was lost.
	CHECK_FALSE(Holds(error.RawMessage(), "Anything that lived in the old session is gone"));
	CHECK(error.ExtraInfo().at("redshift_error") == "session_unusable");
	// The client ended the session, and no replacement opened to describe the statement again.
	CHECK(fixture.CallsTo("SQLDisconnect") == 1);
	CHECK(fixture.CallsTo("SQLDriverConnect") == 1);
	CHECK(fixture.fake.AllocatedHandles() == "environment");

	auto next = database.Run(USE_QUERY);
	CHECK(FirstColumn(*next) == std::vector<std::string> {"x", "y"});
	CHECK(fixture.CallsTo("SQLDriverConnect") == 2);
}

TEST_CASE("DETACH closes another DuckDB connection's idle session, which is told at its next statement", "[odbc]") {
	RecoveryFixture fixture;
	ExtensionDatabase database;
	database.Run(ATTACH_RS);
	Connection bystander(database.db);
	// A statement that ran marks the bystander's session as one that may hold state.
	REQUIRE_FALSE(bystander.Query(USE_QUERY)->HasError());
	// The ATTACH's session and the bystander's.
	REQUIRE(fixture.CallsTo("SQLDriverConnect") == 2);

	// The bystander's connection is idle, so DETACH on the other DuckDB connection closes it at once,
	// with the detaching connection's own.
	database.Run("DETACH rs");
	CHECK(fixture.CallsTo("SQLDisconnect") == 2);
	CHECK(fixture.fake.AllocatedHandles() == "environment");

	// Attached again, which opens the attaching connection's session: the bystander's next statement
	// is refused once, before it opens anything, and the one after runs on a new session.
	database.Run(ATTACH_RS);
	CHECK(fixture.CallsTo("SQLDriverConnect") == 3);
	const auto refused = bystander.Query(USE_QUERY);
	REQUIRE(refused->HasError());
	CHECK(Holds(refused->GetErrorObject().RawMessage(), "a new handle is attached under the name now"));
	CHECK(fixture.CallsTo("SQLDriverConnect") == 3);
	auto next = bystander.Query(USE_QUERY);
	REQUIRE_FALSE(next->HasError());
	CHECK(FirstColumn(*next) == std::vector<std::string> {"x", "y"});
	CHECK(fixture.CallsTo("SQLDriverConnect") == 4);
}
