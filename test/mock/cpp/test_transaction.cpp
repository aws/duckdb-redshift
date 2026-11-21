// The DuckDB transaction gate when a client turns autocommit off, which SQL cannot do: every
// statement then runs inside an explicit DuckDB transaction, so redshift_query is refused, at bind
// and at execution, until the client ends the transaction or turns autocommit back on, unless
// redshift_allow_query_in_duckdb_transaction is on.

#include "catch.hpp"

#include "duckdb.hpp"
#include "test_log_storage.hpp"
#include "test_util.hpp"

using namespace duckdb;
using redshift_test::ErrorOf;
using redshift_test::Run;

namespace {

const char *ALLOW = "redshift_allow_query_in_duckdb_transaction";

const char *SET_WARNING = "redshift_query: redshift_allow_query_in_duckdb_transaction was set to true, so "
                          "redshift_query can run inside a DuckDB transaction. DuckDB COMMIT or ROLLBACK does not "
                          "control its Redshift operations.";

const char *USE_WARNING = "redshift_query: running inside a DuckDB transaction because "
                          "redshift_allow_query_in_duckdb_transaction is on. DuckDB COMMIT or ROLLBACK does not "
                          "control the Redshift operations it runs there. This is logged once per DuckDB connection.";

const char *GATE = "redshift_query cannot run while a DuckDB transaction is open because DuckDB COMMIT or ROLLBACK "
                   "does not control Redshift operations.";

const char *SCAN = "SELECT * FROM redshift_query('rs', 'rows=1')";

using Fixture = redshift_test::LoggedDatabase;

} // namespace

TEST_CASE("redshift_query is refused while the client has autocommit off, and runs once it is back on",
          "[transaction]") {
	Fixture f;
	Connection con(f.db);
	Run(con, "CREATE TABLE local (i INTEGER)");
	// Prepared in autocommit, so only execution's gate can refuse it.
	auto prepared = con.Prepare("SELECT count(*) FROM redshift_query('rs', 'rows=2')");
	REQUIRE_FALSE(prepared->HasError());
	REQUIRE(f.lines->Described() == 1);
	// One refusal per transaction: under DuckDB's default policy the refusal aborts it, and a second
	// statement would meet that instead of the gate at execution; at bind the gate still runs, and
	// would give the already-aborted message instead.
	for (const bool commit : {true, false}) {
		for (const bool at_bind : {true, false}) {
			INFO((commit ? "committed" : "rolled back") << ", refused at " << (at_bind ? "bind" : "execution"));
			con.SetAutoCommit(false);
			Run(con, "INSERT INTO local VALUES (1)");
			auto refused = at_bind ? con.Query(SCAN) : prepared->Execute();
			REQUIRE(refused->HasError());
			// An executor error at execution, which aborts the transaction under every policy.
			CHECK(refused->GetErrorType() == (at_bind ? ExceptionType::BINDER : ExceptionType::EXECUTOR));
			CHECK_THAT(refused->GetError(), Catch::Contains(GATE));
			CHECK(f.lines->Described() == 1);
			CHECK(f.lines->Executed() == 0);
			if (commit) {
				con.Commit();
			} else {
				con.Rollback();
			}
			con.SetAutoCommit(true);
		}
	}
	// With autocommit back on, both run.
	CHECK(ErrorOf(con, SCAN).empty());
	auto again = prepared->Execute();
	INFO(redshift_test::ErrorIn(*again));
	CHECK_FALSE(again->HasError());
	CHECK(f.lines->Executed() == 2);
}

TEST_CASE("a statement after the client turns autocommit back on without ending the transaction runs, and commits it",
          "[transaction]") {
	// SetAutoCommit(true) leaves the transaction open; the next statement runs in it, and, since it
	// succeeds, its end commits the transaction, as an autocommit statement's end does. No DuckDB
	// COMMIT or ROLLBACK can follow it in that transaction, so the gate does not refuse it.
	for (const bool begun_by_sql : {false, true}) {
		INFO((begun_by_sql ? "opened by BEGIN" : "opened by SetAutoCommit(false)"));
		Fixture f;
		Connection con(f.db);
		Run(con, "CREATE TABLE local (i INTEGER)");
		if (begun_by_sql) {
			Run(con, "BEGIN");
		} else {
			con.SetAutoCommit(false);
		}
		Run(con, "INSERT INTO local VALUES (1)");
		con.SetAutoCommit(true);
		REQUIRE(con.context->transaction.HasActiveTransaction());
		CHECK(ErrorOf(con, SCAN).empty());
		CHECK(f.lines->Executed() == 1);
		CHECK_FALSE(con.context->transaction.HasActiveTransaction());
		Connection other(f.db);
		auto rows = other.Query("SELECT count(*) FROM local");
		REQUIRE_FALSE(rows->HasError());
		CHECK(rows->Fetch()->GetValue(0, 0).GetValue<int64_t>() == 1);
	}
}

