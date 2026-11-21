#include "odbc/odbc_diagnostics.hpp"

#include "duckdb/common/helper.hpp"
#include "duckdb/common/numeric_utils.hpp"
#include "duckdb/common/string_util.hpp"
#include "odbc/diagnostic_redaction.hpp"
#include "odbc/odbc_message_framing.hpp"
#include "odbc/odbc_return_code.hpp"

#include <algorithm>
#include <cstring>

namespace duckdb {

namespace {

//! The most records one read takes, so a driver with an endless list cannot keep the reader going.
constexpr idx_t MAX_DIAGNOSTIC_RECORDS = 64;

//! The largest buffer SQLGetDiagRec can be given, since it takes an SQLSMALLINT: 32766 bytes and the
//! NUL. Driver 2.3.0.0 keeps far less of a message; the buffer is for a driver that keeps more.
constexpr idx_t MAX_MESSAGE_CAPACITY = 32767;

//! The most bytes of a message driver 2.3.0.0 keeps: addError writes it, prefix included, into
//! szErrMsg, a char[MAX_ERR_MSG_LEN] of 327, and drops the rest (source v2.3.0, rsodbc.h, rsutil.c
//! addError).
constexpr idx_t DRIVER_MESSAGE_BYTES = 326;

//! Whether `written` bytes and their NUL fill a buffer of `capacity`, so the text may go on past it.
bool FillsBuffer(idx_t written, idx_t capacity) {
	return written + 1 == capacity;
}

//! Whether the driver may have cut a message of `written` bytes: one that fills the largest buffer,
//! or one as long as driver 2.3.0.0 lets a message be.
MessageExtent ExtentOf(idx_t written) {
	const auto is_cut = FillsBuffer(written, MAX_MESSAGE_CAPACITY) || written == DRIVER_MESSAGE_BYTES;
	return is_cut ? MessageExtent::CUT : MessageExtent::WHOLE;
}

//! `bytes` up to its NUL, at most `size` bytes.
string TextUpToNul(const SQLCHAR *bytes, idx_t size) {
	const auto *text = reinterpret_cast<const char *>(bytes);
	return string(text, strnlen(text, size));
}

//! Whether `text` has a SQLSTATE's form: five characters, each a digit or an upper-case letter.
bool HasSqlStateForm(const string &text) {
	return text.size() == SQL_SQLSTATE_SIZE && std::all_of(text.begin(), text.end(), [](char character) {
		       return StringUtil::CharacterIsDigit(character) || (character >= 'A' && character <= 'Z');
	       });
}

//! `message` without trailing white space: libpq ends a server message with a newline
//! (fe-protocol3.c pqGetErrorNotice3), which would leave a blank line in a message built around it.
string WithoutTrailingSpace(string message) {
	StringUtil::RTrim(message);
	return message;
}

} // namespace

//! Reads the records on one handle and redacts them with the function its phase uses.
class DiagnosticReader {
public:
	DiagnosticReader(const redshift_driver::OdbcApi &api_p, DiagnosticHandle handle_p) : api(api_p), handle(handle_p) {
	}

	template <class REDACT>
	DiagnosticSet Read(DiagnosticPhase phase, const REDACT &redact) const {
		DiagnosticSet set {phase, {}, false};
		const auto count = RecordCount();
		const auto limit =
		    count < 0 ? MAX_DIAGNOSTIC_RECORDS : MinValue(NumericCast<idx_t>(count), MAX_DIAGNOSTIC_RECORDS);
		for (idx_t number = 1; number <= limit; number++) {
			auto raw = ReadRecord(number);
			if (raw.rc == SQL_NO_DATA && count < 0) {
				return set;
			}
			if (!Succeeded(raw.rc)) {
				set.is_incomplete = true;
				return set;
			}
			// Judged before redaction, which can replace part of the driver's prefix, or of the server's line, with a
			// parameter marker.
			const DiagnosticRecord::RawJudgement judgement {RecordTextShowsConnectionLoss(raw.sqlstate, raw.message),
			                                                RecordStateIsCarriedOver(raw.sqlstate, raw.message),
			                                                RecordTextShowsUnusableSession(raw.sqlstate, raw.message)};
			// The framing's end is found before redaction too, and redaction says where it lands.
			auto redacted =
			    redact(raw.message, ExtentOf(raw.message.size()), ShownTextStart(raw.sqlstate, raw.message));
			// Trimmed only after redaction, which then still finds a bound value that ends in white space.
			auto message = WithoutTrailingSpace(std::move(redacted.text));
			const auto unframed_from = MinValue<idx_t>(redacted.from, message.size());
			set.records.push_back(
			    DiagnosticRecord(std::move(raw.sqlstate), raw.native, std::move(message), unframed_from, judgement));
		}
		// The loop ran to its limit: with no count the driver may hold more, and a count past the cap
		// says that it does.
		set.is_incomplete = count < 0 || NumericCast<idx_t>(count) > MAX_DIAGNOSTIC_RECORDS;
		return set;
	}

private:
	//! One record as SQLGetDiagRec returned it, before redaction.
	struct RawRecord {
		SQLRETURN rc;
		string sqlstate;
		int32_t native;
		string message;
	};

