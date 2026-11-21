// The fetch, bound over a row array or through SQLGetData: each column read in the C type its
// carrier calls for, text checked once it is whole, numbers parsed from text with an overflow
// refused, and batches bounded by rows and bytes.

// catch.hpp comes first, as in this directory's other tests.
#include "catch.hpp"

#include "duckdb/common/numeric_utils.hpp"
#include "duckdb/common/string_util.hpp"
#include "extension_database.hpp"
#include "fake_driver_fixture.hpp"
#include "odbc/bulk_fetch.hpp"
#include "thrown.hpp"

#include <cfloat>
#include <cstddef>
#include <cmath>
#include <string>
#include <vector>

using namespace duckdb;
using redshift_driver::testing::FakeColumn;
using redshift_driver::testing::FakeDiag;
using redshift_driver::testing::FakeResultSet;
using redshift_test::ATTACH_RS;
using redshift_test::DRIVER_FAILURE_TRAILER;
using redshift_test::ExtensionDatabase;
using redshift_test::FakeDriverFixture;
using redshift_test::LongVarcharColumn;
using redshift_test::OneColumn;
using redshift_test::ReadBatches;
using redshift_test::Thrown;
using redshift_test::VarcharColumn;

namespace {

const char *const QUERY = "SELECT c FROM t";
//! What a batch holding text that is not valid UTF-8 throws.
const char *const INVALID_UTF8 =
    "Redshift returned text in column 1 that is not valid UTF-8, so the batch was not returned; check the column's "
    "data";
//! What NextBatch says once the result's connection has closed.
const char *const CLOSED_RESULT =
    "this Redshift result's connection has closed; run the statement again on a new connection";

//! A column named `c` as the driver describes one of `type_name`.
FakeColumn Column(const std::string &type_name, SQLSMALLINT sql_type) {
	FakeColumn column;
	column.name = "c";
	column.type_name = type_name;
	column.sql_type = sql_type;
	return column;
}

//! A path text is fetched on: its column, and the SQLBindCol calls that show the path was taken.
struct TextPath {
	const char *what;
	FakeColumn column;
	size_t binds;
};

//! Both paths, since each reads text into its own buffers before the one check of a whole value.
std::vector<TextPath> TextPaths() {
	return {{"read with SQLGetData", LongVarcharColumn("c"), 0},
	        {"bound over a row array", VarcharColumn("c", 300), 1}};
}

//! The only value of a one-row result of `column` holding `cell`.
RedshiftValue OnlyValue(FakeDriverFixture &fixture, const FakeColumn &column, const std::string &cell) {
	fixture.fake.AddResult(QUERY, {OneColumn(column, {cell})});
	auto connection = fixture.OpenConnection();
	auto result = redshift_test::ExecuteStatement(*connection, QUERY, {});
	RedshiftBatch batch;
	REQUIRE(result->NextBatch(batch));
	REQUIRE(batch.rows.size() == 1);
	REQUIRE(batch.rows[0].size() == 1);
	return batch.rows[0][0];
}

//! What each SQLGetData call asked for, "<column> <C type>", in call order.
std::vector<std::string> GetDataRequests(const FakeDriverFixture &fixture) {
	return fixture.DetailsOf("SQLGetData");
}

//! What each SQLBindCol call bound, "<column> <C type> <buffer length>", in call order.
std::vector<std::string> BindRequests(const FakeDriverFixture &fixture) {
	return fixture.DetailsOf("SQLBindCol");
}

//! `bytes` bytes cycling through the values 0 to 126, so a lost or doubled byte shows.
std::string Bytes(size_t bytes) {
	std::string value;
	for (size_t i = 0; i < bytes; i++) {
		value.push_back(NumericCast<char>(i % 127));
	}
	return value;
}

//! What the first NextBatch of `cells` in `column` throws, and the rows it left in the batch.
struct FirstBatch {
	Thrown thrown;
	idx_t rows_left;
};

FirstBatch FailFirstBatch(FakeDriverFixture &fixture, const FakeColumn &column, const std::vector<std::string> &cells) {
	fixture.fake.AddResult(QUERY, {OneColumn(column, cells)});
	auto connection = fixture.OpenConnection();
	auto result = redshift_test::ExecuteStatement(*connection, QUERY, {});
	RedshiftBatch batch;
	const auto thrown = redshift_test::Capture([&] { result->NextBatch(batch); });
	return FirstBatch {thrown, batch.rows.size()};
}

} // namespace

TEST_CASE("A BOOLEAN column is bound as text, which is parsed into the BOOLEAN carrier", "[odbc]") {
	FakeDriverFixture fixture;
	SECTION("true") {
		const auto value = OnlyValue(fixture, Column("BOOL", SQL_BIT), "1");
		CHECK(value.kind == RedshiftValueKind::BOOLEAN);
		CHECK(value.boolean);
	}
	SECTION("false") {
		const auto value = OnlyValue(fixture, Column("BOOL", SQL_BIT), "0");
		CHECK(value.kind == RedshiftValueKind::BOOLEAN);
		CHECK_FALSE(value.boolean);
	}
	// SQL_C_CHAR is 1, at a boolean's 32 bytes and the NUL.
	CHECK(BindRequests(fixture) == std::vector<std::string> {"1 1 33"});
}

