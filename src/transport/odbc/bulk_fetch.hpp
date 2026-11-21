//===----------------------------------------------------------------------===//
//                         DuckDB - Redshift
//
// src/transport/odbc/bulk_fetch.hpp
//
// How a result's columns are fetched: each bounded column at a width its
// type sets, from the driver's reported size only for a character column or a DECIMAL's precision,
// bound column-wise over a ring of row arrays, the buffer sets, whose buffers and values share the
// result's budget, or over a row array of 1 for the columns before a
// result's first long column.
//===----------------------------------------------------------------------===//

#pragma once

#include "duckdb/common/common.hpp"
#include "odbc_driver.hpp"
#include "redshift_transport.hpp"

namespace duckdb {

//! The fetch sizes: a character column declared longer than LONG_COLUMN_BYTES is a long column, and
//! a batch, read either way, is one of RING_SETS sets: at most MAX_BATCH_ROWS rows, whose values take
//! about a set's share of MAX_BATCH_BYTES.
constexpr SQLULEN LONG_COLUMN_BYTES = 1024;
constexpr SQLULEN MAX_BATCH_BYTES = SQLULEN(16) * 1024 * 1024;
constexpr SQLULEN MAX_BATCH_ROWS = 2048;
//! The buffer sets a result fetches into in turn, which share MAX_BATCH_BYTES: the measured best
//! depth for a producer thread that fills one set while the caller reads another.
constexpr idx_t RING_SETS = 4;

//! How BOOLEAN, the integers, FLOAT and DOUBLE are fetched: as text
//! NextBatch parses with no locale, or in the native C type the driver parses the text into. Its
//! float parse is atof, which follows the locale and turns finite text past the range into an
//! infinity with no error (public source v2.3.0, rsutil.c).
enum class NumberBinding : uint8_t { TEXT, NATIVE };
//! The binding a shipped build uses: text.
constexpr NumberBinding DEFAULT_NUMBER_BINDING = NumberBinding::TEXT;

//! How one result column is fetched: bound as `c_type` at `width` bytes a row, the NUL included for
//! text, or, when `width` is 0, a long column read whole with SQLGetData.
struct ColumnBinding {
	SQLSMALLINT c_type;
	SQLLEN width;

	//! Whether the column is read whole with SQLGetData rather than bound.
	bool IsLong() const {
		return width == 0;
	}
	//! Whether its cells are numbers in their native C type, of `width` bytes, rather than text or bytes.
	bool IsNative() const {
		return !IsLong() && c_type != SQL_C_CHAR;
	}
	//! The most bytes a value can have and still be whole in its buffer.
	SQLLEN Holds() const {
		return c_type == SQL_C_CHAR ? width - 1 : width;
	}
};

//! How the bulk fetch reads `column`, which the driver described at `reported_size`, with its
//! numbers bound as `numbers` says. Only a VARCHAR or CHAR width comes from that size, and a
//! DECIMAL's from its precision, which is that size, plus 3: for the other types the driver reports
//! fewer bytes than their longest text.
ColumnBinding BindingOf(const RedshiftColumn &column, SQLULEN reported_size, NumberBinding numbers);

//! The rows one fetch of these bounded columns takes, into one of RING_SETS buffer sets: as many as
//! the set's share of MAX_BATCH_BYTES holds, counting each row's buffers, length indicators and
//! status and the RedshiftValue each cell becomes, but not the text copied into it, from 1 to
//! MAX_BATCH_ROWS.
SQLULEN RowArraySize(const vector<ColumnBinding> &bindings);

//! The bytes the process's fetch buffers hold now, bound sets and arenas alike, but not the batch built
//! from them: the allocation counter that shows a result's sets stay inside its budget and are freed
//! at the end of its rows.
idx_t HeldFetchBytes();
//! Adds `added` bytes to what HeldFetchBytes reports and removes `removed`.
void CountFetchBytes(idx_t added, idx_t removed);

//! Appends `length` bytes of a long value's SQLGetData part to `out`, the value's own string.
//! `remaining` is the part's indicator: when it gives the length still to come and `out` lacks room
//! for it, `out` first reserves the rest of the value, so a string that starts empty is allocated
//! once, at the value's size.
void AppendLongPart(string &out, const char *part, idx_t length, SQLLEN remaining);

//! The buffers row-array fetches fill column by column, in `sets` sets of equal layout, with the
//! latest fetch's rows fetched and row status. A fetch fills the selected set, which the driver
//! finds through SQL_ATTR_ROW_BIND_OFFSET_PTR, so the columns are bound once.
class BoundRowArray {
public:
	//! Buffers for `sets` sets of `rows` rows of columns 1 to bindings.size(), each bounded; there may
	//! be none. Set 0 is selected.
	BoundRowArray(const vector<ColumnBinding> &bindings, SQLULEN rows, idx_t sets = 1);
	~BoundRowArray();
	BoundRowArray(const BoundRowArray &) = delete;
	BoundRowArray &operator=(const BoundRowArray &) = delete;

	//! Points `statement`'s row array at set 0's buffers and binds every column, and with more than
	//! one set names the bind offset that selects the others. Returns false at the first call that
	//! fails, whose records are then on the statement.
	bool Bind(const redshift_driver::OdbcApi &api, SQLHANDLE statement);
	//! Makes the next fetch fill set `set`, and the reads below read it.
	void Select(idx_t set);
	//! The rows the latest fetch reported.
	SQLULEN Fetched() const {
		return fetched;
	}
	//! The bytes of every set, which HeldFetchBytes counts.
	idx_t Bytes() const {
		return sets * set_bytes;
	}
	//! The columns bound, the result's first ones.
	idx_t ColumnCount() const {
		return columns.size();
	}
	//! The status the latest fetch gave `row`, counting from 0.
	SQLUSMALLINT Status(idx_t row) const {
		return status[row];
	}
	//! How `column`, counting from 0, is bound.
	const ColumnBinding &Binding(idx_t column) const {
		return columns[column].binding;
	}
	//! The length indicator of `column`'s cell in `row` of the selected set, both counting from 0.
	SQLLEN Length(idx_t column, idx_t row) const {
		return LengthIn(selected, column, row);
	}
	//! The first byte of that cell's buffer, aligned for the column's C type.
	const char *Value(idx_t column, idx_t row) const {
		return ValueIn(selected, column, row);
	}
	//! The length indicator of `column`'s cell in `row` of set `set`, independent of the selected set.
	SQLLEN LengthIn(idx_t set, idx_t column, idx_t row) const;
	//! The first byte of that cell's buffer, aligned for the column's C type.
	const char *ValueIn(idx_t set, idx_t column, idx_t row) const;

private:
	struct Column {
		ColumnBinding binding;
		//! Where the column's length indicators and values start in a set.
		idx_t lengths_offset;
		idx_t values_offset;
	};

	SQLULEN rows;
	idx_t sets;
	vector<Column> columns;
	//! The bytes of one set. Every column's indicators and values start at a multiple of SQLLEN's
	//! size, as does every set, so a native number's buffer is aligned for it.
	idx_t set_bytes = 0;
	//! The sets, one after another; SQLLEN elements keep the indicators aligned.
	vector<SQLLEN> storage;
	idx_t selected = 0;
	//! The driver reads it at each fetch: the selected set's distance from set 0, in bytes.
	SQLULEN bind_offset = 0;
	SQLULEN fetched = 0;
	vector<SQLUSMALLINT> status;
};

} // namespace duckdb
