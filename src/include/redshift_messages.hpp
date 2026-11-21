//===----------------------------------------------------------------------===//
//                         DuckDB - Redshift
//
// redshift_messages.hpp
//
// Sentences that more than one of the extension's error messages share, so they read the same
// wherever they appear, and whom the messages of a redshift_query call are written for.
//===----------------------------------------------------------------------===//

#pragma once

#include "duckdb/common/common.hpp"
#include "duckdb/common/sql_identifier.hpp"

namespace duckdb {

//! Whom a redshift_query call's messages are written for: the user who wrote the call, or, for a
//! call CONNECT routes a statement to, the user who typed the statement while CONNECT-ed to the
//! handle. That user wrote no redshift_query, and until DISCONNECT every statement
//! they type goes to Redshift, so those messages start with CONNECT and give Redshift advice as plain
//! SQL.
struct RedshiftCaller {
	explicit RedshiftCaller(string handle_p, bool routed_by_connect_p = false)
	    : handle(std::move(handle_p)), routed_by_connect(routed_by_connect_p) {
	}

	//! The attach handle, as the caller wrote it, or as CONNECT names it.
	string handle;
	//! Whether CONNECT routed the call.
	bool routed_by_connect;

	//! What starts each message.
	const char *Name() const {
		return routed_by_connect ? "CONNECT" : "redshift_query";
	}
	//! How to run the Redshift statement `sql` on the handle.
	string RunOnRedshift(const string &sql) const {
		if (routed_by_connect) {
			return sql;
		}
		return "redshift_query(" + SQLString::ToString(handle) + ", " + SQLString::ToString(sql) + ")";
	}
	//! What a refusal at execution calls the statement it refused.
	const char *ThisCall() const {
		return routed_by_connect ? "this statement" : "this call";
	}
	//! What ends the sentence of a refusal at execution that says ThisCall() was not run: for a call the
	//! user wrote, that other calls may have run (REDSHIFT_OTHER_CALLS_MAY_HAVE_RUN). A statement CONNECT
	//! routes is the only call in its DuckDB statement.
	string OtherCallsMayHaveRun() const;
};

//! The sentence this extension's own errors add once their statement may have run: the scan's
//! refusals (redshift_conversion.cpp), and a lost connection's report and an unusable session's,
//! for a failure from execution on (storage/redshift_connection_calls.cpp).
//! Running the statement again is not known to be safe, and no message may say it is.
constexpr const char *REDSHIFT_ALREADY_EXECUTED =
    "The statement may already have applied its effects on Redshift; check them before re-running it.";

//! What a refusal at execution adds: DuckDB starts a statement's scans one pipeline at a time, so
//! another call in the statement, on another handle, may have run before this one was refused. Used
//! by redshift_query.cpp's execution-time refusals and the shape refusal
//! (storage/redshift_connection_calls.cpp).
constexpr const char *REDSHIFT_OTHER_CALLS_MAY_HAVE_RUN =
    "other calls in the same DuckDB statement may have run; check their effects on Redshift before running "
    "the statement again.";

//! Whether an error of `type` is rethrown as it came, with no words of the extension's own added: an
//! interrupt, and the errors that end the statement whatever the extension would say.
inline bool ShouldRethrowErrorAsIs(ExceptionType type) {
	return type == ExceptionType::INTERRUPT || type == ExceptionType::FATAL || type == ExceptionType::INTERNAL ||
	       type == ExceptionType::OUT_OF_MEMORY;
}

inline string RedshiftCaller::OtherCallsMayHaveRun() const {
	if (routed_by_connect) {
		return ".";
	}
	return string(", though ") + REDSHIFT_OTHER_CALLS_MAY_HAVE_RUN;
}

} // namespace duckdb
