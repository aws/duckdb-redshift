#include "odbc/bulk_fetch.hpp"

#include "duckdb/common/exception.hpp"
#include "duckdb/common/numeric_utils.hpp"
#include "odbc/odbc_return_code.hpp"

#include <atomic>
#include <cstdint>

namespace duckdb {

namespace {

//! The bytes before the NUL of each temporal type's longest valid text. Named
//! for the text, as the Windows SDK's mmsystem.h defines TIME_BYTES as a macro.
constexpr SQLLEN DATE_TEXT_BYTES = 13;
constexpr SQLLEN TIME_TEXT_BYTES = 15;
constexpr SQLLEN TIMETZ_TEXT_BYTES = 24;
constexpr SQLLEN TIMESTAMP_TEXT_BYTES = 29;
constexpr SQLLEN TIMESTAMPTZ_TEXT_BYTES = 38;
//! An integer's text, as long as a BIGINT's minimum; a float's or a boolean's; and what a DECIMAL adds
//! to its digits: a sign, a leading 0 and the point.
constexpr SQLLEN INTEGER_BYTES = 20;
constexpr SQLLEN FLOAT_BYTES = 32;
constexpr SQLLEN DECIMAL_EXTRA_BYTES = 3;

//! Text of up to `bytes` bytes, bound with room for the NUL.
ColumnBinding Text(SQLLEN bytes) {
	return ColumnBinding {SQL_C_CHAR, bytes + 1};
}

//! A fixed-size native carrier of `width` bytes.
ColumnBinding Native(SQLSMALLINT c_type, size_t width) {
	return ColumnBinding {c_type, NumericCast<SQLLEN>(width)};
}

ColumnBinding Long(SQLSMALLINT c_type) {
	return ColumnBinding {c_type, 0};
}

//! `bytes` rounded up to a whole number of SQLLEN, so what follows it is aligned for one.
idx_t AlignedForLength(idx_t bytes) {
	return (bytes + sizeof(SQLLEN) - 1) / sizeof(SQLLEN) * sizeof(SQLLEN);
}

//! What HeldFetchBytes reports. A function's static, so it is ready before any result uses it.
std::atomic<idx_t> &HeldBytesCounter() {
	static std::atomic<idx_t> held {0};
	return held;
}

} // namespace

idx_t HeldFetchBytes() {
	return HeldBytesCounter();
}

void CountFetchBytes(idx_t added, idx_t removed) {
	// One update, so no reader sees the bytes added but not yet those removed; the difference wraps
	// when fewer are added, and the sum with it is still exact.
	HeldBytesCounter() += added - removed;
}

void AppendLongPart(string &out, const char *part, idx_t length, SQLLEN remaining) {
	// Only a reservation that grows `out` is asked for: before C++20 a smaller one may shrink it, and
	// GCC 8's libstdc++ (RHEL 8's) does.
	if (remaining > 0 && out.size() + NumericCast<idx_t>(remaining) > out.capacity()) {
		out.reserve(out.size() + NumericCast<idx_t>(remaining));
	}
	out.append(part, length);
}

ColumnBinding BindingOf(const RedshiftColumn &column, SQLULEN reported_size, NumberBinding numbers) {
	const bool numbers_as_text = numbers == NumberBinding::TEXT;
	switch (column.type) {
	case RedshiftTypeId::BOOLEAN:
		return numbers_as_text ? Text(FLOAT_BYTES) : Native(SQL_C_BIT, sizeof(SQLCHAR));
	case RedshiftTypeId::SMALLINT:
	case RedshiftTypeId::INTEGER:
	case RedshiftTypeId::BIGINT:
		return numbers_as_text ? Text(INTEGER_BYTES) : Native(SQL_C_SBIGINT, sizeof(SQLBIGINT));
	case RedshiftTypeId::FLOAT:
	case RedshiftTypeId::DOUBLE:
		return numbers_as_text ? Text(FLOAT_BYTES) : Native(SQL_C_DOUBLE, sizeof(SQLDOUBLE));
	case RedshiftTypeId::DECIMAL:
		return Text(column.precision + DECIMAL_EXTRA_BYTES);
	case RedshiftTypeId::DATE:
		return Text(DATE_TEXT_BYTES);
	case RedshiftTypeId::TIME:
		return Text(TIME_TEXT_BYTES);
	case RedshiftTypeId::TIMETZ:
		return Text(TIMETZ_TEXT_BYTES);
	case RedshiftTypeId::TIMESTAMP:
		return Text(TIMESTAMP_TEXT_BYTES);
	case RedshiftTypeId::TIMESTAMPTZ:
		return Text(TIMESTAMPTZ_TEXT_BYTES);
	case RedshiftTypeId::VARCHAR:
	case RedshiftTypeId::CHAR:
		// The driver reports size 0 when the server sends no type modifier (public source v2.3.0,
		// rslibpq.c getResultDescription): text, the unqualified interval. Bound at that size, every
		// non-empty value would fail.
		if (reported_size == 0 || reported_size > LONG_COLUMN_BYTES) {
			return Long(SQL_C_CHAR);
		}
		return Text(NumericCast<SQLLEN>(reported_size));
	case RedshiftTypeId::BLOB:
		return Long(SQL_C_BINARY);
	case RedshiftTypeId::UNKNOWN:
		break;
	}
	throw InternalException("a Redshift result column with no carrier reached the fetch; Execute refuses one");
}

SQLULEN RowArraySize(const vector<ColumnBinding> &bindings) {
	SQLULEN row_bytes = sizeof(SQLUSMALLINT);
	for (const auto &binding : bindings) {
		// The cell's RedshiftValue counts too, as a narrow cell's outweighs its buffer. The text copied
		// out of the buffers does not, so a batch's text, at most a set's buffers, sits beside the
		// RING_SETS sets: up to about one share more than the budget. A row over a quarter of the
		// budget still gets one row a set; Redshift's column limit keeps a row under that.
		row_bytes += NumericCast<SQLULEN>(binding.width) + sizeof(SQLLEN) + sizeof(RedshiftValue);
	}
	return MinValue(MAX_BATCH_ROWS, MaxValue<SQLULEN>(1, MAX_BATCH_BYTES / (RING_SETS * row_bytes)));
}

BoundRowArray::BoundRowArray(const vector<ColumnBinding> &bindings, SQLULEN rows_p, idx_t sets_p)
    : rows(rows_p), sets(sets_p), status(NumericCast<idx_t>(rows)) {
	const auto count = NumericCast<idx_t>(rows);
	// A set holds every column's indicators, which stay aligned, then every column's values.
	for (const auto &binding : bindings) {
		columns.push_back(Column {binding, set_bytes, 0});
		set_bytes += count * sizeof(SQLLEN);
	}
	for (auto &column : columns) {
		column.values_offset = set_bytes;
		set_bytes = AlignedForLength(set_bytes + count * NumericCast<idx_t>(column.binding.width));
	}
	storage.resize(sets * set_bytes / sizeof(SQLLEN));
	CountFetchBytes(Bytes(), 0);
}

BoundRowArray::~BoundRowArray() {
	CountFetchBytes(0, Bytes());
}

bool BoundRowArray::Bind(const redshift_driver::OdbcApi &api, SQLHANDLE statement) {
	// ODBC passes an integer attribute in the pointer itself.
	const auto size = reinterpret_cast<SQLPOINTER>(static_cast<uintptr_t>(rows));
	if (!Succeeded(api.SetStmtAttr(statement, SQL_ATTR_ROW_ARRAY_SIZE, size, SQL_IS_UINTEGER)) ||
	    !Succeeded(api.SetStmtAttr(statement, SQL_ATTR_ROWS_FETCHED_PTR, &fetched, SQL_IS_POINTER)) ||
	    !Succeeded(api.SetStmtAttr(statement, SQL_ATTR_ROW_STATUS_PTR, status.data(), SQL_IS_POINTER))) {
		return false;
	}
	// The driver adds the offset to every bound value and indicator address at each fetch.
	if (sets > 1 &&
	    !Succeeded(api.SetStmtAttr(statement, SQL_ATTR_ROW_BIND_OFFSET_PTR, &bind_offset, SQL_IS_POINTER))) {
		return false;
	}
	auto *base = reinterpret_cast<char *>(storage.data());
	for (idx_t index = 0; index < columns.size(); index++) {
		auto &column = columns[index];
		auto *lengths = reinterpret_cast<SQLLEN *>(base + column.lengths_offset);
		if (!Succeeded(api.BindCol(statement, NumericCast<SQLUSMALLINT>(index + 1), column.binding.c_type,
		                           base + column.values_offset, column.binding.width, lengths))) {
			return false;
		}
	}
	return true;
}

void BoundRowArray::Select(idx_t set) {
	D_ASSERT(set < sets);
	selected = set;
	bind_offset = NumericCast<SQLULEN>(set * set_bytes);
}

SQLLEN BoundRowArray::LengthIn(idx_t set, idx_t column, idx_t row) const {
	const auto *start = reinterpret_cast<const char *>(storage.data()) + set * set_bytes;
	return reinterpret_cast<const SQLLEN *>(start + columns[column].lengths_offset)[row];
}

const char *BoundRowArray::ValueIn(idx_t set, idx_t column, idx_t row) const {
	const auto &bound = columns[column];
	const auto *start = reinterpret_cast<const char *>(storage.data()) + set * set_bytes;
	return start + bound.values_offset + row * NumericCast<idx_t>(bound.binding.width);
}

} // namespace duckdb
