//===----------------------------------------------------------------------===//
//                         DuckDB - Redshift
//
// src/transport/odbc/odbc_errors.hpp
//
// From a failure's diagnostic records to the exception the caller sees, and to the log lines of a
// call that succeeded with records. A user reads the client's own words, with, sanitized, the
// server's first line in a statement failure of a listed class (ShowsServerMessage), and the driver's
// reason in a connect failure no category names.
//===----------------------------------------------------------------------===//

#pragma once

#include "duckdb/common/optional_ptr.hpp"
#include "duckdb/common/string.hpp"
#include "duckdb/common/unordered_map.hpp"
#include "odbc/odbc_diagnostics.hpp"
#include "odbc/target_hints.hpp"
#include "odbc_driver.hpp"
#include "redshift_transport.hpp"

#include <cstdint>

namespace duckdb {

//! What a connect failure was. Each category but OTHER has its own hint, fixed or the target's.
enum class ConnectCategory : uint8_t {
	CREDENTIALS,
	CERTIFICATE,
	PERMISSION,
	TARGET_NOT_FOUND,
	DATABASE_NOT_FOUND,
	RESUME_TIMEOUT,
	LOGIN_REFUSED,
	OTHER
};

//! A connect failure's category and the record that decided it, null only when the failure left
//! no record. `acted_on` points into the classified set, so it lives only as long as that set.
struct ConnectClassification {
	ConnectCategory category;
	optional_ptr<const DiagnosticRecord> acted_on;
};

//! Tries credentials, certificate trust, permission, a target not found, a database not found and
//! resume timeout in turn across all the records, so a cause the user can fix wins over a timeout, and a timeout over
//! class 08. OTHER acts on the first record
//! whose FailureSqlState is not HY000, else the first; an unreadable, success, warning
//! or carried-over state counts as HY000 there.
ConnectClassification ClassifyConnect(const DiagnosticSet &records);
//! Deleted: a temporary set would leave `acted_on` dangling.
ConnectClassification ClassifyConnect(const DiagnosticSet &&records) = delete;

//! Whether a connect failure of `category` is a timeout on a target that may be resuming from
//! idle. Only that one is retried, and only its message gets the resume text and the attempt
//! report; a timeout on any other target gets the network-access text.
bool IsResumeTimeout(ConnectCategory category, const TargetHints &hints);

//! Whether a failure's records show a lost connection, whatever the driver's connection-dead answer: a record in
//! SQLSTATE class 08, or one whose libpq text starts with a text libpq writes when the connection drops.
//! The driver can report that text under the last server SQLSTATE it saw, so it counts in a record
//! of any class; the same words quoted inside a server error do not.
bool RecordsShowConnectionLoss(const DiagnosticSet &records);

//! Whether a statement failure's records show Redshift answering in a way that leaves the session unusable for
//! later statements, although the connection is up: a record whose SQLSTATE is XX000 and whose server message's
//! first line, without its severity word, is "Internal Service Error", or starts "Existing writable session" and
//! says the session "is still open", judged on the record as the driver wrote it, before redaction
//! (RecordTextShowsUnusableSession in odbc_message_framing.hpp). Seen on a Serverless workgroup (Redshift
//! 1.0.539508): the first answered a data-sharing write that the producer failed, which can leave a
//! writable session open on the producer; the second then refused the later writes in the same session until the
//! producer expired that writable session. Any other text, and the same text under another state, does not count,
//! so an ordinary error keeps the session.
bool RecordsShowUnusableSession(const DiagnosticSet &records);

//! What a connect failure is about, beyond its records.
struct ConnectFailureContext {
	//! The connection's Target().
	string target;
	//! What the message says that depends on the target.
	TargetHints hints;
	//! The loaded driver.
	redshift_driver::DriverIdentity identity;
	//! The connect attempts made, the first included.
	idx_t attempts;
	//! Whole seconds from the start of the first attempt to the failure of the last.
	idx_t elapsed_seconds;
};

//! Throws IOException for a connect failure: the category's message and hint in the client's own
//! words, the SQLSTATEs, and the loaded driver's identity under `redshift_driver`. A failure no
//! category names adds the driver's reason, the acted-on record's redacted and sanitized text; no other
//! record text appears, in the message or in `extra_info`. A resume timeout also
//! reports the attempts made and the seconds they took.
//! A timeout on a target that does not resume from idle gets the network-access hint instead. On every
//! target, a failure no category names whose records hold a class 28 state, one the driver carried
//! over included, or the server's refused-password text under any state, is LOGIN_REFUSED, with the target's
//! `login_refused` hint, or a fixed sign-in hint where the target has none.
[[noreturn]] void ThrowConnectFailure(const DiagnosticSet &records, const ConnectFailureContext &context);

//! The `extra_info` naming the loaded driver, which every connect-phase failure after the driver
//! loads carries. The one place the `redshift_driver` key is written.
unordered_map<string, string> DriverInfo(const redshift_driver::DriverIdentity &identity);

//! Throws IOException for a driver call that failed before a connect attempt: `lead` and then
//! `next`, in the client's own words, with a clause between them saying so when no record could be
//! read, the SQLSTATEs as ThrowStatementFailure reports them, and the loaded driver's identity
//! under `redshift_driver`. No record's text appears.
[[noreturn]] void ThrowPreConnectFailure(const DiagnosticSet &records, const redshift_driver::DriverIdentity &identity,
                                         const string &lead, const string &next);

//! Where a statement failed. ALLOCATE is its statement handle's allocation, PARAMETERS its parameter marker count,
//! DESCRIBE its column count or column metadata, and BIND the binding of its parameters, all before anything runs;
//! from EXECUTE on, the statement may have run.
enum class StatementPhase : uint8_t { ALLOCATE, PREPARE, PARAMETERS, DESCRIBE, BIND, EXECUTE, FETCH };

//! Throws for a statement that failed on a connection that is not lost, as StatementPhase describes:
//! BinderException at prepare, parameters and describe, and the executor exception type otherwise.
//! Server text shows only for a statement-phase set. When RecordsShowUnusableSession holds, which the caller
//! has closed the connection for, `extra_info` also has `redshift_error` = `session_unusable`
//! (redshift_transport.hpp); the type stays the phase's.
[[noreturn]] void ThrowStatementFailure(const DiagnosticSet &records, StatementPhase phase);

//! Throws the executor exception for a cell the driver could not read on a connection that is not
//! lost: the fetch phase's failure, naming the column, counting from 1, in place of the phase, with
//! `redshift_error` as ThrowStatementFailure sets it.
[[noreturn]] void ThrowCellReadFailure(const DiagnosticSet &records, idx_t column);

//! Throws the executor exception for a batch refused because a bound value may not be whole:
//! `lead` says why, and `records` holds the fetch's records when they include the
//! driver's 01004, else nothing, and then the failure has no SQLSTATE.
[[noreturn]] void ThrowCutValue(const DiagnosticSet &records, const string &lead);

//! Throws the executor exception for a statement that produced a further result set with columns
//! after its first result. No driver record reports one, so it has no SQLSTATE.
[[noreturn]] void ThrowMultipleResultSets();

//! Throws IOException for a failure in `phase` that ended the connection, naming `target`. It says the statement was
//! not replayed, and from EXECUTE on adds that a write may have taken effect. Before EXECUTE it adds nothing, since
//! the caller's own message may say the statement could have run. No record's text appears.
[[noreturn]] void ThrowTransportFailure(const DiagnosticSet &records, const string &target, StatementPhase phase);

//! Throws IOException for the session setup statement `sql` that failed on a new connection to `target` that is not
//! lost, which the caller then closes. The message names `sql` and says that no statement of the caller's was sent;
//! `extra_info` names the driver under `redshift_driver`. No record's text appears.
[[noreturn]] void ThrowSessionSetupFailure(const DiagnosticSet &records, const string &target,
                                           const redshift_driver::DriverIdentity &identity, const string &sql);

//! Throws IOException for a transport failure during the session setup of a new
//! connection to `target`. Unlike ThrowTransportFailure's, the message says that no statement of
//! the caller's was sent, and `extra_info` names the driver. No record's text appears.
[[noreturn]] void ThrowSessionSetupLoss(const DiagnosticSet &records, const string &target,
                                        const redshift_driver::DriverIdentity &identity);

//! Logs the records of a call that succeeded with records, one line each, through `sink` at debug
//! level: the SQLSTATE alone for a connect-phase record, and for a statement record also its
//! sanitized text when its FailureSqlState is in a class whose server text a failure shows.
void LogInformationalDiagnostics(RedshiftLogSink &sink, const DiagnosticSet &records);

} // namespace duckdb
