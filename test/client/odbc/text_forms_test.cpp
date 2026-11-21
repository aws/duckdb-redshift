// The client's text forms. A string recorded from the server is used as it was; the others are
// built from the forms text_forms.cpp accepts.

#include "catch.hpp"

#include "odbc/text_forms.hpp"

#include <cstdint>
#include <string>
#include <vector>

using duckdb::MatchesTextForm;
using duckdb::RedshiftColumn;
using duckdb::RedshiftTypeId;

namespace {

RedshiftColumn ColumnOf(RedshiftTypeId type) {
	RedshiftColumn column;
	column.type = type;
	return column;
}

bool IsDecimal(const std::string &text, int32_t scale) {
	RedshiftColumn column = ColumnOf(RedshiftTypeId::DECIMAL);
	column.precision = 38;
	column.scale = scale;
	return MatchesTextForm(column, text);
}

bool IsDate(const std::string &text) {
	return MatchesTextForm(ColumnOf(RedshiftTypeId::DATE), text);
}

bool IsTime(const std::string &text) {
	return MatchesTextForm(ColumnOf(RedshiftTypeId::TIME), text);
}

bool IsTimetz(const std::string &text) {
	return MatchesTextForm(ColumnOf(RedshiftTypeId::TIMETZ), text);
}

bool IsTimestamp(const std::string &text) {
	return MatchesTextForm(ColumnOf(RedshiftTypeId::TIMESTAMP), text);
}

bool IsTimestamptz(const std::string &text) {
	return MatchesTextForm(ColumnOf(RedshiftTypeId::TIMESTAMPTZ), text);
}

} // namespace

TEST_CASE("A DECIMAL with exactly its scale's digits after the point is in its form", "[odbc]") {
	CHECK(IsDecimal("12.35", 2));
	CHECK(IsDecimal("-0.33", 2));
	CHECK(IsDecimal("0.00", 2));
	CHECK(IsDecimal("-9999999999999999999999999999.9999999999", 10));
	CHECK(IsDecimal("0.3333333333333333", 16));
}

TEST_CASE("A DECIMAL of scale zero has no point", "[odbc]") {
	CHECK(IsDecimal("12", 0));
	CHECK(IsDecimal("-13", 0));
	CHECK(IsDecimal("0", 0));
	CHECK_FALSE(IsDecimal("12.", 0));
	CHECK_FALSE(IsDecimal("12.0", 0));
}

TEST_CASE("A DECIMAL with fewer digits after the point than its scale is in its form", "[odbc]") {
	// The leader node prints a numeric(37,17) division over generate_series with 16 digits.
	CHECK(IsDecimal("12.3", 2));
	CHECK(IsDecimal("0.0000000000000002", 17));
	CHECK(IsDecimal("-0.5", 4));
	CHECK_FALSE(IsDecimal("-0.0", 2));
}

TEST_CASE("A DECIMAL with more digits after the point than its scale, or none, is not in its form", "[odbc]") {
	CHECK_FALSE(IsDecimal("12.345", 2));
	CHECK_FALSE(IsDecimal("12", 2));
	CHECK_FALSE(IsDecimal("12.", 2));
}

TEST_CASE("A DECIMAL of scale 38 takes 1 to 38 digits after the point", "[odbc]") {
	CHECK(IsDecimal("0.1", 38));
	CHECK(IsDecimal("0." + std::string(37, '0') + "1", 38));
	CHECK(IsDecimal("-0." + std::string(38, '9'), 38));
	CHECK_FALSE(IsDecimal("0." + std::string(38, '0') + "1", 38));
	CHECK_FALSE(IsDecimal("0.", 38));
	CHECK_FALSE(IsDecimal("0", 38));
}

TEST_CASE("A DECIMAL integer part has no leading zero", "[odbc]") {
	CHECK_FALSE(IsDecimal("007", 0));
	CHECK_FALSE(IsDecimal("01.50", 2));
	CHECK_FALSE(IsDecimal("-01.5", 1));
	CHECK(IsDecimal("0.50", 2));
	CHECK(IsDecimal("10.50", 2));
	CHECK(IsDecimal("-0.5", 1));
}

