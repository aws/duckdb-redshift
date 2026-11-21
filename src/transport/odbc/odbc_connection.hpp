//===----------------------------------------------------------------------===//
//                         DuckDB - Redshift
//
// src/transport/odbc/odbc_connection.hpp
//
// RedshiftConnection over one driver connection handle.
//===----------------------------------------------------------------------===//

#pragma once

#include "duckdb/common/common.hpp"
#include "duckdb/common/limits.hpp"
#include "odbc/odbc_diagnostics.hpp"
#include "odbc/odbc_errors.hpp"
#include "odbc/odbc_process_state.hpp"
#include "odbc/odbc_statement.hpp"
#include "odbc/target_hints.hpp"
#include "odbc_driver.hpp"
#include "redshift_transport.hpp"

#ifdef _WIN32
#include "odbc/trust_store.hpp"
#endif

#include <chrono>
#include <functional>

namespace duckdb {

struct TestEndpoints;

//! One Redshift connection, carrying one Redshift session, to the Serverless workgroup or the
//! provisioned cluster its configuration names. Not copyable and not thread-safe: callers use each
//! connection from one thread at a time.
class OdbcConnection : public RedshiftConnection {
public:
	//! Validates the settings and builds the connection string before any driver call, then connects
	//! and sets up the session. On Windows the string, and the trust file it
	//! names, follow the driver source. Frees what it allocated before throwing.
	//! `credentials`, given exactly when `config.aws_secret` is set, is used only to build the
	//! connection string and is not kept.
	OdbcConnection(const RedshiftConfig &config, shared_ptr<RedshiftLogSink> log_sink,
	               optional_ptr<const RedshiftAwsCredentials> credentials = nullptr);
	//! The same, but connects to `test_endpoints` and never reads the test variables: a test's seam,
	//! so a build without the override checks where a test build connects. OpenRedshiftConnection
	//! uses the constructor above.
	OdbcConnection(const RedshiftConfig &config, shared_ptr<RedshiftLogSink> log_sink,
	               const TestEndpoints &test_endpoints,
	               optional_ptr<const RedshiftAwsCredentials> credentials = nullptr);
	//! Runs the teardown, logging each step that fails; the environment is the
	//! process's and stays. Never throws.
	~OdbcConnection() override;
	OdbcConnection(const OdbcConnection &) = delete;
	OdbcConnection &operator=(const OdbcConnection &) = delete;

	//! Prepares `sql` without executing it and reports each column's name and ClassifyColumn's type.
	//! Keeps the prepared statement for an Execute of the same text with no parameters that follows,
	//! when its shape runs with SQLExecDirect. Its refusals and failures are those odbc_errors.hpp
	//! describes.
	RedshiftStatementShape Describe(const string &sql) override;
	//! Prepares `sql`, or with no `params` takes the statement Describe kept for it, binds `params` in
	//! order as text to its markers, never into the SQL, and runs it, draining a statement with no
	//! result columns before it returns. Before anything is bound or run, refuses a prepared shape
	//! that differs from `expected_shape` with `shape_changed`. With no `params` and no FLOAT, DOUBLE
	//! or size-0 VARCHAR column it runs with SQLExecDirect, and an executed shape that differs or has
	//! such a column fails with an executor error saying it may have run; otherwise with SQLExecute.
	//! Refuses and fails as odbc_errors.hpp describes.
	unique_ptr<RedshiftResult> Execute(const string &sql, const vector<string> &params,
	                                   const RedshiftStatementShape &expected_shape) override;
	//! False until Execute has run a statement on this connection, whatever its outcome. A transport
	//! failure leaves it as it was.
	bool SessionHoldsState() const override;
	//! The cluster's identifier, or the workgroup's host, derived or a test build's, then the
	//! configuration's description, also once the connection is closed.
	string Target() const override;

private:
	//! What a connect is given before the driver is: the connection's target, the hints a failed
	//! connect to it carries, and its settings.
	struct ConnectSettings {
		string target;
		TargetHints hints;
		//! On Windows, built only once the trust file is placed.
		string connection_string;
#ifdef _WIN32
		TrustSource trust;
		std::function<string(const TrustSource &)> build_connection_string;
#endif
	};
	//! The constructor's steps from the region check to the connection string, for `config` and
	//! `test_endpoints`, making no driver call; on Windows, to the trust read. Throws what
	//! ValidateAndResolveTarget, ResolveTrustSource and BuildConnectionString throw.
	static ConnectSettings PrepareSettings(const RedshiftConfig &config, RedshiftLogSink &log_sink,
	                                       const TestEndpoints &test_endpoints,
	                                       optional_ptr<const RedshiftAwsCredentials> credentials);
	//! Obtains the driver and connects with `settings`.
	OdbcConnection(ConnectSettings settings, shared_ptr<RedshiftLogSink> &&log_sink);

