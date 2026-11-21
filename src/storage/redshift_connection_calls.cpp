//===----------------------------------------------------------------------===//
// The transport calls a bind or an execution of redshift_query makes on its pair's Redshift
// connection, and what it does when one fails. See storage/redshift_connection_calls.hpp.
//===----------------------------------------------------------------------===//

#include "storage/redshift_connection_calls.hpp"

#include "duckdb/common/error_data.hpp"
#include "duckdb/common/exception.hpp"
#include "duckdb/common/optional_idx.hpp"
#include "duckdb/common/sql_identifier.hpp"
#include "duckdb/common/string_util.hpp"
#include "duckdb/main/client_context.hpp"
#include "redshift_context_log.hpp"
#include "redshift_messages.hpp"
#include "storage/redshift_catalog.hpp"
#include "storage/redshift_client_state.hpp"

#include <chrono>

namespace duckdb {

//===--------------------------------------------------------------------===//
// RedshiftConnectionCalls
//===--------------------------------------------------------------------===//

namespace {

using Clock = std::chrono::steady_clock;

idx_t MicrosecondsSince(Clock::time_point start) {
	return static_cast<idx_t>(std::chrono::duration_cast<std::chrono::microseconds>(Clock::now() - start).count());
}

//! The error for a call for `caller` whose connection the client reported lost with `error`. `what`
//! says where the call was and what that means for the statement. `held_state` is the connection's
//! SessionHoldsState() answer: when it is true the session may have held state, and the message says
//! that state may have been lost, never what it was. The exception keeps the
//! client's extra_info, its SQLSTATE included, and quotes its message, which the client redacts.
IOException ErrorConnectionLost(const ErrorData &error, const RedshiftCaller &caller, const string &what,
                                bool held_state) {
	auto message = StringUtil::Format("%s: the Redshift connection of attach handle %s was lost %s", caller.Name(),
	                                  SQLQuotedIdentifier::ToString(caller.handle), what);
	if (held_state) {
		message +=
		    StringUtil::Format(" The session it carried may have held state, which may now be lost: the next %s "
		                       "on this handle from this DuckDB connection starts a new, empty Redshift session.",
		                       caller.routed_by_connect ? "statement" : "redshift_query");
	}
	return IOException(error.ExtraInfo(), message + " The client reported: " + error.RawMessage());
}

//! Whether `error`, from a transport call, is the client's report that Redshift's answer left the session unusable:
//! its `redshift_error` is `session_unusable` (redshift_transport.hpp). Read from that field alone, never from the
//! message.
bool SessionUnusable(const ErrorData &error) {
	auto &extra_info = error.ExtraInfo();
	auto redshift_error = extra_info.find("redshift_error");
	return redshift_error != extra_info.end() && redshift_error->second == "session_unusable";
}

//! The error for a call for `caller` whose session the client reported unusable with `error`, after the connection
//! was discarded. A refusal (BinderException, at Describe or at Execute's prepare) ran nothing; a statement failure
//! may have run, so the message says which, with REDSHIFT_ALREADY_EXECUTED as a lost connection's report does. A
//! refusal at execution (`at_execution`) also says that other calls in the same DuckDB statement may have run, as
//! the shape refusal does. When the session may have held state (`held_state`, its SessionHoldsState() answer), it
//! says that what lived in it is gone, naming temporary tables, SET values and any open transaction, since
//! SessionHoldsState() cannot tell them apart; a session that held none lost nothing, so the message leaves that out.
//! The exception keeps the client's type and extra_info, its SQLSTATE included, and ends with the client's message,
//! the server's line and the SQLSTATE line last. The base class, because it takes extra_info for every type.
Exception ErrorSessionUnusable(const ErrorData &error, const RedshiftCaller &caller, bool at_execution,
                               bool held_state) {
	const auto handle = SQLQuotedIdentifier::ToString(caller.handle);
	string outcome = REDSHIFT_ALREADY_EXECUTED;
	if (error.Type() == ExceptionType::BINDER) {
		// A statement CONNECT routes is the only call in its DuckDB statement.
		outcome = at_execution && !caller.routed_by_connect
		              ? "This call was not run on Redshift" + caller.OtherCallsMayHaveRun()
		              : "The statement was not run on Redshift.";
	}
	const string lost =
	    held_state ? "Anything that lived in the old session is gone: temporary tables, SET values and any open "
	                 "transaction. "
	               : "";
	return Exception(
	    error.ExtraInfo(), error.Type(),
	    StringUtil::Format("%s: Redshift session error on attach handle %s: the session cannot be used any "
	                       "more. The extension discarded it; the next %s on %s opens a new session. %s%s "
	                       "The client reported: %s",
	                       caller.Name(), handle, caller.routed_by_connect ? "statement" : "redshift_query", handle,
	                       lost, outcome, error.RawMessage()));
}

//! What a bind whose Describe lost its connection logs once its Describe has finished, recovered or
//! not: how long each phase took, leaving out the phases that did not occur.
struct DescribeLossTimings {
	optional_idx initial_open_us;
	optional_idx failed_describe_us;
	optional_idx replacement_open_us;
	optional_idx retried_describe_us;

