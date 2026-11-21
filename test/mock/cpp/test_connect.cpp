// CONNECT to an attach handle, where SQL tests cannot reach: the call CONNECT makes
// for a statement (RemoteExecute), a handle replaced while another DuckDB connection still holds the
// earlier one, so that DuckDB still routes to it, and prepared statements while CONNECT-ed.

#include "catch.hpp"

#include <future>

#include "duckdb.hpp"
#include "duckdb/common/sql_identifier.hpp"
#include "duckdb/common/string_util.hpp"
#include "duckdb/main/attached_database.hpp"
#include "duckdb/main/client_context_state.hpp"
#include "duckdb/main/database_manager.hpp"
#include "duckdb/main/query_result_stream.hpp"
#include "duckdb/main/valid_checker.hpp"
#include "duckdb/optimizer/optimizer_extension.hpp"
#include "duckdb/parser/expression/function_expression.hpp"
#include "duckdb/parser/tableref/table_function_ref.hpp"
#include "duckdb/planner/operator/logical_get.hpp"
#include "duckdb/transaction/meta_transaction.hpp"
#include "mock_transport.hpp"
#include "redshift_conversion.hpp"
#include "redshift_query.hpp"
#include "storage/redshift_catalog.hpp"
#include "storage/redshift_connection_ownership.hpp"
#include "test_log_storage.hpp"
#include "test_util.hpp"

using namespace duckdb;
using redshift_test::BOUND;
using redshift_test::ErrorOf;
using redshift_test::PauseHold;
using redshift_test::Run;

namespace {

using Fixture = redshift_test::LoggedDatabase;

//! The handle attached as `name` in `db` now.
shared_ptr<AttachedDatabase> Handle(DuckDB &db, const string &name) {
	auto attached = db.instance->GetDatabaseManager().GetDatabase(Identifier(name));
	REQUIRE(attached);
	return attached;
}

//! Holds a streaming redshift_query result on `rs`, and with it a pin on the handle, so the handle
//! stays alive through a DETACH or ATTACH OR REPLACE, and DuckDB still routes a CONNECT-ed DuckDB
//! connection's statements to it.
struct HeldStream {
	explicit HeldStream(DuckDB &db) : con(db) {
		// More rows than one DuckDB chunk, so the first fetch leaves the query, and its pin, open.
		stream = make_uniq<QueryResultStream<>>(con.Submit("SELECT * FROM redshift_query('rs', 'rows=5000 cols=1')"));
		REQUIRE_FALSE(stream->HasError());
		REQUIRE(stream->Fetch());
	}

	Connection con;
	unique_ptr<QueryResultStream<>> stream;
};

//! Binds every redshift_query scan in `op` again after the optimizer, keeping its reference's
//! bind_info, so a call CONNECT made stays CONNECT's. The new bind data never passed the fence, as in
//! test_fence.cpp's RebuildRedshiftScans, which stands in for another extension changing the plan.
void RebindRoutedScans(ClientContext &context, LogicalOperator &op) {
	if (op.type == LogicalOperatorType::LOGICAL_GET) {
		auto &get = op.Cast<LogicalGet>();
		if (get.function.GetName() == "redshift_query") {
			TableFunctionRef ref;
			ref.bind_info = get.bind_info;
			TableFunctionBindInput input(get.parameters, get.named_parameters, get.input_table_types,
			                             get.input_table_names, get.function.function_info.get(), nullptr, get.function,
			                             ref, nullptr);
			vector<LogicalType> types;
			vector<Identifier> names;
			get.bind_data = get.function.bind(context, input, types, names);
		}
	}
	for (auto &child : op.children) {
		RebindRoutedScans(context, *child);
	}
}

} // namespace

TEST_CASE("CONNECT runs a statement as redshift_query on the handle, marked as CONNECT's", "[connect]") {
	Fixture f;
	auto handle = Handle(f.db, "rs");
	auto &catalog = handle->GetCatalog();
	CHECK(catalog.Supports(RemoteCapability::CONNECT));
	CHECK_FALSE(catalog.Supports(RemoteCapability::IS_REMOTE));
	CHECK_FALSE(catalog.Supports(RemoteCapability::EXECUTE_QUERY_NODE));
	CHECK_FALSE(catalog.Supports(RemoteCapability::EXECUTE_STATEMENT));

	Connection con(f.db);
	auto ref = catalog.RemoteExecute(*con.context, "SELECT 'it''s'");
	REQUIRE(ref);
	REQUIRE(ref->type == TableReferenceType::TABLE_FUNCTION);
	auto &call = ref->Cast<TableFunctionRef>();
	// A mark SQL cannot set, so no call a user writes is taken for one.
	CHECK(call.bind_info);
	auto &function = call.function->Cast<FunctionExpression>();
	CHECK(function.FunctionName().GetIdentifierName() == "redshift_query");
	auto &arguments = function.GetArguments();
	REQUIRE(arguments.size() == 2);
	CHECK(arguments[0].GetExpression().ToString() == "'rs'");
	CHECK(arguments[1].GetExpression().ToString() == "'SELECT ''it''''s'''");
	// What the user writes as the same call is not CONNECT's: its refusal in a DuckDB transaction is
	// redshift_query's, and it leaves the DuckDB connection as it was.
	Run(con, "BEGIN");
	CHECK(StringUtil::StartsWith(ErrorOf(con, "SELECT * FROM redshift_query('rs', 'rows=1')"),
	                             "Binder Error: redshift_query cannot run while a DuckDB transaction is open"));
	Run(con, "ROLLBACK");
}

