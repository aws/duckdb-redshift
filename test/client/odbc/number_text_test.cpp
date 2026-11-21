// Numbers fetched as text and parsed in NextBatch, against the native C types the driver parses the
// same text into: each script runs on both bindings, over a bound row array, bound before a long
// column and cell by cell after one.

// catch.hpp comes first, as in this directory's other tests.
#include "catch.hpp"

#include "duckdb/common/exception.hpp"
#include "fake_driver_fixture.hpp"
#include "odbc/bulk_fetch.hpp"
#include "thrown.hpp"

#include <cfloat>
#include <clocale>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <limits>
#include <string>
#include <vector>

#if defined(__linux__)
#include "trust_store_test_support.hpp"
#endif

using namespace duckdb;
using redshift_driver::testing::FakeColumn;
using redshift_driver::testing::FakeResultSet;
using redshift_test::FakeDriverFixture;
using redshift_test::ReadBatches;
using redshift_test::Thrown;

namespace {

const char *const QUERY = "SELECT c FROM t";

//! A column named `c` as the driver describes one of `type_name`.
FakeColumn Column(const std::string &type_name, SQLSMALLINT sql_type) {
	FakeColumn column;
	column.name = "c";
	column.type_name = type_name;
	column.sql_type = sql_type;
	return column;
}

FakeColumn Boolean() {
	return Column("BOOL", SQL_BIT);
}

FakeColumn Double() {
	return Column("DOUBLE PRECISION", SQL_DOUBLE);
}

//! How a result is read: over a bound row array; bound one row a fetch, which a long column after
//! the numbers brings about; or cell by cell with SQLGetData, which a long column before them does.
enum class Path : uint8_t { BOUND, BEFORE_LONG, AFTER_LONG };

const NumberBinding BINDINGS[] = {NumberBinding::TEXT, NumberBinding::NATIVE};
const Path PATHS[] = {Path::BOUND, Path::BEFORE_LONG, Path::AFTER_LONG};

const char *BindingName(NumberBinding binding) {
	return binding == NumberBinding::TEXT ? "text binding" : "native binding";
}

const char *PathName(Path path) {
	switch (path) {
	case Path::BOUND:
		return "bound";
	case Path::BEFORE_LONG:
		return "bound before a long column";
	default:
		return "by cell after a long column";
	}
}

//! The index, from 0, of a script's first number column on `path`: after the long column on
//! Path::AFTER_LONG, and otherwise first.
idx_t FirstNumber(Path path) {
	return path == Path::AFTER_LONG ? 1 : 0;
}

//! `rows` of `columns`, with a VARCHAR the driver reports with size 0, which is long, holding `x`
//! in each row: after them on Path::BEFORE_LONG, before them on Path::AFTER_LONG.
FakeResultSet Script(const std::vector<FakeColumn> &columns, const std::vector<std::vector<std::string>> &rows,
                     Path path) {
	FakeResultSet result;
	result.columns = columns;
	for (idx_t i = 0; i < result.columns.size(); i++) {
		result.columns[i].name = "c" + std::to_string(i);
	}
	result.rows = rows;
	if (path != Path::BOUND) {
		auto long_text = Column("CHARACTER VARYING", SQL_VARCHAR);
		long_text.name = "long_text";
		const auto at = path == Path::BEFORE_LONG ? result.columns.size() : 0;
		result.columns.insert(result.columns.begin() + NumericCast<int64_t>(at), long_text);
		for (auto &row : result.rows) {
			row.insert(row.begin() + NumericCast<int64_t>(at), "x");
		}
	}
	return result;
}

//! `cells` in one column of `column`, a row each.
FakeResultSet OneColumn(const FakeColumn &column, const std::vector<std::string> &cells, Path path) {
	std::vector<std::vector<std::string>> rows;
	for (const auto &cell : cells) {
		rows.push_back({cell});
	}
	return Script({column}, rows, path);
}

//! Every row of `script`, read with its numbers bound as `binding`.
std::vector<std::vector<RedshiftValue>> ReadRows(const FakeResultSet &script, NumberBinding binding) {
	FakeDriverFixture fixture;
	SetNumberBindingForTesting(binding);
	fixture.fake.AddResult(QUERY, {script});
	auto connection = fixture.OpenConnection();
	return ReadBatches(*redshift_test::ExecuteStatement(*connection, QUERY, {})).rows;
}

//! The first number column of every row of `script`, built for `path`, read as ReadRows reads it.
std::vector<RedshiftValue> ReadNumbers(const FakeResultSet &script, Path path, NumberBinding binding) {
	std::vector<RedshiftValue> values;
	for (const auto &row : ReadRows(script, binding)) {
		values.push_back(row[FirstNumber(path)]);
	}
	return values;
}

//! What the first NextBatch of `script` threw with its numbers bound as `binding`, and the rows it
//! left in the batch.
struct FirstBatch {
	ErrorData error;
	idx_t rows_left;
};

FirstBatch FailFirstBatch(const FakeResultSet &script, NumberBinding binding) {
	FakeDriverFixture fixture;
	SetNumberBindingForTesting(binding);
	fixture.fake.AddResult(QUERY, {script});
	auto connection = fixture.OpenConnection();
	auto result = redshift_test::ExecuteStatement(*connection, QUERY, {});
	RedshiftBatch batch;
	const auto error = redshift_test::CaptureError([&] { result->NextBatch(batch); });
	return FirstBatch {error, batch.rows.size()};
}

//! A double's bit pattern, so a check tells -0 from 0 and one NaN from another.
uint64_t Bits(double real) {
	uint64_t bits = 0;
	memcpy(&bits, &real, sizeof(bits));
	return bits;
}

//! The REAL values' bit patterns, failing the case on any other carrier.
std::vector<uint64_t> RealBits(const std::vector<RedshiftValue> &values) {
	std::vector<uint64_t> bits;
	for (const auto &value : values) {
		REQUIRE(value.kind == RedshiftValueKind::REAL);
		bits.push_back(Bits(value.real));
	}
	return bits;
}

//! A value as a check prints it: its carrier and what it holds, a REAL by its bits.
std::string Describe(const RedshiftValue &value) {
	switch (value.kind) {
	case RedshiftValueKind::BOOLEAN:
		return value.boolean ? "BOOLEAN true" : "BOOLEAN false";
	case RedshiftValueKind::INTEGER:
		return "INTEGER " + std::to_string(value.integer);
	case RedshiftValueKind::REAL:
		return "REAL bits " + std::to_string(Bits(value.real));
	case RedshiftValueKind::NULL_VALUE:
		return "NULL";
	default:
		return "TEXT or BLOB " + value.text;
	}
}

std::vector<std::string> Describe(const std::vector<std::vector<RedshiftValue>> &rows) {
	std::vector<std::string> described;
	for (const auto &row : rows) {
		for (const auto &value : row) {
			described.push_back(Describe(value));
		}
	}
	return described;
}

//! The refusal of text that is no value of `type_name`, in the first number column on `path`.
Thrown NotInTextForm(const std::string &type_name, Path path) {
	return Thrown {ExceptionType::EXECUTOR, "Redshift returned a value in column " +
	                                            std::to_string(FirstNumber(path) + 1) + " that is not in the " +
	                                            type_name +
	                                            " text form, so the batch was not returned; if the session's "
	                                            "datestyle was changed, run the statement on a new connection, "
	                                            "otherwise report it"};
}

//! LC_NUMERIC set, for the scope, to a locale whose decimal point is a comma, and restored after. An
//! installed locale is used when there is one. A minimal Linux image may install only the C locale, so
//! on Linux one is otherwise built with localedef from a definition that sets only the decimal point.
class ScopedCommaDecimalLocale {
public:
	ScopedCommaDecimalLocale() : previous(setlocale(LC_NUMERIC, nullptr)) {
		for (const auto name : {"de_DE.UTF-8", "de_DE.utf8", "fr_FR.UTF-8", "fr_FR.utf8", "de-DE"}) {
			if (SetCommaLocale(name)) {
				in_force = true;
				return;
			}
		}
#if defined(__linux__)
		in_force = SetBuiltLocale();
#endif
	}
	~ScopedCommaDecimalLocale() {
		setlocale(LC_NUMERIC, previous.c_str());
	}
	ScopedCommaDecimalLocale(const ScopedCommaDecimalLocale &) = delete;
	ScopedCommaDecimalLocale &operator=(const ScopedCommaDecimalLocale &) = delete;
	//! False when no comma-decimal locale is installed and none could be built.
	bool InForce() const {
		return in_force;
	}

private:
	static bool SetCommaLocale(const char *name) {
		return setlocale(LC_NUMERIC, name) != nullptr && std::string(localeconv()->decimal_point) == ",";
	}
#if defined(__linux__)
	//! localedef needs a character map, which a minimal image may lack, so this one lists ASCII. It
	//! exits 1 for the categories the definition leaves out, and with -c writes the locale anyway.
	bool SetBuiltLocale() {
		std::string charmap = "<code_set_name> COMMA_TEST\n<mb_cur_max> 1\n<mb_cur_min> 1\nCHARMAP\n";
		for (int code = 0; code < 128; code++) {
			char line[32];
			snprintf(line, sizeof(line), "<U%04X> \\x%02x\n", code, code);
			charmap += line;
		}
		charmap += "END CHARMAP\n";
		const auto charmap_path = directory.File("ascii.cm", charmap);
		const auto definition = directory.File(
		    "comma.def", "LC_NUMERIC\ndecimal_point \"<U002C>\"\nthousands_sep \"\"\ngrouping -1\nEND LC_NUMERIC\n");
		const auto command = "localedef -c --no-warnings=ascii -i '" + definition + "' -f '" + charmap_path + "' '" +
		                     directory.Path("comma") + "' >/dev/null 2>&1";
		if (std::system(command.c_str()) == -1) {
			return false;
		}
		const redshift_driver::testing::ScopedEnvironmentVariable locale_path("LOCPATH", directory.Path("").c_str());
		return SetCommaLocale("comma");
	}
	redshift_trust_test::TemporaryDirectory directory;
#endif
	std::string previous;
	bool in_force = false;
};

} // namespace

