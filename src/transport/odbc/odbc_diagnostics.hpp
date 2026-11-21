//===----------------------------------------------------------------------===//
//                         DuckDB - Redshift
//
// src/transport/odbc/odbc_diagnostics.hpp
//
// The one reader of the diagnostic records a failed or warning call leaves on a handle.
// It redacts each message as it reads it, so a caller of these readers never
// holds a message that redaction has not seen.
//===----------------------------------------------------------------------===//

#pragma once

#include "duckdb/common/string.hpp"
#include "duckdb/common/vector.hpp"
#include "odbc_driver.hpp"

#include <cstdint>

namespace duckdb {

//! The handle a call left its records on.
struct DiagnosticHandle {
	SQLSMALLINT type; //!< SQL_HANDLE_ENV, SQL_HANDLE_DBC or SQL_HANDLE_STMT
	SQLHANDLE value;
};

//! Which phase's rules redacted a set's messages. The connect phase runs from a connection's first
//! driver call until `SQLDriverConnect` succeeds or the attempt is abandoned.
enum class DiagnosticPhase : uint8_t { CONNECT, STATEMENT };

struct DiagnosticSet;

//! What a record shows in place of a SQLSTATE that is not five characters from [0-9A-Z]: in driver
//! 2.3.0.0 the state is often the server's own bytes, which must not reach a message or a log.
constexpr const char *UNREADABLE_SQLSTATE = "?????";

//! The records on `handle`, up to 64, each message redacted for the connect phase, for classification
//! and for the reason a connect failure no category names shows; no other connect-phase driver text
//! reaches an exception, and none a log.
//! Call it before any other call on that handle: ODBC clears a handle's records at its next call.
DiagnosticSet ReadConnectDiagnostics(const redshift_driver::OdbcApi &api, DiagnosticHandle handle);

//! The records on `handle`, up to 64, each message redacted for the statement phase against the
//! caller's parameter values in bind order. Call it before any other call on that handle, as for
//! ReadConnectDiagnostics.
DiagnosticSet ReadStatementDiagnostics(const redshift_driver::OdbcApi &api, DiagnosticHandle handle,
                                       const vector<string> &bound_values);

//! One diagnostic record, redacted. Its constructor is private, and its one friend is
//! DiagnosticReader, the class behind the two readers above.
class DiagnosticRecord {
public:
	//! The record's SQLSTATE as the driver wrote it when that is five characters from [0-9A-Z], and
	//! otherwise UNREADABLE_SQLSTATE, which a failure counts as HY000.
	const string &SqlState() const {
		return sqlstate;
	}
	//! Whether SqlState() is the state the driver wrote, not UNREADABLE_SQLSTATE.
	bool HasReadableSqlState() const {
		return sqlstate != UNREADABLE_SQLSTATE;
	}
	//! The record's native error code.
	int32_t Native() const {
		return native;
	}
	//! The record's message, redacted, then without trailing white space. A message the driver may
	//! have cut, one of 326 bytes (the most driver 2.3.0.0 keeps) or one that fills the largest buffer
	//! SQLGetDiagRec takes, loses the word the cut may have split and ends with `...`.
	const string &Message() const {
		return message;
	}
	//! Message() without the driver's framing (ShownTextStart in odbc_message_framing.hpp), found on
	//! the message before redaction, so a bound value that matches the framing cannot keep it shown.
	string UnframedMessage() const {
		return message.substr(unframed_from);
	}
	//! Whether the record shows a lost connection, judged on its message before redaction
	//! (RecordTextShowsConnectionLoss in odbc_message_framing.hpp).
	bool ShowsConnectionLoss() const {
		return shows_connection_loss;
	}
	//! Whether the driver may have carried SqlState() over from an earlier server message, judged on
	//! its message before redaction (RecordStateIsCarriedOver in odbc_message_framing.hpp).
	bool StateIsCarriedOver() const {
		return state_is_carried_over;
	}
	//! Whether the record shows Redshift answering in a way that leaves the session unusable, judged on its message
	//! before redaction (RecordTextShowsUnusableSession in odbc_message_framing.hpp).
	bool ShowsUnusableSession() const {
		return shows_unusable_session;
	}

private:
	//! What the reader judged on the raw message, before redaction.
	struct RawJudgement {
		bool shows_connection_loss;
		bool state_is_carried_over;
		bool shows_unusable_session;
	};
	DiagnosticRecord(string sqlstate, int32_t native, string message, idx_t unframed_from, RawJudgement judgement);

	string sqlstate;
	int32_t native;
	string message;
	idx_t unframed_from;
	bool shows_connection_loss;
	bool state_is_carried_over;
	bool shows_unusable_session;

	friend class DiagnosticReader;
};

//! The records of one call, in the order the driver numbers them.
struct DiagnosticSet {
	//! The phase whose rules redacted the messages.
	DiagnosticPhase phase;
	vector<DiagnosticRecord> records;
	//! Whether the set may lack some of the driver's records: a read failed, the driver counted more
	//! than it delivered, or the reader stopped at its cap of 64 with more counted or no count to go
	//! by.
	bool is_incomplete;

	//! Every record's SQLSTATE in order, separated by commas.
	string SqlStates() const;
};

} // namespace duckdb