TEST_CASE("Each integer column is bound as text, which is parsed into the INTEGER carrier", "[odbc]") {
	FakeDriverFixture fixture;
	SECTION("SMALLINT at its minimum") {
		const auto value = OnlyValue(fixture, Column("SMALLINT", SQL_SMALLINT), "-32768");
		CHECK(value.kind == RedshiftValueKind::INTEGER);
		CHECK(value.integer == -32768);
	}
	SECTION("INTEGER at its maximum") {
		const auto value = OnlyValue(fixture, Column("INTEGER", SQL_INTEGER), "2147483647");
		CHECK(value.kind == RedshiftValueKind::INTEGER);
		CHECK(value.integer == 2147483647);
	}
	SECTION("BIGINT at its minimum") {
		const auto value = OnlyValue(fixture, Column("BIGINT", SQL_BIGINT), "-9223372036854775808");
		CHECK(value.kind == RedshiftValueKind::INTEGER);
		CHECK(value.integer == -9223372036854775807LL - 1);
	}
	// SQL_C_CHAR is 1, at an integer's 20 bytes and the NUL.
	CHECK(BindRequests(fixture) == std::vector<std::string> {"1 1 21"});
}

TEST_CASE("A DOUBLE PRECISION column is bound as text, which is parsed into the REAL carrier", "[odbc]") {
	FakeDriverFixture fixture;
	SECTION("17 significant digits") {
		const auto value = OnlyValue(fixture, Column("DOUBLE PRECISION", SQL_DOUBLE), "0.30000000000000004");
		CHECK(value.kind == RedshiftValueKind::REAL);
		CHECK(value.real == 0.30000000000000004);
	}
	SECTION("negative zero") {
		const auto value = OnlyValue(fixture, Column("DOUBLE PRECISION", SQL_DOUBLE), "-0.0");
		CHECK(value.kind == RedshiftValueKind::REAL);
		CHECK(std::signbit(value.real));
	}
	SECTION("the largest value, at 17 digits") {
		CHECK(OnlyValue(fixture, Column("DOUBLE PRECISION", SQL_DOUBLE), "1.7976931348623157e+308").real == DBL_MAX);
	}
	SECTION("a table's infinity") {
		CHECK(OnlyValue(fixture, Column("DOUBLE PRECISION", SQL_DOUBLE), "inf").real == INFINITY);
	}
	SECTION("a table's negative infinity") {
		CHECK(OnlyValue(fixture, Column("DOUBLE PRECISION", SQL_DOUBLE), "-inf").real == -INFINITY);
	}
	SECTION("::VARCHAR's infinity") {
		CHECK(OnlyValue(fixture, Column("DOUBLE PRECISION", SQL_DOUBLE), "Infinity").real == INFINITY);
	}
	SECTION("::VARCHAR's negative infinity") {
		CHECK(OnlyValue(fixture, Column("DOUBLE PRECISION", SQL_DOUBLE), "-Infinity").real == -INFINITY);
	}
	SECTION("a table's NaN with its sign bit set") {
		CHECK(std::isnan(OnlyValue(fixture, Column("DOUBLE PRECISION", SQL_DOUBLE), "-nan").real));
	}
	SECTION("::VARCHAR's NaN") {
		CHECK(std::isnan(OnlyValue(fixture, Column("DOUBLE PRECISION", SQL_DOUBLE), "NaN").real));
	}
	// SQL_C_CHAR is 1, at a float's 32 bytes and the NUL.
	CHECK(BindRequests(fixture) == std::vector<std::string> {"1 1 33"});
}

TEST_CASE("A REAL column arrives as the float its text parses to, widened to a double", "[odbc]") {
	FakeDriverFixture fixture;
	SECTION("a value no float holds exactly") {
		const auto value = OnlyValue(fixture, Column("REAL", SQL_REAL), "0.1");
		CHECK(value.kind == RedshiftValueKind::REAL);
		CHECK(value.real == 0.10000000149011612);
	}
	SECTION("a table's infinity") {
		CHECK(OnlyValue(fixture, Column("REAL", SQL_REAL), "inf").real == INFINITY);
	}
	SECTION("a table's negative infinity") {
		CHECK(OnlyValue(fixture, Column("REAL", SQL_REAL), "-inf").real == -INFINITY);
	}
	SECTION("::VARCHAR's infinity") {
		CHECK(OnlyValue(fixture, Column("REAL", SQL_REAL), "Infinity").real == INFINITY);
	}
	SECTION("::VARCHAR's negative infinity") {
		CHECK(OnlyValue(fixture, Column("REAL", SQL_REAL), "-Infinity").real == -INFINITY);
	}
	SECTION("text just past the largest float, which rounds to it") {
		CHECK(OnlyValue(fixture, Column("REAL", SQL_REAL), "3.40282347e+38").real == FLT_MAX);
	}
	SECTION("text just above a halfway point rounds up, where parsing as a double and then narrowing would not") {
		// 1 + 2^-24 is halfway between 1 and the next float. A double parse lands on it, and narrowing
		// then rounds to even, down; parsing straight to float rounds once, up.
		CHECK(OnlyValue(fixture, Column("REAL", SQL_REAL), "1.0000000596046447753906250001").real ==
		      1.00000011920928955078125);
	}
	CHECK(BindRequests(fixture) == std::vector<std::string> {"1 1 33"});
}