TEST_CASE("a statement CONNECT routes to a replaced handle is not run on the one attached now", "[connect]") {
	Fixture f;
	Connection connected(f.db);
	Connection detaching(f.db);
	Run(connected, "CONNECT rs");
	bool session = false;
	SECTION("before any statement") {
	}
	SECTION("after a statement, whose session may have held state") {
		session = true;
		Run(connected, "rows=1 cols=1");
	}
	HeldStream held(f.db);
	const auto executed = f.lines->Executed();
	Run(detaching, redshift_test::AttachOrReplace("rs"));
	// The earlier handle is alive, so DuckDB routes the statement to it, and the call names the handle
	// attached now. Refused before any transport call.
	const auto opened = f.lines->Opened();
	const auto described = f.lines->Described();
	const auto error = ErrorOf(connected, "rows=1 cols=1");
	CHECK(f.lines->Opened() == opened);
	CHECK(f.lines->Described() == described);
	if (session) {
		CHECK(error == "Binder Error: CONNECT: attach handle \"rs\" was detached or replaced by another DuckDB "
		               "connection, or an ATTACH under its name was rolled back, and a new handle is attached under "
		               "the name now. This DuckDB connection's Redshift session on the earlier handle may have held "
		               "state, which is now lost, so this statement was not run on Redshift. Run DISCONNECT, then "
		               "CONNECT to the handle attached now to start a new, empty Redshift session on it.");
	} else {
		CHECK(error == "Binder Error: CONNECT: attach handle \"rs\" was detached or replaced, or an ATTACH under "
		               "its name was rolled back, since this DuckDB connection CONNECT-ed to it, and the handle "
		               "attached under the name now is another, so this statement was not run on Redshift. Run "
		               "DISCONNECT, then CONNECT to the handle attached now.");
	}
	// Refused every time, not only once, until DISCONNECT.
	CHECK(StringUtil::Contains(ErrorOf(connected, "rows=1 cols=1"), "Run DISCONNECT, then CONNECT"));
	CHECK(f.lines->Executed() == executed);
	Run(connected, "DISCONNECT");
	Run(connected, "CONNECT rs");
	CHECK(ErrorOf(connected, "rows=1 cols=1").empty());
	CHECK(f.lines->Executed() == executed + 1);
}

TEST_CASE("while CONNECT-ed, a prepared statement with parameter values is DuckDB's refusal", "[connect]") {
	Fixture f;
	Connection con(f.db);
	auto with_parameter = con.Prepare("SELECT ?::INTEGER");
	REQUIRE_FALSE(with_parameter->HasError());
	auto without = con.Prepare("SELECT 42");
	REQUIRE_FALSE(without->HasError());
	Run(con, "CONNECT rs");
	const auto described = f.lines->Described();
	auto refused = with_parameter->Execute(42);
	REQUIRE(refused->HasError());
	CHECK(StringUtil::Contains(refused->GetError(), "Parameterized prepared statements cannot be executed while "
	                                                "CONNECT-ed"));
	CHECK(f.lines->Described() == described);
	// One without parameters goes to Redshift as its text.
	auto routed = without->Execute();
	REQUIRE_FALSE(routed->HasError());
	CHECK(routed->Fetch()->GetValue(0, 0) == Value("SELECT 42"));
	Run(con, "DISCONNECT");
}

TEST_CASE("an open that fails while CONNECT-ed says to DISCONNECT before fixing it, and keeps the open's error",
          "[connect]") {
	Fixture f;
	Connection con(f.db);
	Run(con, "CONNECT rs");
	const string refused = "redshift mock test: the AWS credentials from AWS secret 'creds' were refused";
	const unordered_map<string, string> info {{"sqlstate", "HY000"}, {"sqlstates", "HY000"}};
	FailNextMockOpen(refused, ExceptionType::IO, info);
	auto result = con.Query("SELECT 1");
	REQUIRE(result->HasError());
	CHECK(result->GetErrorType() == ExceptionType::IO);
	// The open's extra_info, its SQLSTATEs, survives the wrap.
	const auto wrapped_info = result->GetErrorObject().ExtraInfo();
	CHECK(wrapped_info.at("sqlstate") == "HY000");
	CHECK(wrapped_info.at("sqlstates") == "HY000");
	CHECK(
	    result->GetError() ==
	    "IO Error: CONNECT: attach handle \"rs\" could not open its Redshift connection, so the statement was not run "
	    "on Redshift. Run DISCONNECT before any DuckDB statement that fixes the cause, such as one that recreates a "
	    "secret: while CONNECT-ed, DuckDB sends every statement but DISCONNECT to Redshift. The client reported: " +
	        refused);
	CHECK(f.lines->Executed() == 0);
	// The next statement opens again, and runs; redshift_query's own open failure keeps the client's words.
	Run(con, "SELECT 1");
	Run(con, "DISCONNECT");
	Connection other(f.db);
	FailNextMockOpen(refused);
	CHECK(StringUtil::StartsWith(ErrorOf(other, "SELECT * FROM redshift_query('rs', 'rows=1')"),
	                             "IO Error: " + refused + "\n"));
}

TEST_CASE("an interrupt during an open while CONNECT-ed passes through, not in CONNECT's words", "[connect]") {
	Fixture f;
	Connection con(f.db);
	Run(con, "CONNECT rs");
	const string interrupted = "redshift mock test: the open was interrupted";
	FailNextMockOpen(interrupted, ExceptionType::INTERRUPT);
	auto result = con.Query("SELECT 1");
	REQUIRE(result->HasError());
	CHECK(result->GetErrorType() == ExceptionType::INTERRUPT);
	CHECK(result->GetError().find("CONNECT: attach handle") == string::npos);
	CHECK(result->GetError().find(interrupted) != string::npos);
	Run(con, "SELECT 1");
	Run(con, "DISCONNECT");
}

