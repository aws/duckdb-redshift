// The driver output recorded live on 2.3.0.0, scripted in the fake: how the client classifies each
// recorded column, whether each recorded string is in its text form,
// and what reaches the caller for each recorded value, under the pinned settings and after a
// caller's SET.

// catch.hpp comes first, as in this directory's other tests.
#include "catch.hpp"

#include "fake_driver_fixture.hpp"
#include "odbc/text_forms.hpp"
#include "recorded_driver_output.hpp"
#include "recorded_output_checks.hpp"
#include "recorded_stored_output.hpp"
#include "thrown.hpp"

#include <algorithm>
#include <map>
#include <string>
#include <vector>

using namespace duckdb;
using redshift_driver::testing::FakeColumn;
using redshift_driver::testing::FakeResultSet;
using redshift_test::CarriesRecordedCell;
using redshift_test::Classified;
using redshift_test::FakeDriverFixture;
using redshift_test::FindSelect;
using redshift_test::NodePath;
using redshift_test::RecordedSelect;
using redshift_test::RunsDirect;
using redshift_test::StoredRead;

namespace {

//! Each RedshiftTypeId's name, in the enum's order, so a reordered enum fails every case here.
const char *const TYPE_NAMES[] = {"BOOLEAN",   "SMALLINT",    "INTEGER", "BIGINT", "FLOAT", "DOUBLE",
                                  "DECIMAL",   "VARCHAR",     "CHAR",    "DATE",   "TIME",  "TIMETZ",
                                  "TIMESTAMP", "TIMESTAMPTZ", "BLOB",    "UNKNOWN"};
static_assert(sizeof(TYPE_NAMES) / sizeof(TYPE_NAMES[0]) == static_cast<size_t>(RedshiftTypeId::UNKNOWN) + 1,
              "one name per RedshiftTypeId");

//! A column's type as one word, with a DECIMAL's precision and scale.
std::string TypeText(const RedshiftColumn &column) {
	std::string text = TYPE_NAMES[static_cast<size_t>(column.type)];
	if (column.type == RedshiftTypeId::DECIMAL) {
		text += "(" + std::to_string(column.precision) + "," + std::to_string(column.scale) + ")";
	}
	return text;
}

//! TypeText of each column Describe reports for `sql`, scripted with `columns`.
std::vector<std::string> DescribedTypes(FakeDriverFixture &fixture, RedshiftConnection &connection,
                                        const std::string &sql, const std::vector<FakeColumn> &columns) {
	FakeResultSet result;
	result.columns = columns;
	fixture.fake.AddResult(sql, {result});
	std::vector<std::string> types;
	for (const auto &column : connection.Describe(sql).columns) {
		types.push_back(TypeText(column));
	}
	return types;
}

//! The table applied to each recorded statement, written out by hand.
const std::map<std::string, std::vector<std::string>> EXPECTED_TYPES {
    {"integers", {"SMALLINT", "SMALLINT", "INTEGER", "INTEGER", "BIGINT", "BIGINT", "BOOLEAN", "BOOLEAN", "INTEGER"}},
    {"float8", std::vector<std::string>(9, "DOUBLE")},
    {"float4", std::vector<std::string>(8, "FLOAT")},
    {"numeric",
     {"DECIMAL(38,10)", "DECIMAL(16,16)", "DECIMAL(16,16)", "DECIMAL(38,10)", "DECIMAL(10,2)", "DECIMAL(10,2)",
      "DECIMAL(38,0)", "DECIMAL(2,1)", "DECIMAL(18,0)"}},
    {"date", std::vector<std::string>(8, "DATE")},
    {"time", std::vector<std::string>(7, "TIME")},
    {"timetz", std::vector<std::string>(6, "TIMETZ")},
    {"timestamp", std::vector<std::string>(4, "TIMESTAMP")},
    {"timestamptz", std::vector<std::string>(7, "TIMESTAMPTZ")},
    {"infinities", {"TIMESTAMP", "TIMESTAMP", "TIMESTAMPTZ", "TIMESTAMPTZ"}},
    {"character",
     {"CHAR", "CHAR", "CHAR", "CHAR", "VARCHAR", "VARCHAR", "VARCHAR", "VARCHAR", "VARCHAR", "VARCHAR", "VARCHAR",
      "VARCHAR"}},
    {"bytes_and_super", {"BLOB", "BLOB", "VARCHAR", "VARCHAR", "VARCHAR", "VARCHAR"}},
    {"qualified_intervals", std::vector<std::string>(4, "UNKNOWN")},
    {"unqualified_intervals", {"VARCHAR", "VARCHAR", "VARCHAR", "INTEGER"}},
    {"spatial", {"UNKNOWN", "UNKNOWN"}},
    {"workarounds", {"BOOLEAN", "INTEGER", "BOOLEAN", "BIGINT", "DOUBLE", "DOUBLE", "VARCHAR", "VARCHAR", "BLOB"}},
    {"SELECT NULL AS untyped_null", {"VARCHAR"}},
    {"SELECT $1 AS untyped_parameter", {"VARCHAR"}},
    {"SELECT $1::integer AS typed_parameter", {"INTEGER"}},
    {"SELECT 'x'::char(300) AS char_300, current_user AS user_name", {"CHAR", "CHAR"}},
    {"SELECT hll_create_sketch(1) AS sketch", {"UNKNOWN"}},
    {"SELECT oid, relname, relkind FROM pg_class LIMIT 1", {"INTEGER", "VARCHAR", "CHAR"}},
    {"SELECT '-1.5'::numeric(38,10) AS quoted, -1.5::numeric(38,10) AS negated", {"DECIMAL(38,10)", "DECIMAL(11,10)"}},
    {"SELECT user_id, query_id, query_label, status, result_cache_hit, start_time FROM sys_query_history LIMIT 1",
     {"INTEGER", "BIGINT", "VARCHAR", "CHAR", "BOOLEAN", "TIMESTAMP"}},
    {"SHOW timezone", {"VARCHAR"}}};

//! Whether the client carries `type` as text whose form it fixes.
bool HasTextForm(RedshiftTypeId type) {
	return type == RedshiftTypeId::DECIMAL || type == RedshiftTypeId::DATE || type == RedshiftTypeId::TIME ||
	       type == RedshiftTypeId::TIMETZ || type == RedshiftTypeId::TIMESTAMP || type == RedshiftTypeId::TIMESTAMPTZ;
}

//! The one row the client returns for `sql`, scripted with `select`'s columns and `row`.
std::vector<RedshiftValue> ClientRow(FakeDriverFixture &fixture, RedshiftConnection &connection, const std::string &sql,
                                     const RecordedSelect &select, const std::vector<std::string> &row) {
	FakeResultSet result;
	result.columns = select.result.columns;
	result.rows = {row};
	fixture.fake.AddResult(sql, {result});
	auto client_result = redshift_test::ExecuteStatement(connection, sql, {});
	redshift_test::ReadBatches read(*client_result);
	REQUIRE(read.rows.size() == 1);
	return read.rows[0];
}

//! What the client throws for a cell outside `type`'s text form, in column `column`.
redshift_test::Thrown NotInForm(const std::string &type, idx_t column) {
	return {ExceptionType::EXECUTOR, "Redshift returned a value in column " + std::to_string(column) +
	                                     " that is not in the " + type +
	                                     " text form, so the batch was not returned; if the session's datestyle was "
	                                     "changed, run the statement on a new connection, otherwise report it"};
}

//! Checks each cell of `row`, as the client returns it for `select`'s columns run as `sql`, and
//! returns how many it checked.
size_t CheckCarried(FakeDriverFixture &fixture, RedshiftConnection &connection, const RecordedSelect &select,
                    const std::string &sql, const std::vector<std::string> &row) {
	const auto values = ClientRow(fixture, connection, sql, select, row);
	const auto columns = Classified(select.result.columns);
	for (size_t index = 0; index < row.size(); index++) {
		INFO(sql << ", column " << columns[index].name);
		CHECK(CarriesRecordedCell(columns[index], row[index], values[index]));
	}
	return row.size();
}

//! What the client throws for the float8 select's row when its largest value has 15 digits.
const redshift_test::Thrown LARGEST_ROUNDS_PAST {ExceptionType::EXECUTOR,
                                                 "Redshift returned a DOUBLE PRECISION value in column 7 whose text "
                                                 "rounds past the type's range, so the batch was not returned; select "
                                                 "the column as VARCHAR (::VARCHAR) to read it"};

//! The caller statements a recorded row can follow: none, then each recorded one.
std::vector<std::string> EveryCallerStatement() {
	std::vector<std::string> statements {""};
	const auto &recorded = redshift_test::RECORDED_CALLER_STATEMENTS;
	statements.insert(statements.end(), recorded.begin(), recorded.end());
	return statements;
}

//! The stored select named `name`.
const RecordedSelect &FindStored(const std::string &name) {
	return FindSelect(name, redshift_test::StoredSelects());
}

} // namespace

