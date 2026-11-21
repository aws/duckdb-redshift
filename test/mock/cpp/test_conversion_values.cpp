// Cell values: every value kind against every column type, the native types and their
// ranges, raw BLOB bytes, exact VARCHAR, and every text form the transport sends for DECIMAL
// and the temporal types, checked against the exact value it denotes. No connection is
// involved: the tests build transport-neutral columns and values directly.

#include "catch.hpp"

#include "duckdb.hpp"
#include "duckdb/common/types/date.hpp"
#include "duckdb/common/types/time.hpp"
#include "duckdb/common/operator/cast_operators.hpp"
#include "duckdb/common/types/timestamp.hpp"
#include "redshift_conversion.hpp"
#include "test_util.hpp"

#include <cmath>
#include <cstring>
#include <limits>
#include <type_traits>

using namespace duckdb;
using redshift_test::Capture;

namespace {

RedshiftColumn Column(RedshiftTypeId type, int32_t precision = 0, int32_t scale = 0, const string &name = "c") {
	RedshiftColumn column;
	column.name = name;
	column.type = type;
	column.precision = precision;
	column.scale = scale;
	return column;
}

RedshiftValue Null() {
	return RedshiftValue();
}
RedshiftValue Bool(bool b) {
	RedshiftValue v;
	v.kind = RedshiftValueKind::BOOLEAN;
	v.boolean = b;
	return v;
}
RedshiftValue Int(int64_t i) {
	RedshiftValue v;
	v.kind = RedshiftValueKind::INTEGER;
	v.integer = i;
	return v;
}
RedshiftValue Real(double d) {
	RedshiftValue v;
	v.kind = RedshiftValueKind::REAL;
	v.real = d;
	return v;
}
RedshiftValue Text(const string &s) {
	RedshiftValue v;
	v.kind = RedshiftValueKind::TEXT;
	v.text = s;
	return v;
}
RedshiftValue Blob(const string &bytes) {
	RedshiftValue v;
	v.kind = RedshiftValueKind::BLOB;
	v.text = bytes;
	return v;
}

//! Writes rows of one shape into one chunk, as the scan does.
struct Written {
	explicit Written(vector<RedshiftColumn> columns) : writer(std::move(columns)) {
		chunk.Initialize(Allocator::DefaultAllocator(), writer.DuckDBColumnTypes());
	}

	//! Write `rows` as one chunk. Throws what the writer throws.
	void Run(const vector<vector<RedshiftValue>> &rows) {
		chunk.Reset();
		for (idx_t i = 0; i < rows.size(); i++) {
			writer.WriteRow(rows[i], chunk, i);
		}
		chunk.SetChildCardinality(rows.size());
	}

	template <class T>
	T Get(idx_t column, idx_t row) {
		REQUIRE_FALSE(FlatVector::IsNull(chunk.data[column], row));
		return FlatVector::GetData<T>(chunk.data[column])[row];
	}

	bool IsNull(idx_t column, idx_t row) {
		return FlatVector::IsNull(chunk.data[column], row);
	}

	RedshiftCellWriter writer;
	DataChunk chunk;
};

//! Convert one value in a one-column shape and return the stored cell.
template <class T>
T ConvertOne(const RedshiftColumn &column, const RedshiftValue &value) {
	// A string_t longer than 12 bytes points into the chunk's string heap, which is freed when this
	// returns; ConvertOneString copies the bytes out first.
	static_assert(!std::is_same<T, string_t>::value, "use ConvertOneString for VARCHAR and BLOB cells");
	Written written({column});
	written.Run({{value}});
	return written.Get<T>(0, 0);
}

//! Convert one VARCHAR, CHAR or BLOB value in a one-column shape and return a copy of its bytes, taken
//! while the chunk that holds them is still alive.
string ConvertOneString(const RedshiftColumn &column, const RedshiftValue &value) {
	Written written({column});
	written.Run({{value}});
	return written.Get<string_t>(0, 0).GetString();
}

//! What converting one value in a one-column shape throws.
redshift_test::Thrown RefuseOne(const RedshiftColumn &column, const RedshiftValue &value) {
	Written written({column});
	return Capture([&] { written.Run({{value}}); });
}

//! A refused value: ConversionException naming the column, never claiming re-running is safe.
void RequireValueRefused(const redshift_test::Thrown &thrown, const string &column_text) {
	REQUIRE(thrown.type == ExceptionType::CONVERSION);
	REQUIRE_THAT(thrown.message, Catch::StartsWith("Could not convert Redshift " + column_text + ": "));
	// DuckDB's own reason follows the column, so it is never empty.
	REQUIRE_THAT(thrown.message, !Catch::StartsWith("Could not convert Redshift " + column_text + ": . "));
	REQUIRE_THAT(thrown.message, Catch::Contains("To read the value as text, cast the column to VARCHAR in the "
	                                             "Redshift SQL."));
	REQUIRE_THAT(thrown.message, Catch::Contains("may already have applied its effects"));
	REQUIRE_THAT(thrown.message, !Catch::Contains("safe"));
}

//! A contract violation: ExecutorException, never InternalException, never claiming safety.
void RequireContractViolation(const redshift_test::Thrown &thrown, const string &detail) {
	REQUIRE(thrown.type == ExceptionType::EXECUTOR);
	REQUIRE_THAT(thrown.message,
	             Catch::StartsWith("Redshift returned a result that does not match the shape described at bind: "));
	REQUIRE_THAT(thrown.message, Catch::Contains(detail));
	REQUIRE_THAT(thrown.message, Catch::Contains("remote table may have changed"));
	REQUIRE_THAT(thrown.message, Catch::Contains("may already have applied its effects"));
	REQUIRE_THAT(thrown.message, !Catch::Contains("safe"));
}

timestamp_t Ts(int32_t y, int32_t mo, int32_t d, int32_t h, int32_t mi, int32_t s, int32_t us = 0) {
	return Timestamp::FromDatetime(Date::FromDate(y, mo, d), Time::FromTime(h, mi, s, us));
}

} // namespace

//===--------------------------------------------------------------------===//
// Value kinds and row width
//===--------------------------------------------------------------------===//

TEST_CASE("every column type accepts exactly its value kind and NULL", "[conversion][values]") {
	struct Legal {
		RedshiftColumn column;
		RedshiftValue value;
	};
	const Legal legal[] = {
	    {Column(RedshiftTypeId::BOOLEAN), Bool(true)},
	    {Column(RedshiftTypeId::SMALLINT), Int(1)},
	    {Column(RedshiftTypeId::INTEGER), Int(1)},
	    {Column(RedshiftTypeId::BIGINT), Int(1)},
	    {Column(RedshiftTypeId::FLOAT), Real(1)},
	    {Column(RedshiftTypeId::DOUBLE), Real(1)},
	    {Column(RedshiftTypeId::DECIMAL, 10, 2), Text("1.00")},
	    {Column(RedshiftTypeId::VARCHAR), Text("a")},
	    {Column(RedshiftTypeId::CHAR), Text("a")},
	    {Column(RedshiftTypeId::DATE), Text("2024-01-01")},
	    {Column(RedshiftTypeId::TIME), Text("01:02:03")},
	    {Column(RedshiftTypeId::TIMETZ), Text("01:02:03+00")},
	    {Column(RedshiftTypeId::TIMESTAMP), Text("2024-01-01 01:02:03")},
	    {Column(RedshiftTypeId::TIMESTAMPTZ), Text("2024-01-01 01:02:03+00")},
	    {Column(RedshiftTypeId::BLOB), Blob("a")},
	};
	struct Kind {
		RedshiftValue value;
		const char *name;
	};
	const Kind every_kind[] = {{Bool(true), "a BOOLEAN value"},
	                           {Int(1), "an INTEGER value"},
	                           {Real(1), "a REAL value"},
	                           {Text("1"), "a TEXT value"},
	                           {Blob("1"), "a BLOB value"}};

	for (auto &c : legal) {
		auto type_name = RedshiftTypeMapping::ToLogicalType(c.column, 0).ToString();
		INFO("column type " << type_name);

		// The legal value kind converts, and NULL is legal for every type.
		Written written({c.column});
		written.Run({{c.value}, {Null()}});
		REQUIRE_FALSE(written.IsNull(0, 0));
		REQUIRE(written.IsNull(0, 1));

		// Every other non-NULL value kind is a contract violation.
		for (auto &kind : every_kind) {
			if (kind.value.kind == c.value.kind) {
				continue;
			}
			INFO("value kind " << kind.name);
			RequireContractViolation(RefuseOne(c.column, kind.value), "column 1 (\"c\") is declared " + type_name +
			                                                              " but a cell arrived as " + kind.name);
		}
	}
}

TEST_CASE("the contract-violation message names the declared type and the value kind", "[conversion][values]") {
	auto thrown = RefuseOne(Column(RedshiftTypeId::INTEGER, 0, 0, "qty"), Text("1"));
	RequireContractViolation(thrown, "column 1 (\"qty\") is declared INTEGER but a cell arrived as a TEXT value");

	// An unrecognised kind is still a violation, not an internal error.
	RedshiftValue odd;
	odd.kind = static_cast<RedshiftValueKind>(99);
	RequireContractViolation(RefuseOne(Column(RedshiftTypeId::VARCHAR), odd), "an unrecognised value kind");
}