TEST_CASE("a fatal, internal or out-of-memory error during an open while CONNECT-ed passes through, not in "
          "CONNECT's words",
          "[connect]") {
	for (const auto type : {ExceptionType::FATAL, ExceptionType::INTERNAL, ExceptionType::OUT_OF_MEMORY}) {
		INFO(Exception::ExceptionTypeToString(type));
		Fixture f;
		Connection con(f.db);
		Run(con, "CONNECT rs");
		const string failed = "redshift mock test: the open failed inside the client";
		FailNextMockOpen(failed, type);
		auto result = con.Query("SELECT 1");
		REQUIRE(result->HasError());
		CHECK(result->GetErrorType() == type);
		CHECK(result->GetError().find("CONNECT: attach handle") == string::npos);
		CHECK(result->GetError().find(failed) != string::npos);
	}
}

TEST_CASE("a client-side prepare while CONNECT-ed runs the statement on Redshift at each execution", "[connect]") {
	// DuckDB's behaviour, pinned so a version bump shows a change: a prepare while CONNECT-ed keeps the
	// statement's text, which the hand-off routes at each execution, and runs nothing itself. The mock
	// logs no SQL, so the count is what shows each run.
	Fixture f;
	Connection con(f.db);
	Run(con, "CONNECT rs");
	const auto executed = f.lines->Executed();
	auto prepared = con.Prepare("SELECT 1");
	REQUIRE_FALSE(prepared->HasError());
	CHECK(f.lines->Executed() == executed);
	for (idx_t run = 1; run <= 2; run++) {
		auto result = prepared->Execute();
		REQUIRE_FALSE(result->HasError());
		// The mock answers with the text it was sent.
		CHECK(result->Fetch()->GetValue(0, 0) == Value("SELECT 1"));
		CHECK(f.lines->Executed() == executed + run);
	}
	Run(con, "DISCONNECT");
}

TEST_CASE("CONNECT is refused once a client API has turned autocommit off", "[connect]") {
	Fixture f;
	Connection con(f.db);
	con.SetAutoCommit(false);
	const auto error = ErrorOf(con, "CONNECT rs");
	CHECK(StringUtil::StartsWith(error, "Binder Error: CONNECT: cannot CONNECT to attach handle \"rs\" while a DuckDB "
	                                    "transaction is open"));
	CHECK_FALSE(con.context->IsConnected());
	con.Rollback();
	con.SetAutoCommit(true);
	Run(con, "CONNECT rs");
	Run(con, "DISCONNECT");
}

TEST_CASE("autocommit turned off after CONNECT: the gate keeps the CONNECT in the usable transaction", "[connect]") {
	Fixture f;
	Connection con(f.db);
	Run(con, "CONNECT rs");
	// A flag, not a statement, so nothing binds and the CONNECT refusal cannot see it.
	con.SetAutoCommit(false);
	auto statements = con.ExtractStatements("SELECT 'first'; SELECT 'second'");
	REQUIRE(statements.size() == 2);
	const auto described = f.lines->Described();
	auto refused = con.Query(std::move(statements[0]));
	REQUIRE(refused->HasError());
	CHECK(refused->GetError() ==
	      "Binder Error: CONNECT: this statement was not run on Redshift, because a DuckDB transaction is open and "
	      "DuckDB COMMIT or ROLLBACK does not control Redshift operations. The DuckDB transaction is still usable, and "
	      "this DuckDB connection is still CONNECT-ed to attach handle \"rs\". Run DISCONNECT, then COMMIT or "
	      "ROLLBACK, then CONNECT again outside a DuckDB transaction, and enable autocommit if your client disabled "
	      "it. If a Redshift transaction is already open on this handle, continue working on it once CONNECT-ed "
	      "again.");
	// DuckDB runs a routed statement outside the DuckDB transaction, so under the default policy too the
	// refusal aborts nothing, and the next statement typed for Redshift is refused, never run in DuckDB.
	CHECK_FALSE(ValidChecker::IsInvalidated(con.context->ActiveTransaction()));
	CHECK(con.context->IsConnected());
	auto later = con.Query(std::move(statements[1]));
	REQUIRE(later->HasError());
	CHECK(StringUtil::Contains(later->GetError(), "still CONNECT-ed"));
	CHECK(f.lines->Described() == described);
	Run(con, "DISCONNECT");
	con.Rollback();
	con.SetAutoCommit(true);
	CHECK(con.Query("SELECT 42")->Fetch()->GetValue(0, 0) == Value::INTEGER(42));
}

TEST_CASE("autocommit turned off after CONNECT: under SYNTACTIC_ERRORS_DO_NOT_INVALIDATE the gate keeps the CONNECT",
          "[connect]") {
	Fixture f;
	Connection con(f.db);
	// Set before CONNECT, since while CONNECT-ed a SET goes to Redshift. DuckDB keeps the policy on the
	// DuckDB connection's transaction context past the statement that set it, so the transaction
	// SetAutoCommit(false) opens later has it too.
	Run(con, "SET current_transaction_invalidation_policy = 'SYNTACTIC_ERRORS_DO_NOT_INVALIDATE'");
	Run(con, "CONNECT rs");
	con.SetAutoCommit(false);
	const auto described = f.lines->Described();
	CHECK(ErrorOf(con, "rows=1 cols=1") ==
	      "Binder Error: CONNECT: this statement was not run on Redshift, because a DuckDB transaction is open and "
	      "DuckDB COMMIT or ROLLBACK does not control Redshift operations. The DuckDB transaction is still usable, and "
	      "this DuckDB connection is still CONNECT-ed to attach handle \"rs\". Run DISCONNECT, then COMMIT or "
	      "ROLLBACK, then CONNECT again outside a DuckDB transaction, and enable autocommit if your client disabled "
	      "it. If a Redshift transaction is already open on this handle, continue working on it once CONNECT-ed "
	      "again.");
	// Still CONNECT-ed, so a statement typed for Redshift is refused, never run in DuckDB.
	CHECK(con.context->IsConnected());
	CHECK(StringUtil::Contains(ErrorOf(con, "SELECT 'meant for Redshift'"), "still CONNECT-ed"));
	CHECK(f.lines->Described() == described);
	Run(con, "DISCONNECT");
	con.Commit();
	con.SetAutoCommit(true);
}