TEST_CASE("A DECIMAL zero carries no sign", "[odbc]") {
	CHECK_FALSE(IsDecimal("-0", 0));
	CHECK_FALSE(IsDecimal("-0.00", 2));
	CHECK_FALSE(IsDecimal("-000", 0));
	CHECK(IsDecimal("-0.01", 2));
	CHECK(IsDecimal("-10", 0));
	CHECK(IsDecimal("-10.00", 2));
}

TEST_CASE("A DECIMAL in another notation is not in its form", "[odbc]") {
	CHECK_FALSE(IsDecimal("1.5e3", 1));
	CHECK_FALSE(IsDecimal("1e3", 0));
	CHECK_FALSE(IsDecimal("1,000", 0));
	CHECK_FALSE(IsDecimal("+1.50", 2));
	CHECK_FALSE(IsDecimal(".50", 2));
	CHECK_FALSE(IsDecimal("-.50", 2));
	CHECK_FALSE(IsDecimal(" 1.50", 2));
	CHECK_FALSE(IsDecimal("1.50 ", 2));
	CHECK_FALSE(IsDecimal("1,50", 2));
	CHECK_FALSE(IsDecimal("", 0));
	CHECK_FALSE(IsDecimal("-", 0));
	CHECK_FALSE(IsDecimal("NaN", 0));
}

TEST_CASE("A DATE with a year of 4 to 7 digits is in its form", "[odbc]") {
	CHECK(IsDate("2024-02-29"));
	CHECK(IsDate("10000-01-01"));
	CHECK(IsDate("294276-12-31"));
	CHECK(IsDate("5874897-12-31"));
}

TEST_CASE("An astronomical DATE before year 1 is in its form", "[odbc]") {
	CHECK(IsDate("-4712-01-01"));
	CHECK(IsDate("-4713-11-24"));
	CHECK(IsDate("-043-03-15"));
	CHECK(IsDate("-1234567-01-01"));
	CHECK(IsDate("0000-12-31"));
}

TEST_CASE("A DATE with a BC suffix is in its form", "[odbc]") {
	CHECK(IsDate("4713-01-01 BC"));
	CHECK(IsDate("0044-03-15 BC"));
}

TEST_CASE("A DATE year with too few or too many digits is not in its form", "[odbc]") {
	CHECK_FALSE(IsDate("999-01-01"));
	CHECK_FALSE(IsDate("12345678-01-01"));
	CHECK_FALSE(IsDate("-43-03-15"));
	CHECK_FALSE(IsDate("-12345678-01-01"));
}

TEST_CASE("An astronomical DATE with a BC suffix is not in its form", "[odbc]") {
	CHECK_FALSE(IsDate("-4712-01-01 BC"));
	CHECK_FALSE(IsDate("-043-03-15 BC"));
}

TEST_CASE("A DATE in a non-ISO date style is not in its form", "[odbc]") {
	CHECK_FALSE(IsDate("08/31/2026"));
	CHECK_FALSE(IsDate("31.08.2026"));
	CHECK_FALSE(IsDate("2026-8-31"));
	CHECK_FALSE(IsDate("2026-08-311"));
	CHECK_FALSE(IsDate("2026-08-31 AD"));
	CHECK_FALSE(IsDate("2026-08-31 bc"));
	CHECK_FALSE(IsDate("2026-08-31BC"));
	CHECK_FALSE(IsDate("2026-08-31 "));
	CHECK_FALSE(IsDate(""));
}

TEST_CASE("A TIME with no fraction or 1 to 6 fractional digits is in its form", "[odbc]") {
	CHECK(IsTime("00:00:00"));
	CHECK(IsTime("12:34:56.5"));
	CHECK(IsTime("12:34:56.50"));
	CHECK(IsTime("23:59:59.999999"));
}