TEST_CASE("Each recorded column classifies as the classification table says", "[odbc]") {
	FakeDriverFixture fixture;
	auto connection = fixture.OpenConnection();
	size_t checked = 0;
	for (const auto &select : redshift_test::RecordedSelects()) {
		const auto sql = redshift_test::RecordedSql(select, NodePath::LEADER);
		INFO(sql);
		CHECK(DescribedTypes(fixture, *connection, sql, select.result.columns) == EXPECTED_TYPES.at(select.name));
		checked++;
	}
	for (const auto &describe : redshift_test::RecordedDescribes()) {
		INFO(describe.sql);
		CHECK(DescribedTypes(fixture, *connection, describe.sql, describe.columns) == EXPECTED_TYPES.at(describe.sql));
		checked++;
	}
	CHECK(checked == EXPECTED_TYPES.size());
}

TEST_CASE("A recorded row is the caller statement's, else the node's under the pinned settings", "[odbc]") {
	const auto &timetz = FindSelect("timetz");
	const std::string new_york = "SET timezone TO 'America/New_York'";

	CHECK(redshift_test::RecordedRow(timetz, "", NodePath::LEADER)[0] == "12:34:56.50+05:30");
	CHECK(redshift_test::RecordedRow(timetz, "", NodePath::COMPUTE)[0] == "07:04:56.50+00");
	CHECK(redshift_test::RecordedRow(timetz, "SET extra_float_digits TO 0", NodePath::COMPUTE)[0] == "07:04:56.50+00");
	CHECK(redshift_test::RecordedRow(timetz, new_york, NodePath::LEADER)[0] == "12:34:56.50+05:30");
	CHECK(redshift_test::RecordedRow(timetz, new_york, NodePath::COMPUTE)[0] == "02:04:56.50-05");
}

