//===----------------------------------------------------------------------===//
//                         DuckDB - Redshift
//
// src/transport/odbc/text_forms.hpp
//
// Whether a value's text is the client's text form for its column's type. Under the
// session settings the client pins the server sends these forms, so this checks a form and never converts one.
// Pure: no driver call, no driver handle and no ODBC header.
//===----------------------------------------------------------------------===//

#pragma once

#include "duckdb/common/string.hpp"
#include "redshift_transport.hpp"

namespace duckdb {

//! Whether `text` is exactly the client's text form for `column`'s type. Any VARCHAR text matches.
//! The numbers read as text and parsed match the server's output: an integer as a DECIMAL of scale
//! 0, a FLOAT or DOUBLE as a float. No text matches any other type the client does not carry as
//! `TEXT`. Only the form is checked (a field's digit count, not its range, nor a DECIMAL's precision).
bool MatchesTextForm(const RedshiftColumn &column, const string &text);

} // namespace duckdb
