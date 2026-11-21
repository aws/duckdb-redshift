// What redshift_query does when a transport call fails, where SQL tests cannot look: the
// extra_info a refusal, a statement failure or a lost connection keeps; the 25P02 guidance; a
// replacement connection that cannot open; an open the credentials are refused for; a log storage
// that refuses lines; the order of the lines a Describe recovery logs; a possible-state-loss report
// DuckDB does not show, through a second planning pass, the relation API, or an error it drops; and
// streaming consumers. Lines are read from a log storage the test owns, so their order and counts
// outlive the database.

#include "catch.hpp"

#include "duckdb.hpp"
#include "duckdb/common/exception/binder_exception.hpp"
#include "duckdb/common/string_util.hpp"
#include "duckdb/catalog/catalog.hpp"
#include "duckdb/main/database_manager.hpp"
#include "duckdb/main/client_context_state.hpp"
#include "duckdb/main/extension/extension_loader.hpp"
#include "duckdb/main/query_result_stream.hpp"
#include "duckdb/parser/parser.hpp"
#include "duckdb/parser/statement/select_statement.hpp"
#include "duckdb/planner/logical_operator.hpp"
#include "mock_transport.hpp"
#include "redshift_messages.hpp"
#include "storage/redshift_catalog.hpp"
#include "storage/redshift_connection_calls.hpp"
#include "storage/redshift_connection_ownership.hpp"
#include "test_log_storage.hpp"
#include "test_util.hpp"

using namespace duckdb;
using redshift_test::Capture;
using redshift_test::Run;
using redshift_test::Thrown;
using Fixture = redshift_test::LoggedDatabase;

namespace {

string Query(const string &directive) {
	return "SELECT * FROM redshift_query('rs', '" + directive + "')";
}

//! What `sql`, which must fail, fails with.
Thrown FailureOf(Connection &con, const string &sql) {
	auto result = con.Query(sql);
	INFO(sql);
	REQUIRE(result->HasError());
	return redshift_test::FailureOf(*result);
}

//! The guidance for `handle`, in its fixed wording.
string Guidance(const string &handle) {
	return "The Redshift transaction on attach handle \"" + handle +
	       "\" is aborted, so Redshift refuses every statement until it ends. Run redshift_query('" + handle +
	       "', 'ROLLBACK') to end it. Work done in that transaction since the error is lost; running COMMIT instead "
	       "would also roll it back.";
}

//! The position of the first of `lines` that starts with `prefix`, at or after `from`; lines.size()
//! if there is none.
idx_t Find(const vector<string> &lines, const string &prefix, idx_t from = 0) {
	for (idx_t i = from; i < lines.size(); i++) {
		if (StringUtil::StartsWith(lines[i], prefix)) {
			return i;
		}
	}
	return lines.size();
}

//! Puts the pair's Redshift transaction into the aborted state, as a user's BEGIN and a failing
//! statement do. The mock keeps no transaction, so this is for the sequence's sake: each 25P02 that
//! follows is one the real client could return.
void AbortTransaction(Connection &con) {
	Run(con, Query("BEGIN"));
	CHECK(FailureOf(con, Query("fail=execute sqlstates=22012")).type == ExceptionType::EXECUTOR);
}

//! Answers every planning error by asking DuckDB to plan the statement again, as a state another
//! extension registers may (ClientContextState::OnPlanningError). Given a query, it first replaces
//! the statement with that query, once, as such a state may too.
class RebindOnPlanningError : public ClientContextState {
public:
	explicit RebindOnPlanningError(string rewrite_p = string()) : rewrite(std::move(rewrite_p)) {
	}
	bool CanRequestRebind() override {
		return true;
	}
	RebindQueryInfo OnPlanningError(ClientContext &context, SQLStatement &statement, ErrorData &) override {
		rebinds++;
		if (!rewrite.empty()) {
			Parser parser(context);
			parser.ParseQuery(rewrite);
			statement.Cast<SelectStatement>().node = std::move(parser.statements[0]->Cast<SelectStatement>().node);
			rewrite.clear();
		}
		return RebindQueryInfo::ATTEMPT_TO_REBIND;
	}
	string rewrite;
	idx_t rebinds = 0;
};

//! swallow_redshift_call(handle, sql, then): inside a query, runs `sql` on the handle's session through
//! RedshiftConnectionCalls, reading its whole result, and swallows the failure, as DuckDB drops every error of
//! a query but the first. Then, unless `then` is empty, runs `then` on the same session, with the
//! first result still held. Returns the two failures' messages, NULL for none.
class SwallowRedshiftCall {
public:
	static void Register(DatabaseInstance &db) {
		ExtensionLoader loader(db, "redshift_test_swallow");
		loader.RegisterFunction(TableFunction("swallow_redshift_call",
		                                      {LogicalType::VARCHAR, LogicalType::VARCHAR, LogicalType::VARCHAR}, Scan,
		                                      Bind, Init));
	}

private:
	struct Data : public TableFunctionData {
		string handle;
		string sql;
		string then;
	};
	struct State : public GlobalTableFunctionState {
		Value first;
		Value second;
		bool emitted = false;
	};

	static unique_ptr<FunctionData> Bind(ClientContext &, TableFunctionBindInput &input, vector<LogicalType> &types,
	                                     vector<Identifier> &names) {
		auto data = make_uniq<Data>();
		data->handle = StringValue::Get(input.inputs[0]);
		data->sql = StringValue::Get(input.inputs[1]);
		data->then = StringValue::Get(input.inputs[2]);
		types = {LogicalType::VARCHAR, LogicalType::VARCHAR};
		names = {Identifier("first"), Identifier("second")};
		return std::move(data);
	}

	static Value Swallow(const std::function<void()> &call) {
		try {
			call();
			return Value();
		} catch (std::exception &ex) {
			return Value(ErrorData(ex).RawMessage());
		}
	}

	static unique_ptr<GlobalTableFunctionState> Init(ClientContext &context, TableFunctionInitInput &input) {
		auto &data = input.bind_data->Cast<Data>();
		// Pinned for the calls below, as redshift_query's bind and execution pin their pair.
		auto pin =
		    RedshiftConnectionPin::TryPin(context, DatabaseManager::Get(context).GetDatabase(Identifier(data.handle)));
		if (!pin) {
			throw InternalException("the handle was detached during the test");
		}
		RedshiftConnectionCalls calls(context, pin->Catalog(), RedshiftCaller(data.handle));
		auto state = make_uniq<State>();
		// Kept to the end: a result NextBatch failed to destroy would still be open for `then`.
		unique_ptr<RedshiftResult> result;
		RedshiftBatch batch;
		// NextBatch takes an empty batch, as the scan passes it.
		auto drain = [&](unique_ptr<RedshiftResult> &drained) {
			do {
				batch.rows.clear();
			} while (calls.NextBatch(drained, batch));
		};
		state->first = Swallow([&]() {
			result = calls.Execute(data.sql, vector<string>(), MockDescribedShape(data.sql));
			drain(result);
		});
		if (!data.then.empty()) {
			state->second = Swallow([&]() {
				auto next = calls.Execute(data.then, vector<string>(), MockDescribedShape(data.then));
				drain(next);
			});
		}
		return std::move(state);
	}

