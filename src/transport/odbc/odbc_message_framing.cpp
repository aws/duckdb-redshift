#include "odbc/odbc_message_framing.hpp"

#include "duckdb/common/string_util.hpp"
#include "duckdb/common/vector.hpp"

#include <cstring>

namespace duckdb {

namespace {

//! What the driver writes before a record it adds with no connection (source v2.3.0 rsodbc.h
//! ODBC_DRIVER_ERROR_PREFIX, rsutil.c addError).
constexpr const char *DRIVER_PREFIX = "[Redshift][ODBC Driver]";

//! What the driver writes before every record it adds with a connection; libpq's last SQLSTATE and
//! a colon follow whenever libpq holds one, whoever wrote the text (source v2.3.0 rsutil.c addError).
constexpr const char *SERVER_PREFIX = "[Redshift][ODBC Driver][Server]";

//! How the driver opens the record of a failed streaming-cursor read: libpq's text, if any, follows
//! STREAMING_READ_DETAIL, and the state is libpq's last or the one the driver sets for a link
//! failure (source v2.3.0 rsresult.cpp addCursorIOError and RS_SQLFetchScroll).
constexpr const char *STREAMING_READ_PREFIX =
    "[Redshift][ODBC Driver]An I/O error occurred while reading streaming cursor.";
//! The same for a client side cursor, whose state is HY000, or 24000 with no detail.
constexpr const char *CLIENT_CURSOR_READ_PREFIX =
    "[Redshift][ODBC Driver]An I/O error occurred while reading client side cursor.";
constexpr const char *STREAMING_READ_DETAIL = " Detail: ";
constexpr const char *LINK_FAILURE_SQLSTATE = "08S01";

//! Whether `text` opens with a server error's severity, as libpq writes it.
bool StartsWithServerSeverity(const string &text) {
	for (const auto &severity : {"ERROR:", "FATAL:", "PANIC:"}) {
		if (StringUtil::StartsWith(text, severity)) {
			return true;
		}
	}
	return false;
}

//! The driver's `[Server]<state>:` stamp on a message it took from libpq with state `sqlstate`.
string ServerStamp(const string &sqlstate) {
	return SERVER_PREFIX + sqlstate + ":";
}

//! Where libpq's text starts in a record's `message`, matched exactly: past the driver's stamp of its
//! `sqlstate`, or a failed streaming read's opening and STREAMING_READ_DETAIL; `string::npos` when
//! the driver framed no libpq text that way.
idx_t StampedLibpqTextStart(const string &message, const string &sqlstate) {
	const auto stamp = ServerStamp(sqlstate);
	if (StringUtil::StartsWith(message, stamp)) {
		return stamp.size();
	}
	const auto detail_opening = string(STREAMING_READ_PREFIX) + STREAMING_READ_DETAIL;
	return StringUtil::StartsWith(message, detail_opening) ? detail_opening.size() : string::npos;
}

//! What libpq writes, lower-cased, as the whole start of its error text when the connection under a
//! statement drops. Some of these leave the driver answering alive, so only the text shows the loss
//! (source v2.3.0 libpq fe-secure.c pqsecure_read and pqsecure_write, SSL_ERROR_ZERO_RETURN among
//! them, fe-misc.c pqSendSome and pqReadData, fe-exec.c PQsendQueryStart).
const vector<string> &ConnectionLossPhrases() {
	static const vector<string> PHRASES = {"server closed the connection unexpectedly",
	                                       "ssl connection has been closed unexpectedly",
	                                       "ssl syscall error:",
	                                       "ssl error:",
	                                       "decompress error:",
	                                       "could not receive data from server",
	                                       "could not send data to server",
	                                       "no connection to the server",
	                                       "connection not open"};
	return PHRASES;
}

//! `text`, lower-cased, after DRIVER_PREFIX, which it starts with.
string LowerAfterDriverPrefix(const string &text) {
	return StringUtil::Lower(text.substr(strlen(DRIVER_PREFIX)));
}

//! Where libpq's text starts in a record's lower-cased `message`: after DRIVER_PREFIX, then the "[Server]" of
//! SERVER_PREFIX, the native SQLSTATE and colon, and a cursor read's lead-in, each where present. A server error's
//! text there opens with its severity, so a value or name it quotes is not at the start.
idx_t LibpqTextStart(const string &message, const string &sqlstate) {
	static const auto DRIVER = StringUtil::Lower(DRIVER_PREFIX);
	static const auto SERVER = LowerAfterDriverPrefix(SERVER_PREFIX);
	static const auto DETAIL = StringUtil::Lower(STREAMING_READ_DETAIL);
	static const vector<string> CURSOR_READS = {LowerAfterDriverPrefix(STREAMING_READ_PREFIX) + DETAIL,
	                                            LowerAfterDriverPrefix(CLIENT_CURSOR_READ_PREFIX) + DETAIL};
	if (message.compare(0, DRIVER.size(), DRIVER) != 0) {
		return 0;
	}
	idx_t start = DRIVER.size();
	for (const auto &part : {SERVER, StringUtil::Lower(sqlstate) + ":"}) {
		if (message.compare(start, part.size(), part) == 0) {
			start += part.size();
		}
	}
	for (const auto &cursor_read : CURSOR_READS) {
		if (message.compare(start, cursor_read.size(), cursor_read) == 0) {
			return start + cursor_read.size();
		}
	}
	return start;
}

//! Whether libpq's text in `raw_message` is one it writes for a dropped connection.
bool StartsWithConnectionLossPhrase(const string &sqlstate, const string &raw_message) {
	const auto message = StringUtil::Lower(raw_message);
	const auto start = LibpqTextStart(message, sqlstate);
	for (const auto &phrase : ConnectionLossPhrases()) {
		if (message.compare(start, phrase.size(), phrase) == 0) {
			return true;
		}
	}
	return false;
}

} // namespace

bool RecordTextShowsConnectionLoss(const string &sqlstate, const string &raw_message) {
	return StringUtil::StartsWith(sqlstate, "08") || StartsWithConnectionLossPhrase(sqlstate, raw_message);
}

bool RecordStateIsCarriedOver(const string &sqlstate, const string &raw_message) {
	// libpq keeps the state of an earlier error or notice until the next one (fe-protocol3.c,
	// _pqGetErrorNotice3), and the driver writes it with libpq's text, which is then this failure's
	// own only when a server severity opens it.
	const auto is_streaming_read =
	    StringUtil::StartsWith(raw_message, STREAMING_READ_PREFIX) && sqlstate != LINK_FAILURE_SQLSTATE;
	if (!StringUtil::StartsWith(raw_message, ServerStamp(sqlstate)) && !is_streaming_read) {
		return false;
	}
	const auto libpq_text_start = StampedLibpqTextStart(raw_message, sqlstate);
	return libpq_text_start == string::npos || !StartsWithServerSeverity(raw_message.substr(libpq_text_start));
}

idx_t ShownTextStart(const string &sqlstate, const string &raw_message) {
	const auto libpq_text_start = StampedLibpqTextStart(raw_message, sqlstate);
	if (libpq_text_start != string::npos) {
		return libpq_text_start;
	}
	// The driver writes SERVER_PREFIX alone until libpq holds a state, and DRIVER_PREFIX alone on a
	// record with no connection, such as 22003's "Numeric value out of range" (source v2.3.0 rsutil.c
	// addError). SERVER_PREFIX starts with DRIVER_PREFIX, so it is tried first.
	for (const auto prefix : {SERVER_PREFIX, DRIVER_PREFIX}) {
		if (StringUtil::StartsWith(raw_message, prefix)) {
			return strlen(prefix);
		}
	}
	return 0;
}

string WithoutSeverity(const string &shown, bool &had_severity) {
	auto text = shown;
	StringUtil::Trim(text);
	had_severity = false;
	for (const auto *severity : {"ERROR:", "FATAL:", "PANIC:"}) {
		if (StringUtil::StartsWith(StringUtil::Upper(text), severity)) {
			text = text.substr(strlen(severity));
			StringUtil::Trim(text);
			had_severity = true;
			break;
		}
	}
	return text;
}

bool RecordTextShowsUnusableSession(const string &sqlstate, const string &raw_message) {
	if (sqlstate != "XX000" || RecordStateIsCarriedOver(sqlstate, raw_message)) {
		return false;
	}
	// The server's first line, without the severity word and in lower case.
	auto first_line = raw_message.substr(ShownTextStart(sqlstate, raw_message));
	first_line = first_line.substr(0, first_line.find('\n'));
	bool had_severity = false;
	const auto text = StringUtil::Lower(WithoutSeverity(first_line, had_severity));
	return text == "internal service error" ||
	       (StringUtil::StartsWith(text, "existing writable session ") && StringUtil::Contains(text, " is still open"));
}

} // namespace duckdb