TEST_CASE("Each integer width's minimum and maximum arrive exactly, on both bindings", "[odbc]") {
	struct Case {
		FakeColumn column;
		std::vector<std::string> cells;
		std::vector<int64_t> integers;
	};
	const std::vector<Case> cases {
	    {Column("SMALLINT", SQL_SMALLINT), {"-32768", "32767"}, {-32768, 32767}},
	    {Column("INTEGER", SQL_INTEGER), {"-2147483648", "2147483647"}, {-2147483647 - 1, 2147483647}},
	    {Column("BIGINT", SQL_BIGINT),
	     {"-9223372036854775808", "9223372036854775807"},
	     {std::numeric_limits<int64_t>::min(), std::numeric_limits<int64_t>::max()}},
	};
	for (const auto &integers : cases) {
		for (const auto binding : BINDINGS) {
			for (const auto path : PATHS) {
				INFO(integers.column.type_name << ", " << BindingName(binding) << ", " << PathName(path));
				std::vector<int64_t> read;
				for (const auto &value : ReadNumbers(OneColumn(integers.column, integers.cells, path), path, binding)) {
					CHECK(value.kind == RedshiftValueKind::INTEGER);
					read.push_back(value.integer);
				}
				CHECK(read == integers.integers);
			}
		}
	}
}