TEST_CASE("a row whose width differs from the shape is a contract violation", "[conversion][values]") {
	Written written({Column(RedshiftTypeId::INTEGER), Column(RedshiftTypeId::VARCHAR)});
	RequireContractViolation(Capture([&] { written.Run({{Int(1)}}); }), "a row has 1 values, but 2 columns");
	RequireContractViolation(Capture([&] {
		                         written.Run({{Int(1), Text("a"), Text("b")}});
	                         }),
	                         "a row has 3 values, but 2 columns");
	RequireContractViolation(Capture([&] { written.Run({{}}); }), "a row has 0 values, but 2 columns");
	// The width is checked on every row, not only the first.
	RequireContractViolation(Capture([&] { written.Run({{Int(1), Text("a")}, {Int(2)}}); }), "a row has 1 values");
}

//===--------------------------------------------------------------------===//
// Native values
//===--------------------------------------------------------------------===//

TEST_CASE("BOOLEAN and INTEGER values are stored natively, at their limits", "[conversion][values]") {
	REQUIRE(ConvertOne<bool>(Column(RedshiftTypeId::BOOLEAN), Bool(true)) == true);
	REQUIRE(ConvertOne<bool>(Column(RedshiftTypeId::BOOLEAN), Bool(false)) == false);

	auto smallint = Column(RedshiftTypeId::SMALLINT);
	REQUIRE(ConvertOne<int16_t>(smallint, Int(-32768)) == -32768);
	REQUIRE(ConvertOne<int16_t>(smallint, Int(32767)) == 32767);
	REQUIRE(ConvertOne<int16_t>(smallint, Int(0)) == 0);

	auto integer = Column(RedshiftTypeId::INTEGER);
	REQUIRE(ConvertOne<int32_t>(integer, Int(std::numeric_limits<int32_t>::min())) ==
	        std::numeric_limits<int32_t>::min());
	REQUIRE(ConvertOne<int32_t>(integer, Int(std::numeric_limits<int32_t>::max())) ==
	        std::numeric_limits<int32_t>::max());

	auto bigint = Column(RedshiftTypeId::BIGINT);
	REQUIRE(ConvertOne<int64_t>(bigint, Int(std::numeric_limits<int64_t>::min())) ==
	        std::numeric_limits<int64_t>::min());
	REQUIRE(ConvertOne<int64_t>(bigint, Int(std::numeric_limits<int64_t>::max())) ==
	        std::numeric_limits<int64_t>::max());
}

TEST_CASE("an INTEGER value outside a SMALLINT or INTEGER column's range is refused", "[conversion][values]") {
	auto smallint = Column(RedshiftTypeId::SMALLINT, 0, 0, "qty");
	for (int64_t v : {int64_t(32768), int64_t(-32769), std::numeric_limits<int64_t>::max()}) {
		INFO(v);
		auto thrown = RefuseOne(smallint, Int(v));
		RequireValueRefused(thrown, "column 1 (\"qty\")");
		REQUIRE_THAT(thrown.message, Catch::Contains("value " + std::to_string(v) + " is out of range for SMALLINT"));
	}
	auto integer = Column(RedshiftTypeId::INTEGER, 0, 0, "n");
	for (int64_t v : {int64_t(2147483648LL), int64_t(-2147483649LL), std::numeric_limits<int64_t>::min()}) {
		INFO(v);
		auto thrown = RefuseOne(integer, Int(v));
		RequireValueRefused(thrown, "column 1 (\"n\")");
		REQUIRE_THAT(thrown.message, Catch::Contains("is out of range for INTEGER"));
	}
}

TEST_CASE("REAL values are stored natively, with no decimal round trip", "[conversion][values]") {
	auto dbl = Column(RedshiftTypeId::DOUBLE);
	const double doubles[] = {0.1,
	                          -0.0,
	                          std::numeric_limits<double>::max(),
	                          std::numeric_limits<double>::lowest(),
	                          std::numeric_limits<double>::min(),
	                          std::numeric_limits<double>::denorm_min(),
	                          std::numeric_limits<double>::infinity(),
	                          -std::numeric_limits<double>::infinity()};
	for (auto d : doubles) {
		INFO(d);
		auto stored = ConvertOne<double>(dbl, Real(d));
		REQUIRE(stored == d);
		REQUIRE(std::signbit(stored) == std::signbit(d));
	}
	REQUIRE(std::isnan(ConvertOne<double>(dbl, Real(std::nan("")))));

	auto flt = Column(RedshiftTypeId::FLOAT);
	REQUIRE(ConvertOne<float>(flt, Real(0.1)) == 0.1f);
	REQUIRE(ConvertOne<float>(flt, Real(std::numeric_limits<float>::max())) == std::numeric_limits<float>::max());
	REQUIRE(ConvertOne<float>(flt, Real(std::numeric_limits<float>::lowest())) == std::numeric_limits<float>::lowest());
	// The shortest decimal text of the largest float, 3.4028235e+38, lies just above it as a double.
	// It narrows to that float, not to an infinity, so it is not refused. Redshift prints a stored
	// float4 with 6 digits, 3.40282e+38 for the largest, which lies just below it.
	REQUIRE(ConvertOne<float>(flt, Real(3.4028235e38)) == std::numeric_limits<float>::max());
	REQUIRE(ConvertOne<float>(flt, Real(3.40282e38)) == 3.40282e38f);
	REQUIRE(ConvertOne<float>(flt, Real(-3.4028235e38)) == std::numeric_limits<float>::lowest());
	REQUIRE(std::signbit(ConvertOne<float>(flt, Real(-0.0))));
	REQUIRE(ConvertOne<float>(flt, Real(std::numeric_limits<double>::infinity())) ==
	        std::numeric_limits<float>::infinity());
	REQUIRE(std::isnan(ConvertOne<float>(flt, Real(std::nan("")))));
}

TEST_CASE("a finite REAL that overflows a FLOAT column is refused, not turned into infinity", "[conversion][values]") {
	struct Case {
		double value;
		const char *text;
	};
	for (auto &c : {Case {1e39, "value 1e+39 is out of range for FLOAT"},
	                Case {-1e39, "value -1e+39 is out of range for FLOAT"}}) {
		INFO(c.value);
		auto thrown = RefuseOne(Column(RedshiftTypeId::FLOAT, 0, 0, "f"), Real(c.value));
		RequireValueRefused(thrown, "column 1 (\"f\")");
		REQUIRE_THAT(thrown.message, Catch::Contains(c.text));
	}
}

//===--------------------------------------------------------------------===//
// VARCHAR and BLOB
//===--------------------------------------------------------------------===//

TEST_CASE("VARCHAR text is copied unchanged", "[conversion][values]") {
	const string texts[] = {
	    "",
	    "ab   ", // trailing blanks are kept: VARCHAR is never trimmed
	    "short", // inlined in string_t
	    "a string well past the twelve-byte inline limit",
	    "caf\xC3\xA9 \xE6\x97\xA5\xE6\x9C\xAC \xF0\x9F\xA6\x86", // two-, three- and four-byte UTF-8
	    string("nul\0inside", 10),
	    "{\"super\": [1, 2]}", // SUPER arrives as VARCHAR JSON text and passes through
	};
	for (auto &text : texts) {
		INFO(text);
		REQUIRE(ConvertOneString(Column(RedshiftTypeId::VARCHAR), Text(text)) == text);
	}
}

TEST_CASE("BLOB bytes are copied raw and never parsed as text", "[conversion][values]") {
	const string blobs[] = {
	    "",
	    string("\x00", 1),
	    string("\x00\x01\xFF\x00", 4),
	    "\\x41",    // four literal bytes, not an escape for 'A'
	    "\xC3\x28", // invalid UTF-8 is fine in a BLOB
	    string(1000, '\xAB'),
	};
	for (auto &bytes : blobs) {
		auto stored = ConvertOneString(Column(RedshiftTypeId::BLOB), Blob(bytes));
		REQUIRE(stored.size() == bytes.size());
		REQUIRE(stored == bytes);
	}
}

//===--------------------------------------------------------------------===//
// DECIMAL text
//===--------------------------------------------------------------------===//

TEST_CASE("DECIMAL text converts to the exact unscaled value at every storage width", "[conversion][values]") {
	// DECIMAL(4,2) is stored in int16, (9,s) in int32, (18,s) in int64, (38,s) in hugeint.
	auto d4 = Column(RedshiftTypeId::DECIMAL, 4, 2);
	REQUIRE(ConvertOne<int16_t>(d4, Text("99.99")) == 9999);
	REQUIRE(ConvertOne<int16_t>(d4, Text("-99.99")) == -9999);
	REQUIRE(ConvertOne<int16_t>(d4, Text("0.00")) == 0);
	REQUIRE(ConvertOne<int16_t>(d4, Text("-0.01")) == -1);

	auto d9 = Column(RedshiftTypeId::DECIMAL, 9, 0);
	REQUIRE(ConvertOne<int32_t>(d9, Text("999999999")) == 999999999);
	REQUIRE(ConvertOne<int32_t>(d9, Text("-999999999")) == -999999999);
	REQUIRE(ConvertOne<int32_t>(d9, Text("0")) == 0);

	auto d18 = Column(RedshiftTypeId::DECIMAL, 18, 6);
	REQUIRE(ConvertOne<int64_t>(d18, Text("999999999999.999999")) == 999999999999999999LL);
	REQUIRE(ConvertOne<int64_t>(d18, Text("-0.000001")) == -1);

	auto d38 = Column(RedshiftTypeId::DECIMAL, 38, 0);
	auto max38 = Hugeint::POWERS_OF_TEN[38] - hugeint_t(1);
	REQUIRE(ConvertOne<hugeint_t>(d38, Text(string(38, '9'))) == max38);
	REQUIRE(ConvertOne<hugeint_t>(d38, Text("-" + string(38, '9'))) == -max38);

	auto d38_38 = Column(RedshiftTypeId::DECIMAL, 38, 38);
	REQUIRE(ConvertOne<hugeint_t>(d38_38, Text("0." + string(38, '9'))) == max38);
	REQUIRE(ConvertOne<hugeint_t>(d38_38, Text("-0." + string(37, '0') + "1")) == hugeint_t(-1));
}