	static void Scan(ClientContext &, TableFunctionInput &input, DataChunk &output) {
		auto &state = input.global_state->Cast<State>();
		if (state.emitted) {
			return;
		}
		// As the scan writes its rows: Reset, write at the index, then publish the count.
		output.Reset();
		output.data[0].SetValue(0, state.first);
		output.data[1].SetValue(0, state.second);
		output.SetChildCardinality(1);
		state.emitted = true;
	}
};

//! What a query run through swallow_redshift_call returns: its row's two columns.
std::pair<Value, Value> Swallowed(Connection &con, const string &sql, const string &then = string()) {
	auto result = con.Query("SELECT * FROM swallow_redshift_call('rs', '" + sql + "', '" + then + "')");
	INFO(sql);
	REQUIRE_FALSE(result->HasError());
	auto chunk = result->Fetch();
	REQUIRE(chunk);
	return {chunk->GetValue(0, 0), chunk->GetValue(1, 0)};
}

const string LOSS_REPORT = "redshift_query: the Redshift connection of attach handle \"rs\" was lost while describing "
                           "the statement, so the statement was not run on Redshift. The session it carried may have "
                           "held state";

const string NOTICE = "redshift_query: the Redshift connection of attach handle \"rs\" was lost during Describe";
const string OUTLIVED = "redshift mock: a result outlived its connection";
const string TIMINGS = "redshift_query: Describe lost the Redshift connection of attach handle \"rs\"; bind timings:";
const string OPENED = "redshift mock: opened a connection to ";
const string COULD_NOT_OPEN = "redshift mock: could not open a connection to ";

} // namespace

TEST_CASE("a 25P02 refusal at bind or at execution keeps its type, extra_info and message, and gains the guidance",
          "[failures]") {
	Fixture f;
	Connection con(f.db);
	const string refused = "Redshift rejected the statement: aborted";

	// At execution, the prepare inside Execute is refused: the transaction aborted after the PREPARE
	// described the statement, and before its EXECUTE.
	Run(con, Query("BEGIN"));
	Run(con, "PREPARE aborted_since AS " + Query("error=aborted error_at=execute sqlstates=25P02"));
	CHECK(FailureOf(con, Query("fail=execute sqlstates=22012")).type == ExceptionType::EXECUTOR);
	auto at_execute = FailureOf(con, "EXECUTE aborted_since");
	// At bind, Describe's prepare is refused.
	auto at_bind = FailureOf(con, Query("error=aborted sqlstates=25P02"));
	CHECK(at_bind.type == ExceptionType::BINDER);
	CHECK(at_bind.message == refused + "\nSQLSTATE: 25P02\n" + Guidance("rs"));
	CHECK(at_execute.type == ExceptionType::BINDER);
	CHECK(at_execute.message == refused + "\nSQLSTATE: 25P02\n" + Guidance("rs"));
	for (auto *thrown : {&at_bind, &at_execute}) {
		CHECK(thrown->Extra("sqlstate") == "25P02");
		CHECK(thrown->Extra("sqlstates") == "25P02");
	}
	// None lost the session: the ROLLBACK the guidance names, and the next call, run on it.
	Run(con, Query("ROLLBACK"));
	Run(con, Query("rows=1"));
	CHECK(f.lines->Opened() == 1);
	CHECK(f.lines->Closed() == 0);
}

TEST_CASE("a statement failure carrying 25P02 also keeps its type and extra_info, and gains the guidance",
          "[failures]") {
	// The real client raises 25P02 only as a refusal, so this failure is made here rather than by the mock.
	const unordered_map<string, string> extra_info {{"sqlstate", "25P02"}, {"sqlstates", "HY000,25P02"}};
	const ErrorData failure(Exception(extra_info, ExceptionType::EXECUTOR, "the statement failed\nSQLSTATE: 25P02"));
	auto thrown = Capture([&]() { ThrowIfTransactionAborted(failure, RedshiftCaller("rs")); });
	CHECK(thrown.type == ExceptionType::EXECUTOR);
	CHECK(thrown.Extra("sqlstate") == "25P02");
	CHECK(thrown.Extra("sqlstates") == "HY000,25P02");
	CHECK(thrown.message == "the statement failed\nSQLSTATE: 25P02\n" + Guidance("rs"));
	// Another state, or none, returns without throwing.
	const ErrorData cancelled(
	    Exception(unordered_map<string, string> {{"sqlstate", "57014"}}, ExceptionType::EXECUTOR, "cancelled"));
	CHECK_NOTHROW(ThrowIfTransactionAborted(cancelled, RedshiftCaller("rs")));
	CHECK_NOTHROW(ThrowIfTransactionAborted(ErrorData(ExecutorException("no state")), RedshiftCaller("rs")));
}

TEST_CASE("the shape refusal keeps its type and extra_info, and is recognized by redshift_error alone", "[failures]") {
	const string message = "redshift_query: the statement on attach handle \"rs\" now returns different columns "
	                       "than when it was bound, so this call was not run on Redshift, but other calls in the same "
	                       "DuckDB statement may have run; check their effects on Redshift before running the "
	                       "statement again. If it is a prepared statement, prepare it again. The client reported: ";
	const unordered_map<string, string> extra_info {{"redshift_error", "shape_changed"}, {"detail", "column 1"}};
	auto thrown = Capture(
	    [&]() { ThrowIfShapeChanged(ErrorData(BinderException(extra_info, "changed")), RedshiftCaller("rs")); });
	CHECK(thrown.type == ExceptionType::BINDER);
	CHECK(thrown.message == message + "changed");
	CHECK(thrown.Extra("redshift_error") == "shape_changed");
	CHECK(thrown.Extra("detail") == "column 1");
	CHECK(thrown.Extra("sqlstate") == "<none>");
	// The field alone decides: another type with it is refused the same way, keeping that type.
	thrown = Capture([&]() {
		ThrowIfShapeChanged(ErrorData(Exception(extra_info, ExceptionType::EXECUTOR, "changed")), RedshiftCaller("rs"));
	});
	CHECK(thrown.type == ExceptionType::EXECUTOR);
	CHECK(thrown.message == message + "changed");
	// A call CONNECT made gets CONNECT's words, and the same type and extra_info.
	thrown = Capture(
	    [&]() { ThrowIfShapeChanged(ErrorData(BinderException(extra_info, "changed")), RedshiftCaller("rs", true)); });
	CHECK(thrown.type == ExceptionType::BINDER);
	CHECK(thrown.message == "CONNECT: the statement on attach handle \"rs\" now returns different columns than when "
	                        "it was bound, so it was not run on Redshift. Run it again. The client reported: changed");
	CHECK(thrown.Extra("redshift_error") == "shape_changed");
	CHECK(thrown.Extra("detail") == "column 1");
	// Without it, or with another value, the error is left to the caller, whatever its message says.
	CHECK_NOTHROW(ThrowIfShapeChanged(ErrorData(BinderException("shape_changed")), RedshiftCaller("rs")));
	CHECK_NOTHROW(ThrowIfShapeChanged(
	    ErrorData(BinderException(unordered_map<string, string> {{"redshift_error", "other"}}, "x")),
	    RedshiftCaller("rs")));
	CHECK_NOTHROW(ThrowIfShapeChanged(
	    ErrorData(BinderException(unordered_map<string, string> {{"sqlstate", "shape_changed"}}, "x")),
	    RedshiftCaller("rs")));
}