	//! Throws IOException, before any handle is allocated, when a variable the driver refuses is set
	//! or `connection_string` lacks a keyword the driver's overridden-environment list names.
	void RefuseWeakeningEnvironment(const string &connection_string) const;
	//! Connects on a handle of its own per attempt, starting another only after a timeout on a target
	//! that resumes from idle, within the attempt limit and the time budget, and logging that it
	//! waits. Throws the classified connect failure with the attempts made. Called once, from the
	//! constructor.
	void Open(const string &connection_string);
	//! Allocates the connection handle from `environment` and enables autocommit.
	void AllocateConnection(SQLHANDLE environment);
	//! Asks for Redshift autocommit, before the connect. A refusal throws
	//! ThrowPreConnectFailure's IOException, not a connect failure.
	void EnableAutocommit();
	//! One SQLDriverConnect with no output buffer. Returns false when it fails,
	//! leaving its records on the handle.
	bool Connect(const string &connection_string);
	//! Runs the setup statements that pin the session's output settings, in
	//! order, leaving SessionHoldsState() false. Called once, from the constructor, which closes the
	//! connection when it throws.
	void SetUpSession();
	//! Prepares and executes the setup statement `sql` on a statement handle of its own, which it
	//! frees, logging each call's informational records. Throws ThrowSetupFailure's IOException when a
	//! call fails.
	void RunSetupStatement(const string &sql);
	//! Logs the records a statement call that returned `rc` left on `statement`, when it succeeded
	//! with information, with the redaction, which replaces each of `bound_values` of 8
	//! bytes or more.
	void LogStatementWarnings(SQLRETURN rc, SQLHANDLE statement, const vector<string> &bound_values);
	//! Reads the records the failed setup call left on `failed_on`, judges whether they lost the
	//! connection, and throws the session setup IOException for the answer.
	[[noreturn]] void ThrowSetupFailure(DiagnosticHandle failed_on, const string &sql);
	//! The teardown: the open statements, the rollback, the disconnect and the
	//! connection handle, in that order. A failed step is logged and does not stop the next. Clears
	//! each handle it frees.
	void Close() noexcept;
	//! The teardown's last step, also run between connect attempts: frees the connection handle and
	//! clears it, logging a failed free and abandoning the handle.
	void FreeConnectionHandle() noexcept;
	//! Logs the SQLSTATEs the failed teardown `step` left on the connection handle.
	void LogTeardownFailure(const char *step) noexcept;
	//! The same for the records on `handle`, ending the line with `outcome`.
	void LogTeardownFailure(const char *step, SQLHANDLE handle, const char *outcome) noexcept;
	//! Joins the open result's producer, frees its statement, unless that result has freed it, and
	//! tells every result Execute returned that its connection has closed.
	void CloseOpenResult() noexcept;
	//! Throws InvalidInputException, making no driver call, while a result Execute returned is open.
	void RefuseWhileResultOpen() const;
	//! Throws IOException, making no driver call, when a transport failure, or a session left unusable,
	//! has closed this connection.
	void RefuseClosed() const;
	//! Closes this connection when a result's failure lost it, or left its session unusable.
	void CloseIfResultLostConnection();
	//! Called after a call in `phase` on the established connection failed, with the handle that call
	//! left its records on and the caller's parameter values in bind order, for redaction. Reads
	//! those records. When ConnectionLost judges that they lost the connection, closes it and throws
	//! the transport failure for `phase`; otherwise throws the statement failure for `phase`, and when
	//! the records show a session left unusable (RecordsShowUnusableSession), closes it first.
	[[noreturn]] void ThrowCallFailure(DiagnosticHandle failed_on, const vector<string> &bound_values,
	                                   StatementPhase phase);
	//! A new statement handle on this connection. The caller frees it. A failed allocation is the
	//! allocation-phase statement failure, or the transport failure when the connection is lost.
	SQLHANDLE AllocateStatement();
	//! Sends `sql` to Redshift to be prepared on the server, without running it.
	void Prepare(SQLHANDLE statement, const string &sql);
	//! The prepared statement's parameter marker count. Throws the parameters-phase failure, or the
	//! transport failure when the connection is lost.
	idx_t CountParameters(SQLHANDLE statement);
	//! Binds `values[i]` as the text input of marker i+1. The driver reads the buffers when the
	//! statement runs, so `values` must outlive the run.
	void BindParameters(SQLHANDLE statement, vector<string> &values);
	//! The statement's result column count. Throws the failure of `phase`, DESCRIBE before the
	//! statement runs and EXECUTE after, or the transport failure when the connection is lost.
	idx_t CountColumns(SQLHANDLE statement, StatementPhase phase);
	//! The rows the executed `statement`, which has no result columns, affected, as RowsAffected
	//! reads them, once its further results are drained. Throws the further-result-set failure; when
	//! the drain fails, the fetch-phase failure, or the transport failure when the connection is
	//! lost, with `bound_values` for redaction.
	int64_t FinishWithoutResultSet(SQLHANDLE statement, const vector<string> &bound_values);
	//! A statement's shape, prepared or run, and the column size the driver reported for each of its
	//! columns, which a bound VARCHAR or CHAR buffer is sized from.
	struct PreparedColumns {
		RedshiftStatementShape shape;
		vector<SQLULEN> sizes;
	};
	//! Runs `sql` on the prepared `statement` with SQLExecDirect, which replaces the prepared one.
	SQLRETURN ExecuteDirect(SQLHANDLE statement, const string &sql);
	//! The columns of the result SQLExecDirect produced, to bind from. Throws the executor error that
	//! says the statement may have run when they differ from `checked`, the shape checked before it ran,
	//! or hold a column whose text that protocol may change.
	PreparedColumns DescribeExecuted(SQLHANDLE statement, const RedshiftStatementShape &checked);
	//! The statement's shape, whether it returns rows and each column described, with each column's
	//! reported size. A failure is that of `phase`, as CountColumns throws it.
	PreparedColumns ShapeOf(SQLHANDLE statement, StatementPhase phase);
	//! One column of a statement, prepared or run, and the column size the driver reported for it.
	struct DescribedColumn {
		RedshiftColumn column;
		SQLULEN size;
	};
	//! Column `number` of `statement`, prepared or run, classified from SQLDescribeCol's answer and its
	//! type name. Refuses a column name that does not fit its buffer rather than cutting it.
	DescribedColumn DescribeColumn(SQLHANDLE statement, SQLUSMALLINT number, StatementPhase phase);
	//! Column `number`'s SQL_DESC_TYPE_NAME, or an empty name when the driver's does not fit the
	//! buffer, which ClassifyColumn reports as UNKNOWN.
	string ReadTypeName(SQLHANDLE statement, SQLUSMALLINT number, StatementPhase phase);
	//! Throws the failure of `phase` for a describe call that failed on `statement`, or the transport
	//! failure when the connection is lost.
	[[noreturn]] void ThrowDescribeFailure(SQLHANDLE statement, StatementPhase phase);
	//! The statement Describe kept, when it was prepared from `sql` less than DESCRIBED_STATEMENT_REUSE
	//! ago; the caller then frees it. Otherwise frees any kept statement and returns SQL_NULL_HANDLE.
	SQLHANDLE TakeDescribedStatement(const string &sql);
	//! Frees the statement Describe kept, if any, and clears it.
	void FreeDescribedStatement() noexcept;