TEST_CASE("DECIMAL text that does not fit or is not a number is refused", "[conversion][values]") {
	auto d4 = Column(RedshiftTypeId::DECIMAL, 4, 2, "price");
	// Too many digits before the point for the precision: DuckDB's own reason.
	for (auto &text : {"100.00", "-100.00"}) {
		INFO(text);
		auto thrown = RefuseOne(d4, Text(text));
		RequireValueRefused(thrown, "column 1 (\"price\")");
		REQUIRE_THAT(thrown.message,
		             Catch::Contains("Could not convert string \"" + string(text) + "\" to DECIMAL(4,2)"));
	}
	// Not a number at all: refused by the form check before DuckDB parses it.
	for (auto &text : {"abc", "", "1.2.3", "12,50"}) {
		INFO(text);
		auto thrown = RefuseOne(d4, Text(text));
		RequireValueRefused(thrown, "column 1 (\"price\")");
		REQUIRE_THAT(thrown.message, Catch::Contains("is not a plain decimal number: an optional '-', digits, and "
		                                             "at most 2 digits after a '.'"));
	}
	// 39 digits: the form check passes it, and DuckDB's parser refuses it as too wide.
	auto wide = RefuseOne(Column(RedshiftTypeId::DECIMAL, 38, 0), Text("1" + string(38, '0')));
	RequireValueRefused(wide, "column 1 (\"c\")");
	REQUIRE_THAT(wide.message, Catch::Contains("Could not convert string"));
}

//===--------------------------------------------------------------------===//
// Temporal text
//===--------------------------------------------------------------------===//

TEST_CASE("DATE text converts exactly, including BC dates and year 1 BC", "[conversion][values]") {
	auto date = Column(RedshiftTypeId::DATE);
	REQUIRE(ConvertOne<date_t>(date, Text("2024-02-29")) == Date::FromDate(2024, 2, 29));
	REQUIRE(ConvertOne<date_t>(date, Text("1970-01-01")) == date_t(0));
	REQUIRE(ConvertOne<date_t>(date, Text("0001-01-01")) == Date::FromDate(1, 1, 1));
	REQUIRE(ConvertOne<date_t>(date, Text("294276-12-31")) == Date::FromDate(294276, 12, 31));

	// DuckDB numbers 1 BC as year 0 and 4713 BC as year -4712.
	auto bc = ConvertOne<date_t>(date, Text("4713-01-01 BC"));
	REQUIRE(bc == Date::FromDate(-4712, 1, 1));
	REQUIRE(Date::ToString(bc) == "4713-01-01 (BC)");
	auto year_1_bc = ConvertOne<date_t>(date, Text("0001-12-31 BC"));
	REQUIRE(year_1_bc == Date::FromDate(0, 12, 31));
	REQUIRE(year_1_bc + 1 == Date::FromDate(1, 1, 1));
}

TEST_CASE("TIME text converts exactly, with 1 to 6 fraction digits", "[conversion][values]") {
	auto time = Column(RedshiftTypeId::TIME);
	REQUIRE(ConvertOne<dtime_t>(time, Text("00:00:00")) == Time::FromTime(0, 0, 0));
	REQUIRE(ConvertOne<dtime_t>(time, Text("12:34:56")) == Time::FromTime(12, 34, 56));
	REQUIRE(ConvertOne<dtime_t>(time, Text("12:34:56.5")) == Time::FromTime(12, 34, 56, 500000));
	REQUIRE(ConvertOne<dtime_t>(time, Text("12:34:56.012")) == Time::FromTime(12, 34, 56, 12000));
	REQUIRE(ConvertOne<dtime_t>(time, Text("23:59:59.999999")) == Time::FromTime(23, 59, 59, 999999));
	// DuckDB's TIME range ends at 24:00:00. Redshift stores 24:00:00 as 00:00:00, so this text
	// does not arrive from a server, but accepting it is exact.
	REQUIRE(ConvertOne<dtime_t>(time, Text("24:00:00")) == Time::FromTime(24, 0, 0));
}

TEST_CASE("TIMETZ text keeps its offset, with and without minutes and seconds", "[conversion][values]") {
	struct Case {
		const char *text;
		dtime_t time;
		int32_t offset_seconds;
	};
	const Case cases[] = {
	    {"12:34:56+05", Time::FromTime(12, 34, 56), 5 * 3600},
	    {"12:34:56-08", Time::FromTime(12, 34, 56), -8 * 3600},
	    {"12:34:56+05:30", Time::FromTime(12, 34, 56), 5 * 3600 + 30 * 60},
	    {"12:34:56.123-03:30", Time::FromTime(12, 34, 56, 123000), -(3 * 3600 + 30 * 60)},
	    {"00:00:00+00", Time::FromTime(0, 0, 0), 0},
	    {"12:34:56+05:30:15", Time::FromTime(12, 34, 56), 5 * 3600 + 30 * 60 + 15},
	    {"12:34:56-05:59:59", Time::FromTime(12, 34, 56), -(5 * 3600 + 59 * 60 + 59)},
	};
	for (auto &c : cases) {
		INFO(c.text);
		auto stored = ConvertOne<dtime_tz_t>(Column(RedshiftTypeId::TIMETZ), Text(c.text));
		REQUIRE(stored.time() == c.time);
		REQUIRE(stored.offset() == c.offset_seconds);
	}
}

TEST_CASE("TIMESTAMP text converts exactly, including BC values", "[conversion][values]") {
	auto ts = Column(RedshiftTypeId::TIMESTAMP);
	REQUIRE(ConvertOne<timestamp_t>(ts, Text("2024-01-01 12:34:56")) == Ts(2024, 1, 1, 12, 34, 56));
	REQUIRE(ConvertOne<timestamp_t>(ts, Text("2024-01-01 12:34:56.000001")) == Ts(2024, 1, 1, 12, 34, 56, 1));
	REQUIRE(ConvertOne<timestamp_t>(ts, Text("2024-01-01 00:00:00.25")) == Ts(2024, 1, 1, 0, 0, 0, 250000));
	REQUIRE(ConvertOne<timestamp_t>(ts, Text("4713-11-24 00:00:00 BC")) == Ts(-4712, 11, 24, 0, 0, 0));
	REQUIRE(ConvertOne<timestamp_t>(ts, Text("0001-12-31 23:59:59.5 BC")) == Ts(0, 12, 31, 23, 59, 59, 500000));
}

TEST_CASE("TIMESTAMP text with a time zone is refused, not stored with the zone dropped", "[conversion][values]") {
	// A TIMESTAMP has no time zone, and the client's TIMESTAMP form never carries one. Each of these
	// would otherwise be stored as 12:00 with the zone silently dropped.
	auto ts = Column(RedshiftTypeId::TIMESTAMP, 0, 0, "t");
	for (auto text :
	     {"2024-01-01 12:00:00+05", "2024-01-01 12:00:00-08:30", "2024-01-01 12:00:00+00", "2024-01-01 12:00:00Z",
	      "2024-01-01 12:00:00 UTC", "2024-01-01 12:00:00 America/New_York", "0044-03-15 12:00:00+00 BC"}) {
		INFO(text);
		auto thrown = RefuseOne(ts, Text(text));
		RequireValueRefused(thrown, "column 1 (\"t\")");
		REQUIRE_THAT(thrown.message, Catch::Contains("has a time zone, which a TIMESTAMP does not take"));
	}

	// The same numeric offset in a TIMESTAMPTZ column is applied. A `Z` is not in the client's form, so
	// it is refused there too.
	auto tstz = Column(RedshiftTypeId::TIMESTAMPTZ, 0, 0, "t");
	REQUIRE(timestamp_t(ConvertOne<timestamp_tz_t>(tstz, Text("2024-01-01 12:00:00+05")).value) ==
	        Ts(2024, 1, 1, 7, 0, 0));
	RequireValueRefused(RefuseOne(tstz, Text("2024-01-01 12:00:00Z")), "column 1 (\"t\")");
}

TEST_CASE("temporal text in another temporal type's form is refused", "[conversion][values]") {
	// Every temporal value arrives as TEXT, so the value-kind check cannot tell them apart. Each of
	// these would otherwise be read as a plausible wrong value: a date as midnight, a time without
	// an offset as +00, a timestamp without an offset as UTC.
	struct Case {
		RedshiftTypeId type;
		const char *text;
		const char *reason;
	};
	const Case cases[] = {
	    {RedshiftTypeId::TIMESTAMP, "2024-01-01", "has no time of day, which a TIMESTAMP needs"},
	    {RedshiftTypeId::TIMESTAMP, "4713-01-01 BC", "has no time of day, which a TIMESTAMP needs"},
	    // DuckDB's special date words: `epoch` parses as a midnight, `infinity` as the infinity marker.
	    {RedshiftTypeId::TIMESTAMP, "epoch", "has no time of day, which a TIMESTAMP needs"},
	    {RedshiftTypeId::TIMESTAMP, "infinity", "timestamp field value out of range"},
	    {RedshiftTypeId::TIMESTAMPTZ, "-infinity", "timestamp field value out of range"},
	    {RedshiftTypeId::TIMESTAMPTZ, "2024-01-01", "has no time of day, which a TIMESTAMP WITH TIME ZONE needs"},
	    {RedshiftTypeId::TIMESTAMPTZ, "2024-01-01 12:00:00",
	     "has no time zone offset, which a TIMESTAMP WITH TIME ZONE needs"},
	    {RedshiftTypeId::TIMESTAMPTZ, "2024-01-01 12:00:00 UTC",
	     "names a time zone; a TIMESTAMP WITH TIME ZONE needs a numeric offset"},
	    {RedshiftTypeId::TIMESTAMPTZ, "2024-01-01 12:00:00 America/New_York",
	     "names a time zone; a TIMESTAMP WITH TIME ZONE needs a numeric offset"},
	    {RedshiftTypeId::TIMETZ, "12:34:56", "has no time zone offset, which a TIME WITH TIME ZONE needs"},
	    {RedshiftTypeId::TIMETZ, "12:34:56.5", "has no time zone offset, which a TIME WITH TIME ZONE needs"},
	};
	for (auto &c : cases) {
		INFO(c.text);
		auto thrown = RefuseOne(Column(c.type, 0, 0, "t"), Text(c.text));
		RequireValueRefused(thrown, "column 1 (\"t\")");
		REQUIRE_THAT(thrown.message, Catch::Contains(c.reason));
	}
}