// An `oid` of 2^31 or more is described as INTEGER. Only the text binding is read: the driver's
// native parse wraps such a value, and the fake's, strtoll, does not.
TEST_CASE("An INTEGER column's text past 2^31 keeps its value on the text binding", "[odbc]") {
	for (const auto path : PATHS) {
		INFO(PathName(path));
		std::vector<std::string> read;
		for (const auto &value :
		     ReadNumbers(OneColumn(Column("INTEGER", SQL_INTEGER), {"2147483648", "4294967295"}, path), path,
		                 NumberBinding::TEXT)) {
			read.push_back(Describe(value));
		}
		CHECK(read == std::vector<std::string> {"INTEGER 2147483648", "INTEGER 4294967295"});
	}
}

TEST_CASE("float8 text with 17 significant digits arrives bit-exactly, on both bindings", "[odbc]") {
	const std::vector<std::string> cells {"0.30000000000000004",
	                                      "1.7976931348623157e+308",
	                                      "4.9406564584124654e-324",
	                                      "-0.0",
	                                      "NaN",
	                                      "Infinity",
	                                      "-Infinity"};
	const std::vector<uint64_t> expected {Bits(0.30000000000000004),
	                                      Bits(DBL_MAX),
	                                      Bits(std::numeric_limits<double>::denorm_min()),
	                                      Bits(-0.0),
	                                      Bits(std::numeric_limits<double>::quiet_NaN()),
	                                      Bits(std::numeric_limits<double>::infinity()),
	                                      Bits(-std::numeric_limits<double>::infinity())};
	for (const auto binding : BINDINGS) {
		for (const auto path : PATHS) {
			INFO(BindingName(binding) << ", " << PathName(path));
			const auto bits = RealBits(ReadNumbers(OneColumn(Double(), cells, path), path, binding));
			CHECK(bits == expected);
		}
	}
}