TEST_CASE("Finite number text that rounds past its type's range fails its batch", "[odbc]") {
	FakeDriverFixture fixture;
	FakeColumn column;
	std::string type_name;
	std::string cell;
	SECTION("DOUBLE PRECISION's largest value at 15 digits, negative") {
		column = Column("DOUBLE PRECISION", SQL_DOUBLE);
		type_name = "DOUBLE PRECISION";
		cell = "-1.79769313486232e+308";
	}
	SECTION("DOUBLE PRECISION's largest value at 15 digits, positive") {
		column = Column("DOUBLE PRECISION", SQL_DOUBLE);
		type_name = "DOUBLE PRECISION";
		cell = "1.79769313486232e+308";
	}
	SECTION("a REAL past the float range") {
		column = Column("REAL", SQL_REAL);
		type_name = "REAL";
		cell = "3.5e+38";
	}
	SECTION("a negative REAL past the float range") {
		column = Column("REAL", SQL_REAL);
		type_name = "REAL";
		cell = "-3.5e+38";
	}

	const auto batch = FailFirstBatch(fixture, column, {"1", cell});
	CHECK(batch.thrown == Thrown {ExceptionType::EXECUTOR,
	                              "Redshift returned a " + type_name +
	                                  " value in column 1 whose text rounds past the type's range, so the batch was "
	                                  "not returned; select the column as VARCHAR (::VARCHAR) to read it"});
	CHECK(batch.rows_left == 0);
}

TEST_CASE("A VARBYTE column is read as SQL_C_BINARY into the BLOB carrier", "[odbc]") {
	FakeDriverFixture fixture;
	const std::string bytes("\x00\xff\x10", 3);

	const auto value = OnlyValue(fixture, Column("VARBYTE", SQL_LONGVARBINARY), bytes);
	CHECK(value.kind == RedshiftValueKind::BLOB);
	CHECK(value.text == bytes);
	// SQL_C_BINARY is -2.
	CHECK(GetDataRequests(fixture) == std::vector<std::string> {"1 -2"});
}

TEST_CASE("A binary value longer than one read arrives whole, each part filling the buffer", "[odbc]") {
	FakeDriverFixture fixture;
	// Two whole 4096-byte parts and one byte: a binary part keeps no byte for a NUL.
	const auto bytes = Bytes(8193);

	const auto value = OnlyValue(fixture, Column("VARBYTE", SQL_LONGVARBINARY), bytes);
	CHECK(value.text == bytes);
	CHECK(GetDataRequests(fixture).size() == 3);
}

TEST_CASE("A text or binary cell the driver fails to read fails its batch, returning none of its rows", "[odbc]") {
	FakeDriverFixture fixture;
	// The second row's read fails, so a batch that kept the first row would show it.
	fixture.fake.FailCall("SQLGetData", 2, SQL_ERROR, {FakeDiag {"HY000", 0, "Error reading column data", {}}});
	const Thrown unread {
	    ExceptionType::EXECUTOR,
	    "the Redshift driver failed while returning column 1 of the current row (SQLSTATE class HY, "
	    "driver error), and its message is not shown. The statement may have run; check before running it again." +
	        DRIVER_FAILURE_TRAILER + "\nSQLSTATE: HY000"};

	SECTION("VARCHAR, read as SQL_C_CHAR") {
		const auto batch = FailFirstBatch(fixture, LongVarcharColumn("c"), {"a", "b"});
		CHECK(batch.thrown == unread);
		CHECK(batch.rows_left == 0);
	}
	SECTION("VARBYTE, read as SQL_C_BINARY") {
		const auto batch = FailFirstBatch(fixture, Column("VARBYTE", SQL_LONGVARBINARY), {"a", "b"});
		CHECK(batch.thrown == unread);
		CHECK(batch.rows_left == 0);
	}
}

TEST_CASE("A NULL cell arrives as NULL in every carrier", "[odbc]") {
	FakeDriverFixture fixture;
	FakeResultSet result;
	result.columns = {Column("BOOL", SQL_BIT), Column("INTEGER", SQL_INTEGER), Column("DOUBLE PRECISION", SQL_DOUBLE),
	                  Column("VARBYTE", SQL_LONGVARBINARY), LongVarcharColumn("c")};
	result.rows = {{"1", "1", "1", "1", "1"}};
	result.nulls = {{true, true, true, true, true}};
	fixture.fake.AddResult(QUERY, {result});
	auto connection = fixture.OpenConnection();

	const ReadBatches read(*redshift_test::ExecuteStatement(*connection, QUERY, {}));
	REQUIRE(read.rows.size() == 1);
	for (const auto &value : read.rows[0]) {
		CHECK(value.kind == RedshiftValueKind::NULL_VALUE);
	}
}