TEST_CASE("a routed statement whose handle goes between bind and execution is refused in CONNECT's words",
          "[connect]") {
	Fixture f;
	Connection connected(f.db);
	Connection detaching(f.db);
	Run(connected, "CONNECT rs");
	const auto executed = f.lines->Executed();
	vector<string> removal {"DETACH rs"};
	string refusal =
	    "Executor Error: CONNECT: attach handle \"rs\" was detached, or an ATTACH under its name was rolled back, "
	    "after this statement was bound, so this statement was not run on Redshift. Run DISCONNECT, then attach "
	    "the handle again and CONNECT to it.";
	SECTION("DETACH") {
	}
	SECTION("ATTACH OR REPLACE") {
		removal = {redshift_test::AttachOrReplace("rs")};
		refusal = "Executor Error: CONNECT: attach handle \"rs\" was detached or replaced, or an ATTACH under its "
		          "name was rolled back, after this statement was bound, and another handle has the name now, so "
		          "this statement was not run on Redshift. Run DISCONNECT, then CONNECT to the handle attached now.";
	}
	SECTION("another database under the name") {
		removal = {"DETACH rs", "ATTACH ':memory:' AS rs"};
		refusal = "Executor Error: CONNECT: \"rs\" names a duckdb database now, not the Redshift handle this statement "
		          "was bound to, so this statement was not run on Redshift. Run DISCONNECT, then attach a Redshift "
		          "handle under another name and CONNECT to it.";
	}
	PauseHold hold;
	// Past the bind's pin request, to the execution's.
	hold.Arm(1);
	auto run = std::async(std::launch::async, [&]() { return ErrorOf(connected, "rows=1 cols=1"); });
	const bool held = hold.WaitUntilHeldFor(BOUND);
	for (auto &statement : removal) {
		CHECK(ErrorOf(detaching, statement).empty());
	}
	hold.Release();
	REQUIRE(run.wait_for(BOUND) == std::future_status::ready);
	CHECK(held);
	CHECK(run.get() == refusal);
	CHECK(f.lines->Executed() == executed);
}

TEST_CASE("a routed statement whose handle the name no longer reaches is refused in CONNECT's words", "[connect]") {
	const bool other_database = GENERATE(false, true);
	Fixture f;
	Connection connected(f.db);
	Connection detaching(f.db);
	Run(connected, "CONNECT rs");
	// After a statement, the session may have held state, and the refusal also says that state is lost.
	string lost = ".";
	SECTION("before any statement") {
	}
	SECTION("after a statement") {
		Run(connected, "rows=1 cols=1");
		lost = ". Another DuckDB connection detached or replaced the Redshift handle that had this name, or an "
		       "ATTACH under this name was rolled back, while this DuckDB connection had a Redshift session on it, "
		       "and that session may have held state, which is now lost.";
	}
	HeldStream held(f.db);
	const auto end = lost + " Run DISCONNECT, then attach a Redshift handle and CONNECT to it.";
	string expected = "Binder Error: CONNECT: no attached database is named \"rs\" now, so the handle this DuckDB "
	                  "connection is CONNECT-ed to has been detached, or an ATTACH under its name was rolled back" +
	                  end;
	Run(detaching, "DETACH rs");
	if (other_database) {
		Run(detaching, "ATTACH ':memory:' AS rs");
		expected = "Binder Error: CONNECT: \"rs\" is a duckdb database now, not the Redshift handle this DuckDB "
		           "connection is CONNECT-ed to" +
		           end;
	}
	const auto opened = f.lines->Opened();
	const auto described = f.lines->Described();
	CHECK(ErrorOf(connected, "rows=1 cols=1") == expected);
	CHECK(f.lines->Opened() == opened);
	CHECK(f.lines->Described() == described);
	Run(connected, "DISCONNECT");
}

TEST_CASE("an invalid described shape is reported in CONNECT's words", "[connect]") {
	RedshiftStatementShape shape;
	shape.returns_rows = true;
	auto thrown =
	    redshift_test::Capture([&] { RedshiftTypeMapping::ToOutputSchema(shape, RedshiftCaller("rs", true)); });
	CHECK(thrown.message == "CONNECT: the Redshift client described the statement on handle \"rs\" as returning rows "
	                        "with no columns, which is not a valid result shape. This is a fault in the client, not in "
	                        "the SQL. No Redshift statement was executed.");
}

TEST_CASE("the string form of CONNECT attaches nothing", "[connect]") {
	Fixture f;
	Connection con(f.db);
	const string options = " (TYPE redshift, REGION 'us-east-1', CREDENTIAL_PROVIDER aws_default_chain, "
	                       "WORKGROUP 'test-wg', ACCOUNT_ID '012345678901')";
	CHECK(StringUtil::Contains(ErrorOf(con, "CONNECT ''" + options), "CONNECT with no target is not yet implemented"));
	CHECK(StringUtil::Contains(ErrorOf(con, "CONNECT 'rs'" + options), "Invalid CONNECT: CONNECT to a string"));
	CHECK_FALSE(con.context->IsConnected());
	// DuckDB strips a `redshift:` prefix, leaving an empty path, so the handle's ATTACH refuses
	// any attach a CONNECT makes, inside a transaction too.
	const string refused = "Invalid CONNECT: CONNECT to a string or an external resource is not supported";
	CHECK(StringUtil::Contains(ErrorOf(con, "CONNECT 'redshift:'" + options), refused));
	con.BeginTransaction();
	CHECK(StringUtil::Contains(ErrorOf(con, "CONNECT 'redshift:'" + options), refused));
	con.Rollback();
	CHECK_FALSE(con.context->IsConnected());
	// duckdb_databases() leaves hidden databases out, so the database list is read directly.
	for (auto &db : f.db.instance->GetDatabaseManager().GetDatabases()) {
		INFO(db->GetName().GetIdentifierName());
		CHECK_FALSE(StringUtil::StartsWith(db->GetName().GetIdentifierName(), "__connect_"));
	}
}

