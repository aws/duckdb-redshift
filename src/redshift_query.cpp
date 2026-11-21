//===----------------------------------------------------------------------===//
// redshift_query('<handle>', '<Redshift SQL>' [, params := ['<value>', ...]])
//
// The execution surface. Bind describes the statement without running it and publishes its
// columns; the same-handle statement fence checks each planned statement before anything runs; and
// execution checks that the handle is still the one bind described the statement on, runs the
// statement once and streams its rows. It reaches Redshift only through the transport interface:
// Describe, Execute and NextBatch by way of RedshiftConnectionCalls
// (storage/redshift_connection_calls.hpp), which also decides what to do when one fails, and RowCount
// on the result.
//
// The first argument is an attach handle: an attached database created by
// `ATTACH '' AS <handle> (TYPE <REDSHIFT_ATTACH_TYPE>, ...)`. Bind and
// execution each resolve it, and both use the Redshift session the handle keeps for this DuckDB
// connection.
//
// CONNECT runs each statement typed while CONNECT-ed to a handle as a call of this function too
// (MakeConnectedQuery), whose messages are written for that user (RedshiftCaller).
//===----------------------------------------------------------------------===//

#include "redshift_query.hpp"

#include "redshift_attach.hpp"
#include "redshift_context_log.hpp"
#include "redshift_conversion.hpp"
#include "redshift_messages.hpp"
#include "redshift_transport.hpp"
#include "storage/redshift_catalog.hpp"
#include "storage/redshift_client_state.hpp"
#include "storage/redshift_connection_calls.hpp"
#include "storage/redshift_connection_ownership.hpp"

#include "duckdb.hpp"
#include "duckdb/common/exception.hpp"
#include "duckdb/common/sql_identifier.hpp"
#include "duckdb/common/string_util.hpp"
#include "duckdb/common/types/uuid.hpp"
#include "duckdb/common/unordered_map.hpp"
#include "duckdb/execution/operator/helper/physical_connect.hpp"
#include "duckdb/execution/operator/helper/physical_execute.hpp"
#include "duckdb/execution/physical_plan_generator.hpp"
#include "duckdb/function/table_function.hpp"
#include "duckdb/main/attached_database.hpp"
#include "duckdb/main/client_data.hpp"
#include "duckdb/main/database_manager.hpp"
#include "duckdb/main/extension/extension_loader.hpp"
#include "duckdb/main/prepared_statement_data.hpp"
#include "duckdb/main/valid_checker.hpp"
#include "duckdb/parser/expression/constant_expression.hpp"
#include "duckdb/parser/expression/function_expression.hpp"
#include "duckdb/parser/parsed_data/connect_info.hpp"
#include "duckdb/parser/parsed_data/transaction_info.hpp"
#include "duckdb/parser/tableref/table_function_ref.hpp"
#include "duckdb/planner/expression/bound_subquery_expression.hpp"
#include "duckdb/planner/expression_iterator.hpp"
#include "duckdb/planner/logical_operator_visitor.hpp"
#include "duckdb/planner/planner_extension.hpp"
#include "duckdb/planner/operator/logical_connect.hpp"
#include "duckdb/planner/operator/logical_create.hpp"
#include "duckdb/planner/operator/logical_cteref.hpp"
#include "duckdb/planner/operator/logical_execute.hpp"
#include "duckdb/planner/operator/logical_explain.hpp"
#include "duckdb/planner/operator/logical_get.hpp"
#include "duckdb/planner/operator/logical_materialized_cte.hpp"
#include "duckdb/transaction/meta_transaction.hpp"

namespace duckdb {

namespace {

//! Marks the redshift_query call CONNECT runs a statement as (MakeConnectedQuery). It is the
//! reference's bind_info, which SQL cannot set, so no call a user writes is taken for one.
struct RedshiftConnectedCall : public TableFunctionInfo {
	RedshiftConnectedCall(string handle_p, idx_t handle_id_p) : handle(std::move(handle_p)), handle_id(handle_id_p) {
	}

	//! The name of the handle the DuckDB connection is CONNECT-ed to, as CONNECT names it.
	string handle;
	//! That handle's ID (RedshiftCatalog::HandleId): the call runs on that handle and no other.
	idx_t handle_id;
};

//! The mark of a call CONNECT made, or null for a call a user wrote.
optional_ptr<const RedshiftConnectedCall> ConnectedCall(const TableFunctionRef &ref) {
	return dynamic_cast<const RedshiftConnectedCall *>(ref.bind_info.get());
}

//! What bind learned, for the scan and the checks between bind and execution. It holds no
//! connection and nothing that keeps the handle alive, because DuckDB can cache it in a prepared
//! statement for as long as the statement lives.
struct RedshiftQueryBindData : public TableFunctionData {
	//! The attach handle's name, as the caller wrote it or, for a call CONNECT made, as the CONNECT-ed
	//! handle is attached; for messages. Execution resolves the name again.
	string handle;
	//! Whether CONNECT made the call, for messages and for the transaction gate.
	bool routed_by_connect = false;
	//! The ID of the handle bind resolved and described the statement on (RedshiftCatalog::HandleId).
	//! It identifies the handle without keeping it alive, and is never reused, so a handle attached
	//! later under the same alias has another.
	idx_t handle_id = 0;
	//! Sent to Redshift byte for byte, never parsed or rewritten.
	string sql;
	//! The `params` strings, in order, which Execute binds positionally.
	vector<string> params;
	//! The shape Describe reported, kept whole: returns_rows, column names, type IDs, and DECIMAL
	//! precision and scale. The scan reads `returns_rows` and converts cells to the types of these
	//! columns, which are the types bind declared, and Execute passes it to the client, which refuses a
	//! statement whose shape has changed since, before running it.
	RedshiftStatementShape shape;
	//! Set by the same-handle statement fence when a full walk of a plan holding this scan passes, and
	//! never cleared, so a cached plan stays checked. Execution refuses a scan the fence never
	//! reached.
	bool fence_checked = false;
	//! The DuckDB statement that last started this scan (its query number), so execution can refuse
	//! to start it again in the same statement. Written by execution, which sees the bind data as
	//! const, and only by the one thread that initializes the scan at a time.
	mutable transaction_t started_in_statement = MAXIMUM_QUERY_ID;

	//! Whom the call's messages are written for.
	RedshiftCaller Caller() const {
		return RedshiftCaller(handle, routed_by_connect);
	}
};

struct RedshiftQueryGlobalState : public GlobalTableFunctionState {
	RedshiftQueryGlobalState(ClientContext &context, unique_ptr<RedshiftConnectionPin> pin_p, RedshiftCaller caller)
	    : pin(std::move(pin_p)), calls(context, pin->Catalog(), std::move(caller)) {
	}

	//! The execution's pin on the pair, which keeps the handle and the pair's connection until this
	//! state is destroyed, through a retirement. Declared first, so it ends last, after the result.
	unique_ptr<RedshiftConnectionPin> pin;
	//! The calls on the pair's connection, which ran Execute and read the result's batches.
	RedshiftConnectionCalls calls;
	//! The statement's result, from calls.Execute; owned here, and destroyed with this state, or
	//! first by calls.NextBatch when a fetch fails. DuckDB destroys this state when the query ends,
	//! however it ends (the rows drained, a LIMIT met, an error anywhere in the plan, an interrupt;
	//! ClientContext::EndQueryInternal), so the pair's next call finds no result open.
	//! The connection the result came from outlives it: the pin keeps the
	//! connection until after the result is destroyed, unless `calls` discards it after a failure,
	//! which destroys the result first.
	unique_ptr<RedshiftResult> result;
	//! Converts each row of the result into the output chunk, built before Execute; owned here. Null
	//! for a rowcount result.
	unique_ptr<RedshiftCellWriter> writer;
	RedshiftBatch batch;
	idx_t row_in_batch = 0;
	bool drained = false;
	bool rowcount_emitted = false;