TEST_CASE("A text type's value arrives unchanged through SQL_C_CHAR", "[odbc]") {
	FakeDriverFixture fixture;
	SECTION("DECIMAL") {
		auto column = Column("NUMERIC", SQL_NUMERIC);
		column.column_size = 10;
		column.decimal_digits = 2;
		CHECK(OnlyValue(fixture, column, "-12.50").text == "-12.50");
	}
	SECTION("DECIMAL with fewer digits after the point than its scale, not padded") {
		auto column = Column("NUMERIC", SQL_NUMERIC);
		column.column_size = 37;
		column.decimal_digits = 17;
		CHECK(OnlyValue(fixture, column, "0.0000000000000002").text == "0.0000000000000002");
	}
	SECTION("DATE before year 1, astronomical") {
		CHECK(OnlyValue(fixture, Column("DATE", SQL_TYPE_DATE), "-4712-01-01").text == "-4712-01-01");
	}
	SECTION("TIME keeping its trailing zero") {
		CHECK(OnlyValue(fixture, Column("TIME", SQL_TYPE_TIME), "12:34:56.50").text == "12:34:56.50");
	}
	SECTION("TIMETZ") {
		CHECK(OnlyValue(fixture, Column("TIMETZ", SQL_TYPE_TIME), "07:04:56.50+00").text == "07:04:56.50+00");
	}
	SECTION("TIMESTAMP before year 1, at its longest") {
		CHECK(OnlyValue(fixture, Column("TIMESTAMP", SQL_TYPE_TIMESTAMP), "4713-01-01 00:00:00.123456 BC").text ==
		      "4713-01-01 00:00:00.123456 BC");
	}
	SECTION("TIMESTAMPTZ before year 1") {
		CHECK(OnlyValue(fixture, Column("TIMESTAMPTZ", SQL_TYPE_TIMESTAMP), "4713-01-01 00:00:00+00 BC").text ==
		      "4713-01-01 00:00:00+00 BC");
	}
	SECTION("CHAR keeping its blank padding") {
		auto character = Column("CHARACTER", SQL_CHAR);
		character.column_size = 5;
		CHECK(OnlyValue(fixture, character, "ab   ").text == "ab   ");
	}
	// SQL_C_CHAR is 1; each type's buffer width is pinned with the bound widths.
	REQUIRE(BindRequests(fixture).size() == 1);
	CHECK(StringUtil::StartsWith(BindRequests(fixture)[0], "1 1 "));
}

TEST_CASE("Multibyte and 4-byte UTF-8 characters arrive unchanged on either fetch path", "[odbc]") {
	const std::string text = "a\xC3\xA9\xE2\x82\xAC\xF0\x9F\x98\x80z";
	for (const auto &path : TextPaths()) {
		INFO(path.what);
		FakeDriverFixture fixture;

		const auto value = OnlyValue(fixture, path.column, text);
		CHECK(value.kind == RedshiftValueKind::TEXT);
		CHECK(value.text == text);
		CHECK(BindRequests(fixture).size() == path.binds);
	}
}

TEST_CASE("A 4-byte character split across two reads arrives whole", "[odbc]") {
	FakeDriverFixture fixture;
	// A text part holds 4095 bytes, so the first holds the 4093 letters and two of the character's
	// four bytes: on its own that part is not valid UTF-8.
	const auto text = std::string(4093, 'a') + "\xF0\x9F\x98\x80" + "z";

	CHECK(OnlyValue(fixture, LongVarcharColumn("c"), text).text == text);
	CHECK(GetDataRequests(fixture).size() == 2);
}

TEST_CASE("Text that is not valid UTF-8 fails its batch on either fetch path, returning none of its rows", "[odbc]") {
	std::string invalid;
	SECTION("a lead byte with no continuation byte") {
		invalid = "\xC3\x28";
	}
	SECTION("an overlong form of '/'") {
		invalid = "\xC0\xAF";
	}
	SECTION("a lone UTF-16 surrogate, U+D800") {
		invalid = "\xED\xA0\x80";
	}
	// What a value cut inside its last character would hold, were its length to fit.
	SECTION("a 4-byte character that ends the value a byte short") {
		invalid = "\xF0\x9F\x98";
	}

	for (const auto &path : TextPaths()) {
		INFO(path.what);
		FakeDriverFixture fixture;

		const auto batch = FailFirstBatch(fixture, path.column, {"ok", "bad " + invalid, "later"});
		CHECK(batch.thrown == Thrown {ExceptionType::EXECUTOR, INVALID_UTF8});
		CHECK(batch.rows_left == 0);
		CHECK(BindRequests(fixture).size() == path.binds);
	}
}

//! A column, a value in its type's text form, and one the form refuses.
struct TextFormCase {
	const char *what;
	FakeColumn column;
	std::string type_name;
	std::string valid;
	std::string refused;
};

//! A column `type_name` reports as NUMERIC(10,1).
FakeColumn NumericColumn() {
	auto column = Column("NUMERIC", SQL_NUMERIC);
	column.column_size = 10;
	column.decimal_digits = 1;
	return column;
}

