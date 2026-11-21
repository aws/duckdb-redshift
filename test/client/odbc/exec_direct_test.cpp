// A statement with no parameters runs with SQLExecDirect once the prepared shape check has passed,
// and its executed result is described again before any fetch. A statement with parameters, or with
// a FLOAT, DOUBLE or size-0 VARCHAR column, runs with SQLExecute on a statement prepared for it.

// catch.hpp comes first, as in this directory's other tests.
#include "catch.hpp"

#include "fake_driver_fixture.hpp"
#include "thrown.hpp"

#include <algorithm>
#include <chrono>
#include <functional>
#include <future>
#include <memory>
#include <string>
#include <vector>

using namespace duckdb;
using redshift_driver::testing::FakeColumn;
using redshift_driver::testing::FakeDiag;
using redshift_driver::testing::FakeGate;
using redshift_driver::testing::FakeResultSet;
using redshift_test::FakeDriverFixture;
using redshift_test::ManualClock;
using redshift_test::ReadyWithin;
using redshift_test::Thrown;

namespace {

constexpr std::chrono::milliseconds EXPECTED_WAIT {10000};
const char *const QUERY = "SELECT a FROM t";
const char *const UPDATE = "UPDATE t SET a = 1";
const std::string MAY_HAVE_RUN = "The statement may have run; check before running it again.";

//! One VARCHAR column named `name`, declared `size` bytes long, holding `values`.
FakeResultSet Texts(const std::string &name, SQLULEN size, const std::vector<std::string> &values) {
	return redshift_test::OneColumn(redshift_test::VarcharColumn(name, size), values);
}

//! A column named `name` as the driver reports one of `type_name`.
FakeColumn Typed(const std::string &name, const std::string &type_name, SQLSMALLINT sql_type, SQLULEN size,
                 SQLSMALLINT digits = 0) {
	FakeColumn column;
	column.name = name;
	column.type_name = type_name;
	column.sql_type = sql_type;
	column.column_size = size;
	column.decimal_digits = digits;
	return column;
}

//! One row of the one `column`, holding `cell`.
FakeResultSet OneCell(const FakeColumn &column, const std::string &cell) {
	return redshift_test::OneColumn(column, {cell});
}

FakeResultSet Affected(SQLLEN rows) {
	FakeResultSet result;
	result.row_count = rows;
	return result;
}

//! The first column of every row `result` returns, read to its end.
std::vector<std::string> Cells(RedshiftResult &result) {
	std::vector<std::string> cells;
	const redshift_test::ReadBatches read(result);
	for (const auto &row : read.rows) {
		cells.push_back(row.at(0).text);
	}
	return cells;
}

//! The index of the first call to `entry_point` after the first `skipped`, or the call count.
size_t FirstCall(const FakeDriverFixture &fixture, const std::string &entry_point, size_t skipped) {
	const auto &calls = fixture.fake.Calls();
	for (size_t index = skipped; index < calls.size(); index++) {
		if (calls[index].entry_point == entry_point) {
			return index;
		}
	}
	return calls.size();
}

//! How many calls to `entry_point` came after the first `skipped`: the session setup runs its own
//! statements with SQLExecute.
size_t CountCallsAfter(const FakeDriverFixture &fixture, const std::string &entry_point, size_t skipped) {
	const auto names = fixture.CallsAfter(skipped);
	return NumericCast<size_t>(std::count(names.begin(), names.end(), entry_point));
}

//! The executor error for a statement whose shape the server changed between the check and the run.
Thrown ChangedAfterRun(const std::string &difference) {
	return Thrown {ExceptionType::EXECUTOR,
	               "the statement's result shape changed on the server after it was checked, so its rows were "
	               "not read; it may have run, so check before running it again: " +
	                   difference};
}

//! Releases a gate when it goes out of scope, so a held call never outlives a failed case.
struct ReleaseAtExit {
	std::shared_ptr<FakeGate> gate;
	~ReleaseAtExit() {
		gate->Release();
	}
};

//! Runs `run` on its own thread while SQLExecDirect is held, and scripts `changed` for `sql` before
//! the call goes on: the server changing the statement between the check and the run.
void RunWhileChanged(FakeDriverFixture &fixture, const std::string &sql, const FakeResultSet &changed,
                     const std::function<void()> &run) {
	auto gate = std::make_shared<FakeGate>();
	fixture.fake.BlockCall("SQLExecDirect", 1, gate);
	auto running = std::async(std::launch::async, run);
	bool held = false;
	{
		const ReleaseAtExit release {gate};
		held = gate->WaitUntilHeldFor(EXPECTED_WAIT);
		fixture.fake.AddResult(sql, {changed});
	}
	REQUIRE(held);
	REQUIRE(ReadyWithin(running, EXPECTED_WAIT));
	running.get();
}

} // namespace

