//===----------------------------------------------------------------------===//
//                         DuckDB - Redshift
//
// src/transport/odbc/type_classification.hpp
//
// One result column's driver metadata to a RedshiftTypeId. The type name
// decides and the SQL type code only confirms it, because several Redshift types share a code.
// Pure: no driver call and no ODBC header; the caller passes the metadata as plain values.
//===----------------------------------------------------------------------===//

#pragma once

#include "duckdb/common/string.hpp"
#include "redshift_transport.hpp"

#include <cstdint>

namespace duckdb {

//! What the driver reported about one result column, from `SQLDescribeCol` and the
//! `SQL_DESC_TYPE_NAME` field, kept in the driver's own widths so nothing is narrowed before
//! the range check accepts it.
struct DriverColumnMetadata {
	//! The column's name, copied to the result unchanged.
	string name;
	//! `SQL_DESC_TYPE_NAME` as reported; empty for a type the driver has no name for
	//! (rsutil.c `getTypeName`'s default case).
	string type_name;
	//! `SQLDescribeCol`'s concise SQL type code.
	int16_t sql_type = 0;
	//! `SQLDescribeCol`'s column size, which is the precision for NUMERIC.
	uint64_t column_size = 0;
	//! `SQLDescribeCol`'s decimal digits, which is the scale for NUMERIC.
	int16_t decimal_digits = 0;
};

//! Classifies one column by its trimmed type name, confirmed by its SQL type code. Only DECIMAL
//! carries a precision and scale, the driver's own; a column the classification table
//! or the DECIMAL range check refuses is UNKNOWN, never an exception.
RedshiftColumn ClassifyColumn(const DriverColumnMetadata &metadata);

} // namespace duckdb