TEST_CASE("A float's table and ::VARCHAR spellings arrive as the values they denote, on both bindings", "[odbc]") {
	const auto nan = std::numeric_limits<double>::quiet_NaN();
	const auto infinity = std::numeric_limits<double>::infinity();
	const std::vector<std::string> spellings {"nan", "-nan", "inf", "-inf", "NaN", "Infinity", "-Infinity"};
	const std::vector<uint64_t> denoted {Bits(nan), Bits(-nan),     Bits(infinity), Bits(-infinity),
	                                     Bits(nan), Bits(infinity), Bits(-infinity)};
	for (const auto binding : BINDINGS) {
		for (const auto path : PATHS) {
			INFO(BindingName(binding) << ", " << PathName(path));
			const auto bits = RealBits(ReadNumbers(OneColumn(Double(), spellings, path), path, binding));
			CHECK(bits == denoted);
			// A float4 read from a table, at its 6 digits.
			const auto table_real =
			    RealBits(ReadNumbers(OneColumn(Column("REAL", SQL_REAL), {"1.67772e+07"}, path), path, binding));
			CHECK(table_real == std::vector<uint64_t> {Bits(16777200.0)});
		}
	}
}

// glibc's strtod keeps a NaN's payload, so the fake delivers the bits MSVC's C runtime parses a NaN into.
TEST_CASE("A native NaN with every payload bit set arrives as the quiet NaN, signed", "[odbc]") {
	const std::vector<std::string> cells {"nan(0xfffffffffffff)", "-nan(0xfffffffffffff)"};
	const std::vector<uint64_t> expected {Bits(std::numeric_limits<double>::quiet_NaN()),
	                                      Bits(-std::numeric_limits<double>::quiet_NaN())};
	for (const auto path : PATHS) {
		INFO(PathName(path));
		CHECK(RealBits(ReadNumbers(OneColumn(Double(), cells, path), path, NumberBinding::NATIVE)) == expected);
	}
}

TEST_CASE("Finite text past DOUBLE PRECISION's range fails its batch on the text binding alone", "[odbc]") {
	for (const auto path : PATHS) {
		for (const auto *const cell : {"1.79769313486232e+308", "-1.79769313486232e+308"}) {
			INFO(PathName(path) << ", " << cell);
			const auto script = OneColumn(Double(), {"1", cell}, path);
			const auto batch = FailFirstBatch(script, NumberBinding::TEXT);
			CHECK((Thrown {batch.error.Type(), batch.error.RawMessage()}) ==
			      Thrown {ExceptionType::EXECUTOR,
			              "Redshift returned a DOUBLE PRECISION value in column " +
			                  std::to_string(FirstNumber(path) + 1) +
			                  " whose text rounds past the type's range, so the batch was not returned; select the "
			                  "column as VARCHAR (::VARCHAR) to read it"});
			CHECK(batch.rows_left == 0);
			// The driver's native parse turns the same text into an infinity with no error.
			const auto native = ReadNumbers(script, path, NumberBinding::NATIVE);
			REQUIRE(native.size() == 2);
			CHECK(std::isinf(native[1].real));
			CHECK(std::signbit(native[1].real) == (cell[0] == '-'));
		}
	}
}