TEST_CASE("while CONNECT-ed, the client API's transaction calls and a trailing comment go to Redshift", "[connect]") {
	Fixture f;
	Connection con(f.db);
	Run(con, "CONNECT rs");
	const auto executed = f.lines->Executed();
	con.BeginTransaction();
	con.Commit();
	CHECK(f.lines->Executed() == executed + 2);
	CHECK(con.IsAutoCommit());
	auto echoed = con.Query("SELECT 'x';  -- after the terminator");
	REQUIRE_FALSE(echoed->HasError());
	CHECK(echoed->Fetch()->GetValue(0, 0) == Value("SELECT 'x';  -- after the terminator"));
	Run(con, "DISCONNECT");
}

TEST_CASE("a routed statement in a transaction an earlier error aborted gets the gate's already-aborted words",
          "[connect]") {
	Fixture f;
	Connection con(f.db);
	Run(con, "CONNECT rs");
	con.SetAutoCommit(false);
	// DuckDB's own refusals while CONNECT-ed leave the transaction valid (the next case shows two), so
	// the earlier error is recorded here, in the transaction SetAutoCommit opened, as DuckDB records one.
	ValidChecker::Invalidate(con.context->ActiveTransaction(), "an earlier error");
	CHECK(ErrorOf(con, "rows=1 cols=1") ==
	      "Binder Error: CONNECT: this statement was not run on Redshift, because a DuckDB transaction is open and "
	      "DuckDB COMMIT or ROLLBACK does not control Redshift operations. This DuckDB connection is no longer "
	      "CONNECT-ed to attach handle \"rs\": while CONNECT-ed, ROLLBACK would go to Redshift. The DuckDB "
	      "transaction was already aborted by an earlier error. Run ROLLBACK (COMMIT would also roll it back, "
	      "without an error), then CONNECT again outside a DuckDB transaction, and enable autocommit if your client "
	      "disabled it. If a Redshift transaction is already open on this handle, continue working on it once "
	      "CONNECT-ed again.");
	CHECK_FALSE(con.context->IsConnected());
	con.Rollback();
	con.SetAutoCommit(true);
}

TEST_CASE("DuckDB runs DISCONNECT in an aborted transaction", "[connect]") {
	// DuckDB's behaviour, pinned so a version bump shows a change: its DISCONNECT needs no valid
	// transaction (Binder::Bind for DisconnectStatement), so it returns to DuckDB after an error.
	Fixture f;
	Connection con(f.db);
	Run(con, "CONNECT rs");
	con.SetAutoCommit(false);
	ValidChecker::Invalidate(con.context->ActiveTransaction(), "an earlier error");
	Run(con, "DISCONNECT");
	CHECK_FALSE(con.context->IsConnected());
	con.Rollback();
	con.SetAutoCommit(true);
}

TEST_CASE("DISCONNECT ends a CONNECT whose handle another connection detached in a transaction", "[connect]") {
	// DuckDB's behaviour, which never reaches the extension, pinned so that a version bump that changes
	// it shows: its refusal of a statement whose database is detached, and of one with parameter values,
	// leaves the transaction valid, so DISCONNECT returns to DuckDB and ROLLBACK then ends the transaction.
	Fixture f;
	Connection con(f.db);
	Connection other(f.db);
	auto with_parameter = con.Prepare("SELECT ?::INTEGER");
	REQUIRE_FALSE(with_parameter->HasError());
	// CONNECT inside a transaction is refused, so the transaction comes from the API, after CONNECT.
	Run(con, "CONNECT rs");
	con.SetAutoCommit(false);
	CHECK(StringUtil::Contains(with_parameter->Execute(42)->GetError(), "while CONNECT-ed"));
	Run(other, "DETACH rs");
	CHECK(StringUtil::Contains(ErrorOf(con, "SELECT 1"),
	                           "The connected database has been detached out from under this connection"));
	CHECK_FALSE(ValidChecker::IsInvalidated(con.context->ActiveTransaction()));
	Run(con, "DISCONNECT");
	CHECK_FALSE(con.context->IsConnected());
	con.Rollback();
	CHECK(con.IsAutoCommit());
	auto local = con.Query("SELECT 42");
	REQUIRE_FALSE(local->HasError());
	CHECK(local->Fetch()->GetValue(0, 0) == Value::INTEGER(42));
}