	//! How many records the driver says the handle holds, from the SQL_DIAG_NUMBER header field, or
	//! -1 when it will not say. A negative count is read as no count.
	int64_t RecordCount() const {
		SQLINTEGER count = 0;
		const auto rc = api.GetDiagField(handle.type, handle.value, 0, SQL_DIAG_NUMBER, &count, 0, nullptr);
		return Succeeded(rc) ? count : -1;
	}

	//! Record `number`, read again into the largest buffer when its message filled the first, so
	//! that redaction sees as much of it as SQLGetDiagRec can return.
	RawRecord ReadRecord(idx_t number) const {
		auto raw = ReadRecord(number, SQL_MAX_MESSAGE_LENGTH);
		if (FillsBuffer(raw.message.size(), SQL_MAX_MESSAGE_LENGTH)) {
			raw = ReadRecord(number, MAX_MESSAGE_CAPACITY);
		}
		return raw;
	}

	//! Record `number`, read into a buffer of `capacity` bytes.
	RawRecord ReadRecord(idx_t number, idx_t capacity) const {
		vector<SQLCHAR> buffer(capacity, 0);
		SQLCHAR state[SQL_SQLSTATE_SIZE + 1] = {};
		SQLINTEGER native = 0;
		// The length is not asked for: the bytes written up to the NUL show whether the message
		// filled the buffer.
		const auto rc = api.GetDiagRec(handle.type, handle.value, NumericCast<SQLSMALLINT>(number), state, &native,
		                               buffer.data(), NumericCast<SQLSMALLINT>(capacity), nullptr);
		return {rc, TextUpToNul(state, SQL_SQLSTATE_SIZE), native, TextUpToNul(buffer.data(), capacity)};
	}

	const redshift_driver::OdbcApi &api;
	DiagnosticHandle handle;
};

DiagnosticRecord::DiagnosticRecord(string sqlstate_p, int32_t native_p, string message_p, idx_t unframed_from_p,
                                   RawJudgement judgement)
    : sqlstate(HasSqlStateForm(sqlstate_p) ? std::move(sqlstate_p) : string(UNREADABLE_SQLSTATE)), native(native_p),
      message(std::move(message_p)), unframed_from(unframed_from_p),
      shows_connection_loss(judgement.shows_connection_loss), state_is_carried_over(judgement.state_is_carried_over),
      shows_unusable_session(judgement.shows_unusable_session) {
}

string DiagnosticSet::SqlStates() const {
	return StringUtil::Join(records, records.size(), ",",
	                        [](const DiagnosticRecord &record) { return record.SqlState(); });
}

DiagnosticSet ReadConnectDiagnostics(const redshift_driver::OdbcApi &api, DiagnosticHandle handle) {
	return DiagnosticReader(api, handle)
	    .Read(DiagnosticPhase::CONNECT, [](const string &message, MessageExtent extent, idx_t from) {
		    return RedactConnectDiagnostic(message, extent, from);
	    });
}

DiagnosticSet ReadStatementDiagnostics(const redshift_driver::OdbcApi &api, DiagnosticHandle handle,
                                       const vector<string> &bound_values) {
	return DiagnosticReader(api, handle)
	    .Read(DiagnosticPhase::STATEMENT, [&bound_values](const string &message, MessageExtent extent, idx_t from) {
		    return RedactStatementDiagnostic(message, bound_values, extent, from);
	    });
}

} // namespace duckdb