TEST_CASE("Every recorded difference fits a recorded select's row under a recorded caller statement", "[odbc]") {
	for (const auto &difference : redshift_test::RecordedDifferences()) {
		INFO(difference.select << " after " << difference.caller_statement);
		const auto &statements = redshift_test::RECORDED_CALLER_STATEMENTS;
		CHECK((std::string(difference.caller_statement).empty() ||
		       std::count(statements.begin(), statements.end(), difference.caller_statement) == 1));
		CHECK(difference.row.size() == FindSelect(difference.select).result.columns.size());
	}
}

TEST_CASE("Each value recorded under the pinned settings is in its text form, but an infinity", "[odbc]") {
	size_t checked = 0;
	for (const auto &select : redshift_test::RecordedSelects()) {
		const auto columns = Classified(select.result.columns);
		for (const auto path : {NodePath::LEADER, NodePath::COMPUTE}) {
			const auto row = redshift_test::RecordedRow(select, "", path);
			for (size_t index = 0; index < row.size(); index++) {
				if (!HasTextForm(columns[index].type)) {
					continue;
				}
				INFO(select.name << " " << columns[index].name << " " << row[index]);
				CHECK(MatchesTextForm(columns[index], row[index]) == (std::string(select.name) != "infinities"));
				checked++;
			}
		}
	}
	CHECK(checked == 2 * (9 + 8 + 7 + 6 + 4 + 7 + 4));
}

TEST_CASE("Each value recorded under the pinned settings reaches the caller as the driver sent it", "[odbc]") {
	FakeDriverFixture fixture;
	auto connection = fixture.OpenConnection();
	for (const auto &select : redshift_test::RecordedSelects()) {
		if (redshift_test::UNREADABLE.count(select.name) != 0) {
			continue;
		}
		for (const auto path : {NodePath::LEADER, NodePath::COMPUTE}) {
			CheckCarried(fixture, *connection, select, redshift_test::RecordedSql(select, path),
			             redshift_test::RecordedRow(select, "", path));
		}
	}
}

// 2.3.0.0 sends the words; DuckDB holds both infinities, but the extension refuses them too, so the batch fails loudly
// rather than carrying a value either side would refuse.
TEST_CASE("A TIMESTAMP or TIMESTAMPTZ infinity 2.3.0.0 sends fails the batch as outside its form", "[odbc]") {
	FakeDriverFixture fixture;
	auto connection = fixture.OpenConnection();
	const auto &infinities = FindSelect("infinities");
	const std::vector<std::string> types {"TIMESTAMP", "TIMESTAMP", "TIMESTAMPTZ", "TIMESTAMPTZ"};
	REQUIRE(infinities.result.columns.size() == types.size());
	for (size_t index = 0; index < types.size(); index++) {
		INFO(infinities.result.columns[index].name);
		// Each infinity behind an in-form cell, so the refusal names its own column.
		RecordedSelect pair {"infinity", "", {}};
		pair.result.columns = {FindSelect("timestamp").result.columns[0], infinities.result.columns[index]};
		const std::vector<std::string> row {FindSelect("timestamp").result.rows[0][0],
		                                    infinities.result.rows[0][index]};
		const auto thrown = redshift_test::Capture(
		    [&] { ClientRow(fixture, *connection, "SELECT infinity " + std::to_string(index), pair, row); });
		CHECK(thrown == NotInForm(types[index], 2));
	}
}

