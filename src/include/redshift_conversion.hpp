//===----------------------------------------------------------------------===//
//                         DuckDB - Redshift
//
// redshift_conversion.hpp
//
// Pure conversion of a transport result to DuckDB: column types at bind, cell values at
// scan. Nothing here opens a connection or reads configuration, so all of it is testable
// by constructing transport-neutral columns and values directly.
//===----------------------------------------------------------------------===//

#pragma once

#include "duckdb/common/identifier.hpp"
#include "duckdb/common/types.hpp"
#include "duckdb/common/types/data_chunk.hpp"
#include "redshift_messages.hpp"
#include "redshift_transport.hpp"

namespace duckdb {

// Converting a result happens in two separate steps:
//
//   1. Column types, once at bind. RedshiftTypeMapping reads each RedshiftColumn's type ID
//      (and DECIMAL precision and scale) and returns the DuckDB LogicalType the statement
//      binds as. It never sees a value.
//   2. Cell values, at scan. RedshiftCellWriter takes each RedshiftValue of each row and
//      writes it into the DuckDB output vector of its column.
//
// A RedshiftValue does not say what type it is, only which of its fields holds the value:
// its RedshiftValueKind. The transport fixes one kind per column type (integers arrive as
// INTEGER, DECIMAL and the temporal types as TEXT, and so on), so step 2 checks each cell's
// kind against its column's before reading it.

//===--------------------------------------------------------------------===//
// 1. Column types
//===--------------------------------------------------------------------===//

//! The columns redshift_query publishes for a described statement, in order.
struct RedshiftOutputSchema {
	vector<LogicalType> types;
	vector<Identifier> names;
};

//! Maps transport column types to the DuckDB types a statement binds as.
struct RedshiftTypeMapping {
	//! Map one column to its DuckDB type.
	//!
	//! Throws BinderException naming the column for UNKNOWN, and for DECIMAL metadata DuckDB cannot
	//! represent, which is never narrowed or read as floating point. `column_index` is the 0-based
	//! position, shown 1-based in the error.
	static LogicalType ToLogicalType(const RedshiftColumn &column, idx_t column_index);

	//! Map every column of a described shape, in order.
	static vector<LogicalType> ToLogicalTypes(const vector<RedshiftColumn> &columns);

	//! The columns `shape` publishes, or one `rowcount BIGINT` without a result set, with repeated names
	//! renamed as DuckDB's own scans rename them. Throws BinderException,
	//! written for `caller`, for a shape whose returns_rows flag and columns disagree.
	static RedshiftOutputSchema ToOutputSchema(const RedshiftStatementShape &shape, const RedshiftCaller &caller);
};

//===--------------------------------------------------------------------===//
// 2. Cell values
//===--------------------------------------------------------------------===//

//! Writes transport rows straight into a DuckDB output chunk, one cell at a time.
//!
//! Each cell is converted completely when it is written: integers, floats and booleans are stored
//! into flat vector data, VARCHAR and BLOB are copied into the string heap, CHAR is copied there
//! without its trailing blanks, and DECIMAL and temporal text is parsed straight into the output
//! vector by DuckDB's per-value parsing routines.
//! No DuckDB Value is built per cell. Text that cannot be converted exactly is refused: trailing
//! text, including whitespace; DECIMAL text with more fraction digits than the scale or in any
//! other form than a plain number; and temporal text that lacks a part its type needs or carries
//! one it does not take (a TIMESTAMP or TIMESTAMPTZ needs a time of day, a TIMETZ or TIMESTAMPTZ
//! needs a numeric offset, a TIMESTAMP takes no time zone, and a TIMESTAMPTZ takes no zone name),
//! or is otherwise outside the grammar of the client's text form for its type, which also admits
//! lossless variants such as "+05:00" for "+05". Rows may come from several transport batches. The
//! writer stores values without sizing any vector, so the caller then calls
//! DataChunk::SetChildCardinality with the number of rows written.
//!
//! Every error WriteRow raises is raised after the statement ran on Redshift, so none says that
//! re-running is safe. A cell of the wrong value kind, or a row of the wrong width, is a transport
//! contract violation and raises ExecutorException. A value DuckDB refuses raises
//! ConversionException naming the column and saying how to read the value as text instead. The
//! caller discards the chunk.
class RedshiftCellWriter {
public:
	//! Prepare to write rows of the described shape. The column types come from
	//! RedshiftTypeMapping, so this throws its BinderException for a column it cannot map.
	explicit RedshiftCellWriter(vector<RedshiftColumn> redshift_columns);

	//! The DuckDB type of each output column, in shape order, for allocating the output chunk.
	const vector<LogicalType> &DuckDBColumnTypes() const {
		return duckdb_column_types;
	}

	//! Write one row into `output` at `output_row`, which must be below STANDARD_VECTOR_SIZE.
	void WriteRow(const vector<RedshiftValue> &row, DataChunk &output, idx_t output_row);

private:
	//! The RedshiftValueKind every non-NULL cell of a column of `type` must arrive as.
	static RedshiftValueKind RequiredKind(RedshiftTypeId type);

	void WriteCell(idx_t column_index, const RedshiftValue &value, DataChunk &output, idx_t output_row);

	//! The columns as the transport described them: name, Redshift type, and DECIMAL precision and
	//! scale.
	vector<RedshiftColumn> redshift_columns;
	//! The DuckDB type of each column, in the same order, from RedshiftTypeMapping. A DECIMAL cell
	//! is stored at its type's storage width.
	vector<LogicalType> duckdb_column_types;
};

} // namespace duckdb