	//! One statement, one result stream, so this scan cannot be split.
	idx_t MaxThreads() const override {
		return 1;
	}
};

//! The attach handle `handle` names now. Throws a BinderException, making no transport call of its
//! own, when no attached database has that name or the one that does is not a Redshift handle.
//!
//! Looked up in the database list, not through the query's transaction, which keeps a database it
//! used under its name after a DETACH or a rolled-back ATTACH, so a handle another DuckDB connection
//! detached or replaced, or that the rollback of an ATTACH under its name removed, is never found
//! here. Nor is it held in the transaction: the caller's RedshiftConnectionPin holds it for as long
//! as the caller uses it, and no longer.
//!
//! `note`, if given, is added to either error: the possible-state-loss context.
//! It starts with ". " and ends with a full stop.
shared_ptr<AttachedDatabase> ResolveHandle(ClientContext &context, const RedshiftCaller &caller,
                                           const string &note = string()) {
	const auto &handle = caller.handle;
	const Identifier name(handle);
	// The attached database that has this name, not yet known to be a Redshift handle: a DuckDB
	// file or `memory` can have the name too. Not the DatabaseInstance, which holds them all.
	auto attached = DatabaseManager::Get(context).GetDatabase(name);
	string type;
	if (attached) {
		type = attached->GetCatalog().GetCatalogType();
	} else {
		// Only for the message, and read without adding a reference to the transaction: `temp` is the
		// DuckDB connection's own, outside the list, and the transaction can still hold a database
		// under the name that has left the list (DETACH or a rolled-back ATTACH). A Redshift one there
		// has left the list, so it is not found; another type is refused below all the same.
		optional_ptr<AttachedDatabase> elsewhere;
		if (name == TEMP_CATALOG) {
			elsewhere = ClientData::Get(context).temporary_objects.get();
		} else {
			elsewhere = MetaTransaction::Get(context).GetReferencedDatabase(name);
		}
		if (elsewhere && elsewhere->GetCatalog().GetCatalogType() != REDSHIFT_ATTACH_TYPE) {
			type = elsewhere->GetCatalog().GetCatalogType();
		}
	}
	// While CONNECT-ed, DuckDB sends an ATTACH to Redshift, so a CONNECT-ed user disconnects first.
	const auto connected_end =
	    (note.empty() ? string(".") : note) + " Run DISCONNECT, then attach a Redshift handle and CONNECT to it.";
	if (!attached && type.empty()) {
		if (caller.routed_by_connect) {
			throw BinderException("CONNECT: no attached database is named %s now, so the handle this DuckDB "
			                      "connection is CONNECT-ed to has been detached, or an ATTACH under its name was "
			                      "rolled back%s",
			                      SQLQuotedIdentifier::ToString(handle), connected_end);
		}
		throw BinderException("redshift_query: no attached database is named %s. Attach a Redshift handle first with "
		                      "ATTACH '' AS %s (TYPE %s, ...)%s",
		                      SQLQuotedIdentifier::ToString(handle), SQLIdentifier::ToString(handle),
		                      REDSHIFT_ATTACH_TYPE, note);
	}
	if (type != REDSHIFT_ATTACH_TYPE) {
		if (caller.routed_by_connect) {
			throw BinderException("CONNECT: %s is a %s database now, not the Redshift handle this DuckDB connection is "
			                      "CONNECT-ed to%s",
			                      SQLQuotedIdentifier::ToString(handle), type, connected_end);
		}
		throw BinderException("redshift_query: %s is a %s database, not a Redshift handle. Attach one with "
		                      "ATTACH '' AS <name> (TYPE %s, ...)%s",
		                      SQLQuotedIdentifier::ToString(handle), type, REDSHIFT_ATTACH_TYPE, note);
	}
	return attached;
}

//! The handle `handle` names now, for a bind, by the rules below for a DuckDB
//! connection that had a session on an earlier handle under the name, which another DuckDB
//! connection detached or replaced, or that the rollback of an ATTACH under its name removed.
//! Before any transport call of this attempt:
//!
//! - when that session may have held state, which is now lost, the user is told once: the
//!   unresolved-handle error says so when nothing has the name, and when a new handle has it the
//!   statement is refused, not run;
//! - otherwise, or when the user has been told already, the name means what it means now, and a new
//!   handle under it gets a fresh session, silently.
//!
//! The report is held (RedshiftClientState::HoldLossReport) until the user has seen it, as the
//! possible-state-loss report of a lost Describe is.
//!
//! `connected_handle_id`, for a call CONNECT made, is the ID of the handle the DuckDB connection is
//! CONNECT-ed to, which the report's advice depends on.
shared_ptr<AttachedDatabase> ResolveForBind(ClientContext &context, const RedshiftCaller &caller,
                                            optional_idx connected_handle_id = optional_idx()) {
	const auto &handle = caller.handle;
	// Resolved first. A handle the name no longer reaches, or now reaches a replacement of, is out of
	// DuckDB's list by now, since DuckDB changes the list under its own lock, so the check below finds
	// the session this DuckDB connection had on it even before DuckDB calls OnDetach or the rollback
	// hook.
	shared_ptr<AttachedDatabase> db;
	ErrorData unresolved;
	try {
		db = ResolveHandle(context, caller);
	} catch (std::exception &ex) {
		unresolved = ErrorData(ex);
	}
	auto &client_state = RedshiftClientState::Get(context);
	const bool lost = client_state.ConnectionMap()->TakeLostSession(client_state.ConnectionId(), Identifier(handle),
	                                                                RedshiftCatalog::ListedHandleIds(context));
	if (!lost) {
		if (unresolved.HasError()) {
			unresolved.Throw();
		}
		return db;
	}
	// Resolved again, whatever the first lookup found: a removal of the name between it and
	// TakeLostSession leaves `db` holding a handle out of the list.
	ErrorData report;
	db = nullptr;
	try {
		db = ResolveHandle(context, caller,
		                   ". Another DuckDB connection detached or replaced the Redshift handle that had this name, "
		                   "or an ATTACH under this name was rolled back, while this DuckDB connection had a "
		                   "Redshift session on it, and that session may have held state, which is now lost.");
	} catch (std::exception &ex) {
		report = ErrorData(ex);
	}
	if (db) {
		// Typed again while CONNECT-ed, the statement runs on the handle attached now only if the DuckDB
		// connection is CONNECT-ed to it.
		const bool connected_elsewhere =
		    connected_handle_id.IsValid() &&
		    db->GetCatalog().Cast<RedshiftCatalog>().HandleId() != connected_handle_id.GetIndex();
		report = ErrorData(BinderException(
		    "%s: attach handle %s was detached or replaced by another DuckDB connection, or an ATTACH under its name "
		    "was rolled back, and a new handle is attached under the name now. This DuckDB connection's Redshift "
		    "session on the earlier handle may have held state, which is now lost, so this statement was not run on "
		    "Redshift. %s",
		    caller.Name(), SQLQuotedIdentifier::ToString(handle),
		    connected_elsewhere ? "Run DISCONNECT, then CONNECT to the handle attached now to start a new, empty "
		                          "Redshift session on it."
		                        : "Run it again to start a new, empty Redshift session on the handle attached now."));
	}
	client_state.HoldLossReport(context, handle, report);
	report.Throw();
}

//! The refusal of an execution whose handle was detached, or removed by the rollback of an ATTACH
//! under its name, after the statement was bound, and not attached again, which runs nothing on
//! Redshift.
ExecutorException DetachedAfterBind(const RedshiftCaller &caller) {
	return ExecutorException(
	    "%s: attach handle %s was detached, or an ATTACH under its name was rolled back, after this statement was "
	    "bound, so %s was not run on Redshift%s %s",
	    caller.Name(), SQLQuotedIdentifier::ToString(caller.handle), caller.ThisCall(), caller.OtherCallsMayHaveRun(),
	    caller.routed_by_connect ? "Run DISCONNECT, then attach the handle again and CONNECT to it."
	                             : "Attach the handle again, then prepare the statement again.");
}

//! Checks that the attach handle is still the one bind used, so a handle detached or replaced, or
//! removed by the rollback of an ATTACH under its name, between bind and execution is refused, and
//! runs nothing on Redshift: a replacement may reach another Redshift than the one bind described
//! the statement on.
//! Returns the handle; keeping it alive for the scan is the caller's pin's. The refusals are
//! ExecutorExceptions, as the scan's other execution-time refusals are (see
//! REDSHIFT_OTHER_CALLS_MAY_HAVE_RUN).
shared_ptr<AttachedDatabase> ResolveBoundHandle(ClientContext &context, const RedshiftQueryBindData &bind_data) {
	const auto caller = bind_data.Caller();
	const auto handle = SQLQuotedIdentifier::ToString(bind_data.handle);
	// The database attached under the name now, which must be the handle bind described on.
	auto db_now = DatabaseManager::Get(context).GetDatabase(Identifier(bind_data.handle));
	if (!db_now) {
		throw DetachedAfterBind(caller);
	}
	auto &catalog = db_now->GetCatalog();
	if (catalog.GetCatalogType() != REDSHIFT_ATTACH_TYPE) {
		throw ExecutorException(
		    "%s: %s names a %s database now, not the Redshift handle this statement was bound to, "
		    "so %s was not run on Redshift%s %s",
		    caller.Name(), handle, catalog.GetCatalogType(), caller.ThisCall(), caller.OtherCallsMayHaveRun(),
		    caller.routed_by_connect ? "Run DISCONNECT, then attach a Redshift handle under another name "
		                               "and CONNECT to it."
		                             : "Attach a Redshift handle under another name and query that.");
	}
	if (catalog.Cast<RedshiftCatalog>().HandleId() != bind_data.handle_id) {
		throw ExecutorException("%s: attach handle %s was detached or replaced, or an ATTACH under its name was "
		                        "rolled back, after this statement was bound, and another handle has the name now, "
		                        "so %s was not run on Redshift%s %s",
		                        caller.Name(), handle, caller.ThisCall(), caller.OtherCallsMayHaveRun(),
		                        caller.routed_by_connect
		                            ? "Run DISCONNECT, then CONNECT to the handle attached now."
		                            : "Its result shape was described through the earlier handle; prepare "
		                              "the statement again to bind it to the handle attached now.");
	}
	return db_now;
}

//! The refusal of an execution whose handle was retired after ResolveBoundHandle found it: its pin was
//! refused, or the pair had no connection to run on. The name is checked again first, so a name that
//! has another handle by then gets ResolveBoundHandle's refusal instead.
[[noreturn]] void RefuseRetiredHandle(ClientContext &context, const RedshiftQueryBindData &bind_data) {
	ResolveBoundHandle(context, bind_data);
	throw DetachedAfterBind(bind_data.Caller());
}

//! The pieces of the transaction gate's messages.
const char *const GATE_REASON = "redshift_query cannot run while a DuckDB transaction is open because DuckDB COMMIT "
                                "or ROLLBACK does not control Redshift operations.";
const char *const GATE_RUN_OUTSIDE = "run redshift_query outside a DuckDB transaction, and enable autocommit if your "
                                     "client disabled it.";
//! Once the transaction is aborted, COMMIT rolls it back without an error, and most statements fail
//! until it ends; every message that tells the user to run ROLLBACK says so.
const char *const GATE_COMMIT_ROLLS_BACK = "(COMMIT would also roll it back, without an error)";
//! Always included, so no Redshift transaction state is read.
const char *const GATE_REDSHIFT_TRANSACTION = "If a Redshift transaction is already open on this handle, continue "
                                              "working on it through redshift_query after the DuckDB transaction "
                                              "ends.";

//! The pieces CONNECT's transaction refusals share, as the gate's messages share
//! GATE_*: the reason a statement CONNECT routed was refused, the step once the DuckDB transaction has
//! ended, and the sentence about an already-open Redshift transaction.
const char *const CONNECT_GATE_REASON = "CONNECT: this statement was not run on Redshift, because a DuckDB transaction "
                                        "is open and DuckDB COMMIT or ROLLBACK does not control Redshift operations.";
const char *const CONNECT_GATE_AGAIN = "then CONNECT again outside a DuckDB transaction, and enable autocommit if your "
                                       "client disabled it.";
const char *const CONNECT_GATE_REDSHIFT_TRANSACTION = "If a Redshift transaction is already open on this handle, "
                                                      "continue working on it once CONNECT-ed again.";

//! The option, which lets redshift_query run inside an explicit DuckDB transaction.
const char *const ALLOW_IN_DUCKDB_TRANSACTION = "redshift_allow_query_in_duckdb_transaction";

//! The option's set callback, which DuckDB runs on SET and RESET, with the value cast to BOOLEAN,
//! before it stores the value. Refuses NULL, which would leave the option neither on nor off, and
//! warns each time the option is set to true.
void OnSetAllowInDuckDBTransaction(ClientContext &context, SetScope, Value &parameter) {
	if (parameter.IsNull()) {
		throw InvalidInputException("%s cannot be NULL. Set it to true or false, or RESET it",
		                            ALLOW_IN_DUCKDB_TRANSACTION);
	}
	if (BooleanValue::Get(parameter)) {
		LogWarning(context, StringUtil::Format("redshift_query: %s was set to true, so redshift_query can run inside "
		                                       "a DuckDB transaction. DuckDB COMMIT or ROLLBACK does not control its "
		                                       "Redshift operations.",
		                                       ALLOW_IN_DUCKDB_TRANSACTION));
	}
}

//! Whether the option is on. A value DuckDB stored without the set callback is neither checked nor
//! cast: DBConfig::SetOption stores one as given, and DBConfig::AddExtensionOption copies one that
//! DBConfig::SetOptionByName stored before the extension loaded. So NULL, and a value that does not
//! cast to BOOLEAN, count as off.
bool AllowedInDuckDBTransaction(ClientContext &context) {
	Value value;
	if (!context.TryGetCurrentSetting(ALLOW_IN_DUCKDB_TRANSACTION, value)) {
		return false;
	}
	auto allowed = value.DefaultTryCastAs(LogicalType::BOOLEAN);
	// A NULL casts to a NULL BOOLEAN, which BooleanValue::Get reads unchecked (GetValueUnsafe).
	return allowed && !allowed->IsNull() && BooleanValue::Get(*allowed);
}

//! Whether the gate sees an explicit transaction: BEGIN, SetAutoCommit(false) and EXPLAIN_SQL
//! verification turn DuckDB's autocommit flag off. SetAutoCommit(true) can turn it back on while the
//! transaction stays active, so the gate does not refuse the next statement. DuckDB commits the
//! transaction if that statement succeeds and rolls it back if it fails.
bool InExplicitTransaction(ClientContext &context) {
	return !context.transaction.IsAutoCommit();
}

//! Logs the warning for the first call on this DuckDB connection that the option lets run in a
//! transaction, just before its transport call, so a refused call does not use it up.
void NoteRunInDuckDBTransaction(ClientContext &context) {
	if (!InExplicitTransaction(context) || !RedshiftClientState::Get(context).TakeFirstUseInDuckDBTransaction()) {
		return;
	}
	LogWarning(context, StringUtil::Format("redshift_query: running inside a DuckDB transaction because %s is on. "
	                                       "DuckDB COMMIT or ROLLBACK does not control the Redshift operations it "
	                                       "runs there. This is logged once per DuckDB connection.",
	                                       ALLOW_IN_DUCKDB_TRANSACTION));
}

//! What a refusal at bind inside an explicit DuckDB transaction leaves the transaction in.
enum class RefusedTransaction : uint8_t {
	//! An earlier error aborted it: DuckDB binds a statement before it refuses one in an aborted
	//! transaction (ClientContext::CheckIfPreparedStatementIsExecutable).
	ALREADY_ABORTED,
	//! The refusal aborts it, as the transaction's policy has every error do.
	ABORTED_BY_REFUSAL,
	//! SYNTACTIC_ERRORS_DO_NOT_INVALIDATE keeps it usable after a binder error, unless DuckDB expanded
	//! the statement into several and an earlier one wrote (ClientContext::ErrorInvalidatesQueryTransaction).
	USABLE,
};

RefusedTransaction TransactionAfterRefusalAtBind(ClientContext &context) {
	if (ValidChecker::IsInvalidated(MetaTransaction::Get(context))) {
		return RefusedTransaction::ALREADY_ABORTED;
	}
	// DuckDB's rule for a binder error (ClientContext::ErrorInvalidatesTransaction, private).
	if (context.transaction.GetInvalidationPolicy() !=
	    TransactionInvalidationPolicy::SYNTACTIC_ERRORS_DO_NOT_INVALIDATE) {
		return RefusedTransaction::ABORTED_BY_REFUSAL;
	}
	return RefusedTransaction::USABLE;
}

//! What a refusal at bind that left the DuckDB transaction aborted, `state` ALREADY_ABORTED or
//! ABORTED_BY_REFUSAL, says about it and what to run: a sentence without its full stop, shared by the
//! gate for a call a user wrote and CONNECT's.
string AbortedTransactionAdvice(RefusedTransaction state) {
	D_ASSERT(state != RefusedTransaction::USABLE);
	if (state == RefusedTransaction::ALREADY_ABORTED) {
		return StringUtil::Format("The DuckDB transaction was already aborted by an earlier error. Run ROLLBACK %s",
		                          GATE_COMMIT_ROLLS_BACK);
	}
	return StringUtil::Format("This error has aborted the DuckDB transaction: under the transaction's invalidation "
	                          "policy, every error does. Run ROLLBACK %s",
	                          GATE_COMMIT_ROLLS_BACK);
}

//! Throws a refusal at bind inside an explicit DuckDB transaction: `reason`, then what the refusal
//! left the transaction in and what to do, ending with `next`, the step once the transaction has
//! ended, and, if `advise_open_redshift_transaction`, GATE_REDSHIFT_TRANSACTION.
//! A BinderException because it is one of the types SYNTACTIC_ERRORS_DO_NOT_INVALIDATE
//! leaves the transaction usable after.
[[noreturn]] void ThrowRefusalAtBindInDuckDBTransaction(ClientContext &context, const string &reason,
                                                        const string &next, bool advise_open_redshift_transaction) {
	const string trailer = advise_open_redshift_transaction ? string(" ") + GATE_REDSHIFT_TRANSACTION : string();
	const auto state = TransactionAfterRefusalAtBind(context);
	// No policy set now would have kept a transaction an earlier error aborted usable, so its message
	// gives no policy advice.
	if (state == RefusedTransaction::ALREADY_ABORTED) {
		throw BinderException("%s %s, then %s%s", reason, AbortedTransactionAdvice(state), next, trailer);
	}
	if (state == RefusedTransaction::ABORTED_BY_REFUSAL) {
		throw BinderException("%s %s, then %s To keep a DuckDB transaction usable after this refusal, run SET "
		                      "current_transaction_invalidation_policy = 'SYNTACTIC_ERRORS_DO_NOT_INVALIDATE' in it "
		                      "before calling redshift_query; for a transaction BEGIN opens, setting "
		                      "default_transaction_invalidation_policy to that value before BEGIN does the same.%s",
		                      reason, AbortedTransactionAdvice(state), next, trailer);
	}
	// USABLE is hedged: DuckDB may have expanded the statement and aborted the transaction anyway, as
	// a PIVOT without an IN list does, and that state is private. So the message says to run SELECT 1
	// to find out, since after such an abort a COMMIT rolls back without an error.
	throw BinderException(
	    "%s Unless DuckDB had already run part of this statement as a separate statement that wrote, as a PIVOT "
	    "without an IN list does, the DuckDB transaction is still usable. If you are not sure, run SELECT 1: if it "
	    "fails with \"Current transaction is aborted\", run ROLLBACK %s; otherwise commit or roll back the "
	    "transaction. Then %s%s",
	    reason, GATE_COMMIT_ROLLS_BACK, next, trailer);
}

//! The gate at bind. With the option on, it still refuses inside an aborted
//! transaction, which DuckDB refuses only after bind would have described the call on Redshift.
void RefuseInDuckDBTransactionAtBind(ClientContext &context) {
	if (!InExplicitTransaction(context)) {
		return;
	}
	if (!AllowedInDuckDBTransaction(context)) {
		ThrowRefusalAtBindInDuckDBTransaction(context, GATE_REASON, GATE_RUN_OUTSIDE, true);
	}
	if (ValidChecker::IsInvalidated(MetaTransaction::Get(context))) {
		throw BinderException("redshift_query cannot run in a DuckDB transaction an earlier error aborted, so this "
		                      "call was not described or run on Redshift. Run ROLLBACK %s, then run the statement "
		                      "again.",
		                      GATE_COMMIT_ROLLS_BACK);
	}
}

//! Refuses a call at bind on the handle `handle_id`, named `handle`, that the open DuckDB transaction
//! attached, since its rollback would close the handle's Redshift session and any Redshift
//! transaction on it.
void RefuseHandleAttachedInDuckDBTransaction(ClientContext &context, const string &handle, idx_t handle_id) {
	if (!InExplicitTransaction(context) || !RedshiftClientState::Get(context).AttachedInTransaction(handle_id)) {
		return;
	}
	// The transaction is not aborted here: RefuseInDuckDBTransactionAtBind refused that first, so of
	// the three wordings only the two for a transaction this refusal aborts or leaves usable arise.
	ThrowRefusalAtBindInDuckDBTransaction(
	    context,
	    StringUtil::Format("redshift_query cannot run on attach handle %s because the open DuckDB transaction "
	                       "attached it, which %s does not change: a rollback of the transaction would detach the "
	                       "handle and close its Redshift session, rolling back any Redshift transaction open on it.",
	                       SQLQuotedIdentifier::ToString(handle), ALLOW_IN_DUCKDB_TRANSACTION),
	    // A condition, not a COMMIT step: after ROLLBACK under autocommit the ATTACH again commits by
	    // itself, and a COMMIT would fail with "no transaction is active".
	    "attach the handle again if a rollback undid its ATTACH, and run redshift_query on the handle once that "
	    "ATTACH has committed.",
	    false);
}

//! The gate at execution, for a plan bound outside the transaction: an executor error, which aborts
//! the transaction under every policy, since other parts of the statement may already have written
//! and DuckDB has no statement-level rollback.
void RefuseInDuckDBTransactionAtExecution(ClientContext &context) {
	if (!InExplicitTransaction(context) || AllowedInDuckDBTransaction(context)) {
		return;
	}
	throw ExecutorException("%s It was refused as the statement started to run, after other parts of the statement "
	                        "may have written, so this error has aborted the DuckDB transaction. Run ROLLBACK %s, "
	                        "then %s %s",
	                        GATE_REASON, GATE_COMMIT_ROLLS_BACK, GATE_RUN_OUTSIDE, GATE_REDSHIFT_TRANSACTION);
}

//! What a refusal at bind of a CONNECT left the DuckDB transaction in, and what to run: a sentence
//! without its full stop, for the caller to continue. CONNECT is a single statement
//! DuckDB never expands, so USABLE is certain here, not hedged as for a call a user wrote.
string ConnectRefusalAdvice(RefusedTransaction state) {
	if (state == RefusedTransaction::USABLE) {
		return "The DuckDB transaction is still usable: commit or roll it back";
	}
	return AbortedTransactionAdvice(state);
}

//! What the CONNECT the physical plan `plan` runs names, or null when it runs none. A prepared
//! statement keeps the plan DuckDB runs: an EXECUTE holds the plan of the statement it was bound to
//! (PhysicalExecute::plan), whatever its name means now, and EXPLAIN ANALYZE runs its child, as does
//! the PhysicalVerifyVector DuckDB wraps a plan in under debug_verify_vector. Each step reaches a
//! plan prepared earlier or a child, so the walk ends.
optional_ptr<const ConnectInfo> PlannedConnect(const PhysicalOperator &plan) {
	reference<const PhysicalOperator> current = plan;
	while (true) {
		auto &op = current.get();
		switch (op.type) {
		case PhysicalOperatorType::CONNECT:
			return op.Cast<PhysicalConnect>().info.get();
		case PhysicalOperatorType::EXECUTE:
			current = op.Cast<PhysicalExecute>().plan;
			break;
		case PhysicalOperatorType::EXPLAIN_ANALYZE:
		case PhysicalOperatorType::VERIFY_VECTOR:
			if (op.children.size() != 1) {
				return nullptr;
			}
			current = op.children[0].get();
			break;
		default:
			return nullptr;
		}
	}
}

//! What the CONNECT `plan` runs names, or null when it runs none: one bound now, alone or under
//! EXPLAIN ANALYZE, or one in the physical plan of the prepared statement an EXECUTE runs. Only the
//! C++ API can prepare a CONNECT; a plain EXPLAIN runs nothing.
optional_ptr<const ConnectInfo> FindConnect(const LogicalOperator &plan) {
	if (plan.type == LogicalOperatorType::LOGICAL_EXPLAIN) {
		auto &explain = plan.Cast<LogicalExplain>();
		if (explain.explain_type != ExplainType::EXPLAIN_ANALYZE || plan.children.size() != 1) {
			return nullptr;
		}
		return FindConnect(*plan.children[0]);
	}
	if (plan.type == LogicalOperatorType::LOGICAL_CONNECT) {
		return plan.Cast<LogicalConnect>().info.get();
	}
	if (plan.type == LogicalOperatorType::LOGICAL_EXECUTE) {
		// Without a physical plan, DuckDB bound the statement again, and this hook saw that bind.
		auto &prepared = *plan.Cast<LogicalExecute>().prepared;
		if (prepared.physical_plan) {
			return PlannedConnect(prepared.physical_plan->Root());
		}
	}
	return nullptr;
}

//! Refuses CONNECT to a Redshift handle inside an explicit DuckDB transaction, at bind, before DuckDB
//! connects: every statement it would route would meet the transaction gate, and
//! the option does not change that, since while CONNECT-ed DuckDB sends COMMIT and
//! ROLLBACK to Redshift. Other targets are DuckDB's or another extension's.
void RefuseConnectInDuckDBTransaction(ClientContext &context, LogicalOperator &plan) {
	if (!InExplicitTransaction(context)) {
		return;
	}
	auto found = FindConnect(plan);
	if (!found) {
		return;
	}
	auto &info = *found;
	// The string form attaches its target only when it runs, and the handle's ATTACH refuses it then;
	// LOCAL, a bare CONNECT and an external resource name no handle.
	if (info.name.empty() || info.target_is_local || info.name_is_string_literal || info.external_resource) {
		return;
	}
	auto target = DatabaseManager::Get(context).GetDatabase(info.name);
	if (!target || target->GetCatalog().GetCatalogType() != REDSHIFT_ATTACH_TYPE) {
		return;
	}
	throw BinderException("CONNECT: cannot CONNECT to attach handle %s while a DuckDB transaction is open, because "
	                      "DuckDB COMMIT or ROLLBACK does not control Redshift operations. %s, %s",
	                      SQLQuotedIdentifier::ToString(info.name.GetIdentifierName()),
	                      ConnectRefusalAdvice(TransactionAfterRefusalAtBind(context)), CONNECT_GATE_AGAIN);
}

//! The refusal of a statement CONNECT routed inside a DuckDB transaction an earlier error aborted; it
//! ends the CONNECT. `advice` says what to run.
string ConnectEndedRefusal(const string &handle, const string &advice) {
	return StringUtil::Format("%s This DuckDB connection is no longer CONNECT-ed to attach handle %s: while "
	                          "CONNECT-ed, ROLLBACK would go to Redshift. %s, %s %s",
	                          CONNECT_GATE_REASON, SQLQuotedIdentifier::ToString(handle), advice, CONNECT_GATE_AGAIN,
	                          CONNECT_GATE_REDSHIFT_TRANSACTION);
}

//! The refusal of a statement CONNECT routed inside a usable DuckDB transaction, which keeps the
//! CONNECT. `when`, if not empty, is a sentence on when it was refused.
string ConnectKeptRefusal(const string &handle, const string &when) {
	return StringUtil::Format("%s%s The DuckDB transaction is still usable, and this DuckDB connection is still "
	                          "CONNECT-ed to attach handle %s. Run DISCONNECT, then COMMIT or ROLLBACK, %s %s",
	                          CONNECT_GATE_REASON, when, SQLQuotedIdentifier::ToString(handle), CONNECT_GATE_AGAIN,
	                          CONNECT_GATE_REDSHIFT_TRANSACTION);
}

//! The transaction gates for a call CONNECT made. CONNECT inside a DuckDB transaction is refused
//! (RefuseConnectInDuckDBTransaction), so these are reached only when a client API turns autocommit
//! off after CONNECT, which runs no statement. The option does not apply. DuckDB runs a statement
//! routed while CONNECT-ed outside the DuckDB transaction (ClientContext::SubmitStatement's
//! outside_local_transaction), so the refusal leaves the transaction as it was. When an earlier error
//! had aborted it, the CONNECT is ended first, so that ROLLBACK runs in DuckDB: while CONNECT-ed DuckDB
//! sends ROLLBACK and COMMIT to Redshift. Otherwise the CONNECT is kept, so that a statement typed for
//! Redshift is refused rather than run in DuckDB, and DISCONNECT still works.
//!
//! The gate at bind.
void RefuseConnectedInDuckDBTransactionAtBind(ClientContext &context, const string &handle) {
	if (!InExplicitTransaction(context)) {
		return;
	}
	if (!ValidChecker::IsInvalidated(MetaTransaction::Get(context))) {
		throw BinderException(ConnectKeptRefusal(handle, string()));
	}
	context.DisconnectFromCatalog();
	throw BinderException(ConnectEndedRefusal(handle, AbortedTransactionAdvice(RefusedTransaction::ALREADY_ABORTED)));
}

//! The gate at execution, for a call bound before autocommit went off: a statement submitted, then
//! fetched after a client API turned autocommit off.
void RefuseConnectedInDuckDBTransactionAtExecution(ClientContext &context, const string &handle) {
	if (!InExplicitTransaction(context)) {
		return;
	}
	if (!ValidChecker::IsInvalidated(MetaTransaction::Get(context))) {
		throw ExecutorException(ConnectKeptRefusal(handle, " It was refused as the statement started to run."));
	}
	context.DisconnectFromCatalog();
	throw ExecutorException(ConnectEndedRefusal(handle, AbortedTransactionAdvice(RefusedTransaction::ALREADY_ABORTED)));
}

string RequiredArgument(const Value &value, const char *what) {
	if (value.IsNull()) {
		// Checked rather than left to StringValue::Get, which raises an INTERNAL error on
		// a NULL -- the category DuckDB reserves for engine bugs, with a stack trace. A
		// user passing NULL has not found an engine bug, and a table function's arguments
		// are folded constants, so NULL is reachable from plain SQL.
		throw BinderException("redshift_query: the %s argument cannot be NULL", what);
	}
	return StringValue::Get(value);
}

//! The `params` named argument, as strings in order. Neither the list nor an element can be NULL,
//! because Execute binds strings and has no way to bind NULL.
vector<string> ParameterStrings(const Value &params) {
	if (params.IsNull()) {
		throw BinderException("redshift_query: params cannot be NULL. Omit it, or pass a list of strings such as "
		                      "params := ['a', 'b']");
	}
	vector<string> strings;
	auto &elements = ListValue::GetChildren(params);
	for (idx_t i = 0; i < elements.size(); i++) {
		if (elements[i].IsNull()) {
			throw BinderException("redshift_query: parameter %llu is NULL, and NULL parameters are not supported. "
			                      "Write NULL in the Redshift SQL instead of binding it",
			                      static_cast<uint64_t>(i + 1));
		}
		strings.push_back(StringValue::Get(elements[i]));
	}
	return strings;
}

unique_ptr<FunctionData> RedshiftQueryBind(ClientContext &context, TableFunctionBindInput &input,
                                           vector<LogicalType> &return_types, vector<Identifier> &names) {
	auto connected_call = ConnectedCall(input.ref);
	if (connected_call) {
		RefuseConnectedInDuckDBTransactionAtBind(context, connected_call->handle);
	} else {
		RefuseInDuckDBTransactionAtBind(context);
	}
	auto bind_data = make_uniq<RedshiftQueryBindData>();
	bind_data->routed_by_connect = connected_call != nullptr;
	bind_data->handle = RequiredArgument(input.inputs[0], "handle");
	if (bind_data->handle.empty()) {
		throw BinderException("redshift_query: the handle argument is empty. Pass the name of a Redshift handle "
		                      "attached with ATTACH '' AS <name> (TYPE %s, ...)",
		                      REDSHIFT_ATTACH_TYPE);
	}
	bind_data->sql = RequiredArgument(input.inputs[1], "SQL");
	if (bind_data->sql.empty()) {
		throw BinderException("redshift_query: the SQL argument is empty");
	}

	// DuckDB has already cast params to VARCHAR[], so a value of another type arrives as its text.
	auto params = input.named_parameters.find("params");
	if (params != input.named_parameters.end()) {
		bind_data->params = ParameterStrings(params->second);
	}
	// A possible-state-loss report that has not reached the user comes first, before the handle is
	// resolved or a connection opened: one this query raised, when a state another extension registers
	// had DuckDB plan the statement again, whatever handle the statement now names, or one an earlier
	// query raised about a handle under this name (RedshiftClientState::HoldLossReport).
	auto &client_state = RedshiftClientState::Get(context);
	client_state.ThrowUnseenLossReport(context, bind_data->handle);
	const auto caller = bind_data->Caller();
	const auto connected_handle_id = connected_call ? optional_idx(connected_call->handle_id) : optional_idx();
	// The shape is learned by preparing the statement, never by executing it, so binding the
	// statement any number of times runs it zero times. DuckDB needs the column
	// count and names before execution in order to plan the surrounding query. A refusal, including
	// Redshift's refusal of a multi-statement string, is the client's, passed through, and a lost
	// connection is handled as for any lost connection (RedshiftConnectionCalls::TryDescribe). The
	// bind's pin ends as soon as Describe has.
	//
	// False when the handle was retired partway, by DETACH, ATTACH OR REPLACE or a rollback on another
	// DuckDB connection: between the lookup and the pin, or before the pair had a connection, its
	// first or Describe's replacement.
	const auto try_bind = [&]() {
		// From what the name means now. The bind data keeps only the handle's ID, never the handle.
		auto db = ResolveForBind(context, caller, connected_handle_id);
		bind_data->handle_id = db->GetCatalog().Cast<RedshiftCatalog>().HandleId();
		if (connected_handle_id.IsValid() && bind_data->handle_id != connected_handle_id.GetIndex()) {
			// Another handle under the name since CONNECT, after a DETACH, ATTACH OR REPLACE or rolled-back
			// ATTACH: the statement was typed for the earlier handle, which may reach another Redshift.
			throw BinderException("CONNECT: attach handle %s was detached or replaced, or an ATTACH under its name "
			                      "was rolled back, since this DuckDB connection CONNECT-ed to it, and the handle "
			                      "attached under the name now is another, so this statement was not run on Redshift. "
			                      "Run DISCONNECT, then CONNECT to the handle attached now.",
			                      SQLQuotedIdentifier::ToString(bind_data->handle));
		}
		RefuseHandleAttachedInDuckDBTransaction(context, bind_data->handle, bind_data->handle_id);
		auto pin = RedshiftConnectionPin::TryPin(context, std::move(db));
		if (!pin) {
			// Handle retired since the lookup, so bind fails for this attempt.
			return false;
		}
		// A bind outside a query, such as a relation's, an extracted plan's or GetTableNames', skips
		// DuckDB's open-result check, so it can meet an earlier call's result here.
		// The pin's destruction ends the use.
		if (!pin->BeginUse(RedshiftConnectionUse::DESCRIBE)) {
			// Never a routed call's: DuckDB refuses a new query while an earlier one's open result holds the
			// DuckDB connection, and otherwise ends the earlier one first (ClientContext::InitialCleanup).
			throw BinderException("redshift_query: attach handle %s is still streaming the result of an earlier "
			                      "redshift_query call on this DuckDB connection, and each handle runs one Redshift "
			                      "statement at a time, so this call was not described or run on Redshift. Read "
			                      "that result to its end or close it, then try again.",
			                      SQLQuotedIdentifier::ToString(bind_data->handle));
		}
		NoteRunInDuckDBTransaction(context);
		RedshiftConnectionCalls calls(context, pin->Catalog(), caller);
		return calls.TryDescribe(bind_data->sql, bind_data->shape);
	};

	// Try to describe the query shape.
	if (!try_bind()) {
		// Once more, as a statement bound after the retirement would be: ResolveForBind fails when
		// nothing has the name now, so a retry reaches the pin only on a handle attached again since.
		if (!try_bind()) {
			// That handle was retired partway too. Fail rather than chase a name that keeps changing.
			throw BinderException("%s: attach handle %s was detached or replaced, or an ATTACH under its name was "
			                      "rolled back, twice while this statement was being bound, so it was not described "
			                      "on Redshift. Run the statement again.",
			                      caller.Name(), SQLQuotedIdentifier::ToString(bind_data->handle));
		}
	}
	// Throws a BinderException for a column DuckDB cannot represent, or a shape that breaks the
	// transport contract. Neither touches the Redshift connection, which the pair keeps.
	auto schema = RedshiftTypeMapping::ToOutputSchema(bind_data->shape, caller);
	return_types = std::move(schema.types);
	names = std::move(schema.names);
	return std::move(bind_data);
}

unique_ptr<GlobalTableFunctionState> RedshiftQueryInitGlobal(ClientContext &context, TableFunctionInitInput &input) {
	auto &bind_data = input.bind_data->Cast<RedshiftQueryBindData>();
	const auto caller = bind_data.Caller();
	if (!bind_data.fence_checked) {
		// Only a plan changed after the fence reaches here, so it may hold a second scan on this handle.
		// Fail closed, before any transport call.
		throw ExecutorException("%s: the call on attach handle %s was not checked for a second use of the handle, so "
		                        "it was not run%s Something, such as another extension, changed the statement's plan "
		                        "after redshift_query checked it. Report this as a fault in the Redshift extension or "
		                        "in how it works with the other extensions loaded.",
		                        caller.Name(), SQLQuotedIdentifier::ToString(bind_data.handle),
		                        caller.OtherCallsMayHaveRun());
	}
	// DuckDB starts a scan again within one statement only to run it again, as a recursive CTE's
	// recursive term does on each iteration. The fence refuses every such plan it can see;
	// this refuses one that something rewrote after the fence, such as another extension's
	// planner hook, before a second Execute, so the statement runs at most once.
	auto statement = context.transaction.GetActiveQuery();
	if (bind_data.started_in_statement == statement) {
		// redshift_query's wording even for a routed call, whose statement is a single scan: only another
		// extension rebuilding its plan around a repeated scan could start it again.
		throw ExecutorException(
		    "redshift_query: DuckDB started the Redshift statement on attach handle %s a second "
		    "time in the same DuckDB statement, which redshift_query does not allow. It ran once and "
		    "was not run again, so it may already have applied its effects on Redshift; check them "
		    "before running the statement again. Stage its result in a DuckDB temporary table in a "
		    "separate statement.",
		    SQLQuotedIdentifier::ToString(bind_data.handle));
	}
	bind_data.started_in_statement = statement;
	// Bind checked too, but a statement bound outside a transaction can execute inside one: one
	// prepared, or one submitted and fetched after a client API turned autocommit off.
	if (bind_data.routed_by_connect) {
		RefuseConnectedInDuckDBTransactionAtExecution(context, bind_data.handle);
	} else {
		RefuseInDuckDBTransactionAtExecution(context);
	}
	// A statement prepared before a possible-state-loss report about a handle under its name, which has
	// not reached the user, would otherwise run on a new session without it.
	RedshiftClientState::Get(context).ThrowUnseenLossReport(context, bind_data.handle);
	auto db = ResolveBoundHandle(context, bind_data);
	// The pin narrows the window after that check to the moment between DuckDB taking the name out of
	// its list and OnDetach or the rollback hook (RedshiftConnectionPin::TryPin): a DETACH, ATTACH OR
	// REPLACE or rollback on another DuckDB connection that has retired the handle since
	// ResolveBoundHandle refuses the pin, and one that comes after the pin lets the scan run on,
	// keeping the pair's connection until the state is destroyed.
	auto pin = RedshiftConnectionPin::TryPin(context, std::move(db));
	if (!pin) {
		RefuseRetiredHandle(context, bind_data);
	}
	// Until the result ends, waiting out a bind's Describe.
	if (!pin->BeginUse(RedshiftConnectionUse::RESULT)) {
		// Another execution's result on the pair is open. The same-handle fence, and DuckDB's one open
		// result per DuckDB connection, leave this to a defect.
		throw ExecutorException("%s: attach handle %s already has a Redshift statement running on this DuckDB "
		                        "connection, so %s was not run%s Report this as a fault in the Redshift extension.",
		                        caller.Name(), SQLQuotedIdentifier::ToString(bind_data.handle), caller.ThisCall(),
		                        caller.OtherCallsMayHaveRun());
	}
	// Checked again: the bind BeginUse waited for may have lost the connection with state held, and
	// this call would otherwise run on its replacement without the report.
	RedshiftClientState::Get(context).ThrowUnseenLossReport(context, bind_data.handle);
	auto state = make_uniq<RedshiftQueryGlobalState>(context, std::move(pin), caller);
	if (bind_data.shape.returns_rows) {
		// Built before Execute, so remapping the columns and allocating the writer cannot fail
		// after the statement may have run. Bind has already refused any column it cannot map.
		state->writer = make_uniq<RedshiftCellWriter>(bind_data.shape.columns);
	}
	NoteRunInDuckDBTransaction(context);
	// The one Execute this scan makes. It is never repeated: a failure ends the scan, and the
	// second-start refusal above keeps the scan from starting again in this statement.
	state->result = state->calls.Execute(bind_data.sql, bind_data.params, bind_data.shape);
	if (!state->result) {
		// The handle was retired after the pin, before the pair had a usable connection: none was
		// opened, or one that finished opening after the retirement was closed unused.
		RefuseRetiredHandle(context, bind_data);
	}
	return std::move(state);
}

//! Fetches the result's next batch into `state.batch`. Returns false once the result is drained,
//! and ends the pair's use, as a failed fetch does before it rethrows.
bool FetchNextBatch(RedshiftQueryGlobalState &state) {
	bool fetched;
	try {
		fetched = state.calls.NextBatch(state.result, state.batch);
	} catch (...) {
		// A failed NextBatch has destroyed the result, so a bind on the pair may run. The pin would end
		// the use too, but only when DuckDB tears the query down, after the error has reached it.
		state.pin->EndUse();
		throw;
	}
	if (!fetched) {
		// The client's result stops being open here, though DuckDB destroys it later.
		state.drained = true;
		state.pin->EndUse();
	}
	return fetched;
}

//! The scan. DuckDB calls it every time it wants another chunk of rows, and stops once a call
//! returns an empty chunk. Each call fills at most STANDARD_VECTOR_SIZE rows; rows left in the
//! current batch stay in the global state for the next call.
void RedshiftQueryFunction(ClientContext &context, TableFunctionInput &data, DataChunk &output) {
	auto &bind_data = data.bind_data->Cast<RedshiftQueryBindData>();
	auto &state = data.global_state->Cast<RedshiftQueryGlobalState>();
	// Reset returns the chunk to zero rows and each child vector to flat and full-width,
	// which is what lets the writes below address any row < STANDARD_VECTOR_SIZE.
	output.Reset();

	if (!bind_data.shape.returns_rows) {
		if (state.rowcount_emitted) {
			return;
		}
		const auto count = state.result->RowCount();
		// Negative means the client had no count to give, which is a NULL rather than a
		// number the user would otherwise believe.
		output.data[0].SetValue(0, count < 0 ? Value(LogicalType::BIGINT) : Value::BIGINT(count));
		output.SetChildCardinality(1);
		state.rowcount_emitted = true;
		return;
	}

	idx_t emitted = 0;
	while (emitted < STANDARD_VECTOR_SIZE) {
		if (state.row_in_batch >= state.batch.rows.size()) {
			if (state.drained) {
				break;
			}
			// Release the used-up rows before fetching, so NextBatch starts from an empty batch
			// whatever the client does with the one it is given.
			state.batch.rows.clear();
			state.row_in_batch = 0;
			if (!FetchNextBatch(state)) {
				break;
			}
			// A legal empty batch means "ask again", not "finished".
			continue;
		}
		// Checks the row's width and each cell's value kind, and converts every cell to the
		// type bind declared for its column. A refusal ends the query, and with it the result;
		// the pair keeps its session, since the server reported no error.
		state.writer->WriteRow(state.batch.rows[state.row_in_batch], output, emitted);
		state.row_in_batch++;
		emitted++;
	}
	// After Reset every vector has size 0, and the writer's stores do not change it.
	// SetChildCardinality sets each vector's size and the chunk's count, so this line is what
	// publishes the rows. Omitting it yields an empty result rather than an error, which is why it
	// is last here.
	output.SetChildCardinality(emitted);
}

//===--------------------------------------------------------------------===//
// The same-handle statement fence
//===--------------------------------------------------------------------===//

//! Walks a bound logical plan for the fence. It refuses a second scan on one handle;
//! a scan a recursive term would start again on each iteration while the last result is still
//! open, directly or through a CTE that inlining would move there; and a scan in DuckDB's
//! PIVOT-values statements. It also keeps CTE inlining from copying a scan.
class FenceWalk {
public:
	void Walk(LogicalOperator &op, bool in_recursive_term, bool in_pivot_values) {
		if (op.type == LogicalOperatorType::LOGICAL_EXPLAIN &&
		    op.Cast<LogicalExplain>().explain_type != ExplainType::EXPLAIN_ANALYZE) {
			// A plain EXPLAIN only prints its plan, so nothing in it is refused or marked; EXPLAIN ANALYZE
			// runs it. DuckDB still optimizes the plan it prints, so its CTEs are pinned all the same.
			explaining = true;
		}
		if (op.type == LogicalOperatorType::LOGICAL_GET) {
			auto &get = op.Cast<LogicalGet>();
			if (get.function.function == RedshiftQueryFunction) {
				CheckScan(get.bind_data->Cast<RedshiftQueryBindData>(), in_recursive_term, in_pivot_values);
			}
		}
		in_pivot_values = in_pivot_values || IsPivotValues(op);
		if (op.type == LogicalOperatorType::LOGICAL_CTE_REF) {
			VisitCTERef(op.Cast<LogicalCTERef>(), in_recursive_term);
		}
		const bool holds_subquery = WalkExpressionSubqueries(op, in_recursive_term, in_pivot_values);
		if (op.type == LogicalOperatorType::LOGICAL_MATERIALIZED_CTE) {
			WalkMaterializedCTE(op.Cast<LogicalMaterializedCTE>(), in_recursive_term, in_pivot_values);
			return;
		}
		// A recursive CTE's first child is the anchor, which runs once; the second is the recursive
		// term, which runs once per iteration. A USING KEY CTE binds to the same operator. PREPARE's
		// statement and EXECUTE's rebound statement are their operators' children. A cached plan that
		// EXECUTE runs as it is was checked when DuckDB bound it. Decorrelating a subquery, after
		// binding, can copy the plan it reads from into a second reference (DelimJoinCTERewriter): a
		// dependent join's outer side, or the inputs of an operator that still holds the subquery.
		for (idx_t i = 0; i < op.children.size(); i++) {
			bool recursive_term = op.type == LogicalOperatorType::LOGICAL_RECURSIVE_CTE && i > 0;
			bool decorrelated = holds_subquery || (op.type == LogicalOperatorType::LOGICAL_DEPENDENT_JOIN && i == 0);
			if (decorrelated) {
				decorrelated_depth++;
			}
			Walk(*op.children[i], in_recursive_term || recursive_term, in_pivot_values);
			if (decorrelated) {
				decorrelated_depth--;
			}
		}
	}

