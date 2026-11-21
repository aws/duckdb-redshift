// With the live variables, what the pinned driver reports on the client's session: each recorded
// statement's columns and text, from the leader and the compute nodes, under the pinned settings and
// after a caller's SET, and through the simple protocol where the client uses it, and what a
// caller's SET or RESET of a pinned setting does to the values the client returns, constant or stored;
// and the recorded values once stored, read back raw and through the client. Every case reads only
// literals or system views and writes only session settings, except the [writes] cases, which store
// SUPER numbers, the setting probes' values or the recorded values in session temporary tables.

// catch.hpp comes first, as in this directory's other tests.
#include "catch.hpp"

#include "fake_driver_fixture.hpp"
#include "live_workgroup.hpp"
#include "recorded_driver_output.hpp"
#include "recorded_output_checks.hpp"
#include "recorded_stored_output.hpp"
#include "session_probe.hpp"
#include "thrown.hpp"

#include <algorithm>
#include <cctype>
#include <cstdio>
#include <string>
#include <vector>

using namespace duckdb;
using redshift_driver::testing::FakeColumn;
using redshift_test::Classified;
using redshift_test::NodePath;
using redshift_test::RecordedSql;
using redshift_test::RunsDirect;
using redshift_test::SessionProbe;
using redshift_test::StoredRead;

namespace {

//! A client connection to the live workgroup on the embedded driver, whose handle SessionProbe keeps.
//! The members go in reverse, so the connection closes before the process state is reset.
struct ProbedConnection {
	ProbedConnection() {
		SetDriverSourceForTesting(&SessionProbe::Source);
		SessionProbe::Start(&redshift_driver::LoadEmbeddedDriver);
		connection = OpenRedshiftConnection(redshift_test::LiveWorkgroupConfig(),
		                                    make_shared_ptr<redshift_test::CapturingLogSink>());
	}