TEST_CASE("Text outside its type's text form fails its batch, returning none of its rows", "[odbc]") {
	const auto date = Column("DATE", SQL_TYPE_DATE);
	const auto timestamp = Column("TIMESTAMP", SQL_TYPE_TIMESTAMP);
	const auto timestamptz = Column("TIMESTAMPTZ", SQL_TYPE_TIMESTAMP);
	const auto real = Column("REAL", SQL_REAL);
	const auto double_precision = Column("DOUBLE PRECISION", SQL_DOUBLE);
	const std::vector<TextFormCase> cases {
	    {"a DATE as datestyle MDY prints it", date, "DATE", "2024-03-15", "03/15/2024"},
	    {"a TIME with no seconds", Column("TIME", SQL_TYPE_TIME), "TIME", "10:30:00", "10:30"},
	    {"a TIMETZ with no offset", Column("TIMETZ", SQL_TYPE_TIME), "TIMETZ", "07:04:56+00", "07:04:56"},
	    {"a TIMESTAMP with a T", timestamp, "TIMESTAMP", "2024-03-15 10:00:00", "2024-03-15T10:00:00"},
	    {"a TIMESTAMPTZ with no offset", timestamptz, "TIMESTAMPTZ", "2024-03-15 10:00:00+00", "2024-03-15 10:00:00"},
	    {"a DECIMAL with a comma", NumericColumn(), "DECIMAL", "12.5", "12,5"},
	    {"a DOUBLE with a comma", double_precision, "DOUBLE PRECISION", "1.5", "1,5"},
	    {"a DOUBLE with a leading space", double_precision, "DOUBLE PRECISION", "1.5", " 1.5"},
	    {"a DOUBLE with a trailing space", double_precision, "DOUBLE PRECISION", "1.5", "1.5 "},
	    {"a DOUBLE with a plus sign", double_precision, "DOUBLE PRECISION", "1.5", "+1.5"},
	    {"a DOUBLE with a leading zero", double_precision, "DOUBLE PRECISION", "0.5", "01.5"},
	    {"a DOUBLE with a digit separator", double_precision, "DOUBLE PRECISION", "1000", "1_000"},
	    {"an infinity in upper case", double_precision, "DOUBLE PRECISION", "inf", "INF"},
	    {"a NaN with a payload", double_precision, "DOUBLE PRECISION", "NaN", "nan(x)"},
	    {"an exponent with no sign", double_precision, "DOUBLE PRECISION", "1e+20", "1e20"},
	    {"a REAL with a plus sign", real, "REAL", "1.5", "+1.5"},
	};
	for (const auto &form : cases) {
		INFO(form.what);
		FakeDriverFixture fixture;
		const auto batch = FailFirstBatch(fixture, form.column, {form.valid, form.refused});
		CHECK(batch.thrown == Thrown {ExceptionType::EXECUTOR,
		                              "Redshift returned a value in column 1 that is not in the " + form.type_name +
		                                  " text form, so the batch was not returned; if the session's datestyle "
		                                  "was changed, run the statement on a new connection, otherwise report it"});
		CHECK(batch.rows_left == 0);
	}
}

TEST_CASE("A failed batch after a full one returns no rows in the reused batch", "[odbc]") {
	FakeDriverFixture fixture;
	std::vector<std::string> cells(2048, "ok");
	cells.push_back("bad \xC3\x28");
	fixture.fake.AddResult(QUERY, {OneColumn(LongVarcharColumn("c"), cells)});
	auto connection = fixture.OpenConnection();
	auto result = redshift_test::ExecuteStatement(*connection, QUERY, {});
	RedshiftBatch batch;
	REQUIRE(result->NextBatch(batch));
	REQUIRE(batch.rows.size() == 2048);

	CHECK(redshift_test::Capture([&] { result->NextBatch(batch); }) == Thrown {ExceptionType::EXECUTOR, INVALID_UTF8});
	CHECK(batch.rows.empty());
}

TEST_CASE("A fetch that succeeds with information logs each record redacted and returns its rows", "[odbc]") {
	FakeDriverFixture fixture;
	fixture.fake.AddResult("SELECT $1", {OneColumn(VarcharColumn("a", 10), {"x"})}, 1);
	auto connection = fixture.OpenConnection();
	auto result = redshift_test::ExecuteStatement(*connection, "SELECT $1", {"bound-value-1"});
	// A class 22 record's text is shown, so its line shows whether the value was
	// redacted; a class 01 notice's shows only its SQLSTATE.
	fixture.fake.WarnCall("SQLFetch", NumericCast<int>(fixture.CallsTo("SQLFetch")) + 1,
	                      {{"01000", 0, "NOTICE:  fetched", {}}, {"22001", 0, "ERROR:  bound-value-1 too long", {}}});
	const auto logged = fixture.log_sink->Lines().size();

	CHECK(ReadBatches(*result).rows.size() == 1);
	const auto lines = fixture.log_sink->Lines();
	CHECK(std::vector<std::string>(lines.begin() + NumericCast<std::ptrdiff_t>(logged), lines.end()) ==
	      std::vector<std::string> {"Redshift driver diagnostic, SQLSTATE 01000",
	                                "Redshift driver diagnostic, SQLSTATE 22001: ERROR:  <param 1> too long"});
}

TEST_CASE("A row the driver fails to convert fails its batch, returning none of its rows", "[odbc]") {
	FakeDriverFixture fixture;
	SetNumberBindingForTesting(NumberBinding::NATIVE);
	// The fake refuses "x" in an INTEGER column, a departure it names: the driver parses it as 0.
	const auto batch = FailFirstBatch(fixture, Column("INTEGER", SQL_INTEGER), {"1", "x"});
	CHECK(batch.thrown ==
	      Thrown {ExceptionType::EXECUTOR,
	              "Redshift could not return the rest of the result: Invalid numeric format\nSQLSTATE: 22018"});
	CHECK(batch.rows_left == 0);
}

TEST_CASE("After a failed batch every later NextBatch fails too, making no driver call", "[odbc]") {
	FakeDriverFixture fixture;
	fixture.fake.AddResult(QUERY, {OneColumn(LongVarcharColumn("c"), {"bad \xC3\x28", "later"})});
	auto connection = fixture.OpenConnection();
	auto result = redshift_test::ExecuteStatement(*connection, QUERY, {});
	RedshiftBatch batch;
	REQUIRE(redshift_test::Capture([&] { result->NextBatch(batch); }) ==
	        Thrown {ExceptionType::EXECUTOR, INVALID_UTF8});
	const auto calls = fixture.fake.Calls().size();

	CHECK(redshift_test::Capture([&] { result->NextBatch(batch); }) ==
	      Thrown {ExceptionType::EXECUTOR, "an earlier batch of this Redshift result failed, and the rest of its rows "
	                                       "are not returned without it; run the statement again"});
	CHECK(batch.rows.empty());
	CHECK(fixture.fake.Calls().size() == calls);
}