TEST_CASE("A BOOLEAN's 1 and 0 arrive as true and false, on both bindings", "[odbc]") {
	for (const auto binding : BINDINGS) {
		for (const auto path : PATHS) {
			INFO(BindingName(binding) << ", " << PathName(path));
			const auto values = ReadNumbers(OneColumn(Boolean(), {"1", "0"}, path), path, binding);
			REQUIRE(values.size() == 2);
			CHECK(values[0].kind == RedshiftValueKind::BOOLEAN);
			CHECK(values[0].boolean);
			CHECK(values[1].kind == RedshiftValueKind::BOOLEAN);
			CHECK_FALSE(values[1].boolean);
		}
	}
}

TEST_CASE("Number text not in its type's form, or past BIGINT's range, fails its batch, returning none of its rows",
          "[odbc]") {
	struct Case {
		FakeColumn column;
		std::string type_name;
		std::string cell;
	};
	const std::vector<Case> cases {
	    {Boolean(), "BOOLEAN", "t"},
	    {Boolean(), "BOOLEAN", "2"},
	    {Column("SMALLINT", SQL_SMALLINT), "SMALLINT", "1.5"},
	    {Column("INTEGER", SQL_INTEGER), "INTEGER", "x"},
	    {Column("INTEGER", SQL_INTEGER), "INTEGER", "+1"},
	    {Column("INTEGER", SQL_INTEGER), "INTEGER", " 1"},
	    {Column("INTEGER", SQL_INTEGER), "INTEGER", ""},
	    {Column("INTEGER", SQL_INTEGER), "INTEGER", "007"},
	    {Column("SMALLINT", SQL_SMALLINT), "SMALLINT", "00"},
	    {Column("BIGINT", SQL_BIGINT), "BIGINT", "-0"},
	    {Column("BIGINT", SQL_BIGINT), "BIGINT", "9223372036854775808"},
	    {Column("BIGINT", SQL_BIGINT), "BIGINT", "-9223372036854775809"},
	};
	for (const auto &malformed : cases) {
		for (const auto path : PATHS) {
			INFO(malformed.type_name << " '" << malformed.cell << "', " << PathName(path));
			const auto batch =
			    FailFirstBatch(OneColumn(malformed.column, {"1", malformed.cell}, path), NumberBinding::TEXT);
			CHECK((Thrown {batch.error.Type(), batch.error.RawMessage()}) == NotInTextForm(malformed.type_name, path));
			CHECK(batch.rows_left == 0);
		}
	}
}

// The fake refuses "x" in an INTEGER column with 22018, a departure it names: the driver parses it
// as 0.
TEST_CASE("Number text the driver cannot parse natively fails its batch on the native binding", "[odbc]") {
	for (const auto path : PATHS) {
		INFO(PathName(path));
		const auto batch =
		    FailFirstBatch(OneColumn(Column("INTEGER", SQL_INTEGER), {"1", "x"}, path), NumberBinding::NATIVE);
		CHECK(batch.error.ExtraInfo().at("sqlstate") == "22018");
		CHECK(batch.rows_left == 0);
	}
}

TEST_CASE("Both bindings give the same carriers for the same script", "[odbc]") {
	const std::vector<FakeColumn> columns {Boolean(),
	                                       Column("SMALLINT", SQL_SMALLINT),
	                                       Column("INTEGER", SQL_INTEGER),
	                                       Column("BIGINT", SQL_BIGINT),
	                                       Column("REAL", SQL_REAL),
	                                       Double()};
	// The 0 and 1 in the integer columns read as integers, not as a BOOLEAN's text.
	const std::vector<std::vector<std::string>> rows {
	    {"1", "0", "1", "-7", "0.1", "0.30000000000000004"},
	    {"0", "-32768", "2147483647", "9223372036854775807", "3.4028235e+38", "-1.5e-300"},
	    {"1", "12", "-1", "0", "-inf", "nan"},
	};
	for (const auto path : PATHS) {
		INFO(PathName(path));
		auto script = Script(columns, rows, path);
		const auto width = script.columns.size();
		script.nulls = {std::vector<bool>(width, false), std::vector<bool>(width, false),
		                std::vector<bool>(width, true)};
		const auto text = ReadRows(script, NumberBinding::TEXT);
		CHECK(Describe(text) == Describe(ReadRows(script, NumberBinding::NATIVE)));
		REQUIRE(text.size() == 3);
		const auto first = FirstNumber(path);
		CHECK(text[0][first + 1].kind == RedshiftValueKind::INTEGER);
		CHECK(text[0][first + 2].kind == RedshiftValueKind::INTEGER);
		CHECK(text[2][first].kind == RedshiftValueKind::NULL_VALUE);
	}
}