TEST_CASE("DECIMAL text must be a plain decimal number", "[conversion][values]") {
	// DuckDB's DECIMAL parser is lenient: it rounds extra fraction digits and accepts exponents,
	// signs and surrounding spaces. Text the column's type cannot hold exactly, or in any other form
	// than an optional '-', digits and a '.' with fraction digits, is refused rather than silently
	// altered.
	auto d52 = Column(RedshiftTypeId::DECIMAL, 5, 2, "price");
	for (auto text : {"1.005", "12.345", "1e2", "+1.00", " 1.00", "1.00 ", ".50", "5.", "-", "- 1.00", "1.0.0"}) {
		INFO(text);
		auto thrown = RefuseOne(d52, Text(text));
		RequireValueRefused(thrown, "column 1 (\"price\")");
		REQUIRE_THAT(thrown.message, Catch::Contains("is not a plain decimal number: an optional '-', digits, and "
		                                             "at most 2 digits after a '.'"));
	}
	// Fewer fraction digits than the scale are exact, so they are accepted.
	REQUIRE(ConvertOne<int32_t>(d52, Text("1.5")) == 150);
	REQUIRE(ConvertOne<int32_t>(d52, Text("12")) == 1200);
	REQUIRE(ConvertOne<int32_t>(d52, Text("-0.50")) == -50);

	// A scale of 0 takes no decimal point at all.
	auto d50 = Column(RedshiftTypeId::DECIMAL, 5, 0, "n");
	REQUIRE(ConvertOne<int32_t>(d50, Text("12")) == 12);
	auto point = RefuseOne(d50, Text("12.0"));
	RequireValueRefused(point, "column 1 (\"n\")");
	REQUIRE_THAT(point.message, Catch::Contains("is not a plain decimal number: an optional '-' and digits."));
}

TEST_CASE("a TIMESTAMPTZ offset cannot carry a value past DuckDB's range", "[conversion][values]") {
	// DuckDB range-checks the local date and time, then applies the offset with only an overflow
	// check, so this instant would land exactly on timestamp infinity.
	auto tstz = Column(RedshiftTypeId::TIMESTAMPTZ, 0, 0, "t");
	auto thrown = RefuseOne(tstz, Text("294247-01-10 00:00:54.775807-04"));
	RequireValueRefused(thrown, "column 1 (\"t\")");
	REQUIRE_THAT(thrown.message, Catch::Contains("out of range"));

	// At the low end an offset can move DuckDB's earliest timestamp further down without reaching
	// -infinity, to a value DuckDB cannot convert back to a date and time.
	auto low = RefuseOne(tstz, Text("290309-12-22 00:00:00+01 BC"));
	RequireValueRefused(low, "column 1 (\"t\")");
	REQUIRE_THAT(low.message, Catch::Contains("out of range"));
	// The earliest timestamp itself is still accepted.
	REQUIRE(timestamp_t(ConvertOne<timestamp_tz_t>(tstz, Text("290309-12-22 00:00:00+00 BC")).value) ==
	        Timestamp::FromDatetime(Date::FromDate(Timestamp::MIN_YEAR, Timestamp::MIN_MONTH, Timestamp::MIN_DAY),
	                                dtime_t(0)));
}

TEST_CASE("a refused BC value's error quotes the text as sent", "[conversion][values]") {
	// The BC rewrite is internal: the error shows what the transport sent, never the " (BC)" form.
	struct Case {
		RedshiftTypeId type;
		const char *text;
		const char *reason;
	};
	const Case cases[] = {
	    {RedshiftTypeId::DATE, "0000-01-01 BC", "\"0000-01-01 BC\""},
	    {RedshiftTypeId::DATE, "2024-13-01 BC", "\"2024-13-01 BC\""},
	    {RedshiftTypeId::TIMESTAMP, "4713-01-01 BC", "\"4713-01-01 BC\" has no time of day"},
	    {RedshiftTypeId::TIMESTAMP, "0044-03-15 12:00:00+00 BC", "\"0044-03-15 12:00:00+00 BC\" has a time zone"},
	    {RedshiftTypeId::TIMESTAMP, "0044-13-15 12:00:00 BC", "\"0044-13-15 12:00:00 BC\""},
	    {RedshiftTypeId::TIMESTAMP, "0044-03-15 12:xx:00 BC", "\"0044-03-15 12:xx:00 BC\""},
	    {RedshiftTypeId::TIMESTAMPTZ, "0044-03-15 12:xx:00+00 BC", "\"0044-03-15 12:xx:00+00 BC\""},
	    {RedshiftTypeId::TIMESTAMPTZ, "0044-03-15 12:00:00 BC", "\"0044-03-15 12:00:00 BC\" has no time zone offset"},
	};
	for (auto &c : cases) {
		INFO(c.text);
		auto thrown = RefuseOne(Column(c.type, 0, 0, "t"), Text(c.text));
		RequireValueRefused(thrown, "column 1 (\"t\")");
		REQUIRE_THAT(thrown.message, Catch::Contains(c.reason));
		REQUIRE_THAT(thrown.message, !Catch::Contains("(BC)"));
	}
}

TEST_CASE("a malformed or out-of-range TIMESTAMP keeps DuckDB's reason", "[conversion][values]") {
	auto ts = Column(RedshiftTypeId::TIMESTAMP, 0, 0, "t");
	auto format = RefuseOne(ts, Text("2024-01-01 12:00:00 junk!"));
	RequireValueRefused(format, "column 1 (\"t\")");
	REQUIRE_THAT(format.message, Catch::Contains("invalid timestamp field format"));

	auto range = RefuseOne(ts, Text("294276-12-31 00:00:00"));
	RequireValueRefused(range, "column 1 (\"t\")");
	REQUIRE_THAT(range.message, Catch::Contains("out of range"));
}

TEST_CASE("a malformed or out-of-range DATE keeps DuckDB's reason", "[conversion][values]") {
	auto date = Column(RedshiftTypeId::DATE, 0, 0, "d");
	auto format = RefuseOne(date, Text("2024-01-01 junk"));
	RequireValueRefused(format, "column 1 (\"d\")");
	REQUIRE_THAT(format.message, Catch::Contains("invalid date field format"));

	auto range = RefuseOne(date, Text("2024-13-01"));
	RequireValueRefused(range, "column 1 (\"d\")");
	REQUIRE_THAT(range.message, Catch::Contains("date field value out of range"));
}

TEST_CASE("a malformed or out-of-range TIME or TIMETZ keeps DuckDB's reason", "[conversion][values]") {
	// The strict parse refuses these before the grammar check, so the message is DuckDB's.
	struct Case {
		RedshiftTypeId type;
		const char *text;
	};
	const Case cases[] = {
	    {RedshiftTypeId::TIME, "12:34:56 junk"},
	    {RedshiftTypeId::TIME, "25:00:00"},
	    {RedshiftTypeId::TIMETZ, "12:34:56+05 junk"},
	};
	for (auto &c : cases) {
		INFO(c.text);
		auto thrown = RefuseOne(Column(c.type, 0, 0, "t"), Text(c.text));
		RequireValueRefused(thrown, "column 1 (\"t\")");
		REQUIRE_THAT(thrown.message, Catch::Contains("time field value out of range: \"" + string(c.text) + "\""));
	}
}

TEST_CASE("DuckDB v2.0.1 still drops a TIMESTAMP's offset in strict mode", "[conversion][values]") {
	// Why the writer parses a TIMESTAMP with Timestamp::TryConvertTimestampTZ rather than DuckDB's
	// cast: the cast drops an offset even in strict mode, and only the lower-level parser reports
	// it. If an upgrade makes the cast refuse the offset, this fails; the writer still needs the
	// lower-level parser to find a date without a time of day, so its zone check stays.
	string_t text("2024-01-01 12:00:00+05");
	string error;
	CastParameters strict(true, &error);
	timestamp_t parsed;
	REQUIRE(TryCastErrorMessage::Operation<string_t, timestamp_t>(text, parsed, strict));
	REQUIRE(parsed == Ts(2024, 1, 1, 12, 0, 0));
}