TEST_CASE("A statement with no parameters runs with SQLExecDirect after SQLPrepare", "[odbc]") {
	FakeDriverFixture fixture;
	fixture.fake.AddResult(QUERY, {Texts("a", 16, {"x", "y"})});
	auto connection = fixture.OpenConnection();
	const auto before = fixture.fake.Calls().size();

	auto result = redshift_test::ExecuteStatement(*connection, QUERY, {});

	const auto prepared = FirstCall(fixture, "SQLPrepare", before);
	const auto direct = FirstCall(fixture, "SQLExecDirect", before);
	CHECK(prepared < direct);
	REQUIRE(direct < fixture.fake.Calls().size());
	CHECK(fixture.fake.Calls()[direct].detail == QUERY);
	CHECK(CountCallsAfter(fixture, "SQLExecute", before) == 0);
	CHECK(Cells(*result) == std::vector<std::string> {"x", "y"});
}

TEST_CASE("A statement with parameters keeps SQLExecute", "[odbc]") {
	FakeDriverFixture fixture;
	const char *const sql = "SELECT a FROM t WHERE b = $1";
	fixture.fake.AddResult(sql, {Texts("a", 16, {"x"})}, 1);
	auto connection = fixture.OpenConnection();
	const auto before = fixture.fake.Calls().size();

	auto result = redshift_test::ExecuteStatement(*connection, sql, {"7"});

	CHECK(fixture.CallsTo("SQLExecDirect") == 0);
	CHECK(CountCallsAfter(fixture, "SQLExecute", before) == 1);
	CHECK(Cells(*result) == std::vector<std::string> {"x"});
}

TEST_CASE("A run with SQLExecDirect that succeeds with information logs its records", "[odbc]") {
	FakeDriverFixture fixture;
	fixture.fake.AddResult(QUERY, {Texts("a", 16, {"x"})});
	auto connection = fixture.OpenConnection();
	fixture.fake.WarnCall("SQLExecDirect", 1, {{"01000", 0, "NOTICE:  ran", {}}, {"01000", 0, "NOTICE:  again", {}}});
	const auto logged = fixture.log_sink->Lines().size();

	auto result = redshift_test::ExecuteStatement(*connection, QUERY, {});

	const auto lines = fixture.log_sink->Lines();
	CHECK(std::vector<std::string>(lines.begin() + NumericCast<std::ptrdiff_t>(logged), lines.end()) ==
	      std::vector<std::string> {"Redshift driver diagnostic, SQLSTATE 01000",
	                                "Redshift driver diagnostic, SQLSTATE 01000"});
	CHECK(Cells(*result) == std::vector<std::string> {"x"});
}

TEST_CASE("A statement with no result that runs with SQLExecDirect reports the rows it affected", "[odbc]") {
	FakeDriverFixture fixture;
	// A DML statement that changed no row answers SQL_NO_DATA, which still counts as run.
	const SQLLEN affected = GENERATE(SQLLEN(3), SQLLEN(0));
	fixture.fake.AddResult(UPDATE, {Affected(affected)});
	auto connection = fixture.OpenConnection();

	auto result = redshift_test::ExecuteStatement(*connection, UPDATE, {});

	CHECK(fixture.CallsTo("SQLExecDirect") == 1);
	CHECK(result->RowCount() == affected);
	RedshiftBatch batch;
	CHECK_FALSE(result->NextBatch(batch));
	result.reset();
	CHECK(fixture.fake.AllocatedHandles() == "environment, connection");
}

