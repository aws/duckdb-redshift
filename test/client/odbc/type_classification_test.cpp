// Column classification. Every expected value is a literal, so a wrong constant
// in the table fails a case rather than agreeing with it; one case pins each code the table uses
// to its ODBC constant.

#include "catch.hpp"

#include "odbc/type_classification.hpp"
#include "odbc_driver.hpp"

#include <cstdint>
#include <string>

using duckdb::ClassifyColumn;
using duckdb::DriverColumnMetadata;
using duckdb::RedshiftColumn;
using duckdb::RedshiftTypeId;

namespace {

//! The metadata the driver reports for a column whose size and digits carry a value, which
//! for NUMERIC are its precision and scale.
DriverColumnMetadata ReportedWithSize(const std::string &type_name, int16_t sql_type, uint64_t column_size,
                                      int16_t decimal_digits) {
	DriverColumnMetadata metadata = {"c", type_name, sql_type, column_size, decimal_digits};
	return metadata;
}

//! The metadata the driver reports for a column with no NUMERIC precision or scale.
DriverColumnMetadata Reported(const std::string &type_name, int16_t sql_type) {
	return ReportedWithSize(type_name, sql_type, 0, 0);
}

} // namespace

TEST_CASE("Each classified type name with its own SQL type code gets that type", "[odbc]") {
	CHECK(ClassifyColumn(Reported("BOOL", -7)).type == RedshiftTypeId::BOOLEAN);
	CHECK(ClassifyColumn(Reported("SMALLINT", 5)).type == RedshiftTypeId::SMALLINT);
	CHECK(ClassifyColumn(Reported("INTEGER", 4)).type == RedshiftTypeId::INTEGER);
	CHECK(ClassifyColumn(Reported("BIGINT", -5)).type == RedshiftTypeId::BIGINT);
	CHECK(ClassifyColumn(Reported("REAL", 7)).type == RedshiftTypeId::FLOAT);
	CHECK(ClassifyColumn(Reported("DOUBLE PRECISION", 8)).type == RedshiftTypeId::DOUBLE);
	CHECK(ClassifyColumn(ReportedWithSize("NUMERIC", 2, 18, 0)).type == RedshiftTypeId::DECIMAL);
	CHECK(ClassifyColumn(Reported("CHARACTER", 1)).type == RedshiftTypeId::CHAR);
	CHECK(ClassifyColumn(Reported("CHARACTER VARYING", 12)).type == RedshiftTypeId::VARCHAR);
	CHECK(ClassifyColumn(Reported("SUPER", -1)).type == RedshiftTypeId::VARCHAR);
	CHECK(ClassifyColumn(Reported("DATE", 91)).type == RedshiftTypeId::DATE);
	CHECK(ClassifyColumn(Reported("TIME", 92)).type == RedshiftTypeId::TIME);
	CHECK(ClassifyColumn(Reported("TIMETZ", 92)).type == RedshiftTypeId::TIMETZ);
	CHECK(ClassifyColumn(Reported("TIMESTAMP", 93)).type == RedshiftTypeId::TIMESTAMP);
	CHECK(ClassifyColumn(Reported("TIMESTAMPTZ", 93)).type == RedshiftTypeId::TIMESTAMPTZ);
	CHECK(ClassifyColumn(Reported("VARBYTE", -4)).type == RedshiftTypeId::BLOB);
}

TEST_CASE("A character or SUPER column carrying the driver's Unicode code gets its type", "[odbc]") {
	CHECK(ClassifyColumn(Reported("CHARACTER", -8)).type == RedshiftTypeId::CHAR);
	CHECK(ClassifyColumn(Reported("CHARACTER VARYING", -9)).type == RedshiftTypeId::VARCHAR);
	CHECK(ClassifyColumn(Reported("SUPER", -10)).type == RedshiftTypeId::VARCHAR);
}