TEST_CASE("TIMESTAMPTZ text converts to the exact instant, with the offset before BC", "[conversion][values]") {
	auto tstz = Column(RedshiftTypeId::TIMESTAMPTZ);
	auto get = [&](const string &text) {
		return timestamp_t(ConvertOne<timestamp_tz_t>(tstz, Text(text)).value);
	};
	REQUIRE(get("2024-01-01 12:00:00+00") == Ts(2024, 1, 1, 12, 0, 0));
	REQUIRE(get("2024-01-01 12:00:00+05:30") == Ts(2024, 1, 1, 6, 30, 0));
	REQUIRE(get("2024-01-01 12:00:00.5-08") == Ts(2024, 1, 1, 20, 0, 0, 500000));
	REQUIRE(get("0044-03-15 12:00:00+00 BC") == Ts(-43, 3, 15, 12, 0, 0));
	// An offset can move a 1 BC instant into 2 BC.
	REQUIRE(get("0001-01-01 00:00:00+01 BC") == Ts(-1, 12, 31, 23, 0, 0));
	// Offsets with seconds, such as New York's local mean time.
	REQUIRE(get("1883-11-18 07:03:58-04:56:02") == Ts(1883, 11, 18, 12, 0, 0));
	REQUIRE(get("0001-01-01 00:00:00-04:56:02 BC") == Ts(0, 1, 1, 4, 56, 2));
}

TEST_CASE("temporal text outside the grammar of the client's forms is refused", "[conversion][values]") {
	// DuckDB's parsers accept more than the client's forms and read some of it as a different value: they
	// keep only 6 fraction digits, read "+0099" or "+00:99" as +01:39, drop a TIMESTAMP's offset
	// that a ':' and a space follow, and skip spaces and a 'T' separator. The writer accepts only
	// the client's grammar for each type.
	struct Case {
		RedshiftTypeId type;
		const char *text;
	};
	const Case cases[] = {
	    {RedshiftTypeId::TIME, "12:34:56.1234569"},
	    {RedshiftTypeId::TIME, " 12:34:56.1234569"},
	    {RedshiftTypeId::TIME, "12:34"},
	    {RedshiftTypeId::TIMETZ, "12:34:56.1234569+00"},
	    {RedshiftTypeId::TIMETZ, "12:00:00+00:99"},
	    {RedshiftTypeId::TIMETZ, "12:00:00+0099"},
	    {RedshiftTypeId::TIMETZ, "12:00:00 +00:99"},
	    {RedshiftTypeId::TIMETZ, "12:00:00+05:60"},
	    {RedshiftTypeId::TIMETZ, "12:00:00+05:30:60"},
	    {RedshiftTypeId::TIMESTAMP, "2024-01-01 12:34:56.1234569"},
	    {RedshiftTypeId::TIMESTAMP, "0044-03-15 12:34:56.1234569 BC"},
	    {RedshiftTypeId::TIMESTAMP, "2024-01-01T12:34:56.1234569"},
	    {RedshiftTypeId::TIMESTAMP, "2024-01-01  12:34:56.1234569"},
	    {RedshiftTypeId::TIMESTAMP, " 2024-01-01 12:34:56"},
	    {RedshiftTypeId::TIMESTAMP, "2024-01-01T12:34:56"},
	    {RedshiftTypeId::TIMESTAMP, "2024/01/01 12:00:00"},
	    {RedshiftTypeId::TIMESTAMPTZ, "2024/01/01 12:00:00+05"},
	    // DuckDB's parser reads these as a TIMESTAMP with the offset dropped.
	    {RedshiftTypeId::TIMESTAMP, "2024-01-01T12:00:00+05: "},
	    {RedshiftTypeId::TIMESTAMP, "2024-01-01  12:00:00+05:30: "},
	    {RedshiftTypeId::TIMESTAMPTZ, "2024-01-01 12:34:56.1234569+00"},
	    {RedshiftTypeId::TIMESTAMPTZ, "2024-01-01 12:00:00+00:99"},
	    {RedshiftTypeId::TIMESTAMPTZ, "2024-01-01 12:00:00+0099"},
	    {RedshiftTypeId::TIMESTAMPTZ, "2024-01-01 12:00:00+05:30:99"},
	    {RedshiftTypeId::TIMESTAMPTZ, "2024-01-01T12:00:00+00:99"},
	    {RedshiftTypeId::TIMESTAMPTZ, " 2024-01-01 12:00:00+00:99"},
	    {RedshiftTypeId::TIMESTAMPTZ, "2024-01-01T12:00:00+00"},
	    {RedshiftTypeId::TIMESTAMPTZ, "0044-03-15 12:00:00+00:99 BC"},
	    {RedshiftTypeId::TIMESTAMPTZ, "2024-01-01 12:00:00Z"},
	    // One row for each grammar rule DuckDB's parser would otherwise let through.
	    {RedshiftTypeId::TIME, "12:34:56."},
	    {RedshiftTypeId::TIME, "1:00:00"},
	    {RedshiftTypeId::TIME, "012:34:56"},
	    {RedshiftTypeId::TIME, "12:3:56"},
	    {RedshiftTypeId::TIME, "12:34:5"},
	    {RedshiftTypeId::TIME, "12:34:56 "},
	    {RedshiftTypeId::TIMESTAMP, "24-01-01 12:00:00"},
	    {RedshiftTypeId::TIMESTAMP, "024-01-01 12:00:00"},
	    {RedshiftTypeId::TIMESTAMP, "0002024-01-01 12:00:00"},
	    {RedshiftTypeId::TIMESTAMPTZ, "0002024-01-01 12:00:00+00"},
	    {RedshiftTypeId::TIMESTAMP, "2024-1-01 12:00:00"},
	    {RedshiftTypeId::TIMESTAMP, "2024-01-1 12:00:00"},
	    {RedshiftTypeId::TIMESTAMP, "-0043-03-15 12:00:00"},
	    {RedshiftTypeId::TIMESTAMPTZ, "2024-01-01 12:00:00+05 "},
	    // Offset hours past DuckDB's TIMETZ range of +-15:59:59, which a timestamp would just apply.
	    {RedshiftTypeId::TIMESTAMPTZ, "2024-01-01 12:00:00+16"},
	    {RedshiftTypeId::TIMESTAMPTZ, "2024-01-01 12:00:00+20"},
	    {RedshiftTypeId::TIMESTAMPTZ, "2024-01-01 12:00:00-16:00"},
	    {RedshiftTypeId::TIMESTAMPTZ, "2024-01-01 12:00:00+99"},
	    {RedshiftTypeId::TIMESTAMPTZ, "2024-01-01 12:00:00-99"},
	    // DATE: DuckDB's special words, other separators and short fields, and spaces.
	    {RedshiftTypeId::DATE, "epoch"},
	    {RedshiftTypeId::DATE, "infinity"},
	    {RedshiftTypeId::DATE, "-infinity"},
	    {RedshiftTypeId::DATE, "2024/01/05"},
	    {RedshiftTypeId::DATE, "2024-1-5"},
	    {RedshiftTypeId::DATE, "2024-1-05"},
	    {RedshiftTypeId::DATE, "2024-01-5"},
	    {RedshiftTypeId::DATE, "24-01-01"},
	    {RedshiftTypeId::DATE, "043-03-15"},
	    {RedshiftTypeId::DATE, "-43-03-15"},
	    {RedshiftTypeId::DATE, "00002024-01-01"},
	    {RedshiftTypeId::DATE, " 2024-01-01"},
	    {RedshiftTypeId::DATE, "2024-01-01 "},
	};
	// Each type's message states that type's whole form, with its limits.
	auto form = [](RedshiftTypeId type) -> string {
		switch (type) {
		case RedshiftTypeId::DATE:
			return "Redshift's DATE text form, YYYY-MM-DD[ BC] or -YYY-MM-DD.";
		case RedshiftTypeId::TIME:
			return "Redshift's TIME text form, HH:MM:SS[.f] where .f is 1 to 6 digits.";
		case RedshiftTypeId::TIMETZ:
			return "Redshift's TIME WITH TIME ZONE text form, HH:MM:SS[.f]{+|-}HH[:MM[:SS]] where .f is 1 to 6 "
			       "digits, HH is 00 to 15, and MM and SS are below 60.";
		case RedshiftTypeId::TIMESTAMP:
			return "Redshift's TIMESTAMP text form, YYYY-MM-DD HH:MM:SS[.f][ BC] where .f is 1 to 6 digits.";
		default:
			return "Redshift's TIMESTAMP WITH TIME ZONE text form, YYYY-MM-DD HH:MM:SS[.f]{+|-}HH[:MM[:SS]][ BC] "
			       "where .f is 1 to 6 digits, HH is 00 to 15, and MM and SS are below 60.";
		}
	};
	for (auto &c : cases) {
		INFO(c.text);
		auto thrown = RefuseOne(Column(c.type, 0, 0, "t"), Text(c.text));
		RequireValueRefused(thrown, "column 1 (\"t\")");
		REQUIRE_THAT(thrown.message, Catch::Contains("\"" + string(c.text) + "\" is not in " + form(c.type)));
	}

	// Refused before the grammar check runs: DuckDB v2.0.1 refuses one offset minute digit, a
	// trailing ':' after an offset, and an astronomical year with a ` BC` as well.
	for (auto text : {"12:00:00+05:3", "12:00:00+05:", "12:00:00+05:30:"}) {
		INFO(text);
		RequireValueRefused(RefuseOne(Column(RedshiftTypeId::TIMETZ, 0, 0, "t"), Text(text)), "column 1 (\"t\")");
	}
	for (auto text : {"2024-01-01 12:00:00+05:", "2024-01-01 12:00:00+05:30:"}) {
		INFO(text);
		RequireValueRefused(RefuseOne(Column(RedshiftTypeId::TIMESTAMPTZ, 0, 0, "t"), Text(text)), "column 1 (\"t\")");
	}
	RequireValueRefused(RefuseOne(Column(RedshiftTypeId::DATE, 0, 0, "t"), Text("-4712-01-01 BC")), "column 1 (\"t\")");
	// DuckDB's TIMETZ parser refuses an offset past +-15:59:59 itself.
	RequireValueRefused(RefuseOne(Column(RedshiftTypeId::TIMETZ, 0, 0, "t"), Text("12:00:00+16:00")),
	                    "column 1 (\"t\")");

	// Exactly 6 fraction digits, offsets with zero minutes, and the largest offset parts are exact.
	REQUIRE(timestamp_t(
	            ConvertOne<timestamp_tz_t>(Column(RedshiftTypeId::TIMESTAMPTZ), Text("2024-01-01 12:00:00+15:59:59"))
	                .value) == Ts(2023, 12, 31, 20, 0, 1));
	REQUIRE(timestamp_t(
	            ConvertOne<timestamp_tz_t>(Column(RedshiftTypeId::TIMESTAMPTZ), Text("2024-01-01 12:00:00-15:59:59"))
	                .value) == Ts(2024, 1, 2, 3, 59, 59));
	REQUIRE(ConvertOne<dtime_t>(Column(RedshiftTypeId::TIME), Text("12:34:56.123456")) ==
	        Time::FromTime(12, 34, 56, 123456));
	REQUIRE(ConvertOne<dtime_tz_t>(Column(RedshiftTypeId::TIMETZ), Text("12:34:56+05:00")).offset() == 5 * 3600);
	auto latest = ConvertOne<dtime_tz_t>(Column(RedshiftTypeId::TIMETZ), Text("23:59:59.999999+15:59:59"));
	REQUIRE(latest.time() == Time::FromTime(23, 59, 59, 999999));
	REQUIRE(latest.offset() == 57599);
	auto earliest = ConvertOne<dtime_tz_t>(Column(RedshiftTypeId::TIMETZ), Text("00:00:00-15:59:59"));
	REQUIRE(earliest.time() == Time::FromTime(0, 0, 0));
	REQUIRE(earliest.offset() == -57599);
	REQUIRE(timestamp_t(ConvertOne<timestamp_tz_t>(Column(RedshiftTypeId::TIMESTAMPTZ),
	                                               Text("2024-01-01 12:00:00.123456+05:59:59"))
	                        .value) == Ts(2024, 1, 1, 6, 0, 1, 123456));
}