TEST_CASE("only the structured sqlstate 25P02 gets the guidance", "[failures]") {
	Fixture f;
	Connection con(f.db);

	// In a later record, after a generic one, as the classifier sets sqlstate.
	AbortTransaction(con);
	auto later = FailureOf(con, Query("error=aborted sqlstates=HY000,25P02"));
	CHECK(later.Extra("sqlstate") == "25P02");
	CHECK(later.Extra("sqlstates") == "HY000,25P02");
	CHECK(StringUtil::EndsWith(later.message, "\n" + Guidance("rs")));
	Run(con, Query("ROLLBACK"));

	// After a warning record, which the classifier counts as HY000.
	AbortTransaction(con);
	auto after_warning = FailureOf(con, Query("error=aborted sqlstates=01000,25P02"));
	CHECK(after_warning.Extra("sqlstate") == "25P02");
	CHECK(StringUtil::EndsWith(after_warning.message, "\n" + Guidance("rs")));
	Run(con, Query("ROLLBACK"));

	// Any other state is passed through unchanged: another class, another state of class 25, a message
	// that mentions 25P02, and 25P02 in a record the classifier did not act on.
	const std::pair<const char *, const char *> unchanged[] = {
	    {"error=aborted sqlstates=42601", "Redshift rejected the statement: aborted\nSQLSTATE: 42601"},
	    {"error=aborted sqlstates=25P01", "Redshift rejected the statement: aborted\nSQLSTATE: 25P01"},
	    {"error=current_transaction_is_aborted_25P02 sqlstates=42601",
	     "Redshift rejected the statement: current_transaction_is_aborted_25P02\nSQLSTATE: 42601"},
	    {"error=aborted sqlstates=42601,25P02", "Redshift rejected the statement: aborted\nSQLSTATE: 42601"},
	};
	for (auto &c : unchanged) {
		INFO(c.first);
		auto thrown = FailureOf(con, Query(c.first));
		CHECK(thrown.type == ExceptionType::BINDER);
		CHECK(thrown.message == c.second);
	}

	// No SQLSTATE at all: unchanged.
	auto none = FailureOf(con, Query("error=aborted"));
	CHECK(none.Extra("sqlstate") == "<none>");
	CHECK(none.message == "Redshift rejected the statement: aborted");
}

TEST_CASE("a statement failure passes through unchanged and keeps the session", "[failures]") {
	Fixture f;
	Connection con(f.db);
	auto at_execute = FailureOf(con, Query("fail=execute sqlstates=22012 rows=3"));
	CHECK(at_execute.type == ExceptionType::EXECUTOR);
	CHECK(at_execute.Extra("sqlstate") == "22012");
	CHECK(at_execute.message == "redshift mock: the statement failed while it ran\nSQLSTATE: 22012");

	auto at_fetch = FailureOf(con, Query("fail=fetch:2 sqlstates=HY000,57014 rows=5"));
	CHECK(at_fetch.type == ExceptionType::EXECUTOR);
	CHECK(at_fetch.Extra("sqlstate") == "57014");
	CHECK(at_fetch.Extra("sqlstates") == "HY000,57014");
	CHECK(at_fetch.message == "redshift mock: the statement failed while it ran\nSQLSTATE: 57014");

	// Each ran once, and the pair still has its first connection, with no result left open on it.
	CHECK(f.lines->Executed() == 2);
	Run(con, Query("rows=1"));
	CHECK(f.lines->Opened() == 1);
	CHECK(f.lines->Closed() == 0);
}

TEST_CASE("a lost connection's error keeps the client's type and extra_info", "[failures]") {
	Fixture f;
	Connection con(f.db);
	Run(con, Query("rows=1"));
	auto lost = FailureOf(con, Query("lose=execute sqlstates=08006 rows=1"));
	CHECK(lost.type == ExceptionType::IO);
	CHECK(lost.Extra("sqlstate") == "08006");
	CHECK(lost.Extra("sqlstates") == "08006");
	CHECK(StringUtil::StartsWith(lost.message, "redshift_query: the Redshift connection of attach handle \"rs\" was "
	                                           "lost while executing the statement, and the statement was not "
	                                           "retried."));
	CHECK(StringUtil::EndsWith(lost.message, " The client reported: redshift mock: lost the connection to " +
	                                             redshift_test::MockTarget() +
	                                             " after sending the statement\nSQLSTATE: 08006"));
	CHECK(f.lines->Closed() == 1);

	// A lost connection gets no aborted-transaction guidance, whatever its SQLSTATE: its session, and
	// the transaction with it, is gone. It is still discarded, and its SQLSTATE kept. Redshift gives
	// 25P02 when it refuses to prepare a statement, so the loss comes while Execute prepares this one.
	AbortTransaction(con);
	auto lost_aborted = FailureOf(con, Query("lose=prepare sqlstates=25P02 rows=1"));
	CHECK(lost_aborted.type == ExceptionType::IO);
	CHECK(lost_aborted.Extra("sqlstate") == "25P02");
	CHECK_FALSE(StringUtil::Contains(lost_aborted.message, "is aborted"));
	CHECK(StringUtil::EndsWith(lost_aborted.message, "while preparing the statement\nSQLSTATE: 25P02"));
	CHECK(f.lines->Closed() == 2);
}

TEST_CASE("a recovered Describe logs why before the replacement opens, and its timings after", "[failures]") {
	Fixture f;
	Connection con(f.db);
	Run(con, "DESCRIBE " + Query("rows=1"));
	Run(con, Query("lose=describe rows=1"));

	auto lines = f.lines->Lines();
	const auto first_open = Find(lines, OPENED);
	const auto notice = Find(lines, NOTICE);
	const auto replacement_open = Find(lines, OPENED, first_open + 1);
	const auto timings = Find(lines, TIMINGS);
	CHECK(first_open < notice);
	CHECK(notice < replacement_open);
	CHECK(replacement_open < timings);
	REQUIRE(timings < lines.size());
	CHECK(StringUtil::Contains(lines[timings], " failed_describe_us="));
	CHECK(StringUtil::Contains(lines[timings], " replacement_open_us="));
	CHECK(StringUtil::Contains(lines[timings], " retried_describe_us="));
	CHECK(StringUtil::Contains(lines[timings], " total_us="));
	CHECK_FALSE(StringUtil::Contains(lines[timings], "initial_open_us"));
	CHECK(StringUtil::EndsWith(lines[timings], " outcome=recovered"));
}