TEST_CASE("A failed result gives IOException from NextBatch once its connection is gone", "[odbc]") {
	FakeDriverFixture fixture;
	fixture.fake.AddResult(QUERY, {OneColumn(LongVarcharColumn("c"), {"bad \xC3\x28", "later"})});
	auto connection = fixture.OpenConnection();
	auto result = redshift_test::ExecuteStatement(*connection, QUERY, {});
	RedshiftBatch batch;
	REQUIRE(redshift_test::Capture([&] { result->NextBatch(batch); }) ==
	        Thrown {ExceptionType::EXECUTOR, INVALID_UTF8});
	connection.reset();
	const auto calls = fixture.fake.Calls().size();

	CHECK(redshift_test::Capture([&] { result->NextBatch(batch); }) == Thrown {ExceptionType::IO, CLOSED_RESULT});
	CHECK(fixture.fake.Calls().size() == calls);
}

// The refusal is a statement failure, so redshift_query keeps the connection,
// which it discards only for a lost one.
TEST_CASE("redshift_query refuses text that is not valid UTF-8, then runs the next statement on its connection",
          "[odbc]") {
	FakeDriverFixture fixture;
	fixture.fake.AddResult(QUERY, {OneColumn(VarcharColumn("c", 300), {"ok", "bad \xC3\x28"})});
	fixture.fake.AddResult("SELECT c FROM u", {OneColumn(VarcharColumn("c", 300), {"h\xC3\xA9llo"})});
	ExtensionDatabase database;
	database.Run(ATTACH_RS);

	const auto refused = database.con.Query(std::string("SELECT * FROM redshift_query('rs', '") + QUERY + "')");
	REQUIRE(refused->HasError());
	const auto &error = refused->GetErrorObject();
	CHECK((Thrown {error.Type(), error.RawMessage()}) == Thrown {ExceptionType::EXECUTOR, INVALID_UTF8});
	auto next = database.Run("SELECT * FROM redshift_query('rs', 'SELECT c FROM u')");
	auto chunk = next->Fetch();
	REQUIRE(chunk);
	REQUIRE(chunk->size() == 1);
	CHECK(chunk->GetValue(0, 0).ToString() == "h\xC3\xA9llo");
	CHECK(fixture.CallsTo("SQLDriverConnect") == 1);
}

TEST_CASE("A row of mixed types reads each cell in its own column's C type and form", "[odbc]") {
	FakeDriverFixture fixture;
	FakeResultSet result;
	// The VARCHAR holds text a DATE refuses, so it passes only if checked against its own column.
	result.columns = {Column("DATE", SQL_TYPE_DATE),        Column("BOOL", SQL_BIT),
	                  Column("INTEGER", SQL_INTEGER),       Column("DOUBLE PRECISION", SQL_DOUBLE),
	                  Column("VARBYTE", SQL_LONGVARBINARY), LongVarcharColumn("c")};
	result.rows = {{"2024-03-15", "1", "-7", "2.5", std::string("\x00\xff", 2), "03/15/2024"}};
	fixture.fake.AddResult(QUERY, {result});
	auto connection = fixture.OpenConnection();

	const ReadBatches read(*redshift_test::ExecuteStatement(*connection, QUERY, {}));
	REQUIRE(read.rows.size() == 1);
	const auto &row = read.rows[0];
	REQUIRE(row.size() == 6);
	CHECK(row[0].kind == RedshiftValueKind::TEXT);
	CHECK(row[0].text == "2024-03-15");
	CHECK(row[1].kind == RedshiftValueKind::BOOLEAN);
	CHECK(row[1].boolean);
	CHECK(row[2].kind == RedshiftValueKind::INTEGER);
	CHECK(row[2].integer == -7);
	CHECK(row[3].kind == RedshiftValueKind::REAL);
	CHECK(row[3].real == 2.5);
	CHECK(row[4].kind == RedshiftValueKind::BLOB);
	CHECK(row[4].text == std::string("\x00\xff", 2));
	CHECK(row[5].kind == RedshiftValueKind::TEXT);
	CHECK(row[5].text == "03/15/2024");
	// The columns before the VARBYTE are bound, the numbers as text; it and the VARCHAR after it are
	// read.
	CHECK(BindRequests(fixture) == std::vector<std::string> {"1 1 14", "2 1 33", "3 1 21", "4 1 33"});
	CHECK(GetDataRequests(fixture) == std::vector<std::string> {"5 -2", "6 1"});
}