// Recorded, not refused: a zone's truncated offset is in form and passes, and
// only a float that rounds past its range fails.
TEST_CASE("Each value recorded after a caller's SET reaches the caller as sent, but a float past its range", "[odbc]") {
	FakeDriverFixture fixture;
	auto connection = fixture.OpenConnection();
	for (const auto &select : redshift_test::RecordedSelects()) {
		if (redshift_test::UNREADABLE.count(select.name) != 0) {
			continue;
		}
		for (const auto &caller_statement : redshift_test::RECORDED_CALLER_STATEMENTS) {
			for (const auto path : {NodePath::LEADER, NodePath::COMPUTE}) {
				const auto row = redshift_test::RecordedRow(select, caller_statement, path);
				if (std::string(select.name) != "float8" || caller_statement != "SET extra_float_digits TO 0") {
					CheckCarried(fixture, *connection, select, redshift_test::RecordedSql(select, path), row);
					continue;
				}
				const auto thrown = redshift_test::Capture(
				    [&] { ClientRow(fixture, *connection, redshift_test::RecordedSql(select, path), select, row); });
				CHECK(thrown == LARGEST_ROUNDS_PAST);
			}
		}
	}
}

TEST_CASE("Every stored difference fits a stored select's row under a recorded caller statement", "[odbc]") {
	const auto statements = EveryCallerStatement();
	for (const auto &difference : redshift_test::StoredDifferences()) {
		INFO(difference.select << " after " << difference.caller_statement);
		CHECK(std::count(statements.begin(), statements.end(), difference.caller_statement) == 1);
		CHECK(difference.row.size() == FindStored(difference.select).result.columns.size());
	}
	// No two differences hold for one select, caller statement and protocol, so none hides another in
	// StoredDifferences' order; one after a given statement overrides one after any, as StoredRow says.
	const auto &differences = redshift_test::StoredDifferences();
	for (size_t first = 0; first < differences.size(); first++) {
		for (size_t second = first + 1; second < differences.size(); second++) {
			const auto &one = differences[first];
			const auto &other = differences[second];
			const bool reads_overlap =
			    one.read == other.read || one.read == StoredRead::EITHER || other.read == StoredRead::EITHER;
			const std::string one_caller = one.caller_statement;
			const std::string other_caller = other.caller_statement;
			INFO(one.select << " " << one_caller << " / " << other_caller);
			CHECK_FALSE((std::string(one.select) == other.select && reads_overlap && one_caller == other_caller));
		}
	}
	for (const auto &unstored : redshift_test::UnstoredSelects()) {
		CHECK_NOTHROW(FindSelect(unstored.select));
	}
	// Each recorded select is stored or refused, never both, and the stored-only selects are the extras.
	CHECK(redshift_test::StoredSelects().size() + redshift_test::UnstoredSelects().size() ==
	      redshift_test::RecordedSelects().size() + redshift_test::StoredOnlySelects().size());
}

TEST_CASE("A stored row is its difference for the caller statement or for any, else the compute nodes'", "[odbc]") {
	const auto &timestamptz = FindStored("timestamptz");
	const std::string new_york = "SET timezone TO 'America/New_York'";
	CHECK(redshift_test::StoredRow(timestamptz, "", StoredRead::DIRECT)[0] == "1883-11-18 12:00:00+00");
	CHECK(redshift_test::StoredRow(timestamptz, new_york, StoredRead::PREPARED)[0] == "1883-11-18 07:03:58-04:56");
	CHECK(redshift_test::StoredRow(timestamptz, new_york, StoredRead::DIRECT)[0] == "1883-11-18 07:03:58-04:56");
	CHECK(redshift_test::RecordedRow(timestamptz, new_york, NodePath::COMPUTE)[0] == "1883-11-18 07:03:56-04:56");
	const auto &date = FindStored("date");
	CHECK(redshift_test::StoredRow(date, "", StoredRead::PREPARED)[0] == "4713-01-01 BC");
	CHECK(redshift_test::StoredRow(date, new_york, StoredRead::DIRECT)[0] == "-4712-01-01");
	CHECK(redshift_test::StoredRow(FindStored("timetz"), new_york, StoredRead::DIRECT)[0] == "02:04:56.50-05");
	const auto &infinity = FindStored("infinity");
	CHECK(redshift_test::StoredRow(infinity, new_york, StoredRead::DIRECT)[1] == "294277-01-08 23:00:54.775807-05");
	CHECK(redshift_test::StoredRow(infinity, "SET intervalstyle TO 'sql_standard'", StoredRead::DIRECT)[1] ==
	      "294277-01-09 04:00:54.775807+00");
	CHECK(redshift_test::StoredRow(infinity, new_york, StoredRead::PREPARED)[1] == "infinity");
}