TEST_CASE("a log storage that refuses the recovery's lines changes neither what is recovered nor what is reported",
          "[failures]") {
	// It refuses every line redshift_query writes, as a storage whose flush fails would.
	Fixture f(true);
	Connection con(f.db);
	Run(con, "DESCRIBE " + Query("rows=1"));
	// Recovered silently: the refused lines are dropped, and the replacement is kept.
	Run(con, Query("lose=describe rows=1"));
	CHECK(f.lines->CountStartingWith(NOTICE) == 1);
	CHECK(f.lines->CountStartingWith(TIMINGS) == 1);
	CHECK(f.lines->Opened() == 2);
	CHECK(f.lines->Closed() == 1);
	// The replacement ran a statement, so a loss now fails, with the connection's error, not the
	// storage's.
	auto lost = FailureOf(con, Query("lose=describe rows=1"));
	CHECK(lost.type == ExceptionType::IO);
	CHECK(StringUtil::StartsWith(lost.message, "redshift_query: the Redshift connection of attach handle \"rs\" was "
	                                           "lost while describing the statement"));
	CHECK(f.lines->Closed() == 2);
}

TEST_CASE("a second planning pass another state asks for raises the possible-state-loss report again", "[failures]") {
	Fixture f;
	Connection con(f.db);
	auto state = con.context->registered_state->GetOrCreate<RebindOnPlanningError>("redshift_test_rebind_on_error");
	// A statement ran, so the session may hold state.
	Run(con, Query("norows"));

	// The first pass loses the connection and raises the report; DuckDB plans the statement again,
	// and that pass raises the same report before it opens or describes anything.
	auto lost = FailureOf(con, Query("lose=describe rows=1"));
	CHECK(state->rebinds == 1);
	CHECK(lost.type == ExceptionType::IO);
	CHECK(StringUtil::StartsWith(lost.message, LOSS_REPORT));
	CHECK(f.lines->Opened() == 1);
	CHECK(f.lines->Closed() == 1);
	CHECK(f.lines->Executed() == 1);
	CHECK(f.lines->CountStartingWith(TIMINGS) == 1);

	// Reported once: the next statement opens a new session, silently.
	Run(con, Query("rows=1"));
	CHECK(state->rebinds == 1);
	CHECK(f.lines->Opened() == 2);
	CHECK(f.lines->Executed() == 2);
}

TEST_CASE("a second planning pass rewritten to use another handle raises the report and opens nothing", "[failures]") {
	Fixture f;
	Connection con(f.db);
	// Attached from another DuckDB connection, whose session the ATTACH opens, so con has none on rs2.
	Connection attaching(f.db);
	Run(attaching, redshift_test::Attach("rs2"));
	auto state = con.context->registered_state->GetOrCreate<RebindOnPlanningError>(
	    "redshift_test_rebind_on_error", "SELECT * FROM redshift_query('rs2', 'rows=1')");
	Run(con, Query("norows"));
	CHECK(f.lines->Opened() == 2);

	auto lost = FailureOf(con, Query("lose=describe rows=1"));
	CHECK(state->rebinds == 1);
	CHECK(StringUtil::StartsWith(lost.message, LOSS_REPORT));
	// Nothing was opened for rs2, nor run.
	CHECK(f.lines->Opened() == 2);
	CHECK(f.lines->Executed() == 1);
	// Reported once: each handle works after, rs on a new session.
	Run(con, "SELECT * FROM redshift_query('rs2', 'rows=1')");
	Run(con, Query("rows=1"));
	CHECK(f.lines->Opened() == 4);
	CHECK(f.lines->Executed() == 3);
}

TEST_CASE("a report a rewritten planning pass dropped comes before the handle's next use, then once only",
          "[failures]") {
	Fixture f;
	Connection con(f.db);
	Run(con, "PREPARE later AS " + Query("rows=1"));
	auto state =
	    con.context->registered_state->GetOrCreate<RebindOnPlanningError>("redshift_test_rebind_on_error", "SELECT 42");
	Run(con, Query("norows"));

	// The second pass no longer reads the handle, so the statement succeeds without the report.
	auto rewritten = con.Query(Query("lose=describe rows=1"));
	REQUIRE_FALSE(rewritten->HasError());
	CHECK(rewritten->Fetch()->GetValue(0, 0).GetValue<int32_t>() == 42);
	CHECK(state->rebinds == 1);
	CHECK(f.lines->Closed() == 1);

	// A statement prepared before raises the report before it runs on a new session, and so does a
	// new one; once a query has ended with it, the next opens a new session silently.
	auto at_execute = FailureOf(con, "EXECUTE later");
	CHECK(at_execute.type == ExceptionType::IO);
	CHECK(StringUtil::StartsWith(at_execute.message, LOSS_REPORT));
	CHECK(f.lines->Opened() == 1);
	CHECK(f.lines->Executed() == 1);
	Run(con, "EXECUTE later");
	CHECK(f.lines->Opened() == 2);
	CHECK(f.lines->Executed() == 2);
}

TEST_CASE("a report a rewritten planning pass dropped comes before a new statement on the handle too", "[failures]") {
	Fixture f;
	Connection con(f.db);
	Run(con, redshift_test::Attach("rs2"));
	auto state =
	    con.context->registered_state->GetOrCreate<RebindOnPlanningError>("redshift_test_rebind_on_error", "SELECT 42");
	Run(con, Query("norows"));
	Run(con, Query("lose=describe rows=1"));

	// The report is about rs: another handle is used as usual meanwhile.
	Run(con, "SELECT * FROM redshift_query('rs2', 'rows=1')");
	CHECK(f.lines->Opened() == 2);
	auto at_bind = FailureOf(con, Query("rows=1"));
	CHECK(StringUtil::StartsWith(at_bind.message, LOSS_REPORT));
	CHECK(f.lines->Opened() == 2);
	Run(con, Query("rows=1"));
	CHECK(f.lines->Opened() == 3);
	CHECK(f.lines->Executed() == 3);
}

TEST_CASE("a report a rewritten planning pass dropped stays with the name when the handle is replaced or detached",
          "[failures]") {
	for (const bool replace : {true, false}) {
		INFO("replaced: " << replace);
		Fixture f;
		Connection con(f.db);
		auto state = con.context->registered_state->GetOrCreate<RebindOnPlanningError>("redshift_test_rebind_on_error",
		                                                                               "SELECT 42");
		Run(con, Query("norows"));
		Run(con, Query("lose=describe rows=1"));
		Run(con, replace ? redshift_test::AttachOrReplace("rs") : "DETACH rs");

		// The next use of the name raises the report, before it resolves the handle or opens anything.
		auto at_bind = FailureOf(con, Query("rows=1"));
		CHECK(StringUtil::StartsWith(at_bind.message, LOSS_REPORT));
		// The replacing ATTACH opened con's session on the new handle; the report opens nothing more.
		CHECK(f.lines->Opened() == (replace ? 2 : 1));
		CHECK(f.lines->Executed() == 1);
		// Once only: the call after it binds as usual, on the session the ATTACH opened.
		if (replace) {
			Run(con, Query("rows=1"));
			CHECK(f.lines->Opened() == 2);
			CHECK(f.lines->Executed() == 2);
		} else {
			CHECK(StringUtil::StartsWith(FailureOf(con, Query("rows=1")).message,
			                             "redshift_query: no attached database is named \"rs\""));
		}
	}
}