TEST_CASE("A result of SQLExecDirect closed after its first batch frees its statement for the next", "[odbc]") {
	FakeDriverFixture fixture;
	fixture.fake.AddResult(QUERY, {Texts("a", 16, std::vector<std::string>(3 * 2048, "z"))});
	auto connection = fixture.OpenConnection();
	auto result = redshift_test::ExecuteStatement(*connection, QUERY, {});
	RedshiftBatch batch;
	REQUIRE(result->NextBatch(batch));

	result.reset();

	CHECK(fixture.fake.AllocatedHandles() == "environment, connection");
	auto next = redshift_test::ExecuteStatement(*connection, QUERY, {});
	CHECK(Cells(*next).size() == 3 * 2048);
}

TEST_CASE("A shape the server changes between the check and SQLExecDirect is an error saying it may have run",
          "[odbc]") {
	FakeDriverFixture fixture;
	fixture.fake.AddResult(QUERY, {Texts("a", 16, {"x"})});
	auto connection = fixture.OpenConnection();
	const auto bound = redshift_test::ScriptedShape(QUERY);
	ErrorData error;

	// The column is renamed between the check and the run.
	RunWhileChanged(fixture, QUERY, Texts("b", 16, {"x"}),
	                [&]() { error = redshift_test::CaptureError([&] { connection->Execute(QUERY, {}, bound); }); });

	CHECK(Thrown {error.Type(), error.RawMessage()} ==
	      ChangedAfterRun("column 1 was bound as \"a\" VARCHAR and is now \"b\" VARCHAR"));
	// shape_changed would tell the caller that nothing ran.
	CHECK(redshift_test::InfoOf(error, "redshift_error").empty());
	CHECK(fixture.fake.AllocatedHandles() == "environment, connection");
	CHECK(connection->SessionHoldsState());
	CHECK(connection->Describe(QUERY).columns.at(0).name == "b");
}

TEST_CASE("A statement bound with no result that returns rows once SQLExecDirect runs it is not read", "[odbc]") {
	FakeDriverFixture fixture;
	fixture.fake.AddResult(UPDATE, {Affected(1)});
	auto connection = fixture.OpenConnection();
	const auto bound = redshift_test::ScriptedShape(UPDATE);
	Thrown thrown;

	RunWhileChanged(fixture, UPDATE, Texts("a", 16, {"x"}),
	                [&]() { thrown = redshift_test::Capture([&] { connection->Execute(UPDATE, {}, bound); }); });

	CHECK(thrown == ChangedAfterRun("it was bound as a statement without a result set and now returns rows"));
	CHECK(fixture.fake.AllocatedHandles() == "environment, connection");
}

TEST_CASE("The executed result's description sizes its buffers, not the prepared one's", "[odbc]") {
	FakeDriverFixture fixture;
	fixture.fake.AddResult(QUERY, {Texts("a", 16, {"x"})});
	auto connection = fixture.OpenConnection();
	const auto bound = redshift_test::ScriptedShape(QUERY);
	unique_ptr<RedshiftResult> result;
	// The same shape, a VARCHAR, now declared longer, with a value the old buffer could not hold.
	const std::string longer(40, 'w');

	RunWhileChanged(fixture, QUERY, Texts("a", 64, {longer}),
	                [&]() { result = connection->Execute(QUERY, {}, bound); });

	REQUIRE(result);
	CHECK(Cells(*result) == std::vector<std::string> {longer});
}

TEST_CASE("A failure to describe what SQLExecDirect ran says the statement may have run", "[odbc]") {
	const std::string entry_point = GENERATE("SQLNumResultCols", "SQLDescribeCol", "SQLColAttribute");
	const bool dead = GENERATE(false, true);
	INFO(entry_point << (dead ? " on a lost connection" : " on a live connection"));
	FakeDriverFixture fixture;
	fixture.fake.AddResult(QUERY, {Texts("a", 16, {"x"})});
	auto connection = fixture.OpenConnection();
	const auto bound = redshift_test::ScriptedShape(QUERY);
	// The check makes each call once for the one column; the next describes what ran.
	fixture.fake.FailCall(entry_point.c_str(), NumericCast<int>(fixture.CallsTo(entry_point)) + 2, SQL_ERROR,
	                      {FakeDiag {"HY000", 0, "describe failed", {}}});
	fixture.fake.SetConnectionDead(dead);

	const auto thrown = redshift_test::Capture([&] { connection->Execute(QUERY, {}, bound); });

	CHECK(fixture.CallsTo("SQLExecDirect") == 1);
	CHECK(thrown.type == (dead ? ExceptionType::IO : ExceptionType::EXECUTOR));
	CHECK_THAT(thrown.message, Catch::Contains(dead ? "may already have taken effect" : MAY_HAVE_RUN));
	// A lost connection is closed with its statement.
	CHECK(fixture.fake.AllocatedHandles() == (dead ? "environment" : "environment, connection"));
}

