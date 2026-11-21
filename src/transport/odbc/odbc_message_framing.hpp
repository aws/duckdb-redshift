//===----------------------------------------------------------------------===//
//                         DuckDB - Redshift
//
// src/transport/odbc/odbc_message_framing.hpp
//
// How the driver frames a diagnostic message around libpq's text, and what the message says read
// as the driver wrote it, before redaction. Its own header, so the diagnostic reader takes it
// without the error path's header, which includes the reader's.
//===----------------------------------------------------------------------===//

#pragma once

#include "duckdb/common/string.hpp"
#include "duckdb/common/typedefs.hpp"

namespace duckdb {

//! Whether a record shows a lost connection, from its SQLSTATE and its message as the driver wrote
//! it: SQLSTATE class 08, or libpq text that starts with a text libpq writes when the connection
//! drops. The diagnostic reader asks before it redacts, so a bound value that
//! matches the driver's prefix cannot hide a loss.
bool RecordTextShowsConnectionLoss(const string &sqlstate, const string &raw_message);

//! Whether a record's SQLSTATE may be libpq's from an earlier server message rather than its own,
//! from its message as the driver wrote it. Asked before redaction for the same reason, so a bound
//! value that matches the driver's framing cannot keep a stale state.
bool RecordStateIsCarriedOver(const string &sqlstate, const string &raw_message);

//! Where the text a user may see starts in a record's message as the driver wrote it: past the
//! driver's stamp of `sqlstate`, or a failed streaming read's opening and lead-in, or else the driver's
//! prefix and any "[Server]" after it; 0 with no framing.
idx_t ShownTextStart(const string &sqlstate, const string &raw_message);

//! `shown`, trimmed, without the server severity word (ERROR:, FATAL: or PANIC:, in any case) that opens
//! it, and in `had_severity` whether one did.
string WithoutSeverity(const string &shown, bool &had_severity);

//! Whether a record shows Redshift answering in a way that leaves the session unusable, from its SQLSTATE and its
//! message as the driver wrote it: SQLSTATE XX000, not carried over, and a server first line after the framing
//! (ShownTextStart) that, without its severity word and in any case, is "Internal Service Error", or starts "Existing
//! writable session" and says "is still open". Asked before redaction too, which replaces each bound value of 8
//! bytes or more wherever it occurs, so a value that matches part of the server's line cannot hide the answer.
bool RecordTextShowsUnusableSession(const string &sqlstate, const string &raw_message);

} // namespace duckdb
