//===----------------------------------------------------------------------===//
//                         DuckDB - Redshift
//
// test/client/odbc/recorded_driver_output.hpp
//
// What driver 2.3.0.0 reported live over the shipped connection string, after the client's session
// setup: each statement's columns and the text of its one row. The [live] cases
// check that the pinned driver still reports exactly this; the offline cases script it in the fake.
//===----------------------------------------------------------------------===//

#pragma once

#include "../fake_driver/fake_odbc_driver.hpp"

#include <string>
#include <vector>

namespace redshift_test {

//! Which node printed a row. A statement that reads a table is printed by the compute nodes, which
//! render TIMETZ in the session's zone; one that reads none by the leader node, which keeps the offset.
enum class NodePath : uint8_t { LEADER, COMPUTE };

//! The FROM clause that sends a select list to the compute nodes: a system view any user can read,
//! so nothing is written to make the table.
const char *const COMPUTE_NODE_FROM = " FROM sys_query_history LIMIT 1";

//! A select list of edge values, as the leader node printed it under the client's pinned settings.
struct RecordedSelect {
	const char *name;
	std::string select_list;
	//! Its columns and its one row, as the fake's script for either node's statement.
	duckdb::redshift_driver::testing::FakeResultSet result;
};

//! A row that differs from its RecordedSelect's: printed by `path`'s node after `caller_statement`,
//! the caller's statement on the session, or after none when it is empty.
struct RecordedDifference {
	const char *select;
	const char *caller_statement;
	NodePath path;
	std::vector<std::string> row;
};

//! A statement whose columns alone are recorded: its rows change between runs or name the user.
struct RecordedDescribe {
	std::string sql;
	std::vector<duckdb::redshift_driver::testing::FakeColumn> columns;
};

//! The caller statements the differences were recorded after, each run alone on a new session.
const std::vector<std::string> RECORDED_CALLER_STATEMENTS {
    "SET extra_float_digits TO 0", "SET timezone TO 'America/New_York'", "SET timezone TO 'Asia/Kolkata'",
    "SET intervalstyle TO 'postgres_verbose'", "SET intervalstyle TO 'sql_standard'"};

//! The statement for `select` on `path`'s node.
inline std::string RecordedSql(const RecordedSelect &select, NodePath path) {
	return "SELECT " + select.select_list + (path == NodePath::COMPUTE ? COMPUTE_NODE_FROM : "");
}

//! Every recorded select list, in the order it was run. The data below is kept out of clang-format,
//! which would give each recorded column and cell a line of its own.
inline const std::vector<RecordedSelect> &RecordedSelects() {
	// clang-format off
	static const std::vector<RecordedSelect> selects {
	    {"integers",
	     "32767::smallint AS smallint_max, (-32768)::smallint AS smallint_min, 2147483647 AS integer_max, "
	     "(-2147483648)::integer AS integer_min, 9223372036854775807::bigint AS bigint_max, "
	     "'-9223372036854775808'::bigint AS bigint_min, true AS bool_true, false AS bool_false, 1 + 2 AS integer_sum",
	     {{{"smallint_max", "SMALLINT", 5, 5}, {"smallint_min", "SMALLINT", 5, 5}, {"integer_max", "INTEGER", 4, 10},
	       {"integer_min", "INTEGER", 4, 10}, {"bigint_max", "BIGINT", -5, 19}, {"bigint_min", "BIGINT", -5, 19},
	       {"bool_true", "BOOL", -7, 1}, {"bool_false", "BOOL", -7, 1}, {"integer_sum", "INTEGER", 4, 10}},
	      {{"32767", "-32768", "2147483647", "-2147483648", "9223372036854775807", "-9223372036854775808", "1", "0",
	        "3"}}}},
	    {"float8",
	     "'-0'::float8 AS negative_zero, 'NaN'::float8 AS nan, 'Infinity'::float8 AS infinity, "
	     "'-Infinity'::float8 AS negative_infinity, '0.1'::float8 AS tenth, "
	     "'0.30000000000000004'::float8 AS sum_of_tenths, '1.7976931348623157e308'::float8 AS largest, "
	     "'2.2250738585072014e-308'::float8 AS smallest_normal, '9007199254740993'::float8 AS above_2_53",
	     {{{"negative_zero", "DOUBLE PRECISION", 8, 15}, {"nan", "DOUBLE PRECISION", 8, 15},
	       {"infinity", "DOUBLE PRECISION", 8, 15}, {"negative_infinity", "DOUBLE PRECISION", 8, 15},
	       {"tenth", "DOUBLE PRECISION", 8, 15}, {"sum_of_tenths", "DOUBLE PRECISION", 8, 15},
	       {"largest", "DOUBLE PRECISION", 8, 15}, {"smallest_normal", "DOUBLE PRECISION", 8, 15},
	       {"above_2_53", "DOUBLE PRECISION", 8, 15}},
	      {{"-0", "NaN", "Infinity", "-Infinity", "0.10000000000000001", "0.30000000000000004",
	        "1.7976931348623157e+308", "2.2250738585072014e-308", "9007199254740992"}}}},
	    {"float4",
	     "'-0'::real AS negative_zero, 'NaN'::real AS nan, '-Infinity'::real AS negative_infinity, "
	     "'0.1'::real AS tenth, '0.30000000000000004'::real AS sum_of_tenths, '3.4028234e38'::real AS largest, "
	     "'1.1754944e-38'::real AS smallest_normal, '16777217'::real AS above_2_24",
	     {{{"negative_zero", "REAL", 7, 7}, {"nan", "REAL", 7, 7}, {"negative_infinity", "REAL", 7, 7},
	       {"tenth", "REAL", 7, 7}, {"sum_of_tenths", "REAL", 7, 7}, {"largest", "REAL", 7, 7},
	       {"smallest_normal", "REAL", 7, 7}, {"above_2_24", "REAL", 7, 7}},
	      {{"-0", "NaN", "-Infinity", "0.1", "0.30000001", "3.4028235e+38", "1.1754944e-38", "16777216"}}}},
	    {"numeric",
	     "'-9999999999999999999999999999.9999999999'::numeric(38,10) AS n38_10, "
	     "'0.3333333333333333'::numeric(16,16) AS n16_16, '-0.3333333333333333'::numeric(16,16) AS n16_16_negative, "
	     "'0'::numeric(38,10) AS zero, '-0.001'::numeric(10,2) AS rounds_to_zero, "
	     "'12.345'::numeric(10,2) AS rounds_half_up, "
	     "'99999999999999999999999999999999999999'::numeric(38,0) AS n38_0, 1.5 AS untyped, 1::numeric AS bare",
	     {{{"n38_10", "NUMERIC", 2, 38, 10}, {"n16_16", "NUMERIC", 2, 16, 16},
	       {"n16_16_negative", "NUMERIC", 2, 16, 16}, {"zero", "NUMERIC", 2, 38, 10},
	       {"rounds_to_zero", "NUMERIC", 2, 10, 2}, {"rounds_half_up", "NUMERIC", 2, 10, 2},
	       {"n38_0", "NUMERIC", 2, 38}, {"untyped", "NUMERIC", 2, 2, 1}, {"bare", "NUMERIC", 2, 18}},
	      {{"-9999999999999999999999999999.9999999999", "0.3333333333333333", "-0.3333333333333333", "0.0000000000",
	        "0.00", "12.35", "99999999999999999999999999999999999999", "1.5", "1"}}}},
	    {"date",
	     "'4713-01-01 BC'::date AS first_julian, '4714-11-24 BC'::date AS first_gregorian, "
	     "'0001-12-31 BC'::date AS last_bc, '0044-03-15 BC'::date AS ides, '0001-01-01'::date AS first_ad, "
	     "'294276-12-31'::date AS last_timestamp, '10000-01-01'::date AS five_digits, "
	     "'5874897-12-31'::date AS last_date",
	     {{{"first_julian", "DATE", 91, 10}, {"first_gregorian", "DATE", 91, 10}, {"last_bc", "DATE", 91, 10},
	       {"ides", "DATE", 91, 10}, {"first_ad", "DATE", 91, 10}, {"last_timestamp", "DATE", 91, 10},
	       {"five_digits", "DATE", 91, 10}, {"last_date", "DATE", 91, 10}},
	      {{"4713-01-01 BC", "4714-11-24 BC", "0001-12-31 BC", "0044-03-15 BC", "0001-01-01", "294276-12-31",
	        "10000-01-01", "5874897-12-31"}}}},
	    {"time",
	     "'12:34:56.5'::time AS one_digit, '12:34:56.50'::time AS trailing_zero, '23:59:59.999999'::time AS last, "
	     "'24:00:00'::time AS midnight_end, '00:00:00.000001'::time AS first, "
	     "'12:00:00.123456789'::time AS nine_digits, '12:34:56'::time AS whole",
	     {{{"one_digit", "TIME", 92, 8}, {"trailing_zero", "TIME", 92, 8}, {"last", "TIME", 92, 8},
	       {"midnight_end", "TIME", 92, 8}, {"first", "TIME", 92, 8}, {"nine_digits", "TIME", 92, 8},
	       {"whole", "TIME", 92, 8}},
	      {{"12:34:56.50", "12:34:56.50", "23:59:59.999999", "00:00:00", "00:00:00.000001", "12:00:00.123457",
	        "12:34:56"}}}},
	    {"timetz",
	     "'12:34:56.5+05:30'::timetz AS half_hour, '23:59:59.999999+14:59'::timetz AS widest, "
	     "'12:00:00-03:30:15'::timetz AS offset_seconds, '12:00:00-03:30'::timetz AS offset_minutes, "
	     "'00:00:00+00'::timetz AS utc, '12:34:56-08'::timetz AS whole_hours",
	     {{{"half_hour", "TIMETZ", 92, 8}, {"widest", "TIMETZ", 92, 8}, {"offset_seconds", "TIMETZ", 92, 8},
	       {"offset_minutes", "TIMETZ", 92, 8}, {"utc", "TIMETZ", 92, 8}, {"whole_hours", "TIMETZ", 92, 8}},
	      {{"12:34:56.50+05:30", "23:59:59.999999+14:59", "12:00:00-03:30", "12:00:00-03:30", "00:00:00+00",
	        "12:34:56-08"}}}},
	    {"timestamp",
	     "'294276-12-31 23:59:59.999999'::timestamp AS last, '0044-03-15 12:00:00 BC'::timestamp AS ides, "
	     "'2024-06-01 12:00:00.5'::timestamp AS one_digit, '4713-01-01 00:00:00 BC'::timestamp AS first_julian",
	     {{{"last", "TIMESTAMP", 93, 26, 6}, {"ides", "TIMESTAMP", 93, 26, 6}, {"one_digit", "TIMESTAMP", 93, 26, 6},
	       {"first_julian", "TIMESTAMP", 93, 26, 6}},
	      {{"294276-12-31 23:59:59.999999", "0044-03-15 12:00:00 BC", "2024-06-01 12:00:00.50",
	        "4713-01-01 00:00:00 BC"}}}},
	    {"timestamptz",
	     "'1883-11-18 12:00:00+00'::timestamptz AS before_zones, "
	     "'4713-01-01 00:00:00+00 BC'::timestamptz AS first_julian, "
	     "'0001-01-01 00:00:00+00'::timestamptz AS first_ad, "
	     "'2024-02-29 12:34:56.123456+05:30'::timestamptz AS leap_day, "
	     "'10000-01-01 00:00:00+00'::timestamptz AS five_digits, "
	     "'294276-12-31 23:59:59.999999+00'::timestamptz AS last, '1900-01-01 00:00:00+00'::timestamptz AS kolkata_lmt",
	     {{{"before_zones", "TIMESTAMPTZ", 93, 26, 6}, {"first_julian", "TIMESTAMPTZ", 93, 26, 6},
	       {"first_ad", "TIMESTAMPTZ", 93, 26, 6}, {"leap_day", "TIMESTAMPTZ", 93, 26, 6},
	       {"five_digits", "TIMESTAMPTZ", 93, 26, 6}, {"last", "TIMESTAMPTZ", 93, 26, 6},
	       {"kolkata_lmt", "TIMESTAMPTZ", 93, 26, 6}},
	      {{"1883-11-18 12:00:00+00", "4713-01-01 00:00:00+00 BC", "0001-01-01 00:00:00+00",
	        "2024-02-29 07:04:56.123456+00", "10000-01-01 00:00:00+00", "294276-12-31 23:59:59.999999+00",
	        "1900-01-01 00:00:00+00"}}}},
	    {"infinities",
	     "'infinity'::timestamp AS timestamp_infinity, '-infinity'::timestamp AS timestamp_negative_infinity, "
	     "'infinity'::timestamptz AS timestamptz_infinity, '-infinity'::timestamptz AS timestamptz_negative_infinity",
	     {{{"timestamp_infinity", "TIMESTAMP", 93, 26, 6}, {"timestamp_negative_infinity", "TIMESTAMP", 93, 26, 6},
	       {"timestamptz_infinity", "TIMESTAMPTZ", 93, 26, 6},
	       {"timestamptz_negative_infinity", "TIMESTAMPTZ", 93, 26, 6}},
	      {{"infinity", "-infinity", "infinity", "-infinity"}}}},
	    {"character",
	     "'a'::char(5) AS char_padded, ''::char(5) AS char_empty, 'ab  '::char(5) AS char_trailing, "
	     "'GERMANY'::char(25) AS char_25, 'xy  '::varchar AS varchar_trailing, "
	     "'trailing  '::varchar(10) AS varchar_10, 'emoji 😀 𝄞 é 日本'::varchar AS utf8, 'abc' AS untyped, "
	     "'x'::text AS text, 'x'::varchar(65535) AS varchar_65535, 'x'::varchar(256) AS varchar_256, "
	     "''::varchar AS varchar_empty",
	     {{{"char_padded", "CHARACTER", 1, 5}, {"char_empty", "CHARACTER", 1, 5}, {"char_trailing", "CHARACTER", 1, 5},
	       {"char_25", "CHARACTER", 1, 25}, {"varchar_trailing", "CHARACTER VARYING", 12, 4},
	       {"varchar_10", "CHARACTER VARYING", 12, 10}, {"utf8", "CHARACTER VARYING", 12, 25},
	       {"untyped", "CHARACTER VARYING", 12, 3}, {"text", "CHARACTER VARYING", 12, 1},
	       {"varchar_65535", "CHARACTER VARYING", 12, 65535}, {"varchar_256", "CHARACTER VARYING", 12, 256},
	       {"varchar_empty", "CHARACTER VARYING", 12, 65535}},
	      {{"a    ", "     ", "ab   ", "GERMANY                  ", "xy  ", "trailing  ", "emoji 😀 𝄞 é 日本", "abc", "x",
	        "x", "x", ""}}}},
	    {"bytes_and_super",
	     "from_hex('00ff10') AS varbyte_hex, 'CAFE'::varbyte AS varbyte_text, "
	     "JSON_PARSE('{\"b\":1,\"a\":[1,2],\"k\":\"😀\"}') AS super_object, JSON_PARSE('\"str\"') AS super_string, "
	     "JSON_PARSE('1') AS super_number, JSON_PARSE('{\"z\":1,\"a\":2,\"m\":3}') AS super_key_order",
	     {{{"varbyte_hex", "VARBYTE", -4, 3}, {"varbyte_text", "VARBYTE", -4, 64000},
	       {"super_object", "SUPER", -1, 4194304}, {"super_string", "SUPER", -1, 4194304},
	       {"super_number", "SUPER", -1, 4194304}, {"super_key_order", "SUPER", -1, 4194304}},
	      {{std::string("\x00\xff\x10", 3), "CAFE", "{\"b\":1,\"a\":[1,2],\"k\":\"😀\"}", "\"str\"", "1",
	        "{\"z\":1,\"a\":2,\"m\":3}"}}}},
	    {"qualified_intervals",
	     "INTERVAL '1-2' YEAR TO MONTH AS year_to_month, INTERVAL '-5-3' YEAR TO MONTH AS year_to_month_negative, "
	     "INTERVAL '3 04:05:06.789012' DAY TO SECOND AS day_to_second, "
	     "INTERVAL '-3 04:05:06.789012' DAY TO SECOND AS day_to_second_negative",
	     {{{"year_to_month", "INTERVAL YEAR TO MONTH", 107, 13},
	       {"year_to_month_negative", "INTERVAL YEAR TO MONTH", 107, 13},
	       {"day_to_second", "INTERVAL DAY TO SECOND", 110, 26, 6},
	       {"day_to_second_negative", "INTERVAL DAY TO SECOND", 110, 26, 6}},
	      {{"1-2", "-5-3", "3 04:05:06.789012", "-2 19:54:53.210988"}}}},
	    {"unqualified_intervals",
	     "INTERVAL '1 day' AS unqualified, INTERVAL '-1 day 02:00:00' AS unqualified_negative, "
	     "'2024-01-02'::timestamp - '2024-01-01'::timestamp AS timestamp_difference, 1::oid AS oid",
	     {{{"unqualified", "CHARACTER VARYING", 12, 0}, {"unqualified_negative", "CHARACTER VARYING", 12, 0},
	       {"timestamp_difference", "CHARACTER VARYING", 12, 0}, {"oid", "INTEGER", 4, 10}},
	      {{"1 day", "-22:00:00", "1 day", "1"}}}},
	    {"spatial",
	     "ST_GeomFromText('POINT(1 2)') AS geometry, ST_GeogFromText('POINT(1 2)') AS geography",
	     {{{"geometry", "GEOMETRY", -4, 1024000}, {"geography", "GEOGRAPHY", -4, 1048316}},
	      {{"0101000000000000000000F03F0000000000000040", "0101000020E6100000000000000000F03F0000000000000040"}}}},
	    {"workarounds",
	     "'xy  '::varchar = 'xy'::varchar AS varchar_blanks_equal, LENGTH('xy  '::varchar) AS varchar_blanks_length, "
	     "'xy  '::char(4) = 'xy'::char(4) AS char_blanks_equal, DATEDIFF(second, '2024-01-01'::timestamp, "
	     "'2024-01-02 02:00:00'::timestamp) AS datediff_seconds, "
	     "EXTRACT(EPOCH FROM INTERVAL '1 day 02:00:00') AS interval_epoch, "
	     "EXTRACT(EPOCH FROM '2024-01-02'::timestamp - '2024-01-01'::timestamp) AS difference_epoch, "
	     "ST_AsText(ST_GeomFromText('POINT(1 2)')) AS geometry_text, ST_AsHexEWKB(ST_GeomFromText('POINT(1 2)', "
	     "4326)) AS geometry_hex, ST_AsBinary(ST_GeomFromText('POINT(1 2)')) AS geometry_bytes",
	     {{{"varchar_blanks_equal", "BOOL", -7, 1}, {"varchar_blanks_length", "INTEGER", 4, 10},
	       {"char_blanks_equal", "BOOL", -7, 1}, {"datediff_seconds", "BIGINT", -5, 19},
	       {"interval_epoch", "DOUBLE PRECISION", 8, 15}, {"difference_epoch", "DOUBLE PRECISION", 8, 15},
	       {"geometry_text", "CHARACTER VARYING", 12, 10}, {"geometry_hex", "CHARACTER VARYING", 12, 50},
	       {"geometry_bytes", "VARBYTE", -4, 21}},
	      {{"0", "4", "1", "93600", "93600", "86400", "POINT(1 2)",
	        "0101000020E6100000000000000000F03F0000000000000040",
	        std::string("\x01\x01\x00\x00\x00\x00\x00\x00\x00\x00\x00\xf0?\x00\x00\x00\x00\x00\x00\x00@", 21)}}}},
	};
	// clang-format on
	return selects;
}

//! Every recorded row that differs from its select's.
inline const std::vector<RecordedDifference> &RecordedDifferences() {
	// clang-format off
	static const std::vector<RecordedDifference> differences {
	    {"timetz", "", NodePath::COMPUTE,
	     {"07:04:56.50+00", "09:00:59.999999+00", "15:30:15+00", "15:30:00+00", "00:00:00+00", "20:34:56+00"}},
	    {"float8", "SET extra_float_digits TO 0", NodePath::LEADER,
	     {"-0", "NaN", "Infinity", "-Infinity", "0.1", "0.3", "1.79769313486232e+308", "2.2250738585072e-308",
	       "9.00719925474099e+15"}},
	    {"float8", "SET extra_float_digits TO 0", NodePath::COMPUTE,
	     {"-0", "NaN", "Infinity", "-Infinity", "0.1", "0.3", "1.79769313486232e+308", "2.2250738585072e-308",
	       "9.00719925474099e+15"}},
	    {"float4", "SET extra_float_digits TO 0", NodePath::LEADER,
	     {"-0", "NaN", "-Infinity", "0.1", "0.3", "3.40282e+38", "1.17549e-38", "1.67772e+07"}},
	    {"float4", "SET extra_float_digits TO 0", NodePath::COMPUTE,
	     {"-0", "NaN", "-Infinity", "0.1", "0.3", "3.40282e+38", "1.17549e-38", "1.67772e+07"}},
	    {"timetz", "SET timezone TO 'America/New_York'", NodePath::COMPUTE,
	     {"02:04:56.50-05", "04:00:59.999999-05", "10:30:15-05", "10:30:00-05", "19:00:00-05", "15:34:56-05"}},
	    {"timestamptz", "SET timezone TO 'America/New_York'", NodePath::LEADER,
	     {"1883-11-18 07:03:58-04:56", "4714-12-31 19:03:58-04:56 BC", "0001-12-31 19:03:58-04:56 BC",
	       "2024-02-29 02:04:56.123456-05", "9999-12-31 19:00:00-05", "294276-12-31 18:59:59.999999-05",
	       "1899-12-31 19:00:00-05"}},
	    {"timestamptz", "SET timezone TO 'America/New_York'", NodePath::COMPUTE,
	     {"1883-11-18 07:03:56-04:56", "4714-12-31 19:03:56-04:56 BC", "0001-12-31 19:03:56-04:56 BC",
	       "2024-02-29 02:04:56.123456-05", "9999-12-31 19:00:00-05", "294276-12-31 18:59:59.999999-05",
	       "1899-12-31 19:00:00-05"}},
	    {"timetz", "SET timezone TO 'Asia/Kolkata'", NodePath::COMPUTE,
	     {"12:34:56.50+05:30", "14:30:59.999999+05:30", "21:00:15+05:30", "21:00:00+05:30", "05:30:00+05:30",
	       "02:04:56+05:30"}},
	    {"timestamptz", "SET timezone TO 'Asia/Kolkata'", NodePath::LEADER,
	     {"1883-11-18 17:21:10+05:21", "4713-01-01 05:53:28+05:53 BC", "0001-01-01 05:53:28+05:53",
	       "2024-02-29 12:34:56.123456+05:30", "10000-01-01 05:30:00+05:30", "294277-01-01 05:29:59.999999+05:30",
	       "1900-01-01 05:21:10+05:21"}},
	    {"timestamptz", "SET timezone TO 'Asia/Kolkata'", NodePath::COMPUTE,
	     {"1883-11-18 17:21:20+05:21", "4713-01-01 05:53:56+05:53 BC", "0001-01-01 05:53:56+05:53",
	       "2024-02-29 12:34:56.123456+05:30", "10000-01-01 05:30:00+05:30", "294277-01-01 05:29:59.999999+05:30",
	       "1900-01-01 05:21:20+05:21"}},
	    {"qualified_intervals", "SET intervalstyle TO 'postgres_verbose'", NodePath::LEADER,
	     {"1-2", "-5-3", "3 04:05:06.780000", "-2 19:54:53.210000"}},
	    {"qualified_intervals", "SET intervalstyle TO 'postgres_verbose'", NodePath::COMPUTE,
	     {"1-2", "-5-3", "3 04:05:06.780000", "-2 19:54:53.210000"}},
	    {"unqualified_intervals", "SET intervalstyle TO 'postgres_verbose'", NodePath::LEADER,
	     {"@ 1 day", "@ 22 hours ago", "@ 1 day", "1"}},
	    {"unqualified_intervals", "SET intervalstyle TO 'postgres_verbose'", NodePath::COMPUTE,
	     {"@ 1 day", "@ 22 hours ago", "@ 1 day", "1"}},
	    {"qualified_intervals", "SET intervalstyle TO 'sql_standard'", NodePath::LEADER,
	     {"1-2", "-5-3", "3 04:05:06.789012", "-3 04:05:06.789012"}},
	    {"qualified_intervals", "SET intervalstyle TO 'sql_standard'", NodePath::COMPUTE,
	     {"1-2", "-5-3", "3 04:05:06.789012", "-3 04:05:06.789012"}},
	    {"unqualified_intervals", "SET intervalstyle TO 'sql_standard'", NodePath::LEADER,
	     {"1 0:00:00.0", "-1 2:00:00.0", "1 0:00:00.0", "1"}},
	    {"unqualified_intervals", "SET intervalstyle TO 'sql_standard'", NodePath::COMPUTE,
	     {"1 0:00:00.0", "-1 2:00:00.0", "1 0:00:00.0", "1"}},
	};
	// clang-format on
	return differences;
}

//! Every statement whose columns alone are recorded.
inline const std::vector<RecordedDescribe> &RecordedDescribes() {
	// clang-format off
	static const std::vector<RecordedDescribe> describes {
	    {"SELECT NULL AS untyped_null",
	     {{"untyped_null", "CHARACTER VARYING", 12, 65535}}},
	    {"SELECT $1 AS untyped_parameter",
	     {{"untyped_parameter", "CHARACTER VARYING", 12, 0}}},
	    {"SELECT $1::integer AS typed_parameter",
	     {{"typed_parameter", "INTEGER", 4, 10}}},
	    {"SELECT 'x'::char(300) AS char_300, current_user AS user_name",
	     {{"char_300", "CHARACTER", 1, 300}, {"user_name", "CHARACTER", 1, 127}}},
	    {"SELECT hll_create_sketch(1) AS sketch",
	     {{"sketch", "HLLSKETCH", -4, 196612}}},
	    {"SELECT oid, relname, relkind FROM pg_class LIMIT 1",
	     {{"oid", "INTEGER", 4, 10, 0, 0}, {"relname", "CHARACTER VARYING", 12, 128, 0, 0},
	       {"relkind", "CHARACTER", 1, 0, 0, 0}}},
	    {"SELECT '-1.5'::numeric(38,10) AS quoted, -1.5::numeric(38,10) AS negated",
	     {{"quoted", "NUMERIC", 2, 38, 10}, {"negated", "NUMERIC", 2, 11, 10}}},
	    {"SELECT user_id, query_id, query_label, status, result_cache_hit, start_time FROM sys_query_history LIMIT 1",
	     {{"user_id", "INTEGER", 4, 10}, {"query_id", "BIGINT", -5, 19}, {"query_label", "CHARACTER VARYING", 12, 320},
	       {"status", "CHARACTER", 1, 10}, {"result_cache_hit", "BOOL", -7, 1},
	       {"start_time", "TIMESTAMP", 93, 26, 6}}},
	    {"SHOW timezone",
	     {{"TimeZone", "CHARACTER VARYING", 12, 65535}}},
	};
	// clang-format on
	return describes;
}

//! The row `differences` records for `select` on `path`'s node after `caller_statement`, else the one
//! it records after any statement, else null.
inline const std::vector<std::string> *DifferenceRow(const std::vector<RecordedDifference> &differences,
                                                     const RecordedSelect &select, const std::string &caller_statement,
                                                     NodePath path) {
	const std::vector<std::string> *for_any = nullptr;
	for (const auto &difference : differences) {
		if (select.name != std::string(difference.select) || difference.path != path) {
			continue;
		}
		if (caller_statement == difference.caller_statement) {
			return &difference.row;
		}
		if (std::string(difference.caller_statement).empty()) {
			for_any = &difference.row;
		}
	}
	return for_any;
}

//! The row `select` arrives with on `path`'s node after `caller_statement`: its difference for both,
//! else the compute node's under the pinned settings, else the leader node's.
inline std::vector<std::string> RecordedRow(const RecordedSelect &select, const std::string &caller_statement,
                                            NodePath path) {
	const auto *row = DifferenceRow(RecordedDifferences(), select, caller_statement, path);
	return row ? *row : select.result.rows[0];
}

} // namespace redshift_test
