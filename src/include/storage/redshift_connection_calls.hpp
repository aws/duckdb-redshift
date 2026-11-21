//===----------------------------------------------------------------------===//
//                         DuckDB - Redshift
//
// storage/redshift_connection_calls.hpp
//
// The transport calls of a redshift_query bind or execution, and what it does when one fails.
//===----------------------------------------------------------------------===//

#pragma once

#include "duckdb/common/common.hpp"
#include "duckdb/common/error_data.hpp"
#include "duckdb/common/optional_ptr.hpp"
#include "redshift_messages.hpp"
#include "redshift_transport.hpp"

namespace duckdb {

class ClientContext;
class RedshiftCatalog;

//! The transport calls one bind or one execution of redshift_query makes on its pair's Redshift
//! connection, each with the rules for when it fails. The session is the server's,
//! which the connection carries; this object lasts one bind or one execution.
//!
//! - A connection the client reports lost, by an IOException, is discarded from the connection map,
//!   so the pair's next use opens a new one, and the call that met the loss reports it. When the
//!   lost session may have held state, RedshiftClientState also keeps that report until a query
//!   ends with it (HoldLossReport), so it reaches the user even when DuckDB shows another error.
//!   Whether the session it carried may have held state is the connection's SessionHoldsState()
//!   answer, read before the connection is destroyed, while the client still answers as it did before
//!   the failure; this layer never computes it, and never reads the SQL to guess it.
//! - Only Describe is tried again, once, on a replacement connection, and only when the lost session
//!   held no state. A statement is never executed again.
//! - A refusal or statement failure the client reports as a session Redshift's answer left unusable
//!   (`redshift_error` = `session_unusable`) discards the connection too, but nothing is tried again,
//!   Describe included: the call fails with its own type, saying the session was replaced, what that
//!   loses when the session may have held state, and whether the statement may have run. Every call
//!   checks for it in one place, RethrowUnlessConnectionLost. Its report is kept as a lost
//!   connection's is, only when the session may have held state, so when DuckDB swallows a bind's
//!   error and binds the statement again (the shell's agent mode does), a session that held none is
//!   replaced and the statement described on the new one.
//! - Any other statement refusal (BinderException), a user error (InvalidInputException) and any
//!   other statement failure (ExceptionType::EXECUTOR) keep the connection and pass through unchanged,
//!   except that one carrying SQLSTATE 25P02 gains the guidance for an aborted Redshift transaction.
//! - Execute's refusal of a statement whose shape has changed since bind (`redshift_error` =
//!   `shape_changed`) keeps the connection too, and becomes the error: this call did
//!   not run, though other calls in the same DuckDB statement may have run, and a prepared statement
//!   must be prepared again.
//!
//! Exceptions are told apart by their ExceptionType, never by class or message, as the transport
//! header asks.
class RedshiftConnectionCalls {
public:
	//! The calls `context` makes on `catalog`'s handle, for `caller`, whom the messages are written
	//! for. The caller holds a RedshiftConnectionPin on the pair for as long as this is used. Opens
	//! nothing until a call needs the connection.
	RedshiftConnectionCalls(ClientContext &context, RedshiftCatalog &catalog, RedshiftCaller caller);