	//! Held by shared reference, so it outlives this connection.
	shared_ptr<RedshiftLogSink> log_sink;
	//! Target()'s answer, fixed once the settings are built.
	string target;
	//! The target's hints, for a failed connect.
	TargetHints hints;
	//! The loaded driver, which lives for the process.
	const redshift_driver::LoadedDriver &driver;
	//! Null until it is allocated and again once Close has run, so a constructed connection whose
	//! handle is null is closed.
	SQLHANDLE connection = SQL_NULL_HANDLE;
	//! Whether SQLDriverConnect succeeded, so teardown knows there is a session to end.
	bool connected = false;
	//! How this connection's results fetch numbers, and on which thread, read from the process state
	//! as it opens.
	NumberBinding number_binding = DEFAULT_NUMBER_BINDING;
	FetchThread fetch_thread = FetchThread::PRODUCER;
	//! What this connection shares with every result Execute returns.
	shared_ptr<OdbcResultState> result_state = make_shared_ptr<OdbcResultState>();
	//! The statement Describe, Execute or the session setup is working on, until it is freed or
	//! handed to a result. Close frees it before the connection handle.
	SQLHANDLE working_statement = SQL_NULL_HANDLE;
	//! Set to true just before a caller's statement runs and not cleared again; the session setup leaves
	//! it alone.
	bool holds_state = false;
	//! The statement the last Describe prepared, when its shape runs with SQLExecDirect, until the next
	//! Describe or Execute takes or frees it, or Close frees it; null when there is none.
	SQLHANDLE described_statement = SQL_NULL_HANDLE;
	//! The text `described_statement` was prepared from, cleared with it.
	string described_sql;
	//! When `described_statement`'s prepare was sent, read just before it.
	std::chrono::steady_clock::time_point described_at;
	//! What `described_at` and its bound are read from: the process state's clock as this connection opened.
	ConnectClock statement_clock;
};

//! How long after Describe an Execute of the same text with no parameters takes the statement
//! Describe prepared rather than prepare it again.
constexpr std::chrono::milliseconds DESCRIBED_STATEMENT_REUSE(1000);

//! The longest text parameter driver 2.3.0 sends whole. Its convertCParamDataToSQLData and
//! getParamVal (both rsutil.c, called from rslibpq.c) take the length as int, and a negative one
//! makes getParamVal send SQL NULL in place of the value.
constexpr idx_t MAX_PARAMETER_BYTES = NumericLimits<int32_t>::Maximum();

//! Whether Execute may run a statement with no parameters whose result has `columns`, of the driver's
//! reported `sizes`, with SQLExecDirect. Compute nodes write FLOAT, DOUBLE and an unqualified
//! interval, a VARCHAR of size 0, with a fixed formatter there, so those keep SQLExecute.
bool SimpleProtocolKeepsText(const vector<RedshiftColumn> &columns, const vector<SQLULEN> &sizes);

//! Refuses with InvalidInputException, before any driver call, a parameter holding a NUL byte or
//! longer than `max_bytes`, naming its position. Execute passes MAX_PARAMETER_BYTES; a test passes
//! less, to reach the length refusal without a 2 GiB value.
void RefuseUnsendableParameters(const vector<string> &params, idx_t max_bytes);

} // namespace duckdb
