//===----------------------------------------------------------------------===//
//                         DuckDB - Redshift
//
// src/transport/statement_shape.hpp
//
// The comparison of a bound shape with a statement's, which every client's Execute makes before it
// runs the statement (redshift_transport.hpp, Execute). Header-only and included by every client,
// the mock and the ODBC client alike, so both name a difference in the same words and no build file
// lists it.
//===----------------------------------------------------------------------===//

#pragma once

#include "duckdb/common/string.hpp"
#include "duckdb/common/string_util.hpp"
#include "redshift_transport.hpp"

namespace duckdb {

//! ShapeDifference's helpers, which nothing else calls.
namespace statement_shape_detail {

//! A type ID's name, as a difference names it.
inline const char *TypeName(RedshiftTypeId type) {
	switch (type) {
	case RedshiftTypeId::BOOLEAN:
		return "BOOLEAN";
	case RedshiftTypeId::SMALLINT:
		return "SMALLINT";
	case RedshiftTypeId::INTEGER:
		return "INTEGER";
	case RedshiftTypeId::BIGINT:
		return "BIGINT";
	case RedshiftTypeId::FLOAT:
		return "FLOAT";
	case RedshiftTypeId::DOUBLE:
		return "DOUBLE";
	case RedshiftTypeId::DECIMAL:
		return "DECIMAL";
	case RedshiftTypeId::VARCHAR:
		return "VARCHAR";
	case RedshiftTypeId::CHAR:
		return "CHAR";
	case RedshiftTypeId::DATE:
		return "DATE";
	case RedshiftTypeId::TIME:
		return "TIME";
	case RedshiftTypeId::TIMETZ:
		return "TIMETZ";
	case RedshiftTypeId::TIMESTAMP:
		return "TIMESTAMP";
	case RedshiftTypeId::TIMESTAMPTZ:
		return "TIMESTAMPTZ";
	case RedshiftTypeId::BLOB:
		return "BLOB";
	case RedshiftTypeId::UNKNOWN:
		break;
	}
	return "UNKNOWN";
}

//! A column as a difference names it: its name in quotes, its type and, for DECIMAL, its precision
//! and scale.
inline string ColumnText(const RedshiftColumn &column) {
	auto text = StringUtil::Format("\"%s\" %s", column.name, TypeName(column.type));
	if (column.type == RedshiftTypeId::DECIMAL) {
		text += StringUtil::Format("(%d,%d)", column.precision, column.scale);
	}
	return text;
}

} // namespace statement_shape_detail

//! The first difference between the shape the caller bound and the one the statement has now, in
//! words, or empty when there is none. Compares what the transport header's Execute contract lists:
//! whether the statement returns rows, then each column's name, type ID, and DECIMAL precision and
//! scale.
inline string ShapeDifference(const RedshiftStatementShape &bound, const RedshiftStatementShape &now) {
	if (bound.returns_rows != now.returns_rows) {
		return now.returns_rows ? "it was bound as a statement without a result set and now returns rows"
		                        : "it was bound as returning rows and now has no result set";
	}
	if (bound.columns.size() != now.columns.size()) {
		return StringUtil::Format("it was bound with %llu column%s and now has %llu", (uint64_t)bound.columns.size(),
		                          bound.columns.size() == 1 ? "" : "s", (uint64_t)now.columns.size());
	}
	for (idx_t c = 0; c < bound.columns.size(); c++) {
		auto &was = bound.columns[c];
		auto &is = now.columns[c];
		const bool decimal_differs =
		    is.type == RedshiftTypeId::DECIMAL && (was.precision != is.precision || was.scale != is.scale);
		if (was.name != is.name || was.type != is.type || decimal_differs) {
			return StringUtil::Format("column %llu was bound as %s and is now %s", (uint64_t)(c + 1),
			                          statement_shape_detail::ColumnText(was), statement_shape_detail::ColumnText(is));
		}
	}
	return string();
}

} // namespace duckdb