TEST_CASE("A TIME with a 7-digit or empty fraction is not in its form", "[odbc]") {
	CHECK_FALSE(IsTime("12:00:00.1234567"));
	CHECK_FALSE(IsTime("12:00:00."));
}

TEST_CASE("A TIME missing a field, or with a field of one digit, is not in its form", "[odbc]") {
	CHECK_FALSE(IsTime("12:00"));
	CHECK_FALSE(IsTime("1:02:03"));
	CHECK_FALSE(IsTime("12:2:03"));
	CHECK_FALSE(IsTime("12:02:3"));
	CHECK_FALSE(IsTime("12-02-03"));
	CHECK_FALSE(IsTime("12:02-03"));
	CHECK_FALSE(IsTime("12:00:00+00"));
	CHECK_FALSE(IsTime("12:00:00,5"));
}

TEST_CASE("A TIMETZ with an offset of whole hours is in its form", "[odbc]") {
	CHECK(IsTimetz("00:00:00+00"));
	CHECK(IsTimetz("07:04:56.50+00"));
	CHECK(IsTimetz("09:00:59.999999+00"));
	CHECK(IsTimetz("02:04:56-05"));
}

TEST_CASE("A TIMETZ zero offset is in its form with either sign", "[odbc]") {
	CHECK(IsTimetz("12:00:00+00"));
	CHECK(IsTimetz("12:00:00-00"));
}

TEST_CASE("A TIMETZ offset carries its minutes when they are non-zero", "[odbc]") {
	CHECK(IsTimetz("12:00:00-03:30"));
	CHECK(IsTimetz("12:00:00+05:30"));
}

TEST_CASE("A TIMETZ offset carries its seconds when they are non-zero", "[odbc]") {
	CHECK(IsTimetz("12:00:00-03:30:15"));
	CHECK(IsTimetz("12:00:00+05:00:30"));
}

TEST_CASE("A TIMETZ offset that carries a zero field it could leave off is not in its form", "[odbc]") {
	CHECK_FALSE(IsTimetz("12:00:00+05:00"));
	CHECK_FALSE(IsTimetz("12:00:00+00:00"));
	CHECK_FALSE(IsTimetz("12:00:00+05:30:00"));
	CHECK_FALSE(IsTimetz("12:00:00+05:00:00"));
}

TEST_CASE("A TIMETZ with no offset, or an offset written another way, is not in its form", "[odbc]") {
	CHECK_FALSE(IsTimetz("12:00:00"));
	CHECK_FALSE(IsTimetz("12:00:00+5"));
	CHECK_FALSE(IsTimetz("12:00:00+0530"));
	CHECK_FALSE(IsTimetz("12:00:00+05:"));
	CHECK_FALSE(IsTimetz("12:00:00+05:3"));
	CHECK_FALSE(IsTimetz("12:00:00+05:30:1"));
	CHECK_FALSE(IsTimetz("12:00:00+05:30:15:00"));
	CHECK_FALSE(IsTimetz("12:00:00 +05"));
	CHECK_FALSE(IsTimetz("12:00:00Z"));
	CHECK_FALSE(IsTimetz("12:00:00 EDT"));
	CHECK_FALSE(IsTimetz("12:00:00.+05"));
}

TEST_CASE("A time of day takes no BC suffix", "[odbc]") {
	CHECK_FALSE(IsTime("12:00:00 BC"));
	CHECK_FALSE(IsTimetz("12:00:00+00 BC"));
}

TEST_CASE("A TIMESTAMP is a date with a year of 4 to 6 digits, a space and a time", "[odbc]") {
	CHECK(IsTimestamp("2024-02-29 07:04:56"));
	CHECK(IsTimestamp("2024-06-01 12:00:00.50"));
	CHECK(IsTimestamp("294276-12-31 23:59:59.999999"));
	CHECK(IsTimestamp("294277-01-09 04:00:54.775807"));
	CHECK_FALSE(IsTimestamp("5874897-12-31 00:00:00"));
	CHECK_FALSE(IsTimestamp("999-12-31 00:00:00"));
}