TEST_CASE("A type name whose SQL type code disagrees with it is UNKNOWN", "[odbc]") {
	// -6 is the driver's other code for a name it spells BOOL, 5 is SMALLINT's, 91 is DATE's.
	CHECK(ClassifyColumn(Reported("BOOL", -6)).type == RedshiftTypeId::UNKNOWN);
	CHECK(ClassifyColumn(Reported("INTEGER", 5)).type == RedshiftTypeId::UNKNOWN);
	CHECK(ClassifyColumn(Reported("TIME", 91)).type == RedshiftTypeId::UNKNOWN);
	CHECK(ClassifyColumn(Reported("TIMESTAMPTZ", 92)).type == RedshiftTypeId::UNKNOWN);
	CHECK(ClassifyColumn(Reported("VARBYTE", -1)).type == RedshiftTypeId::UNKNOWN);
	CHECK(ClassifyColumn(ReportedWithSize("NUMERIC", 3, 10, 2)).type == RedshiftTypeId::UNKNOWN);
}

TEST_CASE("A CHARACTER VARYING promoted to SUPER's code is UNKNOWN, not VARCHAR", "[odbc]") {
	CHECK(ClassifyColumn(Reported("CHARACTER VARYING", -1)).type == RedshiftTypeId::UNKNOWN);
	CHECK(ClassifyColumn(Reported("CHARACTER VARYING", -10)).type == RedshiftTypeId::UNKNOWN);
}

TEST_CASE("A column the driver reports no type name for is UNKNOWN, whatever its code", "[odbc]") {
	CHECK(ClassifyColumn(Reported("", -4)).type == RedshiftTypeId::UNKNOWN);
	CHECK(ClassifyColumn(Reported("", 92)).type == RedshiftTypeId::UNKNOWN);
	CHECK(ClassifyColumn(Reported("", 93)).type == RedshiftTypeId::UNKNOWN);
	CHECK(ClassifyColumn(Reported("", 12)).type == RedshiftTypeId::UNKNOWN);
	CHECK(ClassifyColumn(Reported("", 4)).type == RedshiftTypeId::UNKNOWN);
	CHECK(ClassifyColumn(Reported("", 0)).type == RedshiftTypeId::UNKNOWN);
}

TEST_CASE("The four types that share code -4 are told apart by name", "[odbc]") {
	CHECK(ClassifyColumn(Reported("VARBYTE", -4)).type == RedshiftTypeId::BLOB);
	CHECK(ClassifyColumn(Reported("GEOMETRY", -4)).type == RedshiftTypeId::UNKNOWN);
	CHECK(ClassifyColumn(Reported("GEOGRAPHY", -4)).type == RedshiftTypeId::UNKNOWN);
	CHECK(ClassifyColumn(Reported("HLLSKETCH", -4)).type == RedshiftTypeId::UNKNOWN);
	CHECK(ClassifyColumn(Reported("BINARY", -4)).type == RedshiftTypeId::UNKNOWN);
}

TEST_CASE("The pairs that share codes 92 and 93 are told apart by name", "[odbc]") {
	CHECK(ClassifyColumn(Reported("TIME", 92)).type == RedshiftTypeId::TIME);
	CHECK(ClassifyColumn(Reported("TIMETZ", 92)).type == RedshiftTypeId::TIMETZ);
	CHECK(ClassifyColumn(Reported("TIMESTAMP", 93)).type == RedshiftTypeId::TIMESTAMP);
	CHECK(ClassifyColumn(Reported("TIMESTAMPTZ", 93)).type == RedshiftTypeId::TIMESTAMPTZ);
}

TEST_CASE("A qualified interval is UNKNOWN", "[odbc]") {
	CHECK(ClassifyColumn(Reported("INTERVAL YEAR TO MONTH", 107)).type == RedshiftTypeId::UNKNOWN);
	CHECK(ClassifyColumn(ReportedWithSize("INTERVAL DAY TO SECOND", 110, 26, 6)).type == RedshiftTypeId::UNKNOWN);
}

TEST_CASE("A lower-case PostgreSQL type name is UNKNOWN", "[odbc]") {
	CHECK(ClassifyColumn(Reported("bool", -7)).type == RedshiftTypeId::UNKNOWN);
	CHECK(ClassifyColumn(Reported("bool", 12)).type == RedshiftTypeId::UNKNOWN);
	CHECK(ClassifyColumn(Reported("int4", 4)).type == RedshiftTypeId::UNKNOWN);
	CHECK(ClassifyColumn(Reported("bpchar", 1)).type == RedshiftTypeId::UNKNOWN);
	CHECK(ClassifyColumn(Reported("character varying", 12)).type == RedshiftTypeId::UNKNOWN);
	CHECK(ClassifyColumn(ReportedWithSize("numeric", 2, 10, 2)).type == RedshiftTypeId::UNKNOWN);
}