TEST_CASE("a report the user has seen as JSON is not raised again", "[failures]") {
	Fixture f;
	Connection con(f.db);
	Run(con, "SET errors_as_json = true");
	Run(con, Query("norows"));
	auto lost = con.Query(Query("lose=describe rows=1"));
	REQUIRE(lost->HasError());
	CHECK(StringUtil::StartsWith(lost->GetErrorObject().RawMessage(), "{"));
	CHECK(StringUtil::Contains(lost->GetErrorObject().RawMessage(), "may have held state"));
	// Seen once: the next statement opens a new session, silently.
	Run(con, Query("rows=1"));
	CHECK(f.lines->Opened() == 2);
	CHECK(f.lines->Executed() == 2);
}

TEST_CASE("a report through the relation API comes once, and the next bind opens a new session", "[failures]") {
	for (const bool from_query : {false, true}) {
		INFO("RelationFromQuery: " << from_query);
		Fixture f;
		Connection con(f.db);
		Run(con, Query("norows"));
		// DuckDB binds a relation outside any query, so no query end forgets the report.
		auto first = Capture([&]() {
			if (from_query) {
				con.RelationFromQuery(Query("lose=describe rows=1"));
			} else {
				con.TableFunction("redshift_query", {Value("rs"), Value("lose=describe rows=1")});
			}
		});
		CHECK(StringUtil::StartsWith(first.message, LOSS_REPORT));
		CHECK(f.lines->Opened() == 1);
		// Its caller got it, so the next relation bind forgets it and opens a new session.
		auto relation = con.TableFunction("redshift_query", {Value("rs"), Value("rows=1")});
		CHECK(f.lines->Opened() == 2);
		REQUIRE_FALSE(relation->Execute()->HasError());
		Run(con, Query("rows=1"));
		CHECK(f.lines->Opened() == 2);
	}
}

TEST_CASE("a report a caller swallows outside a query is raised by the next query on the handle", "[failures]") {
	// As the DuckDB shell's agent mode does: it extracts each statement's plan, swallowing any error,
	// before it runs the statement.
	Fixture f;
	Connection con(f.db);
	Run(con, Query("norows"));
	CHECK_THROWS(con.context->ExtractPlan(Query("lose=describe rows=1")));
	auto in_query = FailureOf(con, Query("rows=1"));
	CHECK(StringUtil::StartsWith(in_query.message, LOSS_REPORT));
	CHECK(f.lines->Opened() == 1);
	Run(con, Query("rows=1"));
	CHECK(f.lines->Opened() == 2);
}

TEST_CASE("a report DuckDB drops for an earlier error is raised before the handle's next session is used",
          "[failures]") {
	const std::pair<const char *, const char *> losses[] = {
	    {"lose=execute rows=1", "was lost while executing the statement"},
	    {"lose=fetch:1 rows=3", "was lost after 1 row of the statement's result"},
	};
	for (auto &loss : losses) {
		INFO(loss.first);
		Fixture f;
		SwallowRedshiftCall::Register(*f.db.instance);
		Connection con(f.db);
		Run(con, Query("norows"));
		auto swallowed = Swallowed(con, loss.first);
		CHECK(StringUtil::Contains(swallowed.first.ToString(), loss.second));
		CHECK(f.lines->Closed() == 1);
		// The query ended without the report, so the next call raises it before it opens anything.
		auto at_bind = FailureOf(con, Query("rows=1"));
		CHECK(StringUtil::Contains(at_bind.message, loss.second));
		CHECK(StringUtil::Contains(at_bind.message, "The session it carried may have held state"));
		CHECK(f.lines->Opened() == 1);
		Run(con, Query("rows=1"));
		CHECK(f.lines->Opened() == 2);
	}
}

TEST_CASE("a report a query dropped is raised by the next relation bind on the handle, then forgotten", "[failures]") {
	Fixture f;
	SwallowRedshiftCall::Register(*f.db.instance);
	Connection con(f.db);
	Run(con, Query("norows"));
	auto swallowed = Swallowed(con, "lose=execute rows=1");
	CHECK(StringUtil::Contains(swallowed.first.ToString(), "was lost while executing the statement"));
	// The user has not seen it, so the relation bind, outside any query, raises it before it opens
	// anything; its caller has it then, so the next relation bind opens a new session.
	auto at_relation = Capture([&]() { con.TableFunction("redshift_query", {Value("rs"), Value("rows=1")}); });
	CHECK(StringUtil::Contains(at_relation.message, "was lost while executing the statement"));
	CHECK(StringUtil::Contains(at_relation.message, "The session it carried may have held state"));
	CHECK(f.lines->Opened() == 1);
	auto relation = con.TableFunction("redshift_query", {Value("rs"), Value("rows=1")});
	CHECK(f.lines->Opened() == 2);
	REQUIRE_FALSE(relation->Execute()->HasError());
	Run(con, Query("rows=1"));
	CHECK(f.lines->Opened() == 2);
}

TEST_CASE("a report a query dropped and an extracted plan swallowed is raised by the next query", "[failures]") {
	Fixture f;
	SwallowRedshiftCall::Register(*f.db.instance);
	Connection con(f.db);
	Run(con, Query("norows"));
	auto swallowed = Swallowed(con, "lose=execute rows=1");
	CHECK(StringUtil::Contains(swallowed.first.ToString(), "was lost while executing the statement"));
	// As the DuckDB shell's agent mode does: the plan it extracts first raises the report, outside any
	// query, and it swallows that error before it runs the statement.
	CHECK_THROWS(con.context->ExtractPlan(Query("rows=1")));
	auto in_query = FailureOf(con, Query("rows=1"));
	CHECK(StringUtil::Contains(in_query.message, "was lost while executing the statement"));
	CHECK(StringUtil::Contains(in_query.message, "The session it carried may have held state"));
	CHECK(f.lines->Opened() == 1);
	Run(con, Query("rows=1"));
	CHECK(f.lines->Opened() == 2);
}

TEST_CASE("a loss whose session held no state, or a statement failure, keeps no report", "[failures]") {
	Fixture f;
	SwallowRedshiftCall::Register(*f.db.instance);
	Connection con(f.db);
	// The pair has only described a statement, so its session held no state.
	Run(con, "DESCRIBE " + Query("rows=1"));
	auto lost = Swallowed(con, "lose=prepare rows=1");
	CHECK(StringUtil::Contains(lost.first.ToString(), "was lost while executing the statement"));
	Run(con, Query("rows=1"));
	CHECK(f.lines->Opened() == 2);
	// A statement failure at NextBatch destroys its result before it is raised, so the same session
	// takes the next statement while the failed one's holder still has it.
	auto failed = Swallowed(con, "fail=fetch:1 sqlstates=57014 rows=3", "rows=1");
	CHECK(StringUtil::Contains(failed.first.ToString(), "the statement failed while it ran"));
	CHECK(failed.second.IsNull());
	Run(con, Query("rows=1"));
	CHECK(f.lines->Opened() == 2);
}