	redshift_test::ResetOnExit reset;
	redshift_test::LiveEndpoints endpoints;
	unique_ptr<RedshiftConnection> connection;
};

//! A column as one line, so a mismatch shows every field.
std::string ColumnText(const FakeColumn &column) {
	return column.name + " " + column.type_name + " " + std::to_string(column.sql_type) + " " +
	       std::to_string(column.column_size) + " " + std::to_string(column.decimal_digits) + " " +
	       std::to_string(column.nullable);
}

std::vector<std::string> ColumnTexts(const std::vector<FakeColumn> &columns) {
	std::vector<std::string> texts;
	for (const auto &column : columns) {
		texts.push_back(ColumnText(column));
	}
	return texts;
}

//! The one row `sql` returns on the probed session, run as `run` says. No recorded cell is NULL.
std::vector<std::string> OnlyRow(const std::string &sql, SessionProbe::Run run = SessionProbe::Run::PREPARED) {
	const auto rows = SessionProbe::Rows(sql, run);
	REQUIRE(rows.size() == 1);
	CHECK(std::find(rows[0].nulls.begin(), rows[0].nulls.end(), true) == rows[0].nulls.end());
	return rows[0].cells;
}

//! Checks every recorded select on both nodes against the row it was recorded with after
//! `caller_statement`, empty for none.
void CheckRecordedRows(const std::string &caller_statement) {
	for (const auto &select : redshift_test::RecordedSelects()) {
		for (const auto path : {NodePath::LEADER, NodePath::COMPUTE}) {
			INFO(RecordedSql(select, path));
			CHECK(OnlyRow(RecordedSql(select, path)) == redshift_test::RecordedRow(select, caller_statement, path));
		}
	}
}

//! A recorded row the simple protocol returns in another form, after its caller statement or, when
//! that is empty, after any: compute nodes write a BC date with an astronomical year, which the
//! client reads as the same date.
const std::vector<redshift_test::RecordedDifference> SIMPLE_PROTOCOL_DIFFERENCES {
    {"date",
     "",
     NodePath::COMPUTE,
     {"-4712-01-01", "-4713-11-24", "0000-12-31", "-043-03-15", "0001-01-01", "294276-12-31", "10000-01-01",
      "5874897-12-31"}}};

//! The recorded select whose compute-node run the server refuses on the simple protocol: an infinite
//! timestamp, which the client refuses on either protocol as outside the TIMESTAMP text form.
const char *const SIMPLE_PROTOCOL_REFUSED = "infinities";

//! Whether the client reads every one of `columns`; Execute refuses a statement with any other before it runs.
bool ClientReads(const std::vector<FakeColumn> &columns) {
	const auto classified = Classified(columns);
	return std::none_of(classified.begin(), classified.end(),
	                    [](const RedshiftColumn &column) { return column.type == RedshiftTypeId::UNKNOWN; });
}

//! The names of the recorded selects the client runs with SQLExecute.
std::vector<std::string> PreparedOnlySelects() {
	std::vector<std::string> names;
	for (const auto &select : redshift_test::RecordedSelects()) {
		if (!RunsDirect(select.result.columns)) {
			names.push_back(select.name);
		}
	}
	return names;
}

//! The row `select` returns on `path`'s node through SQLExecDirect after `caller_statement`, empty
//! for none: DifferenceRow's row from SIMPLE_PROTOCOL_DIFFERENCES, else RecordedRow's.
std::vector<std::string> SimpleProtocolRow(const redshift_test::RecordedSelect &select,
                                           const std::string &caller_statement, NodePath path) {
	const auto *row = redshift_test::DifferenceRow(SIMPLE_PROTOCOL_DIFFERENCES, select, caller_statement, path);
	return row ? *row : redshift_test::RecordedRow(select, caller_statement, path);
}

//! What `error` holds as "<type>: <message>", or empty when nothing was thrown. HasError decides, as a
//! std::exception that is not DuckDB's is INVALID-typed and its message may be empty.
std::string FailureText(const ErrorData &error) {
	return error.HasError() ? Exception::ExceptionTypeToString(error.Type()) + ": " + error.RawMessage() : "";
}

//! What the client returns for the one cell `sql` selects: the text or double it carries, or what
//! refused the batch.
std::string ClientCell(RedshiftConnection &connection, const std::string &sql) {
	std::string cell;
	const auto failure = FailureText(redshift_test::CaptureError([&] {
		auto result = connection.Execute(sql, {}, connection.Describe(sql));
		redshift_test::ReadBatches read(*result);
		REQUIRE(read.rows.size() == 1);
		const auto &value = read.rows[0][0];
		cell = value.text;
		if (value.kind == RedshiftValueKind::REAL) {
			char real[32];
			snprintf(real, sizeof(real), "%.17g", value.real);
			cell = real;
		}
	}));
	return failure.empty() ? cell : "refused: " + failure;
}

//! What the client throws running `sql` and reading every row, as FailureText gives it.
std::string ClientFailure(RedshiftConnection &connection, const std::string &sql) {
	return FailureText(redshift_test::CaptureError([&] {
		auto result = connection.Execute(sql, {}, connection.Describe(sql));
		redshift_test::ReadBatches read(*result);
	}));
}

//! Checks every recorded select the client runs with SQLExecDirect, on both nodes, after
//! `caller_statement`, empty for none: through the driver alone, and through the client, whose
//! check after the run sees the executed columns, which the driver alone does not.
void CheckSimpleProtocolRows(RedshiftConnection &connection, const std::string &caller_statement) {
	for (const auto &select : redshift_test::RecordedSelects()) {
		if (!RunsDirect(select.result.columns)) {
			continue;
		}
		for (const auto path : {NodePath::LEADER, NodePath::COMPUTE}) {
			const auto sql = RecordedSql(select, path);
			INFO(sql);
			const auto failure = ClientFailure(connection, sql);
			if (select.name == std::string(SIMPLE_PROTOCOL_REFUSED)) {
				if (path == NodePath::COMPUTE) {
					CHECK_THROWS_WITH(OnlyRow(sql, SessionProbe::Run::DIRECT), Catch::Contains("XX000"));
				} else {
					CHECK(OnlyRow(sql, SessionProbe::Run::DIRECT) == SimpleProtocolRow(select, caller_statement, path));
				}
				CHECK_FALSE(failure.empty());
				CHECK_THAT(failure, !Catch::Contains("changed on the server"));
				// Any exception reads as a failure, so each node's is pinned: the leader's as the client's
				// text-form refusal, the compute node's as the server's XX000.
				CHECK_THAT(failure, Catch::Contains(path == NodePath::LEADER ? "that is not in the TIMESTAMP text form"
				                                                             : "SQLSTATE: XX000"));
				continue;
			}
			CHECK(OnlyRow(sql, SessionProbe::Run::DIRECT) == SimpleProtocolRow(select, caller_statement, path));
			if (ClientReads(select.result.columns)) {
				CHECK(failure.empty());
			} else {
				CHECK_THAT(failure, Catch::Contains("has a type this client cannot read"));
			}
		}
	}
}

//! The values a caller's change to a pinned setting is read through that a table can store, each a
//! select list.
const std::vector<std::string> STORABLE_SETTING_PROBES {
    "'1.7976931348623157e308'::float8 AS largest", "'0.30000000000000004'::float8 AS sum_of_tenths",
    "'1883-11-18 12:00:00+00'::timestamptz AS before_zones", "'4713-01-01 BC'::date AS first_julian"};

//! STORABLE_SETTING_PROBES and an unqualified interval, which a table cannot store.
std::vector<std::string> AllSettingProbes() {
	auto probes = STORABLE_SETTING_PROBES;
	probes.push_back("INTERVAL '1 day 2 hours' AS day_and_hours");
	return probes;
}

//! The values a caller's change to a pinned setting is read through, each a select list.
const std::vector<std::string> SETTING_PROBES = AllSettingProbes();

//! ClientCell for each SETTING_PROBES value, from the leader node and then the compute node.
std::vector<std::string> SettingProbeCells(RedshiftConnection &connection) {
	std::vector<std::string> cells;
	for (const auto &probe : SETTING_PROBES) {
		cells.push_back(ClientCell(connection, "SELECT " + probe));
		cells.push_back(ClientCell(connection, "SELECT " + probe + redshift_test::COMPUTE_NODE_FROM));
	}
	return cells;
}

//! The statement failure a float cell whose text rounds past DOUBLE's range raises.
const std::string ROUNDS_PAST =
    "refused: Executor: Redshift returned a DOUBLE PRECISION value in column 1 whose text rounds past "
    "the type's range, so the batch was not returned; select the column as VARCHAR "
    "(::VARCHAR) to read it";

//! The statement failure a cell outside `type`'s text form raises.
std::string NotInForm(const std::string &type) {
	return "refused: Executor: Redshift returned a value in column 1 that is not in the " + type +
	       " text form, so the batch was not returned; if the session's datestyle was changed, run the statement "
	       "on a new connection, otherwise report it";
}

//! The compute nodes' DATE text on the simple protocol under each setting below, `SQL, DMY` among them:
//! ISO with an astronomical year, which the client reads as 4713-01-01 (BC), the date the other cells name.
const char *const COMPUTE_FIRST_JULIAN = "-4712-01-01";

//! SettingProbeCells under the pinned settings. A float or interval cell runs with SQLExecute, a
//! date cell with SQLExecDirect.
const std::vector<std::string> PINNED_CELLS {
    "1.7976931348623157e+308", "1.7976931348623157e+308", "0.30000000000000004", "0.30000000000000004",
    "1883-11-18 12:00:00+00",  "1883-11-18 12:00:00+00",  "4713-01-01 BC",       COMPUTE_FIRST_JULIAN,
    "1 day 02:00:00",          "1 day 02:00:00"};

//! A caller's change to one pinned setting, its RESET, and the probe cells read after each.
struct SettingChange {
	std::string set;
	std::string reset;
	std::vector<std::string> after_set;
	std::vector<std::string> after_reset;
};

//! Measured on a workgroup whose defaults are extra_float_digits 0, UTC, 'ISO, MDY' and postgres.
//! Only the float row shows a RESET does not restore the client's setting: the other defaults equal it.
const std::vector<SettingChange> SETTING_CHANGES {
    {"SET extra_float_digits TO 0",
     "RESET extra_float_digits",
     {ROUNDS_PAST, ROUNDS_PAST, "0.29999999999999999", "0.29999999999999999", "1883-11-18 12:00:00+00",
      "1883-11-18 12:00:00+00", "4713-01-01 BC", COMPUTE_FIRST_JULIAN, "1 day 02:00:00", "1 day 02:00:00"},
     {ROUNDS_PAST, ROUNDS_PAST, "0.29999999999999999", "0.29999999999999999", "1883-11-18 12:00:00+00",
      "1883-11-18 12:00:00+00", "4713-01-01 BC", COMPUTE_FIRST_JULIAN, "1 day 02:00:00", "1 day 02:00:00"}},
    {"SET timezone TO 'America/New_York'",
     "RESET timezone",
     {"1.7976931348623157e+308", "1.7976931348623157e+308", "0.30000000000000004", "0.30000000000000004",
      "1883-11-18 07:03:58-04:56", "1883-11-18 07:03:56-04:56", "4713-01-01 BC", COMPUTE_FIRST_JULIAN, "1 day 02:00:00",
      "1 day 02:00:00"},
     PINNED_CELLS},
    {"SET datestyle TO 'SQL, DMY'",
     "RESET datestyle",
     {"1.7976931348623157e+308", "1.7976931348623157e+308", "0.30000000000000004", "0.30000000000000004",
      NotInForm("TIMESTAMPTZ"), NotInForm("TIMESTAMPTZ"), NotInForm("DATE"), COMPUTE_FIRST_JULIAN, "1 day 02:00:00",
      "1 day 02:00:00"},
     PINNED_CELLS},
    {"SET intervalstyle TO 'postgres_verbose'",
     "RESET intervalstyle",
     {"1.7976931348623157e+308", "1.7976931348623157e+308", "0.30000000000000004", "0.30000000000000004",
      "1883-11-18 12:00:00+00", "1883-11-18 12:00:00+00", "4713-01-01 BC", COMPUTE_FIRST_JULIAN, "@ 1 day 2 hours",
      "@ 1 day 2 hours"},
     PINNED_CELLS}};

//! The name a select list gives its value, after its last " AS ".
std::string ProbeName(const std::string &probe) {
	return probe.substr(probe.rfind(" AS ") + 4);
}

//! The temporary table the stored probe case reads: STORABLE_SETTING_PROBES' values, and a date
//! whose day and month a DMY style would swap.
std::string StoredProbesCreate() {
	std::string sql = "CREATE TEMP TABLE stored_probes AS SELECT ";
	for (const auto &probe : STORABLE_SETTING_PROBES) {
		sql += probe + ", ";
	}
	return sql + "'2001-02-03'::date AS plain_date";
}

//! ClientCell for each stored column alone, then for the date beside a float, which makes the client
//! run it with SQLExecute.
std::vector<std::string> StoredProbeCells(RedshiftConnection &connection) {
	std::vector<std::string> reads;
	for (const auto &probe : STORABLE_SETTING_PROBES) {
		reads.push_back(ProbeName(probe));
	}
	reads.push_back("plain_date");
	reads.push_back("plain_date, sum_of_tenths");
	std::vector<std::string> cells;
	for (const auto &read : reads) {
		cells.push_back(ClientCell(connection, "SELECT " + read + " FROM stored_probes"));
	}
	return cells;
}

//! StoredProbeCells under the pinned settings: the dates read alone run with SQLExecDirect.
const std::vector<std::string> STORED_PINNED_CELLS {
    "1.7976931348623157e+308", "0.30000000000000004", "1883-11-18 12:00:00+00",
    COMPUTE_FIRST_JULIAN,      "2001-02-03",          "2001-02-03"};

//! StoredProbeCells after a date style other than ISO: a stored date read alone keeps its ISO form,
//! and beside a float, through SQLExecute, it fails the batch. A stored TIMESTAMPTZ fails even
//! read alone through SQLExecDirect: the compute nodes' fixed formatter covers a DATE, not a
//! TIMESTAMPTZ.
const std::vector<std::string> STORED_NON_ISO_CELLS {
    "1.7976931348623157e+308", "0.30000000000000004", NotInForm("TIMESTAMPTZ"),
    COMPUTE_FIRST_JULIAN,      "2001-02-03",          NotInForm("DATE")};

//! StoredProbeCells at extra_float_digits 0: the server shortens a stored float8 to 15 significant
//! digits, as it does a constant, so the largest value's text rounds past DOUBLE's range and the
//! stored `0.30000000000000004` arrives as `0.3`, which ClientCell gives back as 17 digits.
const std::vector<std::string> STORED_FLOAT_DIGITS_0_CELLS {
    ROUNDS_PAST, "0.29999999999999999", "1883-11-18 12:00:00+00", COMPUTE_FIRST_JULIAN, "2001-02-03", "2001-02-03"};

//! The SETTING_CHANGES statements, the other date styles too, with StoredProbeCells after each. A
//! RESET restores the workgroup's default, so only extra_float_digits stays changed after it. Stored,
//! the New York TIMESTAMPTZ prints as the leader's constant does, not as the compute nodes' does.
const std::vector<SettingChange> STORED_SETTING_CHANGES {
    {"SET extra_float_digits TO 0", "RESET extra_float_digits", STORED_FLOAT_DIGITS_0_CELLS,
     STORED_FLOAT_DIGITS_0_CELLS},
    {"SET timezone TO 'America/New_York'",
     "RESET timezone",
     {"1.7976931348623157e+308", "0.30000000000000004", "1883-11-18 07:03:58-04:56", COMPUTE_FIRST_JULIAN, "2001-02-03",
      "2001-02-03"},
     STORED_PINNED_CELLS},
    {"SET datestyle TO 'SQL, DMY'", "RESET datestyle", STORED_NON_ISO_CELLS, STORED_PINNED_CELLS},
    {"SET datestyle TO 'Postgres, DMY'", "RESET datestyle", STORED_NON_ISO_CELLS, STORED_PINNED_CELLS},
    {"SET datestyle TO 'German'", "RESET datestyle", STORED_NON_ISO_CELLS, STORED_PINNED_CELLS},
    {"SET intervalstyle TO 'postgres_verbose'", "RESET intervalstyle", STORED_PINNED_CELLS, STORED_PINNED_CELLS}};

//! A SUPER number as JSON text, and the text it arrives as on every path the [writes] cases read.
struct StoredSuperNumber {
	std::string json;
	std::string arrives_as;
};

//! Numbers that keep every digit, an integer past BIGINT's range among them.
const std::vector<StoredSuperNumber> EXACT_SUPER_NUMBERS {
    {"0.30000000000000004", "0.30000000000000004"},
    {"{\"a\":0.30000000000000004}", "{\"a\":0.30000000000000004}"},
    {"[0.30000000000000004]", "[0.30000000000000004]"},
    {"12345678901234567890", "12345678901234567890"}};

//! Numbers in exponent form, which arrive rounded on every path, the leader's included.
const std::vector<StoredSuperNumber> EXPONENT_SUPER_NUMBERS {{"1.7976931348623157e308", "1.79769313486232e+308"},
                                                             {"5e-324", "4.9406564584125e-324"},
                                                             {"2.2250738585072014e-308", "2.2250738585072e-308"}};

//! The digits of `number`, a nonzero mantissa with no leading zero, before any exponent.
size_t SignificantDigits(const std::string &number) {
	const auto mantissa = number.substr(0, number.find_first_of("eE"));
	return NumericCast<size_t>(std::count_if(mantissa.begin(), mantissa.end(),
	                                         [](char c) { return std::isdigit(static_cast<unsigned char>(c)); }));
}

//! Runs the caller's statement `sql`, which returns no rows, through the client.
void RunCallerStatement(RedshiftConnection &connection, const std::string &sql) {
	connection.Execute(sql, {}, connection.Describe(sql));
}

//! Stores each of `numbers` in a session temporary table, so that the compute nodes write it, and
//! checks it arrives as pinned through SQLExecDirect, SQLExecute and the client, and as the leader's
//! constant through both protocols.
void CheckStoredSuperNumbers(const std::vector<StoredSuperNumber> &numbers) {
	ProbedConnection probed;
	auto &connection = *probed.connection;
	RunCallerStatement(connection, "CREATE TEMP TABLE super_numbers (id INTEGER, s SUPER)");
	for (size_t id = 0; id < numbers.size(); id++) {
		RunCallerStatement(connection, "INSERT INTO super_numbers VALUES (" + std::to_string(id) + ", JSON_PARSE('" +
		                                   numbers[id].json + "'))");
	}
	// A compute-node scan, so the compute nodes write each value.
	const auto plan = SessionProbe::Rows("EXPLAIN SELECT s FROM super_numbers WHERE id = 0");
	REQUIRE_FALSE(plan.empty());
	CHECK(plan[0].cells.at(0).rfind("XN Seq Scan on super_numbers", 0) == 0);
	for (size_t id = 0; id < numbers.size(); id++) {
		const auto stored = "SELECT s FROM super_numbers WHERE id = " + std::to_string(id);
		const auto constant = "SELECT JSON_PARSE('" + numbers[id].json + "') AS s";
		const auto &expected = numbers[id].arrives_as;
		const std::vector<std::string> row {expected};
		INFO(stored);
		CHECK(RunsDirect(SessionProbe::Columns(stored)));
		CHECK(OnlyRow(stored, SessionProbe::Run::DIRECT) == row);
		CHECK(OnlyRow(stored) == row);
		CHECK(ClientCell(connection, stored) == expected);
		INFO(constant);
		CHECK(OnlyRow(constant, SessionProbe::Run::DIRECT) == row);
		CHECK(OnlyRow(constant) == row);
	}
}

//! Stores every recorded select the server stores, and checks the refusal of each it does not.
void StoreRecordedSelects(RedshiftConnection &connection) {
	for (const auto &select : redshift_test::RecordedSelects()) {
		const auto create = StoredCreate(select);
		const auto thrown = redshift_test::Capture([&] { RunCallerStatement(connection, create); });
		const auto *unstored = redshift_test::FindUnstored(select);
		INFO(create);
		CHECK(thrown ==
		      (unstored ? redshift_test::Thrown {ExceptionType::BINDER, unstored->refusal} : redshift_test::Thrown {}));
	}
	for (const auto &select : redshift_test::StoredOnlySelects()) {
		RunCallerStatement(connection, StoredCreate(select));
	}
}

//! Checks each stored table's row on either protocol against the recorded one after `caller_statement`.
void CheckStoredRows(const std::string &caller_statement) {
	for (const auto &select : redshift_test::StoredSelects()) {
		const auto sql = StoredSql(select);
		INFO(sql << " after " << caller_statement);
		CHECK(OnlyRow(sql) == StoredRow(select, caller_statement, StoredRead::PREPARED));
		if (redshift_test::StoredDirectRefused(select)) {
			CHECK_THROWS_WITH(OnlyRow(sql, SessionProbe::Run::DIRECT), Catch::Contains("XX000"));
		} else {
			CHECK(OnlyRow(sql, SessionProbe::Run::DIRECT) == StoredRow(select, caller_statement, StoredRead::DIRECT));
		}
	}
}

//! The one row the client returns for `sql`.
std::vector<RedshiftValue> ClientRow(RedshiftConnection &connection, const std::string &sql) {
	auto result = connection.Execute(sql, {}, connection.Describe(sql));
	redshift_test::ReadBatches read(*result);
	REQUIRE(read.rows.size() == 1);
	return read.rows[0];
}

//! How Redshift compares and counts a stored VARCHAR's trailing blanks, against `'xy'` and in LENGTH,
//! then a CHAR's against `'ab'`. A stored VARCHAR's blanks count in LENGTH but not in the comparison,
//! where a constant's count in both.
std::string StoredBlanksSql() {
	return "SELECT varchar_trailing = 'xy'::varchar, LENGTH(varchar_trailing), char_trailing = 'ab'::char(5) FROM " +
	       StoredTable(redshift_test::FindSelect("character", redshift_test::StoredSelects()));
}
const std::vector<std::string> STORED_BLANKS_ROW {"1", "4", "1"};

} // namespace