TEST_CASE("A type name the table does not hold is UNKNOWN", "[odbc]") {
	// DECIMAL is the driver's other spelling for a fixed-point column, with its own code 3.
	CHECK(ClassifyColumn(ReportedWithSize("DECIMAL", 3, 10, 2)).type == RedshiftTypeId::UNKNOWN);
	CHECK(ClassifyColumn(Reported("TEXT", 12)).type == RedshiftTypeId::UNKNOWN);
	CHECK(ClassifyColumn(Reported("DOUBLE", 8)).type == RedshiftTypeId::UNKNOWN);
}

TEST_CASE("A type name is compared after its surrounding ASCII whitespace is removed", "[odbc]") {
	CHECK(ClassifyColumn(Reported("  BOOL", -7)).type == RedshiftTypeId::BOOLEAN);
	CHECK(ClassifyColumn(Reported("BOOL\t", -7)).type == RedshiftTypeId::BOOLEAN);
	CHECK(ClassifyColumn(Reported("CHARACTER VARYING ", 12)).type == RedshiftTypeId::VARCHAR);
	CHECK(ClassifyColumn(Reported(" \r\nDOUBLE PRECISION \v\f", 8)).type == RedshiftTypeId::DOUBLE);
	CHECK(ClassifyColumn(Reported("   ", 12)).type == RedshiftTypeId::UNKNOWN);
	// Only the surrounding whitespace goes: the comparison is otherwise exact.
	CHECK(ClassifyColumn(Reported("CHARACTER  VARYING", 12)).type == RedshiftTypeId::UNKNOWN);
	CHECK(ClassifyColumn(Reported("CHARACTER\tVARYING", 12)).type == RedshiftTypeId::UNKNOWN);
}

TEST_CASE("A type name carrying a byte above ASCII is UNKNOWN, not trimmed to one in the table", "[odbc]") {
	const std::string trailing = std::string("BOOL") + '\x80';
	CHECK(ClassifyColumn(Reported(trailing, -7)).type == RedshiftTypeId::UNKNOWN);

	const std::string leading = '\xff' + std::string("BOOL");
	CHECK(ClassifyColumn(Reported(leading, -7)).type == RedshiftTypeId::UNKNOWN);
}

TEST_CASE("NUMERIC carries the driver's precision and scale unchanged", "[odbc]") {
	const RedshiftColumn scaled = ClassifyColumn(ReportedWithSize("NUMERIC", 2, 10, 2));
	CHECK(scaled.type == RedshiftTypeId::DECIMAL);
	CHECK(scaled.precision == 10);
	CHECK(scaled.scale == 2);

	// A column declared `numeric` with no modifier: the driver reports 18 and 0.
	const RedshiftColumn bare = ClassifyColumn(ReportedWithSize("NUMERIC", 2, 18, 0));
	CHECK(bare.precision == 18);
	CHECK(bare.scale == 0);

	// Redshift's widest NUMERIC, and the narrowest, both kept as reported.
	const RedshiftColumn widest = ClassifyColumn(ReportedWithSize("NUMERIC", 2, 38, 37));
	CHECK(widest.precision == 38);
	CHECK(widest.scale == 37);

	const RedshiftColumn narrowest = ClassifyColumn(ReportedWithSize("NUMERIC", 2, 1, 0));
	CHECK(narrowest.precision == 1);
	CHECK(narrowest.scale == 0);

	const RedshiftColumn all_scale = ClassifyColumn(ReportedWithSize("NUMERIC", 2, 38, 38));
	CHECK(all_scale.precision == 38);
	CHECK(all_scale.scale == 38);
}

TEST_CASE("NUMERIC metadata outside the precision range is UNKNOWN, never narrowed", "[odbc]") {
	CHECK(ClassifyColumn(ReportedWithSize("NUMERIC", 2, 0, 0)).type == RedshiftTypeId::UNKNOWN);
	CHECK(ClassifyColumn(ReportedWithSize("NUMERIC", 2, 39, 0)).type == RedshiftTypeId::UNKNOWN);
	CHECK(ClassifyColumn(ReportedWithSize("NUMERIC", 2, 10, -1)).type == RedshiftTypeId::UNKNOWN);
	CHECK(ClassifyColumn(ReportedWithSize("NUMERIC", 2, 10, 11)).type == RedshiftTypeId::UNKNOWN);
	CHECK(ClassifyColumn(ReportedWithSize("NUMERIC", 2, 38, 39)).type == RedshiftTypeId::UNKNOWN);
}

