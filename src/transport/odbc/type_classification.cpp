#include "odbc/type_classification.hpp"

#include "duckdb/common/numeric_utils.hpp"

namespace duckdb {

namespace {

//! One row of the classification table: the name the driver reports, the SQL type code that agrees with
//! it, and the type it means.
struct TypeNameRow {
	//! Compared exactly, so only the driver's own upper-case SQL spelling matches.
	const char *type_name;
	//! The code the driver reports for this name.
	int16_t sql_type;
	//! The code it reports instead with its Unicode option on (rslibpq.c `mapPgTypeToSqlType`);
	//! equal to `sql_type` for a name that has no wide form.
	int16_t wide_sql_type;
	RedshiftTypeId type;
};

//! The table, keyed by the upper-case SQL names rsutil.c `getTypeName` builds, not
//! by PostgreSQL's. GEOMETRY, GEOGRAPHY and HLLSKETCH (bytes of no stated encoding) and the
//! qualified intervals (no RedshiftTypeId) are absent, so they are UNKNOWN.
const TypeNameRow TYPE_NAME_TABLE[] = {{"BOOL", -7, -7, RedshiftTypeId::BOOLEAN},
                                       {"SMALLINT", 5, 5, RedshiftTypeId::SMALLINT},
                                       {"INTEGER", 4, 4, RedshiftTypeId::INTEGER},
                                       {"BIGINT", -5, -5, RedshiftTypeId::BIGINT},
                                       {"REAL", 7, 7, RedshiftTypeId::FLOAT},
                                       {"DOUBLE PRECISION", 8, 8, RedshiftTypeId::DOUBLE},
                                       {"NUMERIC", 2, 2, RedshiftTypeId::DECIMAL},
                                       {"CHARACTER", 1, -8, RedshiftTypeId::CHAR},
                                       {"CHARACTER VARYING", 12, -9, RedshiftTypeId::VARCHAR},
                                       {"SUPER", -1, -10, RedshiftTypeId::VARCHAR},
                                       {"DATE", 91, 91, RedshiftTypeId::DATE},
                                       {"TIME", 92, 92, RedshiftTypeId::TIME},
                                       {"TIMETZ", 92, 92, RedshiftTypeId::TIMETZ},
                                       {"TIMESTAMP", 93, 93, RedshiftTypeId::TIMESTAMP},
                                       {"TIMESTAMPTZ", 93, 93, RedshiftTypeId::TIMESTAMPTZ},
                                       {"VARBYTE", -4, -4, RedshiftTypeId::BLOB}};

//! Trimming removes ASCII whitespace and nothing else. Written out, not
//! taken from `StringUtil::RTrim`, because RTrim's set is DuckDB's to change between releases.
const char *const ASCII_WHITESPACE = " \t\n\v\f\r";

string TrimAsciiWhitespace(const string &text) {
	const idx_t first = text.find_first_not_of(ASCII_WHITESPACE);
	if (first == string::npos) {
		return string();
	}
	return text.substr(first, text.find_last_not_of(ASCII_WHITESPACE) - first + 1);
}

//! The row whose name is exactly `type_name`, or nullptr when the table holds no such name.
//! No row's name is empty, so a column the driver reports no name for finds none.
const TypeNameRow *FindTypeNameRow(const string &type_name) {
	for (const TypeNameRow &row : TYPE_NAME_TABLE) {
		if (type_name == row.type_name) {
			return &row;
		}
	}
	return nullptr;
}

//! The bounds of the precision range, which are Redshift's own limits on NUMERIC.
const uint64_t MIN_DECIMAL_PRECISION = 1;
const uint64_t MAX_DECIMAL_PRECISION = 38;

//! 1 <= p <= 38 and 0 <= s <= p. Takes the driver's widths, so nothing
//! is narrowed before this has accepted it.
bool DecimalMetadataInRange(uint64_t precision, int16_t scale) {
	if (precision < MIN_DECIMAL_PRECISION || precision > MAX_DECIMAL_PRECISION) {
		return false;
	}
	// Compared as signed, so a negative scale is caught by the lower bound rather than by
	// wrapping to a value above the upper one. Precision is at most 38 here, so it converts.
	const int64_t signed_precision = static_cast<int64_t>(precision);
	return scale >= 0 && scale <= signed_precision;
}

//! The code confirms the name and never replaces it: -4, 92 and 93 each serve several Redshift
//! types, and rslibpq.c `applyVarcharPromotion` reports a CHARACTER VARYING wider than a
//! non-zero MaxVarcharSize with SUPER's -1.
bool SqlTypeAgreesWithRow(const TypeNameRow &row, int16_t sql_type) {
	return sql_type == row.sql_type || sql_type == row.wide_sql_type;
}

} // namespace

RedshiftColumn ClassifyColumn(const DriverColumnMetadata &metadata) {
	RedshiftColumn column;
	column.name = metadata.name;

	const TypeNameRow *row = FindTypeNameRow(TrimAsciiWhitespace(metadata.type_name));
	if (row == nullptr) {
		return column;
	}
	if (!SqlTypeAgreesWithRow(*row, metadata.sql_type)) {
		return column;
	}
	if (row->type == RedshiftTypeId::DECIMAL) {
		if (!DecimalMetadataInRange(metadata.column_size, metadata.decimal_digits)) {
			return column;
		}
		column.precision = NumericCast<int32_t>(metadata.column_size);
		column.scale = metadata.decimal_digits;
	}
	column.type = row->type;
	return column;
}

} // namespace duckdb