TEST_CASE("DuckDB v2.0.1 still reads a 7th fraction digit and offset minutes past 59", "[conversion][values]") {
	// If a DuckDB upgrade makes its parsers refuse these, this fails. The writer's grammar check
	// stays either way: it also refuses forms DuckDB accepts, such as a 'T' separator, extra
	// spaces, "+0099", 'Z' and a time without seconds.
	string error;
	CastParameters strict(true, &error);
	dtime_t time;
	REQUIRE(TryCastErrorMessage::Operation<string_t, dtime_t>(string_t("12:34:56.1234569"), time, strict));
	REQUIRE(time == Time::FromTime(12, 34, 56, 123456));

	idx_t pos = 0;
	dtime_tz_t time_tz;
	bool has_offset = false;
	REQUIRE(Time::TryConvertTimeTZ("12:00:00+00:99", 14, pos, time_tz, has_offset, true));
	REQUIRE(time_tz.offset() == 99 * 60);
}

TEST_CASE("temporal text with trailing or malformed content is refused", "[conversion][values]") {
	struct Case {
		RedshiftTypeId type;
		const char *text;
	};
	const Case cases[] = {
	    {RedshiftTypeId::DATE, "2024-01-01 junk"},
	    {RedshiftTypeId::DATE, "2024-01-01 12:00:00"},
	    {RedshiftTypeId::DATE, "2024-13-01"},
	    {RedshiftTypeId::DATE, "0000-01-01 BC"},
	    {RedshiftTypeId::TIME, "12:34:56 junk"},
	    {RedshiftTypeId::TIME, "25:00:00"},
	    {RedshiftTypeId::TIME, "24:00:00.000001"},
	    {RedshiftTypeId::TIMETZ, "12:34:56+05 junk"},
	    {RedshiftTypeId::TIMESTAMP, "2024-01-01 12:00:00 junk"},
	    {RedshiftTypeId::TIMESTAMP, "2024-01-01 12:00:00 BCE"},
	    {RedshiftTypeId::TIMESTAMPTZ, "2024-01-01 12:00:00+00 junk"},
	    {RedshiftTypeId::TIMESTAMPTZ, "2024-01-01 12:00:00+00 bc"},
	    // Beyond DuckDB's TIMESTAMP range, though Redshift allows it.
	    {RedshiftTypeId::TIMESTAMP, "294276-12-31 00:00:00"},
	};
	for (auto &c : cases) {
		INFO(c.text);
		RequireValueRefused(RefuseOne(Column(c.type, 0, 0, "t"), Text(c.text)), "column 1 (\"t\")");
	}
}

TEST_CASE("DuckDB v2.0.1 still needs the BC rewrite", "[conversion][values]") {
	// If a DuckDB upgrade starts reading a trailing " BC", this fails and the rewrite can go.
	// Without it, DuckDB's DATE parse refuses the transport's BC date, strict or not, and its
	// lenient TIMESTAMP parse reads a BC timestamp as the AD one.
	string error;
	CastParameters lenient(false, &error);
	CastParameters strict(true, &error);
	date_t date;
	REQUIRE_FALSE(TryCastErrorMessage::Operation<string_t, date_t>(string_t("4713-01-01 BC"), date, lenient));
	REQUIRE_FALSE(TryCastErrorMessage::Operation<string_t, date_t>(string_t("4713-01-01 BC"), date, strict));
	REQUIRE(TryCastErrorMessage::Operation<string_t, date_t>(string_t("4713-01-01 (BC)"), date, strict));
	REQUIRE(date == Date::FromDate(-4712, 1, 1));

	timestamp_t timestamp;
	REQUIRE(
	    TryCastErrorMessage::Operation<string_t, timestamp_t>(string_t("0044-03-15 12:00:00 BC"), timestamp, lenient));
	REQUIRE(timestamp == Ts(44, 3, 15, 12, 0, 0));

	// The parser the writer calls for both timestamp types reads a trailing " BC" as a zone name on
	// a TIMESTAMP and refuses it after an offset; the rewritten form parses as the BC instant.
	// Literals, so the zone the parser reports points into text that outlives it.
	auto parse = [](const char *text, timestamp_t &result, bool &has_offset, string_t &zone) {
		return Timestamp::TryConvertTimestampTZ(text, strlen(text), result, true, has_offset, zone);
	};
	bool has_offset = false;
	string_t zone(nullptr, 0);
	REQUIRE(parse("0044-03-15 12:00:00 BC", timestamp, has_offset, zone) == TimestampCastResult::SUCCESS);
	REQUIRE(zone.GetString() == "BC");
	REQUIRE(parse("0044-03-15 12:00:00+00 BC", timestamp, has_offset, zone) != TimestampCastResult::SUCCESS);
	zone = string_t(nullptr, 0);
	REQUIRE(parse("0044-03-15 (BC) 12:00:00+00", timestamp, has_offset, zone) == TimestampCastResult::SUCCESS);
	REQUIRE(has_offset);
	REQUIRE(zone.GetSize() == 0);
	REQUIRE(timestamp == Ts(-43, 3, 15, 12, 0, 0));
}

//===--------------------------------------------------------------------===//
// Text captured from Redshift
//
// The strings below are text Redshift returned for each type, plus values derived from the rules
// that text follows. "Stored" strings were read back from a table, which the compute nodes print;
// "constant" strings came from a SELECT that reads no table, which the leader node prints. The two
// print dates, times and TIMETZ differently, so both are kept. Each test asserts the value the text
// denotes: where the server's text already lost something (a truncated offset), that is noted, but
// the conversion is exact.
//===--------------------------------------------------------------------===//

TEST_CASE("captured DATE text, astronomical and BC, converts exactly", "[conversion][values]") {
	struct Case {
		const char *text;
		date_t expected;
	};
	const Case cases[] = {
	    // Stored: the compute nodes print years before 1 astronomically, with year 0 as 1 BC and
	    // sometimes only 3 digits after the sign. DuckDB's parser reads that form as is.
	    {"-4712-01-01", Date::FromDate(-4712, 1, 1)}, // 4713 BC
	    {"-4713-11-24", Date::FromDate(-4713, 11, 24)},
	    {"-043-03-15", Date::FromDate(-43, 3, 15)}, // 44 BC, not 43 BC
	    {"0000-12-31", Date::FromDate(0, 12, 31)},  // 1 BC
	    {"0001-01-01", Date::FromDate(1, 1, 1)},
	    {"9999-12-31", Date::FromDate(9999, 12, 31)},
	    {"10000-01-01", Date::FromDate(10000, 1, 1)},
	    {"294276-12-31", Date::FromDate(294276, 12, 31)},
	    {"5874897-12-31", Date::FromDate(5874897, 12, 31)},
	    // Constant: the leader node prints BC as a suffix.
	    {"4713-01-01 BC", Date::FromDate(-4712, 1, 1)},
	};
	for (auto &c : cases) {
		INFO(c.text);
		REQUIRE(ConvertOne<date_t>(Column(RedshiftTypeId::DATE), Text(c.text)) == c.expected);
	}
	// The two forms of 4713 BC are the same date.
	REQUIRE(ConvertOne<date_t>(Column(RedshiftTypeId::DATE), Text("-4712-01-01")) ==
	        ConvertOne<date_t>(Column(RedshiftTypeId::DATE), Text("4713-01-01 BC")));
}