	//! Marks every scan the walk reached checked. Called only once the whole walk has passed, so a
	//! refused plan leaves its scans unmarked, and a later DuckDB path that ran it without walking it
	//! would meet the backstop. A plan that passed has exactly one scan per handle
	//! in `seen`.
	void MarkChecked() {
		for (auto &entry : seen) {
			entry.second.get().fence_checked = true;
		}
	}

private:
	//! A reference to a CTE that inlining could move a scan through. It is refused in a recursive term,
	//! and otherwise counts as a scan reached, so a CTE whose definition reads this one is inlinable too.
	void VisitCTERef(LogicalCTERef &ref, bool in_recursive_term) {
		auto entry = inlinable_ctes.find(ref.cte_index);
		if (entry == inlinable_ctes.end()) {
			return;
		}
		if (in_recursive_term && !explaining) {
			throw ErrorRecursiveTerm(entry->second.handle);
		}
		// Decorrelation may copy a reference in the plan a subquery reads from (see Walk), so one there
		// counts twice.
		entry->second.references += decorrelated_depth > 0 ? 2 : 1;
		reached_handle = entry->second.handle;
		reached++;
	}

	//! At bind, a subquery in some places, such as an outer join's condition, is still a plan inside an
	//! expression; DuckDB moves it into the operator tree afterwards. Returns whether `op` holds one.
	bool WalkExpressionSubqueries(LogicalOperator &op, bool in_recursive_term, bool in_pivot_values) {
		bool held = false;
		LogicalOperatorVisitor::EnumerateExpressions(op, [&](unique_ptr<Expression> *expression) {
			ExpressionIterator::EnumerateExpression(*expression, [&](Expression &child) {
				if (child.GetExpressionClass() == ExpressionClass::BOUND_SUBQUERY) {
					auto &subquery = child.Cast<BoundSubqueryExpression>().SubqueryMutable();
					if (subquery.plan) {
						held = true;
						Walk(*subquery.plan, in_recursive_term, in_pivot_values);
					}
				}
			});
		});
		return held;
	}