TEST_CASE("a CONNECT the C++ API prepared is refused when it is executed inside a DuckDB transaction", "[connect]") {
	Fixture f;
	Connection con(f.db);
	auto prepared = con.Prepare("CONNECT rs");
	INFO((prepared->HasError() ? prepared->GetError() : string("prepared")));
	REQUIRE_FALSE(prepared->HasError());
	con.BeginTransaction();
	string error;
	SECTION("executed") {
		error = prepared->Execute()->GetError();
	}
	SECTION("under EXPLAIN ANALYZE, which runs it") {
		error = ErrorOf(con, "EXPLAIN ANALYZE EXECUTE " + SQLIdentifier::ToString(prepared->GetName()));
	}
	SECTION("through a prepared EXECUTE of it") {
		con.Rollback();
		auto outer = con.Prepare("EXECUTE " + SQLIdentifier::ToString(prepared->GetName()));
		REQUIRE_FALSE(outer->HasError());
		con.BeginTransaction();
		error = outer->Execute()->GetError();
	}
	SECTION("prepared under debug_verify_vector, which wraps its plan") {
		con.Rollback();
		Run(con, "SET debug_verify_vector = 'dictionary_operator'");
		auto wrapped = con.Prepare("CONNECT rs");
		REQUIRE_FALSE(wrapped->HasError());
		con.BeginTransaction();
		error = wrapped->Execute()->GetError();
	}
	SECTION("through an EXPLAIN ANALYZE EXECUTE of it the C++ API prepared") {
		con.Rollback();
		auto outer = con.Prepare("EXPLAIN ANALYZE EXECUTE " + SQLIdentifier::ToString(prepared->GetName()));
		REQUIRE_FALSE(outer->HasError());
		con.BeginTransaction();
		error = outer->Execute()->GetError();
	}
	CHECK(StringUtil::StartsWith(error, "Binder Error: CONNECT: cannot CONNECT to attach handle \"rs\" while a DuckDB "
	                                    "transaction is open"));
	CHECK_FALSE(con.context->IsConnected());
	con.Rollback();
	// A plain EXPLAIN runs nothing, so inside a transaction it is not refused, and nothing is CONNECT-ed.
	con.BeginTransaction();
	CHECK(ErrorOf(con, "EXPLAIN EXECUTE " + SQLIdentifier::ToString(prepared->GetName())) == "");
	CHECK_FALSE(con.context->IsConnected());
	con.Rollback();
	// Outside a transaction it CONNECTs.
	REQUIRE_FALSE(prepared->Execute()->HasError());
	CHECK(con.context->IsConnected());
	Run(con, "DISCONNECT");
}

namespace {

//! Asks DuckDB to bind each prepared statement again when it is executed, as an extension may.
class RebindOnExecute : public ClientContextState {
public:
	RebindQueryInfo OnRebindPreparedStatement(ClientContext &, BindPreparedStatementCallbackInfo &,
	                                          RebindQueryInfo) override {
		return RebindQueryInfo::ATTEMPT_TO_REBIND;
	}
};

} // namespace

TEST_CASE("a CONNECT is refused in a DuckDB transaction when it is prepared, and when it is bound again", "[connect]") {
	Fixture f;
	Connection con(f.db);
	string error;
	SECTION("prepared inside the transaction") {
		con.BeginTransaction();
		error = con.Prepare("CONNECT rs")->GetError();
	}
	SECTION("prepared before it, and bound again when executed in it") {
		auto prepared = con.Prepare("CONNECT rs");
		REQUIRE_FALSE(prepared->HasError());
		con.context->registered_state->GetOrCreate<RebindOnExecute>("redshift_test_connect_rebind");
		con.BeginTransaction();
		error = prepared->Execute()->GetError();
	}
	CHECK(StringUtil::StartsWith(error, "Binder Error: CONNECT: cannot CONNECT to attach handle \"rs\" while a DuckDB "
	                                    "transaction is open"));
	CHECK_FALSE(con.context->IsConnected());
	con.Rollback();
}

TEST_CASE("prepared statements without a physical plan, or whose EXECUTE was bound again, run in a transaction",
          "[connect]") {
	Fixture f;
	Connection con(f.db);
	// DuckDB binds a statement with a parameter of unknown type again on each execution, so it keeps no
	// physical plan, and an EXECUTE of one with values holds the plan of that bind, not the prepared one.
	auto with_parameter = con.Prepare("SELECT $1");
	REQUIRE_FALSE(with_parameter->HasError());
	Run(con, "PREPARE p AS SELECT $1");
	auto execute_with_value = con.Prepare("EXECUTE p(1)");
	REQUIRE_FALSE(execute_with_value->HasError());
	con.BeginTransaction();
	CHECK(with_parameter->Execute(42)->Fetch()->GetValue(0, 0) == Value::INTEGER(42));
	CHECK(execute_with_value->Execute()->Fetch()->GetValue(0, 0) == Value::INTEGER(1));
	con.Rollback();
}

TEST_CASE("a prepared EXECUTE of a prepared CONNECT is refused whatever the inner name means now", "[connect]") {
	Fixture f;
	Connection con(f.db);
	auto inner = con.Prepare("CONNECT rs");
	REQUIRE_FALSE(inner->HasError());
	const auto name = SQLIdentifier::ToString(inner->GetName());
	auto outer = con.Prepare("EXECUTE " + name);
	REQUIRE_FALSE(outer->HasError());
	// DuckDB runs the CONNECT the outer statement was bound to, not what the name names now.
	SECTION("the inner name deallocated") {
		Run(con, "DEALLOCATE " + name);
	}
	SECTION("the inner statement destroyed") {
		inner.reset();
	}
	SECTION("the inner name prepared again") {
		Run(con, "PREPARE " + name + " AS SELECT 1");
	}
	con.BeginTransaction();
	CHECK(StringUtil::StartsWith(outer->Execute()->GetError(), "Binder Error: CONNECT: cannot CONNECT to attach "
	                                                           "handle \"rs\" while a DuckDB transaction is open"));
	CHECK_FALSE(con.context->IsConnected());
	con.Rollback();
	// Outside a transaction the outer statement still runs that CONNECT.
	REQUIRE_FALSE(outer->Execute()->HasError());
	CHECK(con.context->IsConnected());
	Run(con, "DISCONNECT");
}

