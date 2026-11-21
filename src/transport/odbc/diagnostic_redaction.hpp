//===----------------------------------------------------------------------===//
//                         DuckDB - Redshift
//
// src/transport/odbc/diagnostic_redaction.hpp
//
// What driver diagnostic text loses before anyone sees it: the credential forms,
// and in a server message a user may read, the infrastructure details and markup.
// The text can echo a bound parameter or an IAM token, so redaction is defence
// in depth: it replaces the forms it knows and cannot catch a value rendered some other way.
// Pure logic: no ODBC header, no driver call, no DuckDB runtime state.
//===----------------------------------------------------------------------===//

#pragma once

#include "duckdb/common/string.hpp"
#include "duckdb/common/typedefs.hpp"
#include "duckdb/common/vector.hpp"

#include <cstdint>

namespace duckdb {

//! Whether a message reached the reader whole, or the driver may have cut it short.
enum class MessageExtent : uint8_t { WHOLE, CUT };

//! A redacted message, and where the caller's chosen byte `from` of the original lands in `text`: at
//! the start of a replacement that covers it, and never past the end.
struct RedactedText {
	string text;
	idx_t from;
};

//! A connect-phase message with the forms replaced: access-key IDs, the values of
//! its credential keywords, the literal after a SQL credential keyword such as `SECRET_ACCESS_KEY`,
//! and runs of 40 or more token characters.
//! A CUT message also ends with `...` in place of its last word, where a split token would show.
//! The result is for classification, and for one more use: a connect failure no category names shows
//! its record's text, this redaction's output through SanitizeServerMessage and then this redaction
//! again, as the driver's reason. No other connect-phase driver text reaches an exception, and none a
//! log.
//! `from` is the byte of `message` whose place the result gives; it is kept so both phases build records alike.
RedactedText RedactConnectDiagnostic(const string &message, MessageExtent extent, idx_t from);

//! A statement-phase message with access-key IDs, assignments and SQL credential literals replaced,
//! and each `bound_values[i]` of 8 bytes or more, or a CUT message's end that starts one, replaced
//! by `<param i+1>`, since a parameter can carry a token. A CUT message loses its last word, as
//! above; long runs stay readable, as a customer's identifiers look the same. A replacement for text
//! that held a line end ends with one, so the first line a user sees takes in nothing after it.
//! `from` is the byte of `message` whose place the result gives, as above.
RedactedText RedactStatementDiagnostic(const string &message, const vector<string> &bound_values, MessageExtent extent,
                                       idx_t from);

//! A redacted statement message as a user or a log may see it: its first line,
//! with hosts, addresses, process IDs and URLs as markers, markup made inert, control characters,
//! line separators, other spaces and each byte of no valid UTF-8 character as spaces, bidirectional
//! controls and characters that show nothing left out, credential forms redacted again on the line
//! as joined, and a cut at 256 bytes that splits no marker or UTF-8 sequence. Pass it redaction's
//! output, so that a cut never splits a credential redaction would have recognised.
string SanitizeServerMessage(const string &redacted_message);

} // namespace duckdb