	//! The definition comes first, and is read through references in the body. A definition that
	//! reaches a scan makes the CTE inlinable unless it is AS MATERIALIZED, which DuckDB never inlines.
	//! An inlinable CTE the body reads more than once is pinned AS MATERIALIZED: inlining it would copy
	//! its definition, the scan with it, into each reference. DuckDB keeps such a CTE materialized when
	//! the copy fails, but learns that the copy failed only by catching RedshiftQuerySerialize's exception
	//! by type, which fails for the loadable on macOS: it links its own copy of DuckDB and exports only
	//! its entry point, and macOS matches a catch by type identity. Pinned, the CTE is never copied, on
	//! any platform. The walk reads the setting only before the body, and DuckDB discards a refused plan,
	//! so the pin need not wait for the whole walk to pass.
	void WalkMaterializedCTE(LogicalMaterializedCTE &cte, bool in_recursive_term, bool in_pivot_values) {
		auto before = reached;
		Walk(*cte.children[0], in_recursive_term, in_pivot_values);
		if (reached > before && cte.materialize != CTEMaterialize::CTE_MATERIALIZE_ALWAYS) {
			inlinable_ctes.emplace(cte.table_index, InlinableCTE {reached_handle, 0});
		}
		Walk(*cte.children[1], in_recursive_term, in_pivot_values);
		auto entry = inlinable_ctes.find(cte.table_index);
		if (entry != inlinable_ctes.end() && entry->second.references > 1) {
			cte.materialize = CTEMaterialize::CTE_MATERIALIZE_ALWAYS;
		}
	}