TEST_CASE("redshift_query returns a row of every carrier as DuckDB values of the column types", "[odbc]") {
	FakeDriverFixture fixture;
	auto numeric = Column("NUMERIC", SQL_NUMERIC);
	numeric.column_size = 10;
	numeric.decimal_digits = 2;
	FakeResultSet result;
	result.columns = {Column("BOOL", SQL_BIT),
	                  Column("SMALLINT", SQL_SMALLINT),
	                  Column("INTEGER", SQL_INTEGER),
	                  Column("BIGINT", SQL_BIGINT),
	                  Column("REAL", SQL_REAL),
	                  Column("DOUBLE PRECISION", SQL_DOUBLE),
	                  numeric,
	                  Column("DATE", SQL_TYPE_DATE),
	                  Column("TIME", SQL_TYPE_TIME),
	                  Column("TIMETZ", SQL_TYPE_TIME),
	                  Column("TIMESTAMP", SQL_TYPE_TIMESTAMP),
	                  Column("TIMESTAMPTZ", SQL_TYPE_TIMESTAMP),
	                  Column("VARBYTE", SQL_LONGVARBINARY),
	                  LongVarcharColumn("c")};
	// DuckDB refuses a result whose column names repeat.
	for (idx_t i = 0; i < result.columns.size(); i++) {
		result.columns[i].name = "c" + std::to_string(i);
	}
	result.rows = {{"1", "-32768", "2147483647", "-9223372036854775808", "0.1", "0.30000000000000004", "12.34",
	                "0044-03-15 BC", "12:34:56.50", "07:04:56.50+00", "4713-01-01 00:00:00.123456 BC",
	                "4713-01-01 00:00:00+00 BC", std::string("\x00\xff\x10", 3), "h\xC3\xA9llo \xE2\x9C\x93"}};
	fixture.fake.AddResult(QUERY, {result});
	ExtensionDatabase database;
	database.Run(ATTACH_RS);

	auto read = database.Run(std::string("SELECT * FROM redshift_query('rs', '") + QUERY + "')");
	auto chunk = read->Fetch();
	REQUIRE(chunk);
	REQUIRE(chunk->size() == 1);
	// Checked through strings: client code names none of DuckDB's value types.
	std::vector<std::string> types;
	std::vector<std::string> values;
	for (idx_t i = 0; i < chunk->ColumnCount(); i++) {
		types.push_back(chunk->GetTypes()[i].ToString());
		values.push_back(chunk->GetValue(i, 0).ToString());
	}
	CHECK(types == std::vector<std::string> {"BOOLEAN", "SMALLINT", "INTEGER", "BIGINT", "FLOAT", "DOUBLE",
	                                         "DECIMAL(10,2)", "DATE", "TIME", "TIME WITH TIME ZONE", "TIMESTAMP",
	                                         "TIMESTAMP WITH TIME ZONE", "BLOB", "VARCHAR"});
	// A float prints its shortest round trip, so 0.1 shows the REAL arrived as the float nearest 0.1.
	CHECK(values == std::vector<std::string> {
	                    "true", "-32768", "2147483647", "-9223372036854775808", "0.1", "0.30000000000000004", "12.34",
	                    "0044-03-15 (BC)", "12:34:56.5", "07:04:56.5+00", "4713-01-01 (BC) 00:00:00.123456",
	                    "4713-01-01 (BC) 00:00:00+00", "\\x00\\xFF\\x10", "h\xC3\xA9llo \xE2\x9C\x93"});
}

// The leader node can print a computed DECIMAL with fewer places than it declares; the value is the
// one printed, read at the declared scale.
TEST_CASE("redshift_query reads a DECIMAL printed with fewer places than its scale at that scale", "[odbc]") {
	FakeDriverFixture fixture;
	auto numeric = Column("NUMERIC", SQL_NUMERIC);
	numeric.column_size = 37;
	numeric.decimal_digits = 17;
	fixture.fake.AddResult(QUERY, {OneColumn(numeric, {"0.0000000000000002"})});
	ExtensionDatabase database;
	database.Run(ATTACH_RS);

	auto read = database.Run(std::string("SELECT * FROM redshift_query('rs', '") + QUERY + "')");
	auto chunk = read->Fetch();
	REQUIRE(chunk);
	REQUIRE(chunk->size() == 1);
	CHECK(chunk->GetTypes()[0].ToString() == "DECIMAL(37,17)");
	CHECK(chunk->GetValue(0, 0).ToString() == "0.00000000000000020");
}

// An `oid` of 2^31 or more is described as INTEGER. Its text keeps its value, which the INTEGER
// column refuses with the cast hint instead of storing the value the driver's own parse wraps to.
TEST_CASE("redshift_query refuses an INTEGER value past INTEGER's range, naming the cast to VARCHAR", "[odbc]") {
	FakeDriverFixture fixture;
	fixture.fake.AddResult(QUERY, {OneColumn(Column("INTEGER", SQL_INTEGER), {"3000000000"})});
	ExtensionDatabase database;
	database.Run(ATTACH_RS);

	auto refused = database.con.Query(std::string("SELECT * FROM redshift_query('rs', '") + QUERY + "')");
	REQUIRE(refused->HasError());
	CHECK(refused->GetErrorType() == ExceptionType::CONVERSION);
	CHECK_THAT(refused->GetError(), Catch::Contains("value 3000000000 is out of range for INTEGER. To read the value "
	                                                "as text, cast the column to VARCHAR in the Redshift SQL"));
}

TEST_CASE("Execute refuses a column with no carrier before the statement runs", "[odbc]") {
	FakeDriverFixture fixture;
	auto shape = Column("GEOMETRY", SQL_LONGVARBINARY);
	shape.name = "shape";
	FakeResultSet result;
	std::string message;
	std::vector<std::string> calls {"SQLAllocHandle", "SQLPrepare", "SQLNumParams", "SQLNumResultCols"};
	SECTION("the only column") {
		result.columns = {shape};
		message = "Redshift result column 1, \"shape\", has a type";
		calls.insert(calls.end(), {"SQLDescribeCol", "SQLColAttribute"});
	}
	SECTION("the second column, after one with a carrier") {
		result.columns = {LongVarcharColumn("c"), shape};
		message = "Redshift result column 2, \"shape\", has a type";
		calls.insert(calls.end(), {"SQLDescribeCol", "SQLColAttribute", "SQLDescribeCol", "SQLColAttribute"});
	}
	calls.push_back("SQLFreeHandle");
	fixture.fake.AddResult(QUERY, {result});
	auto connection = fixture.OpenConnection();
	const auto before = fixture.fake.Calls().size();

	CHECK(redshift_test::Capture([&] { redshift_test::ExecuteStatement(*connection, QUERY, {}); }) ==
	      Thrown {ExceptionType::BINDER, message + " this client cannot read, so the statement was not run; cast the "
	                                               "column to a supported type in the Redshift SQL"});
	CHECK(fixture.CallsAfter(before) == calls);
	CHECK_FALSE(connection->SessionHoldsState());
	CHECK(fixture.fake.AllocatedHandles() == "environment, connection");
}