TEST_CASE("a report stays kept through a query that ends with another error, or with an error that only looks "
          "like it",
          "[failures]") {
	const string full_report = string(LOSS_REPORT) +
	                           ", which may now be lost: the next redshift_query on this handle from this DuckDB "
	                           "connection starts a new, empty Redshift session. The client reported: redshift mock: "
	                           "lost the connection to " +
	                           redshift_test::MockTarget() + " during Describe";
	const string unrelated[] = {
	    "SELECT error('unrelated')",
	    // Not JSON, though it starts like it: it is read as it is, and still fails with its own message.
	    "SELECT error('{x')",
	    // The report's text, but not its type.
	    "SELECT error('" + full_report + "')",
	};
	for (auto &other : unrelated) {
		INFO(other);
		Fixture f;
		Connection con(f.db);
		auto state = con.context->registered_state->GetOrCreate<RebindOnPlanningError>("redshift_test_rebind_on_error",
		                                                                               "SELECT 42");
		Run(con, Query("norows"));
		Run(con, Query("lose=describe rows=1"));
		auto own = FailureOf(con, other);
		CHECK(own.type == ExceptionType::INVALID_INPUT);
		CHECK_FALSE(StringUtil::Contains(own.message, "Failed to parse JSON"));
		auto at_bind = FailureOf(con, Query("rows=1"));
		CHECK(at_bind.message == full_report);
		CHECK(f.lines->Opened() == 1);
	}
}

TEST_CASE("one statement over two handles discards only the one whose connection was lost", "[failures]") {
	Fixture f;
	Connection con(f.db);
	Run(con, redshift_test::Attach("rs2"));
	auto lost = FailureOf(con, "SELECT count(*) FROM redshift_query('rs', 'rows=3000 types=int8') x, "
	                           "redshift_query('rs2', 'lose=fetch:1 rows=3 types=int8') y");
	CHECK(StringUtil::Contains(lost.message, "attach handle \"rs2\" was lost after 1 row"));
	CHECK(f.lines->Closed() == 1);
	// rs keeps its session; rs2 opens a new one.
	Run(con, Query("rows=1"));
	CHECK(f.lines->Opened() == 2);
	Run(con, "SELECT * FROM redshift_query('rs2', 'rows=1')");
	CHECK(f.lines->Opened() == 3);
}

TEST_CASE("a replacement that cannot open fails the bind with its own error, and no other open is tried",
          "[failures]") {
	Fixture f;
	Connection con(f.db);
	Run(con, "DESCRIBE " + Query("rows=1"));
	REQUIRE(f.lines->Opened() == 1);
	const string unreachable = "redshift mock test: could not reach Redshift";
	FailNextMockOpen(unreachable);

	auto error = FailureOf(con, Query("lose=describe rows=1"));
	// The open's error as the client gave it: the lost session held no state, so nothing else is said.
	CHECK(error.type == ExceptionType::IO);
	CHECK(error.message == unreachable);
	CHECK(f.lines->Opened() == 1);
	CHECK(f.lines->CountStartingWith(COULD_NOT_OPEN) == 1);
	CHECK(f.lines->Closed() == 1);
	CHECK(f.lines->Executed() == 0);

	auto lines = f.lines->Lines();
	const auto notice = Find(lines, NOTICE);
	const auto failed_open = Find(lines, COULD_NOT_OPEN);
	const auto timings = Find(lines, TIMINGS);
	CHECK(notice < failed_open);
	CHECK(failed_open < timings);
	REQUIRE(timings < lines.size());
	CHECK(StringUtil::Contains(lines[timings], " failed_describe_us="));
	CHECK(StringUtil::Contains(lines[timings], " replacement_open_us="));
	CHECK_FALSE(StringUtil::Contains(lines[timings], "retried_describe_us"));
	CHECK(StringUtil::EndsWith(lines[timings], " outcome=failed"));

	// The pair has no connection now, and its next call opens one as usual.
	Run(con, Query("rows=1"));
	CHECK(f.lines->Opened() == 2);
}

TEST_CASE("an open refused for its credentials is reported as the client gave it, and ends no live session",
          "[failures]") {
	Fixture f;
	Connection live(f.db);
	Connection opening(f.db);
	Run(live, Query("rows=1"));
	const string expired = "redshift mock test: the AWS credentials have expired";
	FailNextMockOpen(expired);

	// A live session needs no credentials again, so its next call runs on it.
	Run(live, Query("rows=1"));
	// The first open after the expiry fails with the client's error, which claims no lost session.
	auto error = FailureOf(opening, Query("rows=1"));
	CHECK(error.type == ExceptionType::IO);
	CHECK(error.message == expired);
	// The live session is still the one it was.
	Run(live, Query("rows=1"));
	CHECK(f.lines->Opened() == 1);
	CHECK(f.lines->Closed() == 0);
	CHECK(f.lines->Executed() == 3);
}

TEST_CASE("a prepared statement whose pair lost its connection opens a new one, or fails with the open's error",
          "[failures]") {
	Fixture f;
	Connection con(f.db);
	Run(con, "PREPARE later AS " + Query("rows=1"));
	CHECK(FailureOf(con, Query("lose=execute rows=1")).type == ExceptionType::IO);
	REQUIRE(f.lines->Closed() == 1);
	const auto executed = f.lines->Executed();

	// The pair has no connection, so EXECUTE opens one; when that open fails, its error passes through
	// and nothing runs.
	FailNextMockOpen("redshift mock test: could not reach Redshift");
	auto error = FailureOf(con, "EXECUTE later");
	CHECK(error.type == ExceptionType::IO);
	CHECK(error.message == "redshift mock test: could not reach Redshift");
	CHECK(f.lines->Executed() == executed);
	// Next time the open succeeds, and the statement runs on the new session.
	Run(con, "EXECUTE later");
	CHECK(f.lines->Opened() == 2);
	CHECK(f.lines->Executed() == executed + 1);
}

TEST_CASE("a connection lost while a stream is read fails the stream, after no more rows than were read",
          "[failures]") {
	Fixture f;
	Connection con(f.db);
	// A small streaming buffer, so DuckDB stops producing once a chunk waits, and the caller holds
	// rows before the result reaches its loss; with the default 10 MiB, the first Fetch would run the
	// whole query.
	Run(con, "SET max_streaming_buffer_size = '16KB'");
	auto stream = make_uniq<QueryResultStream<>>(con.Submit(Query("lose=fetch:5000 rows=10000 types=int8")));
	REQUIRE_FALSE(stream->HasError());
	idx_t rows = 0;
	unique_ptr<DataChunk> chunk;
	while ((chunk = stream->Fetch())) {
		rows += chunk->size();
	}
	REQUIRE(stream->HasError());
	auto &error = stream->GetErrorObject();
	CHECK(error.Type() == ExceptionType::IO);
	CHECK(StringUtil::Contains(error.RawMessage(), "was lost after 5000 rows of the statement's result had been read "
	                                               "from Redshift, so the result may be incomplete"));
	// The caller holds some rows already, never more than were read from Redshift.
	CHECK(rows > 0);
	CHECK(rows <= 5000);
	CHECK(f.lines->Closed() == 1);
	CHECK(f.lines->CountStartingWith(OUTLIVED) == 0);
	stream.reset();
	Run(con, Query("rows=1"));
	CHECK(f.lines->Opened() == 2);
}