	//! Whether `op` is a CREATE TYPE __pivot_enum_<uuid> AS ENUM statement DuckDB generates ahead of a
	//! PIVOT without an IN list or with IN (subquery). A user's
	//! type named in that form, the prefix and a UUID as UUID::ToString writes it, counts as one.
	static bool IsPivotValues(LogicalOperator &op) {
		if (op.type != LogicalOperatorType::LOGICAL_CREATE_TYPE) {
			return false;
		}
		auto &info = op.Cast<LogicalCreate>().info;
		if (!info) {
			return false;
		}
		static const string PREFIX = "__pivot_enum_";
		auto &name = info->GetQualifiedName().Name().GetIdentifierName();
		if (!StringUtil::StartsWith(name, PREFIX)) {
			return false;
		}
		auto suffix = name.substr(PREFIX.size());
		hugeint_t uuid;
		return UUID::FromString(suffix, uuid) && UUID::ToString(uuid) == suffix;
	}

	static BinderException ErrorRecursiveTerm(const string &handle) {
		return BinderException("redshift_query cannot run in the recursive part of a recursive CTE, because DuckDB "
		                       "would run the Redshift statement on attach handle %s once per iteration. Stage its "
		                       "result in a DuckDB temporary table in a separate statement, or, if the recursive "
		                       "part reads it through a CTE defined outside the recursive CTE, declare that CTE AS "
		                       "MATERIALIZED. No Redshift statement was executed.",
		                       SQLQuotedIdentifier::ToString(handle));
	}