TEST_CASE("captured TIME text, with trailing zeros kept, converts exactly", "[conversion][values]") {
	struct Case {
		const char *text;
		dtime_t expected;
	};
	const Case cases[] = {
	    {"12:34:56.50", Time::FromTime(12, 34, 56, 500000)}, // stored: the trailing zero is kept
	    {"23:59:59.999999", Time::FromTime(23, 59, 59, 999999)},
	    {"12:00:00", Time::FromTime(12, 0, 0)},
	    {"00:00:00", Time::FromTime(0, 0, 0)},                 // stored as 24:00:00; the server wraps it
	    {"12:00:00.123457", Time::FromTime(12, 0, 0, 123457)}, // constant: 9 input digits, rounded by the server
	};
	for (auto &c : cases) {
		INFO(c.text);
		REQUIRE(ConvertOne<dtime_t>(Column(RedshiftTypeId::TIME), Text(c.text)) == c.expected);
	}
}

TEST_CASE("captured TIMETZ text keeps the offset it was sent with", "[conversion][values]") {
	struct Case {
		const char *text;
		dtime_t time;
		int32_t offset_seconds;
	};
	const Case cases[] = {
	    // Stored, session UTC: the server prints a stored value in the session's zone.
	    {"07:04:56.50+00", Time::FromTime(7, 4, 56, 500000), 0},     // stored as 12:34:56.5+05:30
	    {"09:00:59.999999+00", Time::FromTime(9, 0, 59, 999999), 0}, // stored with +14:59
	    {"15:30:15+00", Time::FromTime(15, 30, 15), 0},              // stored with -03:30:15
	    {"00:00:00+00", Time::FromTime(0, 0, 0), 0},
	    {"14:59:00+00", Time::FromTime(14, 59, 0), 0},
	    {"21:00:00+00", Time::FromTime(21, 0, 0), 0},
	    {"07:04:56+00", Time::FromTime(7, 4, 56), 0},
	    // Stored, session America/New_York: a fixed -05, even on a daylight-time date.
	    {"02:04:56-05", Time::FromTime(2, 4, 56), -5 * 3600},
	    {"09:59:00-05", Time::FromTime(9, 59, 0), -5 * 3600},
	    // Constants keep their own offset.
	    {"12:00:00+05:30", Time::FromTime(12, 0, 0), 5 * 3600 + 30 * 60},
	    // The input was -03:30:15; the server's text drops the seconds, so this is -03:30 exactly.
	    {"12:00:00-03:30", Time::FromTime(12, 0, 0), -(3 * 3600 + 30 * 60)},
	};
	for (auto &c : cases) {
		INFO(c.text);
		auto stored = ConvertOne<dtime_tz_t>(Column(RedshiftTypeId::TIMETZ), Text(c.text));
		REQUIRE(stored.time() == c.time);
		REQUIRE(stored.offset() == c.offset_seconds);
	}
}

TEST_CASE("captured TIMESTAMP text converts exactly, and past DuckDB's range is refused", "[conversion][values]") {
	struct Case {
		const char *text;
		timestamp_t expected;
	};
	const Case cases[] = {
	    // Stored.
	    {"2024-02-29 12:34:56.123456", Ts(2024, 2, 29, 12, 34, 56, 123456)},
	    {"2024-01-01 00:00:00", Ts(2024, 1, 1, 0, 0, 0)},
	    {"1883-11-18 12:00:00", Ts(1883, 11, 18, 12, 0, 0)},
	    {"1900-01-01 00:00:00", Ts(1900, 1, 1, 0, 0, 0)},
	    {"0001-01-01 00:00:00", Ts(1, 1, 1, 0, 0, 0)},
	    {"10000-01-01 00:00:00", Ts(10000, 1, 1, 0, 0, 0)},
	    {"2024-06-01 12:00:00.50", Ts(2024, 6, 1, 12, 0, 0, 500000)}, // the trailing zero is kept
	    // Stored TIMESTAMP keeps the BC suffix; it is not astronomical like DATE.
	    {"4713-01-01 00:00:00 BC", Ts(-4712, 1, 1, 0, 0, 0)},
	    {"0044-03-15 12:00:00 BC", Ts(-43, 3, 15, 12, 0, 0)},
	    {"0044-03-15 00:00:00 BC", Ts(-43, 3, 15, 0, 0, 0)},
	    // Constant.
	    {"2024-01-01 00:00:00.123457", Ts(2024, 1, 1, 0, 0, 0, 123457)},
	};
	for (auto &c : cases) {
		INFO(c.text);
		REQUIRE(ConvertOne<timestamp_t>(Column(RedshiftTypeId::TIMESTAMP), Text(c.text)) == c.expected);
	}

	// Redshift's range runs past DuckDB's 294247-01-10 04:00:54.775806, and it stores 'infinity' as
	// its last instant, never the word. Both are refused rather than clamped or made infinite.
	for (auto text : {"294276-12-31 23:59:59.999999", "294277-01-09 04:00:54.775807"}) {
		INFO(text);
		auto thrown = RefuseOne(Column(RedshiftTypeId::TIMESTAMP, 0, 0, "t"), Text(text));
		RequireValueRefused(thrown, "column 1 (\"t\")");
		REQUIRE_THAT(thrown.message, Catch::Contains("out of range"));
	}
}

TEST_CASE("captured TIMESTAMPTZ text converts to the instant it denotes", "[conversion][values]") {
	struct Case {
		const char *text;
		timestamp_t utc;
	};
	const Case cases[] = {
	    // Stored, session UTC.
	    {"2024-02-29 07:04:56.123456+00", Ts(2024, 2, 29, 7, 4, 56, 123456)},
	    {"2024-01-15 12:00:00+00", Ts(2024, 1, 15, 12, 0, 0)},
	    {"2024-06-01 12:00:00+00", Ts(2024, 6, 1, 12, 0, 0)},
	    {"1883-11-18 12:00:00+00", Ts(1883, 11, 18, 12, 0, 0)},
	    {"1900-01-01 00:00:00+00", Ts(1900, 1, 1, 0, 0, 0)},
	    {"0001-01-01 00:00:00+00", Ts(1, 1, 1, 0, 0, 0)},
	    {"10000-01-01 00:00:00+00", Ts(10000, 1, 1, 0, 0, 0)},
	    {"4713-01-01 00:00:00+00 BC", Ts(-4712, 1, 1, 0, 0, 0)},
	    {"0044-03-15 12:00:00+00 BC", Ts(-43, 3, 15, 12, 0, 0)},
	    // Stored, session America/New_York.
	    {"2024-02-29 02:04:56.123456-05", Ts(2024, 2, 29, 7, 4, 56, 123456)},
	    {"2024-01-15 07:00:00-05", Ts(2024, 1, 15, 12, 0, 0)},
	    {"2024-06-01 08:00:00-04", Ts(2024, 6, 1, 12, 0, 0)},
	    {"9999-12-31 19:00:00-05", Ts(10000, 1, 1, 0, 0, 0)},
	    {"1899-12-31 19:00:00-05", Ts(1900, 1, 1, 0, 0, 0)},
	    // Stored, session Asia/Kolkata.
	    {"2024-02-29 12:34:56.123456+05:30", Ts(2024, 2, 29, 7, 4, 56, 123456)},
	    {"2024-01-15 17:30:00+05:30", Ts(2024, 1, 15, 12, 0, 0)},
	    // Outside UTC, the server prints a local mean time offset truncated to whole minutes, so
	    // these denote an instant seconds away from the stored one: New York's -04:56:02 prints as
	    // -04:56 (2 seconds early) and Kolkata's +05:21:10 as +05:21 (10 seconds late). The client pins
	    // the session to UTC, where they do not arrive. The conversion of
	    // the text is still exact.
	    {"1883-11-18 07:03:58-04:56", Ts(1883, 11, 18, 11, 59, 58)},     // stored 12:00:00+00
	    {"0001-12-31 19:03:58-04:56 BC", Ts(0, 12, 31, 23, 59, 58)},     // stored 0001-01-01 00:00:00+00
	    {"4714-12-31 19:03:58-04:56 BC", Ts(-4713, 12, 31, 23, 59, 58)}, // stored 4713-01-01 BC 00:00:00+00
	    {"0044-03-15 07:03:58-04:56 BC", Ts(-43, 3, 15, 11, 59, 58)},    // stored 0044-03-15 BC 12:00:00+00
	    {"0044-03-15 00:00:00-04:56 BC", Ts(-43, 3, 15, 4, 56, 0)},
	    {"0001-01-01 00:00:00-04:56", Ts(1, 1, 1, 4, 56, 0)},
	    {"1900-01-01 05:21:10+05:21", Ts(1900, 1, 1, 0, 0, 10)}, // stored 1900-01-01 00:00:00+00
	};
	for (auto &c : cases) {
		INFO(c.text);
		auto stored = ConvertOne<timestamp_tz_t>(Column(RedshiftTypeId::TIMESTAMPTZ), Text(c.text));
		REQUIRE(timestamp_t(stored.value) == c.utc);
	}

	// Past DuckDB's range, in UTC and in New York, where the local time is checked before the offset.
	for (auto text : {"294276-12-31 23:59:59.999999+00", "294277-01-09 04:00:54.775807+00",
	                  "294276-12-31 18:59:59.999999-05", "294277-01-08 23:00:54.775807-05"}) {
		INFO(text);
		auto thrown = RefuseOne(Column(RedshiftTypeId::TIMESTAMPTZ, 0, 0, "t"), Text(text));
		RequireValueRefused(thrown, "column 1 (\"t\")");
		REQUIRE_THAT(thrown.message, Catch::Contains("out of range"));
	}
}