TEST_CASE("a stream abandoned with rows unread, or interrupted, ends its result and keeps the session", "[failures]") {
	for (const bool interrupt : {false, true}) {
		INFO("interrupted: " << interrupt);
		Fixture f;
		Connection con(f.db);
		// A small streaming buffer, so DuckDB stops producing once a chunk waits, and the result still
		// has rows, and is still open, when the stream stops.
		Run(con, "SET max_streaming_buffer_size = '16KB'");
		auto stream = make_uniq<QueryResultStream<>>(con.Submit(Query("rows=100000 batch=2048 types=int8")));
		auto chunk = stream->Fetch();
		REQUIRE(chunk);
		if (interrupt) {
			con.Interrupt();
			while ((chunk = stream->Fetch())) {
			}
			CHECK(stream->HasError());
		}
		// Destroying the stream ends its query, if it still runs, and with it the result; DuckDB starts
		// no other query on the connection while a stream is open. The mock refuses Execute while a
		// result is open, so the next call would fail had the result been kept; it runs on the same
		// session.
		stream.reset();
		Run(con, Query("rows=1"));
		CHECK(f.lines->Opened() == 1);
		CHECK(f.lines->Closed() == 0);
		CHECK(f.lines->Executed() == 2);
		CHECK(f.lines->CountStartingWith(OUTLIVED) == 0);
	}
}

TEST_CASE("a bystander's re-attach report is raised again by a second planning pass", "[reattach]") {
	// The record is taken once, so only the held report can stop the second pass of the same
	// statement from running on the new handle silently.
	Fixture f;
	Connection bystander(f.db);
	Connection detaching(f.db);
	Run(bystander, Query("rows=1"));
	Run(detaching, "DETACH rs");
	Run(detaching, redshift_test::Attach("rs"));
	auto state =
	    bystander.context->registered_state->GetOrCreate<RebindOnPlanningError>("redshift_test_rebind_on_error");

	auto failed = FailureOf(bystander, Query("rows=1"));
	CHECK(StringUtil::Contains(failed.message, "a new handle is attached under the name now"));
	CHECK(state->rebinds == 1);
	// The bystander's first, and the ATTACH's own on the new handle.
	CHECK(f.lines->Opened() == 2);
	// Seen once: the next statement opens a fresh session on the new handle.
	Run(bystander, Query("rows=1"));
	CHECK(f.lines->Opened() == 3);
}

TEST_CASE("a bystander's re-attach report a rewritten planning pass dropped comes before its next use", "[reattach]") {
	Fixture f;
	Connection bystander(f.db);
	Connection detaching(f.db);
	Run(bystander, Query("rows=1"));
	Run(detaching, "DETACH rs");
	Run(detaching, redshift_test::Attach("rs"));
	auto state = bystander.context->registered_state->GetOrCreate<RebindOnPlanningError>(
	    "redshift_test_rebind_on_error", "SELECT 42");

	// The second pass no longer reads the handle, so the statement succeeds without the report.
	auto rewritten = bystander.Query(Query("rows=1"));
	REQUIRE_FALSE(rewritten->HasError());
	CHECK(state->rebinds == 1);
	// The report is kept by name, so the next statement on the name raises it before anything opens,
	// though the handle under the name is a new one.
	auto at_bind = FailureOf(bystander, Query("rows=1"));
	CHECK(StringUtil::Contains(at_bind.message, "a new handle is attached under the name now"));
	// The bystander's first, and the ATTACH's own on the new handle.
	CHECK(f.lines->Opened() == 2);
	Run(bystander, Query("rows=1"));
	CHECK(f.lines->Opened() == 3);
}

namespace {

//! What the session error says about the statement: refused at bind, refused at Execute's prepare, and failed
//! from execution on.
const string NOT_RUN = "The statement was not run on Redshift.";
const string THIS_CALL_NOT_RUN = "This call was not run on Redshift, though other calls in the same DuckDB statement "
                                 "may have run; check their effects on Redshift before running the statement again.";
const string MAY_HAVE_RUN = REDSHIFT_ALREADY_EXECUTED;

//! What the session error says was lost, only when the session may have held state.
const string GONE =
    "Anything that lived in the old session is gone: temporary tables, SET values and any open transaction. ";

//! What redshift_query says, before the client's own message, when the client reports the session unusable;
//! `outcome` is what it says about the statement, and `held_state` whether the session may have held state.
string SessionUnusableLead(const string &outcome, bool held_state) {
	return "redshift_query: Redshift session error on attach handle \"rs\": the session cannot be used any more. The "
	       "extension discarded it; the next redshift_query on \"rs\" opens a new session. " +
	       (held_state ? GONE : string()) + outcome + " The client reported: ";
}

const string REFUSED_XX000 = "Redshift rejected the statement: Internal_Service_Error\nSQLSTATE: XX000";

} // namespace

TEST_CASE("a session the client reports unusable is discarded, its statement fails saying so, and nothing is retried",
          "[failures]") {
	Fixture f;
	Connection con(f.db);
	Run(con, Query("rows=1"));
	REQUIRE(f.lines->Opened() == 1);

	// At bind: the refusal keeps its type; Describe is not tried again on a replacement, as it is for a lost
	// connection whose session held no state, and no replacement opens until the next statement.
	auto at_bind = FailureOf(con, Query("error=Internal_Service_Error sqlstates=XX000 unusable"));
	CHECK(at_bind.type == ExceptionType::BINDER);
	CHECK(at_bind.Extra("sqlstate") == "XX000");
	CHECK(at_bind.Extra("redshift_error") == "session_unusable");
	CHECK(at_bind.message == SessionUnusableLead(NOT_RUN, true) + REFUSED_XX000);
	CHECK(f.lines->Closed() == 1);
	CHECK(f.lines->Opened() == 1);
	CHECK(f.lines->Described() == 1);
	CHECK(f.lines->Executed() == 1);

	// The next statement opens a new session.
	Run(con, Query("rows=1"));
	CHECK(f.lines->Opened() == 2);

	// Refused at Execute's prepare: still a refusal, and this call ran nothing, but another call in the same DuckDB
	// statement may have run by then.
	auto at_prepare = FailureOf(con, Query("error=Internal_Service_Error error_at=execute sqlstates=XX000 unusable"));
	CHECK(at_prepare.type == ExceptionType::BINDER);
	CHECK(at_prepare.Extra("redshift_error") == "session_unusable");
	CHECK(at_prepare.message == SessionUnusableLead(THIS_CALL_NOT_RUN, true) + REFUSED_XX000);
	CHECK(f.lines->Executed() == 2);
	CHECK(f.lines->Closed() == 2);
	Run(con, Query("rows=1"));
	CHECK(f.lines->Opened() == 3);

	// At execution and at fetch: the failure keeps the executor's type, ran once, never again, and the message
	// says it may have run.
	auto at_execute = FailureOf(con, Query("fail=execute sqlstates=XX000 unusable"));
	CHECK(at_execute.type == ExceptionType::EXECUTOR);
	CHECK(at_execute.Extra("redshift_error") == "session_unusable");
	CHECK(at_execute.message == SessionUnusableLead(MAY_HAVE_RUN, true) +
	                                "redshift mock: the statement failed while it ran\nSQLSTATE: XX000");
	CHECK(f.lines->Executed() == 4);
	CHECK(f.lines->Closed() == 3);
	auto at_fetch = FailureOf(con, Query("fail=fetch:1 rows=3 sqlstates=XX000 unusable"));
	CHECK(at_fetch.type == ExceptionType::EXECUTOR);
	CHECK(at_fetch.message == SessionUnusableLead(MAY_HAVE_RUN, true) +
	                              "redshift mock: the statement failed while it ran\nSQLSTATE: XX000");
	CHECK(f.lines->Opened() == 4);
	CHECK(f.lines->Executed() == 5);
	CHECK(f.lines->Closed() == 4);
	// NextBatch destroyed the result before it discarded the connection.
	CHECK(f.lines->CountStartingWith(OUTLIVED) == 0);

	// The report, which the query ended with, is not raised again: the next statement just opens a new session.
	Run(con, Query("rows=1"));
	CHECK(f.lines->Opened() == 5);
}