TEST_CASE("a routed statement bound before autocommit went off is refused at execution, and the CONNECT kept",
          "[connect]") {
	Fixture f;
	Connection con(f.db);
	// One thread, so the submitted statement is bound now and runs only when fetched.
	Run(con, "SET threads = 1");
	Run(con, "CONNECT rs");
	auto submitted = con.Submit("rows=1 cols=1");
	REQUIRE_FALSE(submitted->HasError());
	const auto executed = f.lines->Executed();
	con.SetAutoCommit(false);
	auto fetched = redshift_test::Capture([&]() { submitted->Fetch(); });
	CHECK(StringUtil::Contains(
	    fetched.message,
	    "Executor Error: CONNECT: this statement was not run on Redshift, because a DuckDB transaction is open and "
	    "DuckDB COMMIT or ROLLBACK does not control Redshift operations. It was refused as the statement started to "
	    "run. The DuckDB transaction is still usable, and this DuckDB connection is still CONNECT-ed to attach handle "
	    "\"rs\". Run DISCONNECT, then COMMIT or ROLLBACK, then CONNECT again outside a DuckDB transaction, and enable "
	    "autocommit if your client disabled it."));
	CHECK_FALSE(ValidChecker::IsInvalidated(con.context->ActiveTransaction()));
	CHECK(con.context->IsConnected());
	CHECK(f.lines->Executed() == executed);
	Run(con, "DISCONNECT");
	con.Rollback();
	con.SetAutoCommit(true);
	CHECK(con.Query("SELECT 42")->Fetch()->GetValue(0, 0) == Value::INTEGER(42));
}

TEST_CASE("a routed statement fetched in a transaction an earlier error aborted is refused, and the CONNECT ended",
          "[connect]") {
	Fixture f;
	Connection con(f.db);
	// One thread, so the submitted statement is bound now and runs only when fetched.
	Run(con, "SET threads = 1");
	Run(con, "CONNECT rs");
	auto submitted = con.Submit("rows=1 cols=1");
	REQUIRE_FALSE(submitted->HasError());
	const auto executed = f.lines->Executed();
	con.SetAutoCommit(false);
	// Recorded as DuckDB records an error, as the routed statements' own errors do not abort it.
	ValidChecker::Invalidate(con.context->ActiveTransaction(), "an earlier error");
	auto fetched = redshift_test::Capture([&]() { submitted->Fetch(); });
	CHECK(StringUtil::Contains(
	    fetched.message,
	    "Executor Error: CONNECT: this statement was not run on Redshift, because a DuckDB transaction is open and "
	    "DuckDB COMMIT or ROLLBACK does not control Redshift operations. This DuckDB connection is no longer "
	    "CONNECT-ed to attach handle \"rs\": while CONNECT-ed, ROLLBACK would go to Redshift. The DuckDB "
	    "transaction was already aborted by an earlier error. Run ROLLBACK (COMMIT would also roll it back, "
	    "without an error), then CONNECT again outside a DuckDB transaction"));
	CHECK_FALSE(con.context->IsConnected());
	CHECK(f.lines->Executed() == executed);
	con.Rollback();
	con.SetAutoCommit(true);
}

TEST_CASE("redshift_allow_query_in_duckdb_transaction does not lift CONNECT's refusals", "[connect]") {
	Fixture f;
	Connection con(f.db);
	// Set before CONNECT, since while CONNECT-ed a SET goes to Redshift.
	Run(con, "SET redshift_allow_query_in_duckdb_transaction = true");
	SECTION("CONNECT inside a transaction") {
		con.SetAutoCommit(false);
		CHECK(StringUtil::StartsWith(ErrorOf(con, "CONNECT rs"), "Binder Error: CONNECT: cannot CONNECT to attach "
		                                                         "handle \"rs\" while a DuckDB transaction is open"));
		CHECK_FALSE(con.context->IsConnected());
		con.Rollback();
	}
	SECTION("a routed statement bound before autocommit went off, refused at execution") {
		// One thread, so the submitted statement is bound now and runs only when fetched.
		Run(con, "SET threads = 1");
		Run(con, "CONNECT rs");
		auto submitted = con.Submit("rows=1 cols=1");
		REQUIRE_FALSE(submitted->HasError());
		const auto executed = f.lines->Executed();
		con.SetAutoCommit(false);
		auto fetched = redshift_test::Capture([&]() { submitted->Fetch(); });
		CHECK(StringUtil::Contains(fetched.message, "It was refused as the statement started to run"));
		CHECK(f.lines->Executed() == executed);
		CHECK(con.context->IsConnected());
		Run(con, "DISCONNECT");
		con.Rollback();
	}
	SECTION("a routed statement after autocommit went off") {
		Run(con, "CONNECT rs");
		con.SetAutoCommit(false);
		const auto described = f.lines->Described();
		CHECK(StringUtil::Contains(ErrorOf(con, "rows=1 cols=1"),
		                           "this DuckDB connection is still CONNECT-ed to attach handle \"rs\""));
		CHECK(f.lines->Described() == described);
		CHECK(con.context->IsConnected());
		Run(con, "DISCONNECT");
		con.Rollback();
	}
	con.SetAutoCommit(true);
}