// The server prints a float4 with 8 significant digits at the pinned setting, where 9 would carry every
// float4; a float8's 17 carry every float8.
TEST_CASE("A stored float4 that needs a ninth digit reaches the caller as another float", "[odbc]") {
	FakeDriverFixture fixture;
	auto connection = fixture.OpenConnection();
	const auto &select = FindStored("float4_ninth_digit");
	const auto row = redshift_test::StoredRow(select, "", StoredRead::PREPARED);
	const auto values = ClientRow(fixture, *connection, redshift_test::StoredSql(select), select, row);
	REQUIRE(values[0].kind == RedshiftValueKind::REAL);
	CHECK(values[0].real != static_cast<double>(100000024.0f));
	CHECK(values[0].real == static_cast<double>(100000016.0f));
}

TEST_CASE("Each stored value under the pinned settings is in its text form on either protocol, but an infinity",
          "[odbc]") {
	size_t checked = 0;
	for (const auto &select : redshift_test::StoredSelects()) {
		const auto columns = Classified(select.result.columns);
		for (const auto read : {StoredRead::PREPARED, StoredRead::DIRECT}) {
			if (read == StoredRead::DIRECT && redshift_test::StoredDirectRefused(select)) {
				continue;
			}
			const auto row = redshift_test::StoredRow(select, "", read);
			for (size_t index = 0; index < row.size(); index++) {
				if (!HasTextForm(columns[index].type)) {
					continue;
				}
				INFO(select.name << " " << columns[index].name << " " << row[index]);
				const bool infinity = row[index] == "infinity" || row[index] == "-infinity";
				CHECK(MatchesTextForm(columns[index], row[index]) == !infinity);
				checked++;
			}
		}
	}
	CHECK(checked == 2 * (9 + 8 + 7 + 6 + 4 + 7) + 4 + 2 * 2 + 2);
}

// The client runs a float shape with SQLExecute only, so a float select's SQLExecDirect row never
// reaches it; RunsDirect holds the client's rule. Under the pinned settings each stored float8
// carries 17 digits and arrives exactly. A stored +infinity is unreadable only through SQLExecute,
// which sends the word: through SQLExecDirect, the client's path for it, it is a finite timestamp.
TEST_CASE("Each stored value reaches the caller as the driver sent it, but a float past its range", "[odbc]") {
	FakeDriverFixture fixture;
	auto connection = fixture.OpenConnection();
	size_t checked = 0;
	for (const auto &select : redshift_test::StoredSelects()) {
		if (redshift_test::UNREADABLE.count(select.name) != 0) {
			continue;
		}
		const bool direct_only = std::string(select.name) == "infinity";
		const auto sql = redshift_test::StoredSql(select);
		for (const auto &caller_statement : EveryCallerStatement()) {
			for (const auto read : {StoredRead::PREPARED, StoredRead::DIRECT}) {
				const bool client_reads = read == StoredRead::DIRECT ? RunsDirect(select.result.columns) : !direct_only;
				if (!client_reads) {
					continue;
				}
				INFO(sql << " after " << caller_statement);
				const auto row = redshift_test::StoredRow(select, caller_statement, read);
				if (std::string(select.name) != "float8" || caller_statement != "SET extra_float_digits TO 0") {
					checked += CheckCarried(fixture, *connection, select, sql, row);
					continue;
				}
				const auto thrown = redshift_test::Capture([&] { ClientRow(fixture, *connection, sql, select, row); });
				CHECK(thrown == LARGEST_ROUNDS_PAST);
				checked++;
			}
		}
	}
	// Six caller statements, none and the five recorded; both protocols for a shape the client may run
	// either way, SQLExecute alone for the four float shapes, whose float8 row after extra_float_digits 0
	// counts one refusal for its nine cells, and SQLExecDirect alone for the two +infinity cells.
	CHECK(checked == 6 * 2 * (9 + 9 + 8 + 7 + 6 + 4 + 7 + 12 + 6 + 1) + 6 * (9 + 8 + 9 + 1) - 9 + 1 + 6 * 2);
}
