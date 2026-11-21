#include "odbc/odbc_connection.hpp"

#include "duckdb/common/exception.hpp"
#include "duckdb/common/exception/binder_exception.hpp"
#include "duckdb/common/limits.hpp"
#include "duckdb/common/numeric_utils.hpp"
#include "duckdb/common/optional_idx.hpp"
#include "duckdb/common/string_util.hpp"
#include "duckdb/common/unordered_map.hpp"
#include "odbc/connection_settings.hpp"
#include "odbc/odbc_diagnostics.hpp"
#include "odbc/odbc_errors.hpp"
#include "odbc/odbc_process_state.hpp"
#include "odbc/process_environment.hpp"
#include "odbc/trust_store.hpp"
#ifdef _WIN32
#include "odbc/trust_store_export.hpp"
#endif
#include "odbc/type_classification.hpp"
#include "statement_shape.hpp"

#include <chrono>
#include <cstdint>

namespace duckdb {

namespace {

//! The buffer a column name is read into, terminating NUL included.
constexpr SQLSMALLINT COLUMN_NAME_BYTES = 256;
//! The buffer a column's type name is read into, terminating NUL included.
constexpr SQLSMALLINT TYPE_NAME_BYTES = 256;

//! The statement handle in `slot`, freed on scope exit unless it has been handed to a result or
//! freed already. The slot is the connection's, so the connection's teardown can free the handle
//! first and clear the slot.
class ScopedStatement {
public:
	ScopedStatement(const redshift_driver::OdbcApi &api_p, SQLHANDLE &slot_p, SQLHANDLE handle)
	    : api(api_p), slot(slot_p) {
		slot = handle;
	}
	~ScopedStatement() {
		if (slot != SQL_NULL_HANDLE) {
			api.FreeHandle(SQL_HANDLE_STMT, slot);
			slot = SQL_NULL_HANDLE;
		}
	}
	ScopedStatement(const ScopedStatement &) = delete;
	ScopedStatement &operator=(const ScopedStatement &) = delete;