TEST_CASE("a routed statement's execution backstops are worded for CONNECT", "[connect]") {
	// Each refusal ends with the routed call alone: a routed statement holds no other call that could
	// have run.
	Fixture f;
	SECTION("a scan the fence never checked") {
		OptimizerExtension rebind;
		rebind.optimize_function = [](OptimizerExtensionInput &input, unique_ptr<LogicalOperator> &plan) {
			RebindRoutedScans(input.context, *plan);
		};
		OptimizerExtension::Register(DBConfig::GetConfig(*f.db.instance), std::move(rebind));
		Connection con(f.db);
		Run(con, "CONNECT rs");
		const auto executed = f.lines->Executed();
		CHECK(ErrorOf(con, "rows=1 cols=1") ==
		      "Executor Error: CONNECT: the call on attach handle \"rs\" was not checked for a second use of the "
		      "handle, so it was not run. Something, such as another extension, changed the statement's plan after "
		      "redshift_query checked it. Report this as a fault in the Redshift extension or in how it works with "
		      "the other extensions loaded.");
		CHECK(f.lines->Executed() == executed);
	}
	SECTION("another result open on the pair") {
		// Only a defect reaches this, so the test holds the other result's use itself, between the
		// statement's bind and its execution: one thread, so the submitted statement runs when fetched.
		Connection con(f.db);
		Run(con, "SET threads = 1");
		Run(con, "CONNECT rs");
		auto submitted = con.Submit("rows=1 cols=1");
		REQUIRE_FALSE(submitted->HasError());
		const auto executed = f.lines->Executed();
		auto db = DatabaseManager::Get(*con.context).GetDatabase(Identifier("rs"));
		REQUIRE(db);
		auto held = RedshiftConnectionPin::TryPin(*con.context, std::move(db));
		REQUIRE(held);
		REQUIRE(held->BeginUse(RedshiftConnectionUse::RESULT));
		auto fetched = redshift_test::Capture([&]() { submitted->Fetch(); });
		CHECK(StringUtil::Contains(fetched.message,
		                           "Executor Error: CONNECT: attach handle \"rs\" already has a Redshift statement "
		                           "running on this DuckDB connection, so this statement was not run. Report this as a "
		                           "fault in the Redshift extension."));
		CHECK(f.lines->Executed() == executed);
		held.reset();
		Run(con, "DISCONNECT");
	}
}

TEST_CASE("while CONNECT-ed, an unusable session fails its statement in CONNECT's words, the next statement opens a "
          "new session, and DISCONNECT still works",
          "[connect]") {
	Fixture f;
	Connection con(f.db);
	Run(con, "CONNECT rs");
	Run(con, "rows=1");
	const string lead =
	    "Binder Error: CONNECT: Redshift session error on attach handle \"rs\": the session cannot be used any more. "
	    "The extension discarded it; the next statement on \"rs\" opens a new session. ";
	const string ending = "The statement was not run on Redshift. The client reported: Redshift rejected the "
	                      "statement: Internal_Service_Error\nSQLSTATE: XX000";
	// A session that ran a statement may have held state, which the message says is gone.
	const string expected = lead +
	                        "Anything that lived in the old session is gone: temporary tables, SET values and any "
	                        "open transaction. " +
	                        ending;
	auto result = con.Query("error=Internal_Service_Error sqlstates=XX000 unusable");
	REQUIRE(result->HasError());
	CHECK(result->GetErrorType() == ExceptionType::BINDER);
	CHECK(result->GetError() == expected);
	CHECK(f.lines->Opened() == 1);
	Run(con, "rows=1");
	CHECK(f.lines->Opened() == 2);
	// Refused at Execute's prepare, the same words: the routed statement is the only call in its DuckDB statement.
	auto at_prepare = con.Query("error=Internal_Service_Error error_at=execute sqlstates=XX000 unusable");
	REQUIRE(at_prepare->HasError());
	CHECK(at_prepare->GetError() == expected);
	// The next statement's new session ran nothing before its bind was refused, so it lost nothing, and the message
	// leaves out what was lost.
	auto on_new = con.Query("error=Internal_Service_Error sqlstates=XX000 unusable");
	REQUIRE(on_new->HasError());
	CHECK(on_new->GetError() == lead + ending);
	CHECK(f.lines->Opened() == 3);
	Run(con, "rows=1");
	CHECK(f.lines->Opened() == 4);
	Run(con, "DISCONNECT");
	CHECK_FALSE(con.context->IsConnected());
	auto local = con.Query("SELECT 42");
	REQUIRE_FALSE(local->HasError());
	CHECK(local->Fetch()->GetValue(0, 0).GetValue<int32_t>() == 42);
}

TEST_CASE("while CONNECT-ed, each statement after a failed open tries the open again, the CLI prompt's query "
          "included, and DISCONNECT returns to DuckDB",
          "[connect]") {
	Fixture f;
	Connection con(f.db);
	Run(con, "CONNECT rs");
	const string unreachable = "redshift mock test: could not reach Redshift";
	// DuckDB's own statements and the CLI's prompt query are sent to Redshift too, so each one opens, and each
	// fails with the open's error; FailNextMockOpen fails only the open that comes next, so each is a new attempt.
	for (const auto &sql :
	     {string(DUCKDB_SHELL_PROMPT_QUERY), string("USE memory"), string("SELECT count(*) FROM duckdb_logs()")}) {
		INFO(sql);
		FailNextMockOpen(unreachable);
		auto result = con.Query(sql);
		REQUIRE(result->HasError());
		CHECK(StringUtil::StartsWith(result->GetError(), "IO Error: CONNECT: attach handle \"rs\" could not open "
		                                                 "its Redshift connection"));
		CHECK(StringUtil::EndsWith(result->GetError(), "The client reported: " + unreachable));
	}
	// One open attempt for each statement, none of them a failure kept from an earlier one.
	CHECK(f.lines->CountStartingWith("redshift mock: could not open a connection to ") == 3);
	CHECK(f.lines->Opened() == 0);
	Run(con, "DISCONNECT");
	CHECK_FALSE(con.context->IsConnected());
	Run(con, "USE memory");
	auto local = con.Query("SELECT 42");
	REQUIRE_FALSE(local->HasError());
	CHECK(local->Fetch()->GetValue(0, 0).GetValue<int32_t>() == 42);
	CHECK(f.lines->Opened() == 0);
}