TEST_CASE("Live: the pinned driver describes each recorded statement as recorded, on either node", "[odbc][live]") {
	SKIP_UNLESS_LIVE(redshift_driver::LoadEmbeddedDriver());
	ProbedConnection probed;
	for (const auto &select : redshift_test::RecordedSelects()) {
		for (const auto path : {NodePath::LEADER, NodePath::COMPUTE}) {
			INFO(RecordedSql(select, path));
			CHECK(ColumnTexts(SessionProbe::Columns(RecordedSql(select, path))) == ColumnTexts(select.result.columns));
		}
	}
	for (const auto &describe : redshift_test::RecordedDescribes()) {
		INFO(describe.sql);
		CHECK(ColumnTexts(SessionProbe::Columns(describe.sql)) == ColumnTexts(describe.columns));
	}
}

TEST_CASE("Live: under the pinned settings each recorded value arrives as recorded, on either node", "[odbc][live]") {
	SKIP_UNLESS_LIVE(redshift_driver::LoadEmbeddedDriver());
	ProbedConnection probed;
	CHECK(OnlyRow("SHOW intervalstyle") == std::vector<std::string> {"postgres"});
	CheckRecordedRows("");
}

// The simple protocol returns each type the client sends through it as recorded, but for the
// compute nodes' BC date form and infinite timestamps, so every run checks the routing's list of types.
// The client's own check after each run also sees the executed sizes, which the driver alone does not.
TEST_CASE("Live: each recorded statement the client runs with SQLExecDirect arrives as recorded through it",
          "[odbc][live]") {
	SKIP_UNLESS_LIVE(redshift_driver::LoadEmbeddedDriver());
	ProbedConnection probed;
	CHECK(PreparedOnlySelects() ==
	      std::vector<std::string> {"float8", "float4", "unqualified_intervals", "workarounds"});
	CheckSimpleProtocolRows(*probed.connection, "");
}