TEST_CASE("A TIMESTAMP before year 1 ends in BC", "[odbc]") {
	CHECK(IsTimestamp("0044-03-15 12:00:00 BC"));
	CHECK(IsTimestamp("4713-01-01 00:00:00.5 BC"));
}

TEST_CASE("A TIMESTAMP with an astronomical year is not in its form", "[odbc]") {
	CHECK_FALSE(IsTimestamp("-043-03-15 12:00:00"));
	CHECK_FALSE(IsTimestamp("-4712-01-01 00:00:00"));
}

TEST_CASE("A TIMESTAMP in another notation is not in its form", "[odbc]") {
	CHECK_FALSE(IsTimestamp("2024-06-01T12:00:00"));
	CHECK_FALSE(IsTimestamp("2024-06-01  12:00:00"));
	CHECK_FALSE(IsTimestamp("2024-06-01"));
	CHECK_FALSE(IsTimestamp("2024-06-01 12:00:00+00"));
	CHECK_FALSE(IsTimestamp("2024-06-01 12:00:00 AD"));
	CHECK_FALSE(IsTimestamp("Mon Aug 31 12:34:56.789 2026"));
}

TEST_CASE("A TIMESTAMPTZ is a TIMESTAMP followed by a TIMETZ offset", "[odbc]") {
	CHECK(IsTimestamptz("2024-02-29 07:04:56.123456+00"));
	CHECK(IsTimestamptz("294276-12-31 23:59:59.999999+00"));
	CHECK(IsTimestamptz("2024-02-29 12:34:56.123456+05:30"));
	CHECK(IsTimestamptz("1883-11-18 07:03:58-04:56"));
	CHECK(IsTimestamptz("2024-02-29 12:00:00-03:30:15"));
	CHECK_FALSE(IsTimestamptz("2024-02-29 07:04:56+05:00"));
	CHECK_FALSE(IsTimestamptz("5874897-12-31 00:00:00+00"));
}

TEST_CASE("A TIMESTAMPTZ before year 1 puts its offset before the BC", "[odbc]") {
	CHECK(IsTimestamptz("4713-01-01 00:00:00+00 BC"));
	CHECK(IsTimestamptz("4714-12-31 19:03:58-04:56 BC"));
	CHECK_FALSE(IsTimestamptz("4713-01-01 00:00:00 BC+00"));
}

TEST_CASE("A TIMESTAMPTZ with no offset, or a zone name, is not in its form", "[odbc]") {
	CHECK_FALSE(IsTimestamptz("2024-02-29 07:04:56"));
	CHECK_FALSE(IsTimestamptz("2024-02-29 07:04:56 BC"));
	CHECK_FALSE(IsTimestamptz("08/31/2026 12:00:00.00 EDT"));
	CHECK_FALSE(IsTimestamptz("2024-02-29 07:04:56 UTC"));
	CHECK_FALSE(IsTimestamptz("2024-02-29 07:04:56+00 UTC"));
}

TEST_CASE("Any VARCHAR or CHAR text is in its form, CHAR padding included", "[odbc]") {
	CHECK(MatchesTextForm(ColumnOf(RedshiftTypeId::VARCHAR), "a    "));
	CHECK(MatchesTextForm(ColumnOf(RedshiftTypeId::VARCHAR), ""));
	CHECK(MatchesTextForm(ColumnOf(RedshiftTypeId::VARCHAR), "08/31/2026"));
	// A CHAR(5) arrives padded to its width, or as blanks alone for an empty value.
	CHECK(MatchesTextForm(ColumnOf(RedshiftTypeId::CHAR), "a    "));
	CHECK(MatchesTextForm(ColumnOf(RedshiftTypeId::CHAR), "     "));
	CHECK(MatchesTextForm(ColumnOf(RedshiftTypeId::CHAR), ""));
	CHECK(MatchesTextForm(ColumnOf(RedshiftTypeId::CHAR), "08/31/2026"));
}