TEST_CASE("Execute refuses a 256-byte column name before the statement runs", "[odbc]") {
	FakeDriverFixture fixture;
	auto column = LongVarcharColumn("c");
	column.name = std::string(256, 'n');
	fixture.fake.AddResult(QUERY, {OneColumn(column, {"x"})});
	auto connection = fixture.OpenConnection();

	CHECK(redshift_test::Capture([&] { redshift_test::ExecuteStatement(*connection, QUERY, {}); }) ==
	      Thrown {ExceptionType::BINDER,
	              "Redshift reported a name longer than 255 bytes for column 1; alias the column to a shorter name"});
	// The session setup's statements only.
	CHECK(fixture.CallsTo("SQLExecute") == redshift_test::SessionSetupStatements().size());
	CHECK_FALSE(connection->SessionHoldsState());
}

TEST_CASE("A column description that fails in Execute leaves the session holding no state", "[odbc]") {
	FakeDriverFixture fixture;
	fixture.fake.AddResult(QUERY, {OneColumn(LongVarcharColumn("c"), {"x"})});
	auto connection = fixture.OpenConnection();
	SECTION("SQLDescribeCol") {
		fixture.FailNextCall("SQLDescribeCol", SQL_ERROR, {{"HY091", 0, "Invalid descriptor field identifier", {}}});
	}
	SECTION("SQLColAttribute") {
		fixture.FailNextCall("SQLColAttribute", SQL_ERROR, {{"HY091", 0, "Invalid descriptor field identifier", {}}});
	}

	CHECK(redshift_test::Capture([&] { redshift_test::ExecuteStatement(*connection, QUERY, {}); }) ==
	      Thrown {ExceptionType::BINDER,
	              "the Redshift driver failed while describing the statement's result columns (SQLSTATE class HY, "
	              "driver error), and its message is not shown. Retry the statement." +
	                  DRIVER_FAILURE_TRAILER + "\nSQLSTATE: HY091"});
	// The session setup's statements only.
	CHECK(fixture.CallsTo("SQLExecute") == redshift_test::SessionSetupStatements().size());
	CHECK_FALSE(connection->SessionHoldsState());
}

TEST_CASE("A batch ends once its values reach a set's 4 MiB share of 16 MiB, before its 2048 rows", "[odbc]") {
	FakeDriverFixture fixture;
	// Each value counts its bytes, its length, its string and the RedshiftValue holding it, so 8 of
	// these rows reach 4 MiB.
	SECTION("one value per row") {
		const std::string wide(512 * 1024 - sizeof(RedshiftValue), 'w');
		fixture.fake.AddResult(QUERY, {OneColumn(LongVarcharColumn("c"), std::vector<std::string>(12, wide))});
	}
	SECTION("two values per row, each counted") {
		const std::string wide(256 * 1024 - sizeof(RedshiftValue), 'w');
		FakeResultSet result;
		result.columns = {LongVarcharColumn("c"), LongVarcharColumn("c")};
		result.rows = std::vector<std::vector<std::string>>(12, {wide, wide});
		fixture.fake.AddResult(QUERY, {result});
	}
	auto connection = fixture.OpenConnection();

	const ReadBatches read(*redshift_test::ExecuteStatement(*connection, QUERY, {}));
	CHECK(read.sizes == std::vector<idx_t> {8, 4});
}

TEST_CASE("A row above the 16 MiB budget that starts a batch crosses whole, alone in it", "[odbc]") {
	FakeDriverFixture fixture;
	const std::string huge(20 * 1024 * 1024, 'h');
	fixture.fake.AddResult(QUERY, {OneColumn(LongVarcharColumn("c"), {huge, "small"})});
	auto connection = fixture.OpenConnection();

	const ReadBatches read(*redshift_test::ExecuteStatement(*connection, QUERY, {}));
	CHECK(read.sizes == std::vector<idx_t> {1, 1});
	REQUIRE(read.rows.size() == 2);
	CHECK(read.rows[0][0].text.size() == huge.size());
	// In parentheses, so that a failure does not print the 20 MiB value.
	CHECK((read.rows[0][0].text == huge));
	CHECK(read.rows[1][0].text == "small");
}

TEST_CASE("A row that takes its batch past 16 MiB is kept in that batch", "[odbc]") {
	FakeDriverFixture fixture;
	const std::string huge(20 * 1024 * 1024, 'h');
	fixture.fake.AddResult(QUERY, {OneColumn(LongVarcharColumn("c"), {"small", huge, "small"})});
	auto connection = fixture.OpenConnection();

	const ReadBatches read(*redshift_test::ExecuteStatement(*connection, QUERY, {}));
	CHECK(read.sizes == std::vector<idx_t> {2, 1});
	REQUIRE(read.rows.size() == 3);
	CHECK(read.rows[1][0].text.size() == huge.size());
}