	SQLHANDLE Get() const {
		return slot;
	}
	//! Hands the handle to a caller that frees it.
	SQLHANDLE Release() {
		const auto released = slot;
		slot = SQL_NULL_HANDLE;
		return released;
	}

private:
	const redshift_driver::OdbcApi &api;
	SQLHANDLE &slot;
};

//! The setup statements, in a fixed order and text: the text forms the
//! result passes on hold only under these settings. The interval style keeps
//! interval text in one form whatever the workgroup's or user's default.
const char *const SESSION_SETUP[] = {"SET extra_float_digits TO 2", "SET timezone TO 'UTC'",
                                     "SET datestyle TO 'ISO, MDY'", "SET intervalstyle TO 'postgres'"};

//! Refuses SQL holding a NUL byte, before any driver call. The driver passes the text on with no
//! length (public source v2.3.0, rsprepare.cpp RsPrepare::RS_SQLPrepare, then rslibpq.c
//! libpqPrepareOnThread's pqPrepare), so Redshift would run only the part before the NUL, whatever
//! length SQLPrepare is given.
void RefuseNulByte(const string &sql) {
	if (sql.find('\0') != string::npos) {
		throw InvalidInputException("the SQL text holds a NUL byte, which Redshift cannot receive; remove it");
	}
}

//! Refuses a parameter count that differs from the statement's marker count.
void RefuseParameterCountMismatch(idx_t markers, idx_t given) {
	if (markers != given) {
		throw InvalidInputException("the statement has %d parameter marker(s) but %d parameter(s) were given; pass "
		                            "exactly one parameter for each marker, in marker order",
		                            markers, given);
	}
}

//! Refuses a column the client gives no carrier, so a statement whose rows cannot be read
//! never runs. RedshiftTypeMapping refuses such a column when the scan binds; this also covers a
//! statement Execute runs without a Describe first.
void RefuseColumnsWithNoCarrier(const vector<RedshiftColumn> &columns) {
	for (idx_t index = 0; index < columns.size(); index++) {
		if (columns[index].type == RedshiftTypeId::UNKNOWN) {
			throw BinderException("Redshift result column %d, \"%s\", has a type this client cannot read, so the "
			                      "statement was not run; cast the column to a supported type in the Redshift SQL",
			                      index + 1, columns[index].name);
		}
	}
}

//! Refuses a statement whose prepared shape differs from the one the caller bound, so it never runs.
//! A local comparison, not a driver error, so it carries no SQLSTATE.
void RefuseChangedShape(const RedshiftStatementShape &bound, const RedshiftStatementShape &prepared) {
	const auto difference = ShapeDifference(bound, prepared);
	if (!difference.empty()) {
		throw BinderException(unordered_map<string, string> {{"redshift_error", "shape_changed"}},
		                      "the statement's result shape is not the one it was bound with, so it was not run: " +
		                          difference);
	}
}

//! The first of `columns`, of the driver's reported `sizes`, whose simple-protocol text is not known to
//! read as the same value. Compute nodes write FLOAT, DOUBLE and an unqualified interval, which the
//! driver reports as a VARCHAR of size 0, there with a fixed formatter that ignores the session's settings.
optional_idx ColumnSimpleProtocolChanges(const vector<RedshiftColumn> &columns, const vector<SQLULEN> &sizes) {
	for (idx_t index = 0; index < columns.size(); index++) {
		const auto type = columns[index].type;
		if (type == RedshiftTypeId::FLOAT || type == RedshiftTypeId::DOUBLE ||
		    (type == RedshiftTypeId::VARCHAR && sizes[index] == 0)) {
			return index;
		}
	}
	return optional_idx();
}

//! The rows the executed statement affected, or -1, the interface's "no count available", when the
//! driver reports none or SQLRowCount fails. Driver 2.3.0 gives 0 for SET or CREATE (rslibpq.c
//! setResultInStmt) and -1 for an empty query (rsresult.cpp SQLRowCount). A failed count does not
//! fail the statement, which has run.
int64_t RowsAffected(const redshift_driver::OdbcApi &api, SQLHANDLE statement) {
	SQLLEN count = -1;
	if (!Succeeded(api.RowCount(statement, &count))) {
		return -1;
	}
	return NumericCast<int64_t>(count);
}

//! Whether the driver's answer to running a statement means it ran. SQLExecute and SQLExecDirect
//! answer SQL_NO_DATA for an UPDATE, INSERT or DELETE that changed no row, a command that returns no
//! rows (public source v2.3.0, rslibpq.c setResultInStmt).
bool StatementRan(SQLRETURN rc, idx_t columns) {
	return Succeeded(rc) || (rc == SQL_NO_DATA && columns == 0);
}

//! The most connect attempts to a workgroup that may be resuming from idle suspension. The driver
//! gives each the connection string's LoginTimeout for every address the host resolves to, so a
//! timed-out attempt lasts at least that per address and can overrun.
constexpr idx_t MAX_RESUME_ATTEMPTS = 3;
//! Another attempt starts only while less than this has passed since the first began. It is checked
//! between attempts, not imposed on one, so the last attempt can overrun it.
constexpr std::chrono::seconds RESUME_BUDGET(180);

//! Whole seconds in `elapsed`, for the exhaustion message.
idx_t WholeSeconds(std::chrono::steady_clock::duration elapsed) {
	return NumericCast<idx_t>(std::chrono::duration_cast<std::chrono::seconds>(elapsed).count());
}

//! Whether a connect that failed with `records`, after `attempts` attempts and `elapsed` since the
//! first began, starts another. Only a resume timeout does, judged as the error message judges it
//! so the two agree: any other failure has a cause another attempt would meet again.
bool RetriesResume(const DiagnosticSet &records, const TargetHints &hints, idx_t attempts,
                   std::chrono::steady_clock::duration elapsed) {
	return IsResumeTimeout(ClassifyConnect(records).category, hints) && attempts < MAX_RESUME_ATTEMPTS &&
	       elapsed < RESUME_BUDGET;
}

} // namespace

bool SimpleProtocolKeepsText(const vector<RedshiftColumn> &columns, const vector<SQLULEN> &sizes) {
	return !ColumnSimpleProtocolChanges(columns, sizes).IsValid();
}

void RefuseUnsendableParameters(const vector<string> &params, idx_t max_bytes) {
	for (idx_t index = 0; index < params.size(); index++) {
		// The driver sends parameters as text with no lengths (public source v2.3.0, rslibpq.c
		// libpqExecuteDirectOrPreparedOnThread), which libpq measures with strlen (fe-exec.c
		// PQsendQueryGuts), so Redshift would receive a value only up to its first NUL byte.
		if (params[index].find('\0') != string::npos) {
			throw InvalidInputException(
			    "parameter %d holds a NUL byte, which Redshift cannot receive in a text parameter; remove it",
			    index + 1);
		}
		if (params[index].size() > max_bytes) {
			throw InvalidInputException(
			    "parameter %d is longer than the %d bytes the Redshift driver can send; pass a shorter value",
			    index + 1, max_bytes);
		}
	}
}

OdbcConnection::OdbcConnection(const RedshiftConfig &config, shared_ptr<RedshiftLogSink> log_sink_p,
                               optional_ptr<const RedshiftAwsCredentials> credentials)
    // Read before PrepareSettings runs, so a test started against the wrong build is refused before
    // the trust search and the driver source.
    : OdbcConnection(config, std::move(log_sink_p), ReadTestEndpoints(BuildHasTestEndpoints()), credentials) {
}

OdbcConnection::OdbcConnection(const RedshiftConfig &config, shared_ptr<RedshiftLogSink> log_sink_p,
                               const TestEndpoints &test_endpoints,
                               optional_ptr<const RedshiftAwsCredentials> credentials)
    // The sink goes on by rvalue reference, so it is moved only in the member initialiser, after
    // PrepareSettings has read it: the order in which two arguments are evaluated is unspecified.
    : OdbcConnection(PrepareSettings(config, *log_sink_p, test_endpoints, credentials), std::move(log_sink_p)) {
}

OdbcConnection::OdbcConnection(ConnectSettings settings, shared_ptr<RedshiftLogSink> &&log_sink_p)
    // Members are initialised in declaration order, so the sink is held before the driver is obtained.
    : log_sink(std::move(log_sink_p)), target(std::move(settings.target)), hints(std::move(settings.hints)),
      driver(ObtainDriver(*log_sink)) {
	result_state->target = target;
	result_state->log_sink = log_sink;
#ifdef _WIN32
	// Held open, sharing reads only, until Open returns: across every attempt's SQLDriverConnect no
	// process can delete or replace the file between its check and the driver's read.
	auto trust = PlaceTrustSource(settings.trust, driver.trust_directory);
	settings.connection_string = settings.build_connection_string(trust.source);
#endif
	RefuseWeakeningEnvironment(settings.connection_string);
	try {
		Open(settings.connection_string);
#ifdef _WIN32
		trust.held.reset();
#endif
		SetUpSession();
	} catch (...) {
		// A constructor that throws leaves no object for the destructor to clean up.
		Close();
		throw;
	}
}

OdbcConnection::~OdbcConnection() {
	Close();
}

SQLRETURN OdbcConnection::ExecuteDirect(SQLHANDLE statement, const string &sql) {
	// The driver's buffer parameter is not const.
	auto text = sql;
	return driver.api.ExecDirect(statement, reinterpret_cast<SQLCHAR *>(&text[0]), SQL_NTS);
}

OdbcConnection::PreparedColumns OdbcConnection::DescribeExecuted(SQLHANDLE statement,
                                                                 const RedshiftStatementShape &checked) {
	auto executed = ShapeOf(statement, StatementPhase::EXECUTE);
	auto difference = ShapeDifference(checked, executed.shape);
	// Same names and types, but a VARCHAR whose size became 0 may now be an interval sent as a count.
	const auto changed = ColumnSimpleProtocolChanges(executed.shape.columns, executed.sizes);
	if (difference.empty() && changed.IsValid()) {
		const auto index = changed.GetIndex();
		difference = StringUtil::Format("column %d, \"%s\", is now a VARCHAR the driver reports with no length, "
		                                "such as an interval",
		                                index + 1, executed.shape.columns[index].name);
	}
	if (!difference.empty()) {
		// Not shape_changed, which tells the caller that nothing ran. The handle is freed as the
		// throw unwinds, before the caller sees it.
		throw Exception(ExceptionType::EXECUTOR,
		                "the statement's result shape changed on the server after it was checked, so its rows "
		                "were not read; it may have run, so check before running it again: " +
		                    difference);
	}
	return executed;
}

RedshiftStatementShape OdbcConnection::Describe(const string &sql) {
	CloseIfResultLostConnection();
	RefuseClosed();
	RefuseNulByte(sql);
	RefuseWhileResultOpen();
	FreeDescribedStatement();
	ScopedStatement statement(driver.api, working_statement, AllocateStatement());
	// The timestamp is read before Prepare is sent, so the measured age can only overstate and the
	// reused statement is at most one second old.
	const auto prepared_at = statement_clock();
	Prepare(statement.Get(), sql);
	auto described = ShapeOf(statement.Get(), StatementPhase::DESCRIBE);
	// Kept, not freed, so an Execute of the same text that follows can check it without a second
	// server prepare; only for a shape that runs with SQLExecDirect, as Execute runs a kept one.
	if (SimpleProtocolKeepsText(described.shape.columns, described.sizes)) {
		described_sql = sql;
		described_at = prepared_at;
		described_statement = statement.Release();
	}
	return std::move(described.shape);
}

unique_ptr<RedshiftResult> OdbcConnection::Execute(const string &sql, const vector<string> &params,
                                                   const RedshiftStatementShape &expected_shape) {
	CloseIfResultLostConnection();
	RefuseClosed();
	RefuseNulByte(sql);
	RefuseUnsendableParameters(params, MAX_PARAMETER_BYTES);
	RefuseWhileResultOpen();
	// SQLExecute would run a kept statement as prepared, over a table that may have changed since, so
	// one with parameters, which never runs with SQLExecDirect, never takes it.
	const bool no_params = params.empty();
	const auto described = no_params ? TakeDescribedStatement(sql) : SQL_NULL_HANDLE;
	if (!no_params) {
		FreeDescribedStatement();
	}
	const bool kept = described != SQL_NULL_HANDLE;
	ScopedStatement statement(driver.api, working_statement, kept ? described : AllocateStatement());
	if (!kept) {
		Prepare(statement.Get(), sql);
	}
	RefuseParameterCountMismatch(CountParameters(statement.Get()), params.size());
	// Described before the statement runs, from the description the driver keeps for a prepared
	// statement (public source v2.3.0, rsresult.cpp SQLNumResultCols, RS_SQLDescribeCol and
	// RS_SQLColAttribute), so a failed description leaves the session unmarked.
	auto prepared = ShapeOf(statement.Get(), StatementPhase::DESCRIBE);
	auto &shape = prepared.shape;
	// Compared before anything runs, because a row-returning CALL may already have applied its
	// effects by the time its result shows the difference. The handle is freed as the throw unwinds.
	RefuseChangedShape(expected_shape, shape);
	RefuseColumnsWithNoCarrier(shape.columns);
	const auto columns = shape.columns.size();
	// The driver's buffer parameter is not const, so it is given a copy, which an open result then
	// keeps for redacting its later diagnostics.
	auto values = params;
	BindParameters(statement.Get(), values);
	// Set before the statement runs, whatever it returns.
	holds_state = true;
	// The simple query protocol, whose results the server streams about twice as fast, when it keeps
	// the result's text; a kept statement always passes, as Describe kept it for that shape.
	const bool direct = no_params && SimpleProtocolKeepsText(shape.columns, prepared.sizes);
	const auto executed = direct ? ExecuteDirect(statement.Get(), sql) : driver.api.Execute(statement.Get());
	if (!StatementRan(executed, columns)) {
		ThrowCallFailure({SQL_HANDLE_STMT, statement.Get()}, params, StatementPhase::EXECUTE);
	}
	LogStatementWarnings(executed, statement.Get(), params);
	// SQLExecDirect parsed the text again, so the server may have changed the statement since the check.
	auto result_columns = direct ? DescribeExecuted(statement.Get(), shape) : std::move(prepared);
	// A statement that returns rows has no count to report.
	int64_t row_count = -1;
	if (columns == 0) {
		// It has no rows to fetch, so its handle is freed when Execute returns.
		row_count = FinishWithoutResultSet(statement.Get(), params);
	}
	// Built before the handover, so a failed allocation cannot leave the fence set with no result to
	// clear it.
	auto result = make_uniq<OdbcStatement>(driver.api, result_state, std::move(result_columns.shape.columns),
	                                       result_columns.sizes, number_binding, row_count, fetch_thread);
	if (columns > 0) {
		result_state->statement = statement.Release();
		result_state->connection = connection;
		result_state->bound_values = std::move(values);
	}
	result_state->logical_result_open = true;
	if (result->HasLongColumn()) {
		LogGetDataExtensions(driver.api, connection, *log_sink);
	}
	return std::move(result);
}

bool OdbcConnection::SessionHoldsState() const {
	return holds_state;
}

string OdbcConnection::Target() const {
	return target;
}

OdbcConnection::ConnectSettings
OdbcConnection::PrepareSettings(const RedshiftConfig &config, RedshiftLogSink &log_sink,
                                const TestEndpoints &test_endpoints,
                                optional_ptr<const RedshiftAwsCredentials> credentials) {
	// Refused first, so a refused region, profile or target is InvalidInputException whatever the
	// trust search finds. BuildConnectionString validates them again from the same `config`, so the
	// target named here is the one in the connection string.
	auto resolved = ValidateAndResolveTarget(config);
	// Under the default chain, a credentials failure names AWS_PROFILE when it is set (ConnectHint).
	if (config.profile.empty() && config.aws_secret.empty()) {
		const auto selected = ReadEnvironmentVariable("AWS_PROFILE");
		resolved.hints.aws_profile_variable = selected && !selected->empty();
	}
	auto trust = ResolveTrustSource(log_sink);
	ConnectSettings settings {TargetName(resolved, test_endpoints) + " (" + config.Describe() + ")",
	                          std::move(resolved.hints), string()};
	// The client keeps no copy of the AWS credentials: they are only in this string and its temporary
	// copies, which the constructor discards once it has connected. The driver's handle copy lasts until
	// the connection is freed; driver 2.3.0.0 also kept a copy in its connection-string cache until the
	// process exited.
#ifdef _WIN32
	// The export's folder comes from the driver source, so the string is built after it, from the
	// same validated `config`. The lambda copies `config` and `test_endpoints`, and keeps
	// `credentials` as a pointer, whose object outlives the constructor that calls it.
	// Checked here, before the driver source, as the encoder checks it elsewhere: the string itself
	// can only be built once the trust file is placed.
	ValidateCredentialSource(config, credentials);
	settings.trust = std::move(trust);
	settings.build_connection_string = [config, test_endpoints, credentials](const TrustSource &placed) {
		return BuildConnectionString(config, placed, test_endpoints, credentials);
	};
#else
	settings.connection_string = BuildConnectionString(config, trust, test_endpoints, credentials);
#endif
	return settings;
}

void OdbcConnection::RefuseWeakeningEnvironment(const string &connection_string) const {
	// No driver call has failed, so there is no SQLSTATE to report.
	const auto info = DriverInfo(driver.identity);
	// Only the name is read into the message: the value can hold anything. Under the
	// process state's lock, which the environment's allocation holds: on Linux and macOS it sets ODBCINI
	// for its own duration, which this check would otherwise see as a user's.
	const char *refused = nullptr;
	{
		lock_guard<mutex> guard(CurrentOdbcProcessState()->lock);
		refused = redshift_driver::FindRefusedEnvironmentVariable(driver);
	}
	if (refused) {
		throw IOException(info, string(refused) +
		                            " is set; the Redshift driver would read it, and it can change the driver's "
		                            "connection or security settings. Unset it to connect.");
	}
	for (const auto &overridden : driver.overridden_environment) {
		if (!SetsKeyword(connection_string, overridden.keyword)) {
			throw IOException(info, "the Redshift connection settings do not set " + overridden.keyword +
			                            ", so the driver would read " + overridden.name +
			                            " from the environment. This is a defect in the extension; report it with "
			                            "the extension and driver versions.");
		}
	}
}

void OdbcConnection::Open(const string &connection_string) {
	auto &state = *CurrentOdbcProcessState();
	lock_guard<mutex> serialised(state.connect_lock);
	number_binding = state.number_binding;
	fetch_thread = state.fetch_thread;
	const auto environment = ObtainEnvironment(driver);
	const auto first_started = state.connect_clock();
	statement_clock = state.connect_clock;
	for (idx_t attempts = 1;; attempts++) {
		AllocateConnection(environment);
		if (Connect(connection_string)) {
			return;
		}
		// Read before any other call on the handle, which would clear the records.
		const auto records = ReadConnectDiagnostics(driver.api, {SQL_HANDLE_DBC, connection});
		const auto elapsed = state.connect_clock() - first_started;
		if (!RetriesResume(records, hints, attempts, elapsed)) {
			ThrowConnectFailure(
			    records, ConnectFailureContext {target, hints, driver.identity, attempts, WholeSeconds(elapsed)});
		}
		// Each attempt gets a handle of its own, freed as a failed connect's is, so none carries
		// the state of a connect that failed into the next.
		FreeConnectionHandle();
		log_sink->Debug("Redshift connect timed out; waiting for the workgroup to resume, attempt " +
		                std::to_string(attempts + 1) + " of " + std::to_string(MAX_RESUME_ATTEMPTS));
	}
}

void OdbcConnection::AllocateConnection(SQLHANDLE environment) {
	if (!Succeeded(driver.api.AllocHandle(SQL_HANDLE_DBC, environment, &connection))) {
		// An allocation's records are on its input handle.
		ThrowPreConnectFailure(ReadConnectDiagnostics(driver.api, {SQL_HANDLE_ENV, environment}), driver.identity,
		                       "the Redshift driver could not allocate a connection handle", "run the statement again");
	}
	EnableAutocommit();
}

void OdbcConnection::EnableAutocommit() {
	// Set although it is the driver's default (source v2.3.0 rsodbc.h RS_CONN_ATTR_INFO), because with
	// it off the driver sends BEGIN before a statement outside a transaction (rslibpq.c
	// libpqExecuteDirectOrPreparedOnThread), opening one the client would then have to end.
	const auto on = reinterpret_cast<SQLPOINTER>(static_cast<uintptr_t>(SQL_AUTOCOMMIT_ON));
	if (!Succeeded(driver.api.SetConnectAttr(connection, SQL_ATTR_AUTOCOMMIT, on, SQL_IS_UINTEGER))) {
		// This handle has made no connect attempt, so the failure is not classified as one. On a valid
		// handle before a connect, the driver refuses autocommit only for a value other than SQL_TRUE or
		// SQL_FALSE (public source v2.3.0, rsoptions.cpp RsOptions::RS_SQLSetConnectAttr), so no region,
		// network or retry is involved.
		ThrowPreConnectFailure(ReadConnectDiagnostics(driver.api, {SQL_HANDLE_DBC, connection}), driver.identity,
		                       "the Redshift driver refused to enable autocommit, which this extension needs",
		                       "report it with the extension and driver versions");
	}
}

bool OdbcConnection::Connect(const string &connection_string) {
	// The driver's parameter is not const, so it is given a copy.
	auto text = connection_string;
	SQLSMALLINT completed_length = 0;
	// A zero-capacity output buffer, so the driver has nowhere to write the completed connection
	// string, and the length it reports is ignored.
	const auto rc = driver.api.DriverConnect(connection, nullptr, reinterpret_cast<SQLCHAR *>(&text[0]), SQL_NTS,
	                                         nullptr, 0, &completed_length, SQL_DRIVER_NOPROMPT);
	if (!Succeeded(rc)) {
		return false;
	}
	connected = true;
	if (rc == SQL_SUCCESS_WITH_INFO) {
		// The connect phase logs the records' SQLSTATEs and none of their text.
		LogInformationalDiagnostics(*log_sink, ReadConnectDiagnostics(driver.api, {SQL_HANDLE_DBC, connection}));
	}
	return true;
}

void OdbcConnection::SetUpSession() {
	for (const auto sql : SESSION_SETUP) {
		RunSetupStatement(sql);
	}
}

void OdbcConnection::RunSetupStatement(const string &sql) {
	SQLHANDLE handle = SQL_NULL_HANDLE;
	if (!Succeeded(driver.api.AllocHandle(SQL_HANDLE_STMT, connection, &handle))) {
		// A failed allocation leaves its records on the connection handle (public source v2.3.0,
		// rsconnect.cpp RS_CONN_INFO::RS_SQLAllocStmt).
		ThrowSetupFailure({SQL_HANDLE_DBC, connection}, sql);
	}
	ScopedStatement statement(driver.api, working_statement, handle);
	auto text = sql;
	const auto prepared = driver.api.Prepare(statement.Get(), reinterpret_cast<SQLCHAR *>(&text[0]), SQL_NTS);
	if (!Succeeded(prepared)) {
		ThrowSetupFailure({SQL_HANDLE_STMT, statement.Get()}, sql);
	}
	// The prepare's records are logged before SQLExecute, which clears them.
	LogStatementWarnings(prepared, statement.Get(), {});
	// Success alone counts as run: SQL_NO_DATA marks an UPDATE, INSERT or DELETE that changed no row
	// (public source v2.3.0, rslibpq.c setResultInStmt), which a SET is not.
	const auto executed = driver.api.Execute(statement.Get());
	if (!Succeeded(executed)) {
		ThrowSetupFailure({SQL_HANDLE_STMT, statement.Get()}, sql);
	}
	LogStatementWarnings(executed, statement.Get(), {});
}

void OdbcConnection::LogStatementWarnings(SQLRETURN rc, SQLHANDLE statement, const vector<string> &bound_values) {
	// After the connect phase, so the statement rules apply.
	if (rc == SQL_SUCCESS_WITH_INFO) {
		LogInformationalDiagnostics(*log_sink,
		                            ReadStatementDiagnostics(driver.api, {SQL_HANDLE_STMT, statement}, bound_values));
	}
}

void OdbcConnection::ThrowSetupFailure(DiagnosticHandle failed_on, const string &sql) {
	// Read first: the connection-dead question clears the connection handle's records (public source
	// v2.3.0, rsoptions.cpp RsOptions::RS_SQLGetConnectAttr).
	const auto records = ReadStatementDiagnostics(driver.api, failed_on, {});
	// The constructor closes the connection either way, so the answer decides only the message.
	if (ConnectionLost(driver.api, connection, records)) {
		ThrowSessionSetupLoss(records, target, driver.identity);
	}
	ThrowSessionSetupFailure(records, target, driver.identity, sql);
}

void OdbcConnection::Close() noexcept {
	// Step 1 of the teardown: the open statements, before the connection they belong to. A
	// transport failure can close the connection while Describe or Execute holds a working statement.
	CloseOpenResult();
	FreeDescribedStatement();
	if (working_statement != SQL_NULL_HANDLE) {
		driver.api.FreeHandle(SQL_HANDLE_STMT, working_statement);
		working_statement = SQL_NULL_HANDLE;
	}
	if (connection == SQL_NULL_HANDLE) {
		return;
	}
	// A handle that never connected has no session to end.
	if (connected) {
		connected = false;
		// Step 2, the client's one transaction end: the driver sends ROLLBACK whenever libpq reports
		// the transaction not idle (source v2.3.0 rstransaction.cpp RsTransaction::RS_SQLTransact), so
		// this ends a transaction a user's BEGIN opened.
		if (!Succeeded(driver.api.EndTran(SQL_HANDLE_DBC, connection, SQL_ROLLBACK))) {
			LogTeardownFailure("the rollback");
		}
		// Step 3.
		if (!Succeeded(driver.api.Disconnect(connection))) {
			LogTeardownFailure("the disconnect");
		}
	}
	// Step 4.
	FreeConnectionHandle();
}

void OdbcConnection::FreeConnectionHandle() noexcept {
	// The member is cleared before the free, so a second free would find nothing to free even when
	// this one fails, and the handle is then abandoned.
	const auto handle = connection;
	connection = SQL_NULL_HANDLE;
	if (!Succeeded(driver.api.FreeHandle(SQL_HANDLE_DBC, handle))) {
		// The abandoned handle is read once more, for this free's records only.
		LogTeardownFailure("freeing the connection handle", handle, "the handle is abandoned");
	}
}

void OdbcConnection::LogTeardownFailure(const char *step) noexcept {
	LogTeardownFailure(step, connection, "teardown continues");
}

void OdbcConnection::LogTeardownFailure(const char *step, SQLHANDLE handle, const char *outcome) noexcept {
	try {
		// Read before the next teardown call, which would clear the records. Only their SQLSTATEs are
		// logged: no connect-phase driver text reaches a log.
		const auto records = ReadConnectDiagnostics(driver.api, {SQL_HANDLE_DBC, handle});
		const auto sqlstates =
		    records.records.empty() ? string(", with no diagnostic record") : ", SQLSTATE " + records.SqlStates();
		log_sink->Debug("Redshift connection teardown: " + string(step) + " failed" + sqlstates + "; " + outcome);
	} catch (...) {
		// Reading the records or building the line can fail; teardown runs in a destructor, so the
		// line is dropped.
	}
}

void OdbcConnection::CloseOpenResult() noexcept {
	// The producer may be in a driver call on the handle freed next, so it is joined first.
	result_state->producer.Stop();
	if (result_state->statement != SQL_NULL_HANDLE) {
		driver.api.FreeHandle(SQL_HANDLE_STMT, result_state->statement);
		result_state->statement = SQL_NULL_HANDLE;
	}
	result_state->connection_open = false;
}

void OdbcConnection::RefuseWhileResultOpen() const {
	// Refused before any driver call, and the open result is never read ahead to make room for
	// another.
	if (result_state->logical_result_open) {
		throw InvalidInputException("a result from this Redshift connection is still open, and the connection holds "
		                            "one at a time; read that result to its end or close it, then try again");
	}
}

void OdbcConnection::RefuseClosed() const {
	if (connection == SQL_NULL_HANDLE) {
		throw IOException("the connection to Redshift at %s was lost and is closed; open a new connection to continue",
		                  target);
	}
}

void OdbcConnection::ThrowCallFailure(DiagnosticHandle failed_on, const vector<string> &bound_values,
                                      StatementPhase phase) {
	// Read first: the connection-dead question clears the connection handle's records (public source
	// v2.3.0, rsoptions.cpp RsOptions::RS_SQLGetConnectAttr).
	const auto records = ReadStatementDiagnostics(driver.api, failed_on, bound_values);
	if (ConnectionLost(driver.api, connection, records)) {
		Close();
		ThrowTransportFailure(records, target, phase);
	}
	// A live connection whose session Redshift's answer left unusable is closed as a lost one is, so that every later
	// call fails without reaching the network; the failure keeps its type, and `redshift_error` tells the caller.
	if (RecordsShowUnusableSession(records)) {
		Close();
	}
	ThrowStatementFailure(records, phase);
}

void OdbcConnection::CloseIfResultLostConnection() {
	if (result_state->connection_lost) {
		Close();
	}
}

SQLHANDLE OdbcConnection::AllocateStatement() {
	SQLHANDLE statement = SQL_NULL_HANDLE;
	if (!Succeeded(driver.api.AllocHandle(SQL_HANDLE_STMT, connection, &statement))) {
		// A failed allocation leaves its records on the connection handle (public source v2.3.0,
		// rsconnect.cpp RS_CONN_INFO::RS_SQLAllocStmt). On a live connection it is a statement failure,
		// not an IOException, which RethrowUnlessConnectionLost reads as a lost connection.
		ThrowCallFailure({SQL_HANDLE_DBC, connection}, {}, StatementPhase::ALLOCATE);
	}
	return statement;
}

void OdbcConnection::Prepare(SQLHANDLE statement, const string &sql) {
	// SQLPrepare, not SQLExecDirect, so Describe parses the statement without running it:
	// the driver prepares it on the server and keeps the row description it
	// answers with (public source v2.3.0, rsprepare.cpp RsPrepare::RS_SQLPrepare and rslibpq.c
	// libpqPrepareOnThread).
	auto text = sql;
	const auto prepared = driver.api.Prepare(statement, reinterpret_cast<SQLCHAR *>(&text[0]), SQL_NTS);
	if (!Succeeded(prepared)) {
		ThrowCallFailure({SQL_HANDLE_STMT, statement}, {}, StatementPhase::PREPARE);
	}
	LogStatementWarnings(prepared, statement, {});
}

idx_t OdbcConnection::CountParameters(SQLHANDLE statement) {
	SQLSMALLINT count = 0;
	if (!Succeeded(driver.api.NumParams(statement, &count))) {
		ThrowCallFailure({SQL_HANDLE_STMT, statement}, {}, StatementPhase::PARAMETERS);
	}
	return NumericCast<idx_t>(count);
}

void OdbcConnection::BindParameters(SQLHANDLE statement, vector<string> &values) {
	for (idx_t index = 0; index < values.size(); index++) {
		auto &value = values[index];
		// No length indicator, so the driver copies BufferLength bytes, the whole value (public
		// source v2.3.0, rsutil.c getParamVal).
		const auto length = NumericCast<SQLLEN>(value.size());
		if (!Succeeded(driver.api.BindParameter(statement, NumericCast<SQLUSMALLINT>(index + 1), SQL_PARAM_INPUT,
		                                        SQL_C_CHAR, SQL_VARCHAR, value.size(), 0, &value[0], length,
		                                        nullptr))) {
			ThrowCallFailure({SQL_HANDLE_STMT, statement}, values, StatementPhase::BIND);
		}
	}
}

idx_t OdbcConnection::CountColumns(SQLHANDLE statement, StatementPhase phase) {
	SQLSMALLINT count = 0;
	if (!Succeeded(driver.api.NumResultCols(statement, &count))) {
		ThrowDescribeFailure(statement, phase);
	}
	return NumericCast<idx_t>(count);
}

OdbcConnection::PreparedColumns OdbcConnection::ShapeOf(SQLHANDLE statement, StatementPhase phase) {
	PreparedColumns prepared;
	const auto count = CountColumns(statement, phase);
	prepared.shape.returns_rows = count > 0;
	for (idx_t number = 1; number <= count; number++) {
		auto described = DescribeColumn(statement, NumericCast<SQLUSMALLINT>(number), phase);
		prepared.shape.columns.push_back(std::move(described.column));
		prepared.sizes.push_back(described.size);
	}
	return prepared;
}

int64_t OdbcConnection::FinishWithoutResultSet(SQLHANDLE statement, const vector<string> &bound_values) {
	// Read first: the drain moves the driver past the result whose count SQLRowCount reports
	// (public source v2.3.0, rsresult.cpp SQLRowCount and SQLMoreResults).
	const auto row_count = RowsAffected(driver.api, statement);
	const auto further = DrainFurtherResults(driver.api, statement);
	if (further == FurtherResults::RESULT_SET) {
		ThrowMultipleResultSets();
	}
	if (further == FurtherResults::DRIVER_FAILED) {
		ThrowCallFailure({SQL_HANDLE_STMT, statement}, bound_values, StatementPhase::FETCH);
	}
	return row_count;
}

OdbcConnection::DescribedColumn OdbcConnection::DescribeColumn(SQLHANDLE statement, SQLUSMALLINT number,
                                                               StatementPhase phase) {
	SQLCHAR name[COLUMN_NAME_BYTES] = {};
	SQLSMALLINT name_length = 0;
	SQLSMALLINT sql_type = 0;
	SQLULEN column_size = 0;
	SQLSMALLINT decimal_digits = 0;
	if (!Succeeded(driver.api.DescribeCol(statement, number, name, COLUMN_NAME_BYTES, &name_length, &sql_type,
	                                      &column_size, &decimal_digits, nullptr))) {
		ThrowDescribeFailure(statement, phase);
	}
	if (name_length >= COLUMN_NAME_BYTES) {
		// Refused rather than cut, so no column is bound under a name it does not have.
		const auto refusal = "Redshift reported a name longer than " + std::to_string(COLUMN_NAME_BYTES - 1) +
		                     " bytes for column " + std::to_string(number) + "; alias the column to a shorter name";
		if (phase == StatementPhase::EXECUTE) {
			throw Exception(ExceptionType::EXECUTOR,
			                refusal + ". The statement may have run; check before running it again.");
		}
		throw BinderException(refusal);
	}
	DriverColumnMetadata metadata {string(reinterpret_cast<const char *>(name), NumericCast<idx_t>(name_length)),
	                               ReadTypeName(statement, number, phase), sql_type, column_size, decimal_digits};
	return DescribedColumn {ClassifyColumn(metadata), column_size};
}

string OdbcConnection::ReadTypeName(SQLHANDLE statement, SQLUSMALLINT number, StatementPhase phase) {
	SQLCHAR type_name[TYPE_NAME_BYTES] = {};
	SQLSMALLINT reported = 0;
	if (!Succeeded(driver.api.ColAttribute(statement, number, SQL_DESC_TYPE_NAME, type_name, TYPE_NAME_BYTES, &reported,
	                                       nullptr))) {
		ThrowDescribeFailure(statement, phase);
	}
	// The driver reports the name's full length even when it cut the copy (rsutil.c copyStrDataSmallLen),
	// so a longer one is treated as none: the name is never read past the buffer, and a cut one could
	// still trim to a type name and be classified as it.
	if (reported >= TYPE_NAME_BYTES) {
		return string();
	}
	return string(reinterpret_cast<const char *>(type_name), NumericCast<idx_t>(reported));
}

void OdbcConnection::ThrowDescribeFailure(SQLHANDLE statement, StatementPhase phase) {
	ThrowCallFailure({SQL_HANDLE_STMT, statement}, {}, phase);
}

SQLHANDLE OdbcConnection::TakeDescribedStatement(const string &sql) {
	// Bounded in time, so the shape check reads the server's answer from at most that long ago. An
	// Execute of the text just described takes this statement; a later one is prepared again.
	if (described_statement != SQL_NULL_HANDLE && described_sql == sql &&
	    statement_clock() - described_at < DESCRIBED_STATEMENT_REUSE) {
		const auto taken = described_statement;
		described_statement = SQL_NULL_HANDLE;
		described_sql.clear();
		return taken;
	}
	FreeDescribedStatement();
	return SQL_NULL_HANDLE;
}

void OdbcConnection::FreeDescribedStatement() noexcept {
	if (described_statement != SQL_NULL_HANDLE) {
		driver.api.FreeHandle(SQL_HANDLE_STMT, described_statement);
		described_statement = SQL_NULL_HANDLE;
		described_sql.clear();
	}
}

} // namespace duckdb