TEST_CASE("An integer matches a DECIMAL of scale zero: no leading zero and no signed zero", "[odbc]") {
	for (const auto type : {RedshiftTypeId::SMALLINT, RedshiftTypeId::INTEGER, RedshiftTypeId::BIGINT}) {
		const auto column = ColumnOf(type);
		for (const auto *const accepted : {"0", "1", "-1", "9223372036854775808", "-2147483648"}) {
			INFO(accepted);
			CHECK(MatchesTextForm(column, accepted));
		}
		for (const auto *const refused : {"007", "00", "-0", "-07", "+1", " 1", "1 ", "1.0", "1e3", "", "-"}) {
			INFO(refused);
			CHECK_FALSE(MatchesTextForm(column, refused));
		}
	}
}

TEST_CASE("No text matches a type that is not carried as TEXT", "[odbc]") {
	CHECK_FALSE(MatchesTextForm(ColumnOf(RedshiftTypeId::BOOLEAN), "1"));
	CHECK_FALSE(MatchesTextForm(ColumnOf(RedshiftTypeId::BLOB), "00ff10"));
	CHECK_FALSE(MatchesTextForm(ColumnOf(RedshiftTypeId::UNKNOWN), "1"));
}

TEST_CASE("A FLOAT or DOUBLE matches only the server's float output", "[odbc]") {
	// Digit runs of up to 32 digits pass and one of 33 is refused, as MAX_FLOAT_DIGITS bounds each run.
	const std::vector<std::string> accepted {"1",
	                                         "-0",
	                                         "0.5",
	                                         "0.30000000000000004",
	                                         "0.00012345678901234567",
	                                         "1.79769313486232e+308",
	                                         "1e-05",
	                                         "nan",
	                                         "-nan",
	                                         "inf",
	                                         "-inf",
	                                         "NaN",
	                                         "Infinity",
	                                         "-Infinity",
	                                         std::string(32, '1'),
	                                         "0." + std::string(32, '1'),
	                                         "1e+" + std::string(32, '1')};
	const std::vector<std::string> refused {"",
	                                        "-",
	                                        ".5",
	                                        "1.",
	                                        "01",
	                                        "+1",
	                                        " 1",
	                                        "1 ",
	                                        "1e5",
	                                        "1E+5",
	                                        "1e+",
	                                        "INF",
	                                        "nan(x)",
	                                        "1_000",
	                                        "1,5",
	                                        "--1",
	                                        "+nan",
	                                        std::string(33, '1'),
	                                        "0." + std::string(33, '1'),
	                                        "1e+" + std::string(33, '1')};
	for (const auto type : {RedshiftTypeId::FLOAT, RedshiftTypeId::DOUBLE}) {
		for (const auto &text : accepted) {
			INFO(text);
			CHECK(MatchesTextForm(ColumnOf(type), text));
		}
		for (const auto &text : refused) {
			INFO(text);
			CHECK_FALSE(MatchesTextForm(ColumnOf(type), text));
		}
	}
}

TEST_CASE("A field is checked for its number of digits, not its range", "[odbc]") {
	CHECK(IsDate("2024-13-45"));
	CHECK(IsDate("-000-12-31"));
	CHECK(IsDate("0000-12-31 BC"));
	CHECK(IsTime("25:61:61"));
	CHECK(IsTimetz("12:00:00+99:99"));
	CHECK(IsTimestamp("2024-02-30 24:00:00"));
}

TEST_CASE("A DECIMAL's digits are not checked against its precision", "[odbc]") {
	RedshiftColumn column = ColumnOf(RedshiftTypeId::DECIMAL);
	column.precision = 3;
	column.scale = 1;
	CHECK(MatchesTextForm(column, "12345.6"));
}