// Redshift does not promise an object's key order, so a second and third read check it holds.
TEST_CASE("Live: a SUPER object keeps its key order across reads", "[odbc][live]") {
	SKIP_UNLESS_LIVE(redshift_driver::LoadEmbeddedDriver());
	ProbedConnection probed;
	const auto sql = "SELECT JSON_PARSE('{\"z\":1,\"a\":2,\"m\":3}')";
	for (int read = 0; read < 3; read++) {
		CHECK(OnlyRow(sql) == std::vector<std::string> {"{\"z\":1,\"a\":2,\"m\":3}"});
	}
}

TEST_CASE("Live: after a caller's SET each recorded value arrives as recorded, on either node", "[odbc][live]") {
	SKIP_UNLESS_LIVE(redshift_driver::LoadEmbeddedDriver());
	for (const auto &caller_statement : redshift_test::RECORDED_CALLER_STATEMENTS) {
		INFO(caller_statement);
		ProbedConnection probed;
		RunCallerStatement(*probed.connection, caller_statement);
		CheckRecordedRows(caller_statement);
		CheckSimpleProtocolRows(*probed.connection, caller_statement);
	}
}

TEST_CASE("Live: a caller's SET or RESET of a pinned setting holds for the session, which then holds state",
          "[odbc][live]") {
	SKIP_UNLESS_LIVE(redshift_driver::LoadEmbeddedDriver());
	{
		ProbedConnection pinned;
		CHECK(SettingProbeCells(*pinned.connection) == PINNED_CELLS);
	}
	for (const auto &change : SETTING_CHANGES) {
		INFO(change.set);
		ProbedConnection probed;
		auto &connection = *probed.connection;
		CHECK_FALSE(connection.SessionHoldsState());
		RunCallerStatement(connection, change.set);
		CHECK(connection.SessionHoldsState());
		CHECK(SettingProbeCells(connection) == change.after_set);
		RunCallerStatement(connection, change.reset);
		CHECK(SettingProbeCells(connection) == change.after_reset);
	}
}