TEST_CASE("The text binding returns the same values under a comma-decimal LC_NUMERIC", "[odbc]") {
	const std::vector<FakeColumn> columns {Boolean(), Column("BIGINT", SQL_BIGINT), Column("REAL", SQL_REAL), Double()};
	const std::vector<std::vector<std::string>> rows {{"1", "-7", "0.1", "0.30000000000000004"},
	                                                  {"0", "9223372036854775807", "1.5e-38", "-2.5e+300"}};
	for (const auto path : PATHS) {
		INFO(PathName(path));
		const auto script = Script(columns, rows, path);
		const auto in_c_locale = Describe(ReadRows(script, NumberBinding::TEXT));
		const ScopedCommaDecimalLocale comma;
		if (!comma.InForce()) {
			// Skipped by default, so a host without the locale still runs the suite; an image that
			// guarantees one sets REDSHIFT_REQUIRE_COMMA_LOCALE=1 to fail instead (test/README.md).
			const std::string reason = "no comma-decimal locale is installed, and none could be built";
			const char *required = std::getenv("REDSHIFT_REQUIRE_COMMA_LOCALE");
			if (required != nullptr && std::string(required) == "1") {
				FAIL(reason + "; REDSHIFT_REQUIRE_COMMA_LOCALE=1 requires one");
			}
			SKIP_TEST("skipped: " + reason + " (test/README.md)");
			return;
		}
		CHECK(Describe(ReadRows(script, NumberBinding::TEXT)) == in_c_locale);
		// The locale is in force: the fake's native parse, strtod, now fails "0.5".
		const auto native = FailFirstBatch(OneColumn(Double(), {"0.5"}, path), NumberBinding::NATIVE);
		CHECK(native.error.ExtraInfo().at("sqlstate") == "22018");
	}
}

TEST_CASE("The text binding binds numbers as SQL_C_CHAR and the native one in their native C types", "[odbc]") {
	FakeResultSet script;
	script.columns = {Boolean(), Column("INTEGER", SQL_INTEGER), Double()};
	for (idx_t i = 0; i < script.columns.size(); i++) {
		script.columns[i].name = "c" + std::to_string(i);
	}
	script.rows = {{"1", "1", "1"}};
	std::vector<std::string> expected;
	NumberBinding binding = DEFAULT_NUMBER_BINDING;
	SECTION("the shipped default, the text binding") {
		CHECK(DEFAULT_NUMBER_BINDING == NumberBinding::TEXT);
		// SQL_C_CHAR is 1, at a boolean's or a float's 32 bytes and an integer's 20, each with the NUL.
		expected = {"1 1 33", "2 1 21", "3 1 33"};
	}
	SECTION("the native binding") {
		binding = NumberBinding::NATIVE;
		// SQL_C_BIT is -7, SQL_C_SBIGINT -25 and SQL_C_DOUBLE 8.
		expected = {"1 -7 1", "2 -25 8", "3 8 8"};
	}
	FakeDriverFixture fixture;
	SetNumberBindingForTesting(binding);
	fixture.fake.AddResult(QUERY, {script});
	auto connection = fixture.OpenConnection();
	const ReadBatches read(*redshift_test::ExecuteStatement(*connection, QUERY, {}));
	REQUIRE(read.rows.size() == 1);
	CHECK(fixture.DetailsOf("SQLBindCol") == expected);
}