	void Log(ClientContext &context, const string &handle, Clock::time_point start, bool recovered) const {
		string fields;
		auto add = [&](const char *name, const optional_idx &value) {
			if (value.IsValid()) {
				fields += StringUtil::Format(" %s=%llu", name, static_cast<uint64_t>(value.GetIndex()));
			}
		};
		add("initial_open_us", initial_open_us);
		add("failed_describe_us", failed_describe_us);
		add("replacement_open_us", replacement_open_us);
		add("retried_describe_us", retried_describe_us);
		add("total_us", optional_idx(MicrosecondsSince(start)));
		LogInfo(context,
		        StringUtil::Format("redshift_query: Describe lost the Redshift connection of attach handle "
		                           "%s; bind timings:%s outcome=%s",
		                           SQLQuotedIdentifier::ToString(handle), fields, recovered ? "recovered" : "failed"));
	}
};

} // namespace

void ThrowIfTransactionAborted(const ErrorData &error, const RedshiftCaller &caller) {
	// Redshift refuses every statement in an aborted transaction until it ends, so without the
	// guidance the user meets the same refusal on every call. The real client raises 25P02 only as a
	// refusal, so reading `sqlstate` whatever the exception's type is a defence.
	auto &extra_info = error.ExtraInfo();
	auto sqlstate = extra_info.find("sqlstate");
	if (sqlstate == extra_info.end() || sqlstate->second != "25P02") {
		return;
	}
	// The base class, because it takes extra_info for every type: ExecutorException has no
	// constructor that does, and would drop the SQLSTATE.
	throw Exception(extra_info, error.Type(),
	                error.RawMessage() + "\n" +
	                    StringUtil::Format("The Redshift transaction on attach handle %s is aborted, so Redshift "
	                                       "refuses every statement until it ends. Run %s to end it. Work done in that "
	                                       "transaction since the error is lost; running COMMIT instead would also "
	                                       "roll it back.",
	                                       SQLQuotedIdentifier::ToString(caller.handle),
	                                       caller.RunOnRedshift("ROLLBACK")));
}

void ThrowIfShapeChanged(const ErrorData &error, const RedshiftCaller &caller) {
	auto &extra_info = error.ExtraInfo();
	auto redshift_error = extra_info.find("redshift_error");
	if (redshift_error == extra_info.end() || redshift_error->second != "shape_changed") {
		return;
	}
	// DuckDB can reuse a prepared statement's bind at EXECUTE (PreparedStatementData::RequireRebind), so
	// such a statement keeps the columns it was bound with and is refused at every EXECUTE. One that
	// DuckDB binds again describes the statement again, and reaches this only if the statement
	// changes between that Describe and Execute.
	const auto handle = SQLQuotedIdentifier::ToString(caller.handle);
	if (caller.routed_by_connect) {
		// The statement CONNECT routed is the only call in it, and is bound again when typed again.
		throw Exception(extra_info, error.Type(),
		                StringUtil::Format("CONNECT: the statement on attach handle %s now returns different columns "
		                                   "than when it was bound, so it was not run on Redshift. Run it again. The "
		                                   "client reported: %s",
		                                   handle, error.RawMessage()));
	}
	throw Exception(extra_info, error.Type(),
	                StringUtil::Format("redshift_query: the statement on attach handle %s now returns different "
	                                   "columns than when it was bound, so this call was not run on Redshift, but %s "
	                                   "If it is a prepared statement, prepare it again. The client reported: %s",
	                                   handle, REDSHIFT_OTHER_CALLS_MAY_HAVE_RUN, error.RawMessage()));
}

RedshiftConnectionCalls::RedshiftConnectionCalls(ClientContext &context_p, RedshiftCatalog &catalog_p,
                                                 RedshiftCaller caller_p)
    : context(context_p), catalog(catalog_p), caller(std::move(caller_p)) {
}

void RedshiftConnectionCalls::LookUpConnection(optional_ptr<bool> opened) {
	try {
		connection = catalog.GetRedshiftConnection(context, opened);
	} catch (std::exception &ex) {
		ErrorData error(ex);
		const auto type = error.Type();
		if (!caller.routed_by_connect || ShouldRethrowErrorAsIs(type)) {
			throw;
		}
		// The fix for an open that fails, such as recreating a secret, is DuckDB SQL, which a CONNECT-ed
		// DuckDB connection sends to Redshift: say to DISCONNECT first. The client's message, its
		// SQLSTATE line last, follows as it gave it, and its type and extra_info stay.
		throw Exception(error.ExtraInfo(), type,
		                StringUtil::Format("CONNECT: attach handle %s could not open its Redshift connection, so the "
		                                   "statement was not run on Redshift. Run DISCONNECT before any DuckDB "
		                                   "statement that fixes the cause, such as one that recreates a secret: "
		                                   "while CONNECT-ed, DuckDB sends every statement but DISCONNECT to "
		                                   "Redshift. The client reported: %s",
		                                   SQLQuotedIdentifier::ToString(caller.handle), error.RawMessage()));
	}
}

bool RedshiftConnectionCalls::DiscardLostConnection() {
	D_ASSERT(connection);
	auto &lost = *connection;
	const bool held_state = lost.SessionHoldsState();
	connection = nullptr;
	catalog.DiscardRedshiftConnection(context, lost);
	return held_state;
}

void RedshiftConnectionCalls::ThrowIfSessionUnusable(const ErrorData &error, bool at_execution) {
	if (!SessionUnusable(error)) {
		return;
	}
	// Discarded, and never retried or run again, whatever the session held: the user decides what to run next.
	const bool held_state = DiscardLostConnection();
	ErrorData report(ErrorSessionUnusable(error, caller, at_execution, held_state));
	if (held_state) {
		RedshiftClientState::Get(context).HoldLossReport(context, caller.handle, report);
	}
	report.Throw();
}

void RedshiftConnectionCalls::RethrowUnlessConnectionLost(const ErrorData &error, bool at_execution) {
	if (error.Type() == ExceptionType::IO) {
		return;
	}
	// Every refusal and statement failure of every call comes here, so none keeps a connection the client closed.
	ThrowIfSessionUnusable(error, at_execution);
	ThrowIfTransactionAborted(error, caller);
	throw; // NOLINT: rethrows the exception the calling handler caught
}

void RedshiftConnectionCalls::ThrowConnectionLost(const ErrorData &loss, const string &what, bool held_state) {
	ErrorData report(ErrorConnectionLost(loss, caller, what, held_state));
	if (held_state) {
		RedshiftClientState::Get(context).HoldLossReport(context, caller.handle, report);
	}
	report.Throw();
}

bool RedshiftConnectionCalls::TryDescribe(const string &sql, RedshiftStatementShape &shape) {
	const auto call_start = Clock::now();
	// Filled in only if Describe loses its connection, for the log line that says how long each step
	// took.
	DescribeLossTimings timings;
	// Whether this call opened the pair's connection, rather than reusing its idle one; only for the
	// open time in the log.
	bool opened_now = false;
	// The pair's connection, opened now if the pair has none; none on a retired handle.
	LookUpConnection(&opened_now);
	if (!connection) {
		return false;
	}
	if (opened_now) {
		timings.initial_open_us = MicrosecondsSince(call_start);
	}
	const auto first_describe_start = Clock::now();
	// The client's error for a lost connection, kept for the message; anything else is thrown again.
	ErrorData lost_connection_error;
	try {
		shape = connection->Describe(sql);
		return true;
	} catch (std::exception &ex) {
		lost_connection_error = ErrorData(ex);
		// A session left unusable is never described again on a replacement: the user learns that it was replaced.
		RethrowUnlessConnectionLost(lost_connection_error, false);
	}
	timings.failed_describe_us = MicrosecondsSince(first_describe_start);
	// Read from the lost connection before it is destroyed: the client still answers as before the
	// loss.
	const bool lost_session_held_state = DiscardLostConnection();
	if (lost_session_held_state) {
		timings.Log(context, caller.handle, call_start, false);
		ThrowConnectionLost(lost_connection_error,
		                    "while describing the statement, so the statement was not run on Redshift.", true);
	}

	// Describe runs nothing, and the lost session held no state, so a new connection is equivalent:
	// describe the statement once more on one, silently but for the log.
	// Not on a retired handle, which opens none: the caller starts again from the name, as a
	// statement bound after the retirement would, which for a session that held no state is silent
	// too. Checked before the log line, which would say a replacement opens; GetConnection checks
	// again, for a retirement that comes while the replacement opens.
	if (catalog.IsRetired()) {
		timings.Log(context, caller.handle, call_start, false);
		return false;
	}
	LogInfo(context, StringUtil::Format("redshift_query: the Redshift connection of attach handle %s was lost during "
	                                    "Describe, which runs no statement, and its session held no state, so a "
	                                    "replacement connection is being opened to describe the statement again: "
	                                    "replacement attempt 1/1. Opening it may take another complete "
	                                    "connection-open budget.",
	                                    SQLQuotedIdentifier::ToString(caller.handle)));
	const auto replacement_open_start = Clock::now();
	try {
		LookUpConnection();
	} catch (...) {
		// The open's own error, credentials or reachability, passes through as LookUpConnection gives it:
		// the lost session held no state, so nothing about it is worth reporting.
		timings.replacement_open_us = MicrosecondsSince(replacement_open_start);
		timings.Log(context, caller.handle, call_start, false);
		throw;
	}
	timings.replacement_open_us = MicrosecondsSince(replacement_open_start);
	if (!connection) {
		// The handle was retired while the replacement opened, which was closed unused.
		timings.Log(context, caller.handle, call_start, false);
		return false;
	}
	const auto replacement_describe_start = Clock::now();
	try {
		shape = connection->Describe(sql);
	} catch (std::exception &ex) {
		timings.retried_describe_us = MicrosecondsSince(replacement_describe_start);
		timings.Log(context, caller.handle, call_start, false);
		lost_connection_error = ErrorData(ex);
		RethrowUnlessConnectionLost(lost_connection_error, false);
		// Only one replacement: losing it too fails the bind.
		const bool replacement_held_state = DiscardLostConnection();
		ThrowConnectionLost(lost_connection_error,
		                    "while describing the statement, and so was the replacement connection opened to describe "
		                    "it again, so the statement was not run on Redshift.",
		                    replacement_held_state);
	}
	timings.retried_describe_us = MicrosecondsSince(replacement_describe_start);
	timings.Log(context, caller.handle, call_start, true);
	return true;
}

unique_ptr<RedshiftResult> RedshiftConnectionCalls::Execute(const string &sql, const vector<string> &params,
                                                            const RedshiftStatementShape &expected_shape) {
	LookUpConnection();
	if (!connection) {
		return nullptr;
	}
	try {
		return connection->Execute(sql, params, expected_shape);
	} catch (std::exception &ex) {
		ErrorData error(ex);
		// Nothing ran, and the connection stays.
		ThrowIfShapeChanged(error, caller);
		RethrowUnlessConnectionLost(error, true);
		const bool held_state = DiscardLostConnection();
		// The statement may have run, so it is never run again.
		ThrowConnectionLost(error,
		                    StringUtil::Format("while executing the statement, and the statement was not retried. %s",
		                                       REDSHIFT_ALREADY_EXECUTED),
		                    held_state);
	}
}

bool RedshiftConnectionCalls::NextBatch(unique_ptr<RedshiftResult> &result, RedshiftBatch &batch) {
	// Empty, so the rows counted below are the ones this call put in it, whether the client
	// replaces the batch's rows or adds to them.
	D_ASSERT(batch.rows.empty());
	try {
		const bool more = result->NextBatch(batch);
		rows_returned += batch.rows.size();
		return more;
	} catch (std::exception &ex) {
		ErrorData error(ex);
		// Whatever the failure, the result goes first, with any rows it still holds: before its
		// connection, which the client requires, and so a connection that
		// stays has no active result.
		result.reset();
		RethrowUnlessConnectionLost(error, true);
		const bool held_state = DiscardLostConnection();
		// The statement ran, so it is never run again. A streaming caller may already
		// hold rows of it, so the message says the result may be incomplete, not that it was thrown
		// away: a loss in the client's final drain comes after every row. It counts the rows read from
		// Redshift, which DuckDB may not have handed on yet.
		const auto where =
		    rows_returned == 0
		        ? string("while reading the statement's result, before any row arrived")
		        : StringUtil::Format("after %llu %s of the statement's result had been read from "
		                             "Redshift, so the result may be incomplete",
		                             static_cast<uint64_t>(rows_returned), rows_returned == 1 ? "row" : "rows");
		ThrowConnectionLost(
		    error, StringUtil::Format("%s, and the statement was not retried. %s", where, REDSHIFT_ALREADY_EXECUTED),
		    held_state);
	}
}

} // namespace duckdb