// The values are stored under the pinned settings and read back after the caller's change, so each
// cell shows what the change does to a stored value. The CREATE already sets the session-state latch,
// so the constant SET or RESET case shows that a SET sets it. Tagged [writes] for the temporary table.
TEST_CASE("Live: a caller's SET or RESET of a pinned setting holds for stored values too", "[odbc][live][writes]") {
	SKIP_UNLESS_LIVE(redshift_driver::LoadEmbeddedDriver());
	for (const auto &change : STORED_SETTING_CHANGES) {
		INFO(change.set);
		ProbedConnection probed;
		auto &connection = *probed.connection;
		RunCallerStatement(connection, StoredProbesCreate());
		CHECK(connection.SessionHoldsState());
		CHECK(StoredProbeCells(connection) == STORED_PINNED_CELLS);
		RunCallerStatement(connection, change.set);
		// A stored TIMESTAMPTZ reads the same on either protocol, so its cell cannot show its route:
		// these check that the TIMESTAMPTZ alone runs with SQLExecDirect, the date beside a float with
		// SQLExecute.
		const auto zoned = SessionProbe::Columns("SELECT before_zones FROM stored_probes");
		REQUIRE(zoned.size() == 1);
		CHECK(RunsDirect(zoned));
		CHECK_FALSE(RunsDirect(SessionProbe::Columns("SELECT plain_date, sum_of_tenths FROM stored_probes")));
		CHECK(StoredProbeCells(connection) == change.after_set);
		RunCallerStatement(connection, change.reset);
		CHECK(StoredProbeCells(connection) == change.after_reset);
	}
}