TEST_CASE("A refused NUMERIC reports no precision or scale of its own", "[odbc]") {
	const RedshiftColumn refused = ClassifyColumn(ReportedWithSize("NUMERIC", 2, 39, 5));
	CHECK(refused.type == RedshiftTypeId::UNKNOWN);
	CHECK(refused.precision == 0);
	CHECK(refused.scale == 0);
}

TEST_CASE("Precision and scale are zero for every type other than DECIMAL", "[odbc]") {
	const RedshiftColumn text = ClassifyColumn(ReportedWithSize("CHARACTER VARYING", 12, 256, 0));
	CHECK(text.type == RedshiftTypeId::VARCHAR);
	CHECK(text.precision == 0);
	CHECK(text.scale == 0);

	// The driver reports size 26 and 6 digits for a TIMESTAMP; neither is a precision.
	const RedshiftColumn stamp = ClassifyColumn(ReportedWithSize("TIMESTAMP", 93, 26, 6));
	CHECK(stamp.type == RedshiftTypeId::TIMESTAMP);
	CHECK(stamp.precision == 0);
	CHECK(stamp.scale == 0);
}

TEST_CASE("The column's name is carried through, classified or not", "[odbc]") {
	DriverColumnMetadata named = {"total_sales", "BIGINT", -5, 0, 0};
	CHECK(ClassifyColumn(named).name == "total_sales");

	DriverColumnMetadata unclassified = {"shape", "GEOMETRY", -4, 0, 0};
	const RedshiftColumn column = ClassifyColumn(unclassified);
	CHECK(column.name == "shape");
	CHECK(column.type == RedshiftTypeId::UNKNOWN);

	DriverColumnMetadata unnamed = {"", "BOOL", -7, 0, 0};
	CHECK(ClassifyColumn(unnamed).name.empty());
}

TEST_CASE("Metadata no Redshift type would produce reports UNKNOWN rather than throwing", "[odbc]") {
	// Narrowing the column size to the result's precision is the one step that can throw, and
	// the range check stands in front of it.
	CHECK_NOTHROW(ClassifyColumn(ReportedWithSize("NUMERIC", 2, UINT64_MAX, 32767)));
	CHECK(ClassifyColumn(ReportedWithSize("NUMERIC", 2, UINT64_MAX, 32767)).type == RedshiftTypeId::UNKNOWN);

	// Size and digits are not a precision for any other type, however large they are.
	CHECK(ClassifyColumn(ReportedWithSize("BOOL", -7, UINT64_MAX, -32768)).type == RedshiftTypeId::BOOLEAN);
	CHECK(ClassifyColumn(ReportedWithSize(std::string(4096, 'X'), 12, 0, 0)).type == RedshiftTypeId::UNKNOWN);
}

TEST_CASE("The table's SQL type codes are the ODBC concise type codes", "[odbc]") {
	CHECK(SQL_BIT == -7);
	CHECK(SQL_SMALLINT == 5);
	CHECK(SQL_INTEGER == 4);
	CHECK(SQL_BIGINT == -5);
	CHECK(SQL_REAL == 7);
	CHECK(SQL_DOUBLE == 8);
	CHECK(SQL_NUMERIC == 2);
	CHECK(SQL_CHAR == 1);
	CHECK(SQL_WCHAR == -8);
	CHECK(SQL_VARCHAR == 12);
	CHECK(SQL_WVARCHAR == -9);
	CHECK(SQL_LONGVARCHAR == -1);
	CHECK(SQL_WLONGVARCHAR == -10);
	CHECK(SQL_TYPE_DATE == 91);
	CHECK(SQL_TYPE_TIME == 92);
	CHECK(SQL_TYPE_TIMESTAMP == 93);
	CHECK(SQL_LONGVARBINARY == -4);
	CHECK(SQL_INTERVAL_YEAR_TO_MONTH == 107);
	CHECK(SQL_INTERVAL_DAY_TO_SECOND == 110);
	// The codes the names above are not: 3 is DECIMAL's and -6 is TINYINT's.
	CHECK(SQL_DECIMAL == 3);
	CHECK(SQL_TINYINT == -6);
}