	void CheckScan(RedshiftQueryBindData &bind_data, bool in_recursive_term, bool in_pivot_values) {
		if (explaining) {
			reached_handle = bind_data.handle;
			reached++;
			return;
		}
		if (in_recursive_term) {
			throw ErrorRecursiveTerm(bind_data.handle);
		}
		if (in_pivot_values) {
			throw BinderException(
			    "redshift_query cannot be read by the statements DuckDB runs ahead of a PIVOT to find "
			    "its values, as it does for a PIVOT without an IN list or with IN (subquery). Without "
			    "an IN list there is one per pivoted column, each reading the PIVOT's source, so the "
			    "Redshift statement on attach handle %s would run once for each and again for the "
			    "PIVOT. List the PIVOT's values with IN (...), or stage the result in a DuckDB "
			    "temporary table in a separate statement. No Redshift statement was executed.",
			    SQLQuotedIdentifier::ToString(bind_data.handle));
		}
		reached_handle = bind_data.handle;
		reached++;
		// Keyed on the handle bind resolved, not on the name as written, so 'rs' and 'RS' are one handle
		// and an alias attached again is another. A logical plan is a tree, so the walk reaches each
		// scan once, and a handle seen before is a second scan.
		if (seen.emplace(bind_data.handle_id, bind_data).second) {
			return;
		}
		throw BinderException("redshift_query cannot use attach handle %s more than once in the same DuckDB "
		                      "statement. Each attach handle has one Redshift connection. Stage one result in a "
		                      "DuckDB temporary table in a separate statement, or use different attach handles. No "
		                      "Redshift statement was executed.",
		                      SQLQuotedIdentifier::ToString(bind_data.handle));
	}