	//! Sets `shape` to the shape of `sql`, from Describe on the pair's connection, which this opens if
	//! the pair has none. When Describe loses the connection, and its session held no state, describes
	//! `sql` once more on a replacement connection, logging before the replacement
	//! opens. When the session held state, fails saying that state may have been lost.
	//! Either way, once Describe has finished it logs how long each step took. Log lines that
	//! cannot be written are dropped.
	//!
	//! Returns false, having described nothing, when the handle has been retired and the pair would
	//! need a new connection, its first or the replacement, which opens none
	//! (RedshiftConnectionMap::GetConnection): the caller then starts again from the name.
	bool TryDescribe(const string &sql, RedshiftStatementShape &shape);
	//! Runs `sql` once, with `params`, on the pair's connection, which this opens if the pair has
	//! none, passing `expected_shape`, the shape bind described, for the client to compare with the
	//! statement's before it runs. Returns null, having run nothing, when the pair has none and the
	//! handle has been retired. Never runs it again: when the connection is lost, fails saying the
	//! statement was not retried and may have applied its effects. When the client refuses it
	//! because its shape has changed, fails saying it did not run.
	unique_ptr<RedshiftResult> Execute(const string &sql, const vector<string> &params,
	                                   const RedshiftStatementShape &expected_shape);
	//! Fills `batch`, which the caller passes empty, from `result`, which this object's Execute
	//! returned; the rows a lost connection's message counts are the ones each call put in it. When
	//! NextBatch fails, destroys `result` before anything else: the client needs every result
	//! destroyed before its connection, and a connection that stays keeps no
	//! half-read result. Then discards the connection if it was lost.
	bool NextBatch(unique_ptr<RedshiftResult> &result, RedshiftBatch &batch);

private:
	//! Points `connection` to the pair's connection, which the connection map opens if the pair has
	//! none. Every lookup goes through here, so DiscardLostConnection, NextBatch's included, destroys
	//! the connection that was lost. `opened`, if given, is set to whether this call opened it. Sets
	//! `connection` to null when the pair has none and the handle has been retired
	//! (RedshiftConnectionMap::GetConnection). For a call CONNECT routed, a failed open is rethrown with
	//! its type and extra_info after CONNECT's words and the advice to DISCONNECT before fixing it; an
	//! interrupt, or a fatal, internal or out-of-memory error, passes through unchanged.
	void LookUpConnection(optional_ptr<bool> opened = nullptr);
	//! Destroys the connection `connection` points to, which the client reported lost or left its
	//! session unusable, so the pair's next use opens a new one, and clears `connection`. Returns its
	//! SessionHoldsState() answer, read first: the client still answers as it did before the failure.
	bool DiscardLostConnection();
	//! Throws the error for a call whose connection the client reported lost with `loss`; `what` says
	//! where the call was and what that means for the statement. When the lost session may have held
	//! state, the error is kept too (RedshiftClientState::HoldLossReport), so it reaches the user
	//! before a session under the handle's name is used, even when DuckDB reports another error for
	//! the query instead: it reports only a query's first.
	[[noreturn]] void ThrowConnectionLost(const ErrorData &loss, const string &what, bool held_state);
	//! For a transport call that threw `error`: returns when the client reported the connection lost, which
	//! the caller handles. Anything else is a refusal, a user error or a statement failure on a live
	//! connection: one the client reports as leaving the session unusable is ThrowIfSessionUnusable's, so
	//! every call of every phase meets that check here; any other the pair keeps, so it is thrown again,
	//! unchanged but for the 25P02 guidance (ThrowIfTransactionAborted). Only a lost connection gets no
	//! guidance: its session, and any aborted transaction with it, is gone. `at_execution` says the call is
	//! Execute or NextBatch. Call it only from the handler that caught `error`, after the connection was
	//! looked up, so that `throw;` rethrows it.
	void RethrowUnlessConnectionLost(const ErrorData &error, bool at_execution);
	//! When `error` says the session is unusable (`redshift_error` = `session_unusable`), destroys the connection
	//! and throws, with `error`'s type, the error that says the session was replaced, kept as ThrowConnectionLost
	//! keeps its report when the session may have held state. `at_execution` says the call is Execute or NextBatch,
	//! where a refusal's message adds that other calls may have run. Returns for any other error. Called only by
	//! RethrowUnlessConnectionLost.
	void ThrowIfSessionUnusable(const ErrorData &error, bool at_execution);

	//! The DuckDB connection running the bind or execution, which outlives this object.
	ClientContext &context;
	//! The handle's catalog, which the caller's pin keeps alive for as long as this object is used.
	RedshiftCatalog &catalog;
	//! Whom the messages are written for, and the handle as they name it.
	RedshiftCaller caller;
	//! Points to the pair's Redshift connection, which the database's connection map owns, once a
	//! call has looked it up; null before then and once DiscardLostConnection has destroyed it. The map
	//! keeps it as long as this object is used: the DuckDB connection cannot close during its own
	//! query, the caller's pin holds it through a retirement, and the pin keeps the catalog, whose
	//! destruction closes the handle's connections.
	optional_ptr<RedshiftConnection> connection;
	//! The rows NextBatch has returned, for the message when it then loses the connection.
	idx_t rows_returned = 0;
};

//! Throws `error`, an error other than a lost connection from a transport call for `caller`, again
//! with the guidance appended when its `sqlstate` is 25P02. The exception keeps its
//! type, its extra_info and the client's message. Reads only the structured `sqlstate` field (the
//! error format in redshift_transport.hpp), never the message or the SQL, whatever the exception's type;
//! returns when the SQLSTATE is another, or absent. Declared here so a C++ test can give it a
//! statement failure, which the client never raises with 25P02.
void ThrowIfTransactionAborted(const ErrorData &error, const RedshiftCaller &caller);

//! Throws the error when `error`, from Execute for `caller`, is the client's refusal
//! of a statement whose shape is not the one bind described: its `extra_info` has `redshift_error` =
//! `shape_changed`. Recognizes it by that field alone, never by type, SQLSTATE or message, and keeps
//! the exception's type and extra_info; returns for any other error. The message says this call ran
//! nothing; for a call the user wrote, also that other calls in the statement may have run, and to
//! prepare a prepared statement again. Declared here so a C++ test can give it a refusal directly.
void ThrowIfShapeChanged(const ErrorData &error, const RedshiftCaller &caller);

} // namespace duckdb