TEST_CASE("A column name too long for its buffer once SQLExecDirect runs is an error saying it may have run",
          "[odbc]") {
	FakeDriverFixture fixture;
	fixture.fake.AddResult(QUERY, {Texts("a", 16, {"x"})});
	auto connection = fixture.OpenConnection();
	const auto bound = redshift_test::ScriptedShape(QUERY);
	Thrown thrown;

	RunWhileChanged(fixture, QUERY, Texts(std::string(256, 'n'), 16, {"x"}),
	                [&]() { thrown = redshift_test::Capture([&] { connection->Execute(QUERY, {}, bound); }); });

	CHECK(thrown == Thrown {ExceptionType::EXECUTOR, "Redshift reported a name longer than 255 bytes for column 1; "
	                                                 "alias the column to a shorter name. " +
	                                                     MAY_HAVE_RUN});
	CHECK(fixture.fake.AllocatedHandles() == "environment, connection");
}

TEST_CASE("A statement with no parameters whose columns keep their text runs with SQLExecDirect, prepared once",
          "[odbc]") {
	FakeDriverFixture fixture;
	ManualClock clock;
	FakeResultSet kept;
	// Every other type the client reads, a CHAR of reported size 0 among them, as a catalog's is.
	kept.columns = {Typed("i", "BIGINT", SQL_BIGINT, 19),
	                Typed("v", "CHARACTER VARYING", SQL_VARCHAR, 16),
	                Typed("n", "NUMERIC", SQL_NUMERIC, 15, 2),
	                Typed("d", "DATE", SQL_TYPE_DATE, 10),
	                Typed("t", "TIMESTAMP", SQL_TYPE_TIMESTAMP, 26, 6),
	                Typed("b", "BOOL", SQL_BIT, 1),
	                Typed("s", "SMALLINT", SQL_SMALLINT, 5),
	                Typed("c", "CHARACTER", SQL_CHAR, 0),
	                Typed("tm", "TIME", SQL_TYPE_TIME, 8),
	                Typed("tz", "TIMETZ", SQL_TYPE_TIME, 8),
	                Typed("tstz", "TIMESTAMPTZ", SQL_TYPE_TIMESTAMP, 26, 6),
	                Typed("vb", "VARBYTE", SQL_LONGVARBINARY, 3),
	                Typed("in", "INTEGER", SQL_INTEGER, 10),
	                Typed("j", "SUPER", SQL_LONGVARCHAR, 4194304)};
	kept.rows = {{"7", "x", "1.50", "-4712-01-01", "2024-02-29 13:14:15", "1", "5", "r", "12:34:56", "12:34:56+00",
	              "2024-02-29 13:14:15+00", "ab", "8", "[0.30000000000000004]"}};
	fixture.fake.AddResult(QUERY, {kept});
	auto connection = fixture.OpenConnection();
	const auto shape = connection->Describe(QUERY);
	const auto before = fixture.fake.Calls().size();

	auto result = connection->Execute(QUERY, {}, shape);

	CHECK(CountCallsAfter(fixture, "SQLPrepare", before) == 0);
	CHECK(CountCallsAfter(fixture, "SQLExecDirect", before) == 1);
	CHECK(CountCallsAfter(fixture, "SQLExecute", before) == 0);
	const redshift_test::ReadBatches read(*result);
	REQUIRE(read.rows.size() == 1);
	CHECK(read.rows[0].at(1).text == "x");
	// The simple protocol's date form, which the conversion reads as 4713-01-01 (BC).
	CHECK(read.rows[0].at(3).text == "-4712-01-01");
	CHECK(read.rows[0].at(13).text == "[0.30000000000000004]");
}