TEST_CASE("after a refusal aborted the transaction, turning autocommit back on lets the next statement fail and "
          "roll it back",
          "[transaction]") {
	// SetAutoCommit(true) leaves the aborted transaction open. The gate does not refuse the next
	// statement, but DuckDB does, before it executes ("Current transaction is aborted"), and rolls the
	// transaction back at its end; the statement after that runs, as an ODBC client
	// sees when it turns autocommit back on.
	Fixture f;
	Connection con(f.db);
	Run(con, "CREATE TABLE local (i INTEGER)");
	con.SetAutoCommit(false);
	Run(con, "INSERT INTO local VALUES (1)");
	CHECK_THAT(ErrorOf(con, SCAN), Catch::Contains("This error has aborted the DuckDB transaction"));
	con.SetAutoCommit(true);
	REQUIRE(con.context->transaction.HasActiveTransaction());
	CHECK_THAT(ErrorOf(con, SCAN), Catch::Contains("Current transaction is aborted"));
	CHECK_FALSE(con.context->transaction.HasActiveTransaction());
	CHECK(f.lines->Executed() == 0);
	auto rows = con.Query("SELECT count(*) FROM local");
	REQUIRE_FALSE(rows->HasError());
	CHECK(rows->Fetch()->GetValue(0, 0).GetValue<int64_t>() == 0);
	CHECK(ErrorOf(con, SCAN).empty());
	CHECK(f.lines->Executed() == 1);
}

TEST_CASE("the Relation API's bind, in a transaction DuckDB begins without a query, is not refused", "[transaction]") {
	Fixture f;
	Connection con(f.db);
	auto relation = con.TableFunction("redshift_query", {Value("rs"), Value("rows=2")});
	auto rows = relation->Execute();
	INFO(redshift_test::ErrorIn(*rows));
	REQUIRE_FALSE(rows->HasError());
	CHECK(f.lines->Executed() == 1);

	// Under autocommit off, the same relation is refused when it binds again to execute.
	con.SetAutoCommit(false);
	auto refused = relation->Execute();
	REQUIRE(refused->HasError());
	CHECK_THAT(refused->GetError(), Catch::Contains(GATE));
	CHECK(f.lines->Executed() == 1);
	con.Rollback();
	con.SetAutoCommit(true);
}

TEST_CASE("a statement in a transaction that still holds a detached DuckDB database under the name says what it is",
          "[transaction]") {
	// Only the message reads the transaction's databases (detach.test). The gate refuses inside an
	// explicit transaction by default, so this is reached through one whose client turned autocommit
	// back on.
	Fixture f;
	Connection con(f.db);
	Connection detaching(f.db);
	Run(detaching, "ATTACH ':memory:' AS local_held");
	Run(detaching, "CREATE TABLE local_held.t AS SELECT 1 AS x");
	con.SetAutoCommit(false);
	Run(con, "SELECT x FROM local_held.t");
	Run(detaching, "DETACH local_held");
	con.SetAutoCommit(true);
	CHECK_THAT(ErrorOf(con, "SELECT * FROM redshift_query('local_held', 'rows=1')"),
	           Catch::Contains("\"local_held\" is a duckdb database, not a Redshift handle"));
	CHECK(f.lines->Described() == 0);
}

TEST_CASE("under client-disabled autocommit, only current_transaction_invalidation_policy keeps the transaction usable",
          "[transaction]") {
	// SetAutoCommit(false) begins its transaction without reading default_transaction_invalidation_policy,
	// which only BEGIN reads (TransactionContext::SetInvalidationPolicy); a new connection's transaction
	// then keeps DuckDB's standard policy, under which every error aborts it.
	for (const bool current : {false, true}) {
		INFO((current ? "current_transaction_invalidation_policy" : "default_transaction_invalidation_policy"));
		Fixture f;
		Connection con(f.db);
		Run(con, "CREATE TABLE local (i INTEGER)");
		if (!current) {
			Run(con, "SET default_transaction_invalidation_policy = 'SYNTACTIC_ERRORS_DO_NOT_INVALIDATE'");
		}
		con.SetAutoCommit(false);
		if (current) {
			Run(con, "SET current_transaction_invalidation_policy = 'SYNTACTIC_ERRORS_DO_NOT_INVALIDATE'");
		}
		Run(con, "INSERT INTO local VALUES (1)");
		// The message says what happened to the transaction.
		const auto refusal = ErrorOf(con, SCAN);
		CHECK_THAT(refusal, Catch::Contains(GATE));
		CHECK_THAT(refusal, Catch::Contains(current ? "the DuckDB transaction is still usable"
		                                            : "This error has aborted the DuckDB transaction"));
		const auto after = ErrorOf(con, "INSERT INTO local VALUES (2)");
		if (current) {
			CHECK(after.empty());
		} else {
			CHECK_THAT(after, Catch::Contains("Current transaction is aborted"));
		}
		con.Commit();
		con.SetAutoCommit(true);
		auto rows = con.Query("SELECT count(*) FROM local");
		REQUIRE_FALSE(rows->HasError());
		CHECK(rows->Fetch()->GetValue(0, 0).GetValue<int64_t>() == (current ? 2 : 0));
		// The one refusal on a DuckDB connection with no session yet: before any transport call, an
		// open included.
		CHECK(f.lines->Opened() == 0);
		CHECK(f.lines->Described() == 0);
		CHECK(f.lines->Executed() == 0);
	}
}

