//===----------------------------------------------------------------------===//
//                         DuckDB - Redshift
//
// test/client/odbc/recorded_stored_output.hpp
//
// What driver 2.3.0.0 sent live for the recorded selects of recorded_driver_output.hpp once stored:
// each select's values written by CREATE TEMP TABLE ... AS into a session temporary table and read
// back, through SQLExecute and through SQLExecDirect, under the pinned settings and after each
// recorded caller SET. A stored row is the compute nodes' recorded row for the constant, but for the
// differences below. The [live] cases check the pinned driver still sends exactly this; the offline
// cases replay the rows through the fake.
//===----------------------------------------------------------------------===//

#pragma once

#include "recorded_driver_output.hpp"

#include <set>
#include <string>
#include <vector>

namespace redshift_test {

//! How a stored row was read: SQLExecute, SQLExecDirect, or either, for a difference both share.
enum class StoredRead : uint8_t { PREPARED, DIRECT, EITHER };

//! A stored row that differs from the compute nodes' recorded row for its select, read as `read`
//! after `caller_statement`, or after any statement when it is empty.
struct StoredDifference {
	const char *select;
	const char *caller_statement;
	StoredRead read;
	std::vector<std::string> row;
};

//! A recorded select the server will not store, and what the client throws for its CREATE.
struct UnstoredSelect {
	const char *select;
	std::string refusal;
};

//! The recorded selects the server refuses to store: an unqualified interval has no column type.
inline const std::vector<UnstoredSelect> &UnstoredSelects() {
	static const std::vector<UnstoredSelect> unstored {
	    {"unqualified_intervals",
	     "Redshift refused the statement: ERROR:  Column \"unqualified\" has unsupported type \"interval\".\n"
	     "SQLSTATE: 0A000"}};
	return unstored;
}

//! The stored selects whose read through SQLExecDirect the server fails (XX000): each holds a -infinity
//! timestamp, which SQLExecute sends as a word outside its text form.
const std::set<std::string> STORED_DIRECT_REFUSED {"infinities", "negative_infinity"};

//! Whether the server fails `select`'s stored read through SQLExecDirect.
inline bool StoredDirectRefused(const RecordedSelect &select) {
	return STORED_DIRECT_REFUSED.count(select.name) != 0;
}

//! The temporary table that holds `select`'s values.
inline std::string StoredTable(const RecordedSelect &select) {
	return std::string("stored_") + select.name;
}

//! The statement that stores `select`'s values, run under the pinned settings.
inline std::string StoredCreate(const RecordedSelect &select) {
	return "CREATE TEMP TABLE " + StoredTable(select) + " AS SELECT " + select.select_list;
}

//! The statement that reads `select`'s stored values back, on the compute nodes.
inline std::string StoredSql(const RecordedSelect &select) {
	return "SELECT * FROM " + StoredTable(select);
}

//! The refusal of `select`'s CREATE when the server will not store it, else null.
inline const UnstoredSelect *FindUnstored(const RecordedSelect &select) {
	for (const auto &unstored : UnstoredSelects()) {
		if (select.name == std::string(unstored.select)) {
			return &unstored;
		}
	}
	return nullptr;
}

//! The stored-only selects, of what no recorded select returns: a CHAR(300); an HLLSKETCH, which only a
//! stored read can show; a float4 that needs a ninth significant digit, which the server's eight
//! cannot carry, so 100000024 arrives as 1.0000002e+08, another float; the row of a TIMESTAMP and a
//! TIMESTAMPTZ infinity; and the row of a TIMESTAMP and a TIMESTAMPTZ -infinity, where the recorded
//! select holds all four in one row.
inline const std::vector<RecordedSelect> &StoredOnlySelects() {
	// clang-format off
	static const std::vector<RecordedSelect> selects {
	    {"char_300", "'x'::char(300) AS char_300",
	     {{{"char_300", "CHARACTER", 1, 300}}, {{"x" + std::string(299, ' ')}}}},
	    {"hllsketch", "hll_create_sketch(1) AS sketch",
	     {{{"sketch", "HLLSKETCH", -4, 196612}},
	      {{"{\"version\":1,\"logm\":15,\"sparse\":{\"indices\":[22362299],\"values\":[1]}}"}}}},
	    {"float4_ninth_digit", "'100000024'::real AS ninth_digit",
	     {{{"ninth_digit", "REAL", 7, 7}}, {{"1.0000002e+08"}}}},
	    {"infinity", "'infinity'::timestamp AS timestamp_infinity, 'infinity'::timestamptz AS timestamptz_infinity",
	     {{{"timestamp_infinity", "TIMESTAMP", 93, 26, 6}, {"timestamptz_infinity", "TIMESTAMPTZ", 93, 26, 6}},
	      {{"infinity", "infinity"}}}},
	    {"negative_infinity",
	     "'-infinity'::timestamp AS timestamp_negative_infinity, "
	     "'-infinity'::timestamptz AS timestamptz_negative_infinity",
	     {{{"timestamp_negative_infinity", "TIMESTAMP", 93, 26, 6},
	       {"timestamptz_negative_infinity", "TIMESTAMPTZ", 93, 26, 6}},
	      {{"-infinity", "-infinity"}}}},
	};
	// clang-format on
	return selects;
}

//! Every recorded select the server stores, with the columns its table describes, then
//! StoredOnlySelects. An empty VARCHAR constant describes as size 65535, its stored column as size 1.
inline const std::vector<RecordedSelect> &StoredSelects() {
	static const std::vector<RecordedSelect> stored = [] {
		std::vector<RecordedSelect> selects;
		for (const auto &select : RecordedSelects()) {
			if (!FindUnstored(select)) {
				selects.push_back(select);
			}
		}
		for (auto &select : selects) {
			for (auto &column : select.result.columns) {
				if (column.name == "varchar_empty") {
					column.column_size = 1;
				}
			}
		}
		const auto &stored_only = StoredOnlySelects();
		selects.insert(selects.end(), stored_only.begin(), stored_only.end());
		return selects;
	}();
	return stored;
}

//! Every stored row that differs from the compute nodes' recorded row for its select.
inline const std::vector<StoredDifference> &StoredDifferences() {
	// clang-format off
	static const std::vector<StoredDifference> differences {
	    // The simple protocol's fixed formatter ignores the session's settings: dates with an
	    // astronomical year, floats with 15 or 6 significant digits.
	    {"date", "", StoredRead::DIRECT,
	     {"-4712-01-01", "-4713-11-24", "0000-12-31", "-043-03-15", "0001-01-01", "294276-12-31", "10000-01-01",
	      "5874897-12-31"}},
	    {"float8", "", StoredRead::DIRECT,
	     {"-0", "nan", "inf", "-inf", "0.1", "0.3", "1.79769313486232e+308", "2.2250738585072e-308",
	      "9.00719925474099e+15"}},
	    {"float4", "", StoredRead::DIRECT,
	     {"-0", "nan", "-inf", "0.1", "0.3", "3.40282e+38", "1.17549e-38", "1.67772e+07"}},
	    {"float4_ninth_digit", "", StoredRead::DIRECT, {"1e+08"}},
	    // A stored +infinity alone, through the simple protocol, is the timestamp of the largest
	    // microsecond count, a TIMESTAMPTZ one in the session's zone.
	    {"infinity", "", StoredRead::DIRECT, {"294277-01-09 04:00:54.775807", "294277-01-09 04:00:54.775807+00"}},
	    {"infinity", "SET timezone TO 'America/New_York'", StoredRead::DIRECT,
	     {"294277-01-09 04:00:54.775807", "294277-01-08 23:00:54.775807-05"}},
	    {"infinity", "SET timezone TO 'Asia/Kolkata'", StoredRead::DIRECT,
	     {"294277-01-09 04:00:54.775807", "294277-01-09 09:30:54.775807+05:30"}},
	    // At extra_float_digits 0 SQLExecute prints a float4 with 6 significant digits too.
	    {"float4_ninth_digit", "SET extra_float_digits TO 0", StoredRead::PREPARED, {"1e+08"}},
	    // After a zone's SET a stored instant prints as the leader node prints the constant, with the
	    // offset's seconds kept in the time, where the compute nodes' constant loses them.
	    {"timestamptz", "SET timezone TO 'America/New_York'", StoredRead::EITHER,
	     {"1883-11-18 07:03:58-04:56", "4714-12-31 19:03:58-04:56 BC", "0001-12-31 19:03:58-04:56 BC",
	      "2024-02-29 02:04:56.123456-05", "9999-12-31 19:00:00-05", "294276-12-31 18:59:59.999999-05",
	      "1899-12-31 19:00:00-05"}},
	    {"timestamptz", "SET timezone TO 'Asia/Kolkata'", StoredRead::EITHER,
	     {"1883-11-18 17:21:10+05:21", "4713-01-01 05:53:28+05:53 BC", "0001-01-01 05:53:28+05:53",
	      "2024-02-29 12:34:56.123456+05:30", "10000-01-01 05:30:00+05:30", "294277-01-01 05:29:59.999999+05:30",
	      "1900-01-01 05:21:10+05:21"}},
	    // Stored under the pinned style, the negative DAY TO SECOND holds -2 days 19:54:53.210988, which
	    // sql_standard prints so; the constant was read under sql_standard as another value.
	    {"qualified_intervals", "SET intervalstyle TO 'sql_standard'", StoredRead::EITHER,
	     {"1-2", "-5-3", "3 04:05:06.789012", "-2 19:54:53.210988"}},
	};
	// clang-format on
	return differences;
}

//! The difference for `select` read as `read` after exactly `caller_statement`, or null. One after a
//! given statement overrides one after any, as StoredRow says.
inline const StoredDifference *FindStoredDifference(const RecordedSelect &select, const std::string &caller_statement,
                                                    StoredRead read) {
	for (const auto &difference : StoredDifferences()) {
		if (select.name == std::string(difference.select) && caller_statement == difference.caller_statement &&
		    (difference.read == read || difference.read == StoredRead::EITHER)) {
			return &difference;
		}
	}
	return nullptr;
}

//! The row `select`'s table arrives with, read as `read` (PREPARED or DIRECT) after `caller_statement`:
//! its difference for both, else its difference after any statement, else the compute nodes' row.
inline std::vector<std::string> StoredRow(const RecordedSelect &select, const std::string &caller_statement,
                                          StoredRead read) {
	if (const auto *difference = FindStoredDifference(select, caller_statement, read)) {
		return difference->row;
	}
	if (const auto *difference = FindStoredDifference(select, "", read)) {
		return difference->row;
	}
	return RecordedRow(select, caller_statement, NodePath::COMPUTE);
}

} // namespace redshift_test