TEST_CASE("an unusable session that held no state is not described again on a replacement either", "[failures]") {
	Fixture f;
	Connection con(f.db);
	// The pair has only described a statement, so its session held no state, as before a handle's first write: a
	// lost connection would be replaced silently here, an unusable session is not.
	Run(con, "DESCRIBE " + Query("rows=1"));
	auto at_bind = FailureOf(con, Query("error=Internal_Service_Error sqlstates=XX000 unusable"));
	CHECK(at_bind.type == ExceptionType::BINDER);
	// It lost nothing, so the message does not say what was lost.
	CHECK(at_bind.message == SessionUnusableLead(NOT_RUN, false) + REFUSED_XX000);
	CHECK(f.lines->Opened() == 1);
	CHECK(f.lines->Closed() == 1);
	Run(con, Query("rows=1"));
	CHECK(f.lines->Opened() == 2);
}

TEST_CASE("a replacement whose Describe the client reports unusable is discarded too, and the bind says so",
          "[failures]") {
	Fixture f;
	Connection con(f.db);
	// The idle connection is lost at Describe and its session held no state, so a replacement opens and describes
	// the statement again, which the client then reports unusable.
	Run(con, "DESCRIBE " + Query("rows=1"));
	auto at_bind = FailureOf(con, Query("lose=describe error=Internal_Service_Error sqlstates=XX000 unusable"));
	CHECK(at_bind.type == ExceptionType::BINDER);
	CHECK(at_bind.Extra("redshift_error") == "session_unusable");
	CHECK(at_bind.message == SessionUnusableLead(NOT_RUN, false) + REFUSED_XX000);
	CHECK(f.lines->Opened() == 2);
	CHECK(f.lines->Closed() == 2);
	Run(con, Query("rows=1"));
	CHECK(f.lines->Opened() == 3);
}

TEST_CASE("an unusable session's report DuckDB drops is raised before the handle's next session is used, then once "
          "only",
          "[failures]") {
	Fixture f;
	SwallowRedshiftCall::Register(*f.db.instance);
	Connection con(f.db);
	Run(con, Query("norows"));
	auto swallowed = Swallowed(con, "fail=execute sqlstates=XX000 unusable");
	CHECK(StringUtil::StartsWith(swallowed.first.ToString(), SessionUnusableLead(MAY_HAVE_RUN, true)));
	CHECK(f.lines->Closed() == 1);
	// The query ended without the report, and the session may have held state, so the next call raises it before it
	// opens anything.
	auto at_bind = FailureOf(con, Query("rows=1"));
	CHECK(at_bind.type == ExceptionType::EXECUTOR);
	CHECK(StringUtil::StartsWith(at_bind.message, SessionUnusableLead(MAY_HAVE_RUN, true)));
	CHECK(f.lines->Opened() == 1);
	Run(con, Query("rows=1"));
	CHECK(f.lines->Opened() == 2);
}

TEST_CASE("an unusable session's report DuckDB drops is not kept when the session held no state", "[failures]") {
	Fixture f;
	SwallowRedshiftCall::Register(*f.db.instance);
	Connection con(f.db);
	// The pair has only described a statement, so its session held no state.
	Run(con, "DESCRIBE " + Query("rows=1"));
	auto swallowed = Swallowed(con, "error=Internal_Service_Error error_at=execute sqlstates=XX000 unusable");
	CHECK(StringUtil::StartsWith(swallowed.first.ToString(), SessionUnusableLead(THIS_CALL_NOT_RUN, false)));
	CHECK(f.lines->Closed() == 1);
	// Nothing was lost, so the next statement runs on a new session without the report.
	Run(con, Query("rows=1"));
	CHECK(f.lines->Opened() == 2);
}

TEST_CASE("an XX000 failure the client does not report unusable keeps the session", "[failures]") {
	Fixture f;
	Connection con(f.db);
	Run(con, Query("rows=1"));
	auto refused = FailureOf(con, Query("error=Invalid_digit sqlstates=XX000"));
	CHECK(refused.type == ExceptionType::BINDER);
	CHECK(refused.Extra("redshift_error") == "<none>");
	auto failed = FailureOf(con, Query("fail=execute sqlstates=XX000"));
	CHECK(failed.type == ExceptionType::EXECUTOR);
	Run(con, Query("rows=1"));
	CHECK(f.lines->Opened() == 1);
	CHECK(f.lines->Closed() == 0);
}

TEST_CASE("an unclassified connect failure's driver reason reaches the user whole, through redshift_query, a "
          "replacement open and CONNECT",
          "[failures]") {
	// The client's message for a connect failure no category names, as it builds it: the cause, the driver's
	// reason as its own sentence, the hint, then the SQLSTATE line.
	const string reason = "Could not connect to Redshift at " + redshift_test::MockTarget() +
	                      ": the connection attempt failed. The driver reported \"curlCode: 28, Timeout was "
	                      "reached\".\nCheck the region and that this host can reach the workgroup, then "
	                      "retry.\nSQLSTATE: HY000";
	const unordered_map<string, string> info {{"sqlstate", "HY000"}, {"sqlstates", "HY000"}};
	Fixture f;
	Connection con(f.db);
	// The first open.
	FailNextMockOpen(reason, ExceptionType::IO, info);
	auto first = FailureOf(con, Query("rows=1"));
	CHECK(first.type == ExceptionType::IO);
	CHECK(first.message == reason);
	CHECK(first.Extra("sqlstate") == "HY000");
	// A replacement open after a Describe lost an idle connection whose session held no state.
	Run(con, "DESCRIBE " + Query("rows=1"));
	FailNextMockOpen(reason, ExceptionType::IO, info);
	auto replacement = FailureOf(con, Query("lose=describe rows=1"));
	CHECK(replacement.type == ExceptionType::IO);
	CHECK(replacement.message == reason);
	// A statement CONNECT routes, after CONNECT's words.
	Connection connected(f.db);
	Run(connected, "CONNECT rs");
	FailNextMockOpen(reason, ExceptionType::IO, info);
	auto routed = FailureOf(connected, "SELECT 1");
	CHECK(StringUtil::EndsWith(routed.message, " The client reported: " + reason));
	CHECK(StringUtil::Contains(routed.message, "The driver reported \"curlCode: 28, Timeout was reached\"."));
	Run(connected, "DISCONNECT");
}