// The client runs a SUPER column with SQLExecDirect and returns its text unchecked, so a number the
// compute nodes write must arrive there as through SQLExecute. Tagged [writes] for the session
// temporary table, so a principal that may not write can leave it out (test/README.md).
TEST_CASE("Live: a stored SUPER number arrives the same through SQLExecDirect as through SQLExecute",
          "[odbc][live][writes]") {
	SKIP_UNLESS_LIVE(redshift_driver::LoadEmbeddedDriver());
	CheckStoredSuperNumbers(EXACT_SUPER_NUMBERS);
}

// The server prints a SUPER number in exponent form with at most 15 significant digits, whichever
// node or protocol writes it, so that loss is the server's, not the routing's.
TEST_CASE("Live: a stored SUPER number in exponent form arrives rounded to 15 or fewer significant digits on "
          "every path",
          "[odbc][live][writes]") {
	SKIP_UNLESS_LIVE(redshift_driver::LoadEmbeddedDriver());
	// The pinned texts are what every path must return, so their digit count is the server's.
	for (const auto &number : EXPONENT_SUPER_NUMBERS) {
		INFO(number.arrives_as);
		CHECK(SignificantDigits(number.arrives_as) <= 15);
	}
	CheckStoredSuperNumbers(EXPONENT_SUPER_NUMBERS);
}