	//! Each handle ID seen so far, and the bind data of the scan that uses it, which MarkChecked marks.
	//! The plan being walked owns the bind data, and outlives the walk.
	unordered_map<idx_t, reference<RedshiftQueryBindData>> seen;
	//! A CTE DuckDB may inline whose definition reaches a scan: the handle of a scan it reaches, as
	//! written, for the refusal's message, and the references to it the walk has reached.
	struct InlinableCTE {
		string handle;
		idx_t references;
	};
	unordered_map<TableIndex, InlinableCTE> inlinable_ctes;
	//! The scans, and references to inlinable CTEs, reached so far, and the handle of the latest.
	idx_t reached = 0;
	string reached_handle;
	//! Whether the walk is inside a plain EXPLAIN.
	bool explaining = false;
	//! How many plans that decorrelation may copy the walk is in (see Walk).
	idx_t decorrelated_depth = 0;
};

//===--------------------------------------------------------------------===//
// Copy refusal
//===--------------------------------------------------------------------===//

//! Refuses to serialize a redshift_query scan, which is how DuckDB copies a plan: a copy would bind the
//! call again after the fence checked the plan. Must stay a
//! NotImplementedException: DuckDB's plan check reads that type from the error, and the window self-join
//! rewrite and the common-subplan optimizer catch any exception. CTE inlining catches the type itself,
//! which fails for the loadable on macOS, so the fence pins the CTEs that inlining would copy a scan
//! through (FenceWalk::WalkMaterializedCTE), and CTE inlining does not rely on that catch for them.
void RedshiftQuerySerialize(Serializer &, const optional_ptr<FunctionData>, const BoundTableFunction &) {
	throw NotImplementedException("redshift_query scans are not copied: a copy would describe the Redshift statement "
	                              "again and add a second scan on its attach handle");
}

//! Never reached, since no redshift_query scan is serialized. DuckDB calls the serialize callback only
//! when a deserialize callback is set too.
unique_ptr<FunctionData> RedshiftQueryDeserialize(Deserializer &, BoundTableFunction &) {
	throw NotImplementedException("redshift_query scans cannot be deserialized");
}

//! The same-handle statement fence, run on every statement DuckDB binds, in every planning pass. Its
//! refusal is a BinderException, which ends planning, so nothing executes; a plan another extension
//! changes afterwards meets execution's backstop. It also
//! refuses a CONNECT to a Redshift handle inside a DuckDB transaction.
void RedshiftQueryFence(PlannerExtensionInput &input, BoundStatement &statement) {
	if (!statement.plan) {
		return;
	}
	RefuseConnectInDuckDBTransaction(input.context, *statement.plan);
	FenceWalk walk;
	walk.Walk(*statement.plan, false, false);
	walk.MarkChecked();
}

} // namespace

string ConnectedStatementText(const string &sql) {
	// Compared whole, so the shell's query with a terminator, or any other text, passes unchanged.
	return sql == DUCKDB_SHELL_PROMPT_QUERY ? DUCKDB_SHELL_PROMPT_QUERY_SENT : sql;
}

unique_ptr<TableRef> MakeConnectedQuery(RedshiftCatalog &catalog, const string &sql) {
	const auto &handle = catalog.GetName().GetIdentifierName();
	vector<unique_ptr<ParsedExpression>> arguments;
	arguments.push_back(ConstantExpression::String(handle));
	arguments.push_back(ConstantExpression::String(ConnectedStatementText(sql)));
	auto ref = make_uniq<TableFunctionRef>();
	// Qualified, so a macro a user names redshift_query, which the search path finds first, gets none.
	ref->function = make_uniq<FunctionExpression>(
	    QualifiedName(Identifier(SYSTEM_CATALOG), Identifier(DEFAULT_SCHEMA), Identifier("redshift_query")),
	    std::move(arguments));
	ref->bind_info = make_shared_ptr<RedshiftConnectedCall>(handle, catalog.HandleId());
	return std::move(ref);
}

void RegisterRedshiftQuery(ExtensionLoader &loader) {
	// Registered before the function, so a statement that another DuckDB connection is already
	// binding when this one loads the extension, and that binds redshift_query once it exists, is
	// checked: DuckDB reads the registered extensions when binding ends.
	PlannerExtension fence;
	fence.post_bind_function = RedshiftQueryFence;
	auto &config = DBConfig::GetConfig(loader.GetDatabaseInstance());
	PlannerExtension::Register(config, std::move(fence));
	// A debug option, so duckdb_settings() lists it only when asked for debug settings: an escape hatch
	// for a workload that needs it, which docs/settings.md documents as one. Registered before the
	// function, so it exists whenever a call can bind.
	config.AddExtensionOption(ALLOW_IN_DUCKDB_TRANSACTION,
	                          "Lets redshift_query run inside an explicit DuckDB transaction, where DuckDB COMMIT or "
	                          "ROLLBACK does not control its Redshift operations: a ROLLBACK does not undo them. Still "
	                          "refused on a handle the open transaction attached.",
	                          LogicalType::BOOLEAN, Value::BOOLEAN(false), OnSetAllowInDuckDBTransaction,
	                          SetScope::SESSION, /*is_debug=*/true);

	TableFunction redshift_query("redshift_query", {LogicalType::VARCHAR, LogicalType::VARCHAR}, RedshiftQueryFunction,
	                             RedshiftQueryBind, RedshiftQueryInitGlobal);
	// Named for DuckDB's messages, such as the candidate list when a call does not match. Both stay
	// positional-only.
	redshift_query.GetSignature().GetParameter(0).SetName("handle");
	redshift_query.GetSignature().GetParameter(1).SetName("sql");
	// Parameters bind positionally, in whatever marker form the client accepts, and are never
	// substituted into the SQL text.
	redshift_query.GetSignature().WithTypedKwargs(
	    "options", [](TypedKwargs &options) { options.Add("params", LogicalType::LIST(LogicalType::VARCHAR)); });
	// Never copied or merged, so each call in the SQL is one scan (see RedshiftQuerySerialize).
	redshift_query.SetSerializeCallback(RedshiftQuerySerialize);
	redshift_query.SetDeserializeCallback(RedshiftQueryDeserialize);
	loader.RegisterFunction(redshift_query);
}

} // namespace duckdb