TEST_CASE("A FLOAT, DOUBLE or size-0 VARCHAR column runs with SQLExecute on a statement prepared again after Describe",
          "[odbc]") {
	// A VARCHAR of reported size 0 is how the driver reports an unqualified interval.
	const auto column = GENERATE(Typed("f", "REAL", SQL_REAL, 7), Typed("f", "DOUBLE PRECISION", SQL_DOUBLE, 15),
	                             Typed("f", "CHARACTER VARYING", SQL_VARCHAR, 0));
	INFO(column.type_name);
	FakeDriverFixture fixture;
	ManualClock clock;
	FakeResultSet mixed;
	mixed.columns = {Typed("i", "INTEGER", SQL_INTEGER, 10), column};
	mixed.rows = {{"7", "0.30000000000000004"}};
	fixture.fake.AddResult(QUERY, {mixed});
	auto connection = fixture.OpenConnection();
	const auto shape = connection->Describe(QUERY);
	// Describe frees a statement it would not let Execute take.
	CHECK(fixture.fake.AllocatedHandles() == "environment, connection");
	const auto before = fixture.fake.Calls().size();

	auto result = connection->Execute(QUERY, {}, shape);

	CHECK(CountCallsAfter(fixture, "SQLPrepare", before) == 1);
	CHECK(FirstCall(fixture, "SQLPrepare", before) < FirstCall(fixture, "SQLExecute", before));
	CHECK(CountCallsAfter(fixture, "SQLExecute", before) == 1);
	CHECK(fixture.CallsTo("SQLExecDirect") == 0);
	const redshift_test::ReadBatches read(*result);
	REQUIRE(read.rows.size() == 1);
	CHECK(read.rows[0].at(0).integer == 7);
}

TEST_CASE("A column that becomes a DOUBLE between the check and SQLExecDirect is an error saying it may have run",
          "[odbc]") {
	FakeDriverFixture fixture;
	fixture.fake.AddResult(QUERY, {OneCell(Typed("a", "INTEGER", SQL_INTEGER, 10), "7")});
	auto connection = fixture.OpenConnection();
	const auto bound = redshift_test::ScriptedShape(QUERY);
	Thrown thrown;

	RunWhileChanged(fixture, QUERY, OneCell(Typed("a", "DOUBLE PRECISION", SQL_DOUBLE, 15), "0.3"),
	                [&]() { thrown = redshift_test::Capture([&] { connection->Execute(QUERY, {}, bound); }); });

	CHECK(thrown == ChangedAfterRun("column 1 was bound as \"a\" INTEGER and is now \"a\" DOUBLE"));
	CHECK(fixture.fake.AllocatedHandles() == "environment, connection");
}

TEST_CASE("A VARCHAR whose size becomes 0 between the check and SQLExecDirect is an error saying it may have run",
          "[odbc]") {
	FakeDriverFixture fixture;
	fixture.fake.AddResult(QUERY, {Texts("a", 16, {"x"})});
	auto connection = fixture.OpenConnection();
	const auto bound = redshift_test::ScriptedShape(QUERY);
	Thrown thrown;

	RunWhileChanged(fixture, QUERY, Texts("a", 0, {"93600000000"}),
	                [&]() { thrown = redshift_test::Capture([&] { connection->Execute(QUERY, {}, bound); }); });

	CHECK(thrown ==
	      ChangedAfterRun("column 1, \"a\", is now a VARCHAR the driver reports with no length, such as an interval"));
	CHECK(fixture.fake.AllocatedHandles() == "environment, connection");
}

TEST_CASE("A VARCHAR whose size becomes 0 after the check is named by its position and name", "[odbc]") {
	const std::string sql = "SELECT a, b FROM t";
	FakeResultSet checked;
	checked.columns = {redshift_test::VarcharColumn("a", 16), redshift_test::VarcharColumn("b", 16)};
	checked.rows = {{"x", "y"}};
	FakeDriverFixture fixture;
	fixture.fake.AddResult(sql, {checked});
	auto connection = fixture.OpenConnection();
	const auto bound = redshift_test::ScriptedShape(sql);
	auto executed = checked;
	executed.columns[1].column_size = 0;
	executed.rows = {{"x", "93600000000"}};
	Thrown thrown;

	RunWhileChanged(fixture, sql, executed,
	                [&]() { thrown = redshift_test::Capture([&] { connection->Execute(sql, {}, bound); }); });

	CHECK(thrown ==
	      ChangedAfterRun("column 2, \"b\", is now a VARCHAR the driver reports with no length, such as an interval"));
}