TEST_CASE("under client-disabled autocommit, redshift_allow_query_in_duckdb_transaction lets redshift_query run, "
          "with one warning",
          "[transaction]") {
	// A client that keeps a transaction open, as JDBC with setAutoCommit(false) does, sets the option
	// as a statement in that transaction. A DuckDB ROLLBACK then undoes the DuckDB
	// insert, not the Redshift statements.
	Fixture f;
	Connection con(f.db);
	Run(con, "CREATE TABLE local (i INTEGER)");
	// Prepared in autocommit, so only execution's gate meets the transaction.
	auto prepared = con.Prepare("SELECT count(*) FROM redshift_query('rs', 'rows=2')");
	REQUIRE_FALSE(prepared->HasError());
	con.SetAutoCommit(false);
	Run(con, "SET redshift_allow_query_in_duckdb_transaction = true");
	CHECK(f.lines->Count(SET_WARNING) == 1);
	Run(con, "INSERT INTO local VALUES (1)");
	CHECK(f.lines->Count(USE_WARNING) == 0);
	// Execution alone logs the first-use warning, as bind alone does (transaction_option.test).
	auto executed = prepared->Execute();
	INFO(redshift_test::ErrorIn(*executed));
	CHECK_FALSE(executed->HasError());
	CHECK(f.lines->Count(USE_WARNING) == 1);
	CHECK(ErrorOf(con, SCAN).empty());
	CHECK(f.lines->Executed() == 2);
	CHECK(f.lines->Count(USE_WARNING) == 1);
	con.Rollback();
	con.SetAutoCommit(true);
	auto rows = con.Query("SELECT count(*) FROM local");
	REQUIRE_FALSE(rows->HasError());
	CHECK(rows->Fetch()->GetValue(0, 0).GetValue<int64_t>() == 0);
	CHECK(f.lines->Executed() == 2);
}

TEST_CASE("a handle the transaction attached runs once the client turns autocommit back on", "[transaction]") {
	// The transaction rule refuses a handle the open transaction attached only inside an explicit
	// transaction. After SetAutoCommit(true) the statement ends the transaction, ATTACH included: it
	// commits it if the statement succeeds and rolls it back if it fails, and no COMMIT or ROLLBACK
	// the client sends can follow it. So it is not refused, as the gate does not refuse it,
	// whether or not the option is on.
	for (const bool allowed : {false, true}) {
		INFO((allowed ? "option on" : "option off"));
		Fixture f;
		Connection con(f.db);
		if (allowed) {
			Run(con, "SET redshift_allow_query_in_duckdb_transaction = true");
		}
		con.SetAutoCommit(false);
		Run(con, redshift_test::Attach("rs2"));
		con.SetAutoCommit(true);
		REQUIRE(con.context->transaction.HasActiveTransaction());
		CHECK(ErrorOf(con, "SELECT * FROM redshift_query('rs2', 'rows=1')").empty());
		CHECK(f.lines->Executed() == 1);
		CHECK_FALSE(con.context->transaction.HasActiveTransaction());
	}
}

TEST_CASE("redshift_allow_query_in_duckdb_transaction's warnings never fail their statement", "[transaction]") {
	// A log storage that refuses each line: the SET, and the call, still succeed.
	Fixture f(true);
	Connection con(f.db);
	Run(con, "SET redshift_allow_query_in_duckdb_transaction = true");
	CHECK(f.lines->Count(SET_WARNING) == 1);
	con.SetAutoCommit(false);
	CHECK(ErrorOf(con, SCAN).empty());
	CHECK(f.lines->Count(USE_WARNING) == 1);
	CHECK(f.lines->Executed() == 1);
	con.Rollback();
	con.SetAutoCommit(true);
}