// The stored cases store the recorded values in session temporary tables, one session per case and
// per caller statement; recorded_stored_output.hpp holds what each table reads back as.
TEST_CASE("Live: each recorded select stored in a temporary table is described and read back as recorded",
          "[odbc][live][writes]") {
	SKIP_UNLESS_LIVE(redshift_driver::LoadEmbeddedDriver());
	ProbedConnection probed;
	StoreRecordedSelects(*probed.connection);
	for (const auto &select : redshift_test::StoredSelects()) {
		INFO(StoredSql(select));
		CHECK(ColumnTexts(SessionProbe::Columns(StoredSql(select))) == ColumnTexts(select.result.columns));
	}
	CheckStoredRows("");
	CHECK(OnlyRow(StoredBlanksSql()) == STORED_BLANKS_ROW);
}

TEST_CASE("Live: after a caller's SET each stored value is read back as recorded", "[odbc][live][writes]") {
	SKIP_UNLESS_LIVE(redshift_driver::LoadEmbeddedDriver());
	for (const auto &caller_statement : redshift_test::RECORDED_CALLER_STATEMENTS) {
		INFO(caller_statement);
		ProbedConnection probed;
		StoreRecordedSelects(*probed.connection);
		RunCallerStatement(*probed.connection, caller_statement);
		CheckStoredRows(caller_statement);
	}
}