TEST_CASE("captured DECIMAL text converts to the exact unscaled value", "[conversion][values]") {
	// The server rounds half away from zero on insert and cast, and truncates avg and division
	// to the declared scale, before it prints; each string here is already in that state.
	auto max38 = Hugeint::POWERS_OF_TEN[38] - hugeint_t(1);
	auto d38_10 = Column(RedshiftTypeId::DECIMAL, 38, 10);
	REQUIRE(ConvertOne<hugeint_t>(d38_10, Text("9999999999999999999999999999.9999999999")) == max38);
	REQUIRE(ConvertOne<hugeint_t>(d38_10, Text("-9999999999999999999999999999.9999999999")) == -max38);
	REQUIRE(ConvertOne<hugeint_t>(d38_10, Text("12.3450000000")) == hugeint_t(123450000000LL));
	REQUIRE(ConvertOne<hugeint_t>(d38_10, Text("-12.3450000000")) == hugeint_t(-123450000000LL));
	REQUIRE(ConvertOne<hugeint_t>(d38_10, Text("12.3550000000")) == hugeint_t(123550000000LL));
	REQUIRE(ConvertOne<hugeint_t>(d38_10, Text("-0.0010000000")) == hugeint_t(-10000000));
	REQUIRE(ConvertOne<hugeint_t>(d38_10, Text("0.0000000000")) == hugeint_t(0));

	struct Case {
		const char *text;
		int64_t unscaled;
	};
	auto check = [](int32_t precision, int32_t scale, std::initializer_list<Case> cases) {
		auto column = Column(RedshiftTypeId::DECIMAL, precision, scale);
		for (auto &c : cases) {
			INFO("DECIMAL(" << precision << "," << scale << ") " << c.text);
			REQUIRE(ConvertOne<int64_t>(column, Text(c.text)) == c.unscaled);
		}
	};
	check(10, 2,
	      {{"99999999.99", 9999999999LL},
	       {"-99999999.99", -9999999999LL},
	       {"12.35", 1235},
	       {"-12.35", -1235},
	       {"12.36", 1236},
	       {"0.00", 0}, // stored from -0.001, which rounds to zero and prints with no sign
	       {"100.00", 10000},
	       {"13.85", 1385}}); // an avg, truncated to the column's scale
	check(12, 4,
	      {{"4.1166", 41166},
	       {"-4.1166", -41166}, // a division, truncated
	       {"33333333.3300", 333333333300LL},
	       {"-33333333.3300", -333333333300LL},
	       {"4.1200", 41200},
	       {"0.0000", 0},
	       {"33.3333", 333333}});
	check(18, 0,
	      {{"999999999999999999", 999999999999999999LL},
	       {"-999999999999999999", -999999999999999999LL},
	       {"12", 12},
	       {"-13", -13},
	       {"13", 13},
	       {"0", 0}});
	check(16, 16, {{"0.3333333333333333", 3333333333333333LL}});

	REQUIRE(ConvertOne<int32_t>(Column(RedshiftTypeId::DECIMAL, 5, 3), Text("12.345")) == 12345);
}

TEST_CASE("captured CHAR text lands without its trailing blanks", "[conversion][values]") {
	// CHAR(5) arrives padded to its width; Redshift ignores the trailing blanks, so they are removed.
	const std::pair<string, string> cases[] = {
	    {"a    ", "a"},
	    {"     ", ""},
	    {"ab   ", "ab"},
	    {"abcde", "abcde"},
	    {"", ""},
	    // Only trailing 0x20 is removed: leading and inner blanks stay, and so does other whitespace.
	    {"  a b ", "  a b"},
	    {"a\t   ", "a\t"},
	    {"a \n", "a \n"},
	    {"a\xC2\xA0 ", "a\xC2\xA0"},
	    {"\xC3\xA9   ", "\xC3\xA9"},
	};
	for (auto &c : cases) {
		INFO(c.first);
		REQUIRE(ConvertOneString(Column(RedshiftTypeId::CHAR), Text(c.first)) == c.second);
	}
}

TEST_CASE("a long CHAR value is copied into the chunk without its trailing blanks", "[conversion][values]") {
	// Past the twelve-byte inline limit, so the value lives in the chunk's string heap, and the source
	// text is destroyed before the cell is read.
	const string body(1000, 'x');
	Written written({Column(RedshiftTypeId::CHAR)});
	{
		vector<vector<RedshiftValue>> rows {{Text(body + "   ")}};
		written.Run(rows);
	}
	REQUIRE(written.Get<string_t>(0, 0).GetString() == body);
}

TEST_CASE("captured VARCHAR and SUPER text is copied unchanged", "[conversion][values]") {
	const string texts[] = {
	    // The same padded text in a VARCHAR keeps its trailing blanks: VARCHAR is never trimmed.
	    "a    ",
	    "     ",
	    "ab   ",
	    "abcde",
	    "trailing  ",
	    "",
	    "emoji \xF0\x9F\x98\x80 \xF0\x9D\x84\x9E \xC3\xA9 \xE6\x97\xA5\xE6\x9C\xAC", // 25 bytes
	    "\xF0\x9F\x98\x80",
	    // SUPER arrives as JSON text; a string scalar keeps its quotes.
	    "{\"a\":[1,2],\"k\":\"\xF0\x9F\x98\x80\"}",
	    "\"str\"",
	    "{\"a\":1}",
	};
	for (auto &text : texts) {
		INFO(text);
		REQUIRE(ConvertOneString(Column(RedshiftTypeId::VARCHAR), Text(text)) == text);
	}
}

//===--------------------------------------------------------------------===//
// Chunks
//===--------------------------------------------------------------------===//

TEST_CASE("a mixed chunk converts every column, with NULLs in parsed and native columns", "[conversion][values]") {
	Written written({Column(RedshiftTypeId::INTEGER, 0, 0, "id"), Column(RedshiftTypeId::DECIMAL, 10, 2, "price"),
	                 Column(RedshiftTypeId::VARCHAR, 0, 0, "name"), Column(RedshiftTypeId::DATE, 0, 0, "day"),
	                 Column(RedshiftTypeId::TIMESTAMPTZ, 0, 0, "at")});
	written.Run({
	    {Int(1), Text("12.50"), Text("a"), Text("2024-01-01"), Text("2024-01-01 00:00:00+00")},
	    {Null(), Null(), Null(), Null(), Null()},
	    {Int(3), Text("-0.01"), Text(""), Text("0044-03-15 BC"), Text("0044-03-15 12:00:00+00 BC")},
	});
	REQUIRE(written.chunk.size() == 3);
	REQUIRE(written.Get<int32_t>(0, 0) == 1);
	REQUIRE(written.Get<int64_t>(1, 0) == 1250);
	REQUIRE(written.Get<string_t>(2, 0).GetString() == "a");
	REQUIRE(written.Get<date_t>(3, 0) == Date::FromDate(2024, 1, 1));
	for (idx_t c = 0; c < 5; c++) {
		REQUIRE(written.IsNull(c, 1));
	}
	REQUIRE(written.Get<int64_t>(1, 2) == -1);
	REQUIRE(written.Get<string_t>(2, 2).GetString().empty());
	REQUIRE(written.Get<date_t>(3, 2) == Date::FromDate(-43, 3, 15));
	REQUIRE(written.Get<timestamp_tz_t>(4, 2).value == Ts(-43, 3, 15, 12, 0, 0).value);
}

TEST_CASE("each chunk starts clean, and a full chunk converts", "[conversion][values]") {
	Written written({Column(RedshiftTypeId::VARCHAR), Column(RedshiftTypeId::BIGINT)});

	vector<vector<RedshiftValue>> full;
	for (idx_t i = 0; i < STANDARD_VECTOR_SIZE; i++) {
		full.push_back({i % 7 == 0 ? Null() : Text(std::to_string(i)), Int(static_cast<int64_t>(i))});
	}
	written.Run(full);
	REQUIRE(written.chunk.size() == STANDARD_VECTOR_SIZE);
	for (idx_t i = 0; i < STANDARD_VECTOR_SIZE; i++) {
		REQUIRE(written.IsNull(0, i) == (i % 7 == 0));
		if (i % 7 != 0) {
			REQUIRE(written.Get<string_t>(0, i).GetString() == std::to_string(i));
		}
		REQUIRE(written.Get<int64_t>(1, i) == static_cast<int64_t>(i));
	}

	// The next chunk reuses the output chunk; row 0 was NULL above and is not NULL now.
	written.Run({{Text("42"), Int(1)}});
	REQUIRE(written.chunk.size() == 1);
	REQUIRE(written.Get<string_t>(0, 0).GetString() == "42");

	// An empty chunk is legal.
	written.Run({});
	REQUIRE(written.chunk.size() == 0);
}

TEST_CASE("a refused value names its own column, not the first one", "[conversion][values]") {
	Written written({Column(RedshiftTypeId::DATE, 0, 0, "ok"), Column(RedshiftTypeId::TIME, 0, 0, "bad")});
	auto thrown = Capture([&] {
		written.Run({{Text("2024-01-01"), Text("12:00:00")}, {Text("2024-01-02"), Text("x")}});
	});
	RequireValueRefused(thrown, "column 2 (\"bad\")");
}

TEST_CASE("a shape with an unmappable column cannot build a cell writer", "[conversion][values]") {
	auto thrown = Capture([] { RedshiftCellWriter writer({Column(RedshiftTypeId::UNKNOWN, 0, 0, "g")}); });
	REQUIRE(thrown.type == ExceptionType::BINDER);
}