TEST_CASE("a redshift_allow_query_in_duckdb_transaction value stored without the set callback is read safely",
          "[transaction]") {
	// DBConfig::SetOption stores a value as given, neither checked nor cast: NULL, or a value that is
	// not a boolean, counts as off, and one that casts to true is on.
	const struct {
		Value value;
		bool allowed;
	} cases[] = {{Value(), false}, {Value("nonsense"), false}, {Value("false"), false}, {Value("true"), true}};
	for (auto &c : cases) {
		INFO("stored " << c.value.ToString());
		Fixture f;
		DBConfig::GetConfig(*f.db.instance).SetOption(ALLOW, c.value);
		Connection con(f.db);
		con.SetAutoCommit(false);
		const auto error = ErrorOf(con, SCAN);
		if (c.allowed) {
			CHECK(error.empty());
		} else {
			CHECK_THAT(error, Catch::Contains(GATE));
		}
		CHECK(f.lines->Executed() == (c.allowed ? 1 : 0));
		con.Rollback();
		con.SetAutoCommit(true);
	}
}

TEST_CASE("a redshift_allow_query_in_duckdb_transaction value given before an untyped database opens is refused",
          "[transaction]") {
	// For an untyped path, DuckDB checks the options given at open before any extension loads, and the
	// extension is not in its list of known extensions' settings, so the loadable extension is refused
	// the same way.
	DBConfig config;
	config.SetOptionByName(ALLOW, Value(true));
	REQUIRE_THROWS_WITH(DuckDB(nullptr, &config),
	                    Catch::Contains("The following options were not recognized") && Catch::Contains(ALLOW));
}

TEST_CASE("a redshift_allow_query_in_duckdb_transaction value given before a typed database opens is accepted",
          "[transaction]") {
	// A database path that names the extension's type (`redshift:…`) has DuckDB load the
	// extension, here the statically linked copy, before it checks the options given at open, so the
	// value is accepted, and the gate's read of it casts it. The open then fails past the options
	// check, at the handle's refusal of the default database's name, `memory`.
	DBConfig config;
	config.SetOptionByName(ALLOW, Value(true));
	string error;
	try {
		DuckDB db("redshift:", &config);
	} catch (std::exception &ex) {
		error = ErrorData(ex).RawMessage();
	}
	INFO(error);
	CHECK_THAT(error, !Catch::Contains("not recognized"));
	CHECK_THAT(error, Catch::Contains("a Redshift handle cannot be named memory"));
}

TEST_CASE("the refusal of a handle the transaction attached ends without the Redshift-transaction sentence",
          "[transaction]") {
	// No call can have opened a Redshift transaction on that handle, so the gate's closing sentence
	// about one does not apply.
	Fixture f;
	Connection con(f.db);
	Run(con, "SET redshift_allow_query_in_duckdb_transaction = true");
	con.SetAutoCommit(false);
	Run(con, redshift_test::Attach("rs2"));
	const auto error = ErrorOf(con, "SELECT * FROM redshift_query('rs2', 'rows=1')");
	CHECK_THAT(error, Catch::Contains("attach the handle again if a rollback undid its ATTACH, and run "
	                                  "redshift_query on the handle once that ATTACH has committed."));
	CHECK_THAT(error, !Catch::Contains("If a Redshift transaction is already open"));
	// Only the ATTACH's own session, which runs no statement, so opens no Redshift transaction.
	CHECK(f.lines->Opened() == 1);
	CHECK(f.lines->Executed() == 0);
	// Refused before the first-use warning, which this call has not used up.
	CHECK(f.lines->Count(USE_WARNING) == 0);
	con.Rollback();
	con.SetAutoCommit(true);
}

TEST_CASE("with autocommit off, a handle the transaction attached runs once its ATTACH is committed", "[transaction]") {
	// The refusal's advice: a ROLLBACK undoes the ATTACH, so attaching again and calling is refused
	// again; committing the ATTACH first lets the call run. DuckDB turns autocommit back on at each
	// commit or rollback, so a client that keeps it off, as this one does, turns it off again.
	Fixture f;
	Connection con(f.db);
	Run(con, "SET redshift_allow_query_in_duckdb_transaction = true");
	const auto refused = [&]() {
		con.SetAutoCommit(false);
		Run(con, redshift_test::Attach("rs2"));
		CHECK_THAT(ErrorOf(con, "SELECT * FROM redshift_query('rs2', 'rows=1')"),
		           Catch::Contains("because the open DuckDB transaction attached it"));
		con.Rollback();
	};
	refused();
	refused();
	con.SetAutoCommit(false);
	Run(con, redshift_test::Attach("rs2"));
	con.Commit();
	con.SetAutoCommit(false);
	CHECK(ErrorOf(con, "SELECT * FROM redshift_query('rs2', 'rows=1')") == "");
	CHECK(f.lines->Executed() == 1);
	con.Rollback();
	con.SetAutoCommit(true);
}