// The client runs a shape with SQLExecDirect or SQLExecute as RunsDirect says. Through SQLExecute a
// stored float's text under the pinned settings carries a float8's every digit and a float4's first 8;
// through SQLExecDirect a stored +infinity alone is a finite timestamp in its form, which the client
// returns as sent.
TEST_CASE("Live: the client returns each stored value as the driver sent it under the pinned settings",
          "[odbc][live][writes]") {
	SKIP_UNLESS_LIVE(redshift_driver::LoadEmbeddedDriver());
	ProbedConnection probed;
	auto &connection = *probed.connection;
	StoreRecordedSelects(connection);
	size_t checked = 0;
	for (const auto &select : redshift_test::StoredSelects()) {
		const auto sql = StoredSql(select);
		INFO(sql);
		if (!ClientReads(select.result.columns)) {
			CHECK_THAT(ClientFailure(connection, sql), Catch::Contains("has a type this client cannot read"));
			checked++;
			continue;
		}
		if (redshift_test::StoredDirectRefused(select)) {
			// The server fails the SQLExecDirect read with a class XX error, so the client raises an
			// executor error showing the first line of the server's message.
			CHECK_THAT(ClientFailure(connection, sql),
			           Catch::StartsWith("Executor: Redshift could not run the statement: ") &&
			               Catch::EndsWith("SQLSTATE: XX000"));
			checked++;
			continue;
		}
		const auto read = RunsDirect(select.result.columns) ? StoredRead::DIRECT : StoredRead::PREPARED;
		const auto row = StoredRow(select, "", read);
		const auto values = ClientRow(connection, sql);
		const auto columns = Classified(select.result.columns);
		REQUIRE(values.size() == row.size());
		for (size_t index = 0; index < row.size(); index++) {
			INFO("column " << columns[index].name << " " << row[index]);
			CHECK(redshift_test::CarriesRecordedCell(columns[index], row[index], values[index]));
			checked++;
		}
	}
	// Each stored select's readable columns, or 1 for a refused select, in StoredSelects' order.
	CHECK(checked == 9 + 9 + 8 + 9 + 8 + 7 + 6 + 4 + 7 + 1 + 12 + 6 + 1 + 1 + 9 + 1 + 1 + 1 + 2 + 1);
}
