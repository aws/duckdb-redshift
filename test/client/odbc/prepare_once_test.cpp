// The statement Describe prepares, taken by an Execute of the same text with no parameters within a
// second of its prepare instead of a second server prepare, on one connection to the fake driver;
// one with parameters is prepared again. Each case sets the connection's clock, standing still or
// counting prepares, so wall time never decides the bound.

// catch.hpp comes first, as in this directory's other tests.
#include "catch.hpp"

#include "driver_source_hook.hpp"
#include "extension_database.hpp"
#include "fake_driver_fixture.hpp"
#include "odbc/odbc_connection.hpp"
#include "thrown.hpp"

#include <algorithm>
#include <chrono>
#include <string>
#include <vector>

using namespace duckdb;
using redshift_driver::testing::FakeColumn;
using redshift_driver::testing::FakeDiag;
using redshift_driver::testing::FakeResultSet;
using redshift_test::ATTACH_RS;
using redshift_test::ExtensionDatabase;
using redshift_test::FakeDriverFixture;
using redshift_test::ManualClock;
using redshift_test::Thrown;

namespace {

//! A query with no marker, and another statement.
const char *const QUERY = "SELECT a FROM t";
const char *const OTHER = "SELECT a FROM u";
//! A query with one marker; an Execute given its parameter prepares it again however soon it follows Describe.
const char *const WITH_MARKER = "SELECT a FROM t WHERE a = $1";

//! One INTEGER column `a` holding 7, in one row.
FakeResultSet Seven() {
	FakeColumn column;
	column.name = "a";
	column.type_name = "INTEGER";
	column.sql_type = SQL_INTEGER;
	column.column_size = 10;
	return redshift_test::OneColumn(column, {"7"});
}

//! The calls a run of QUERY's kept statement makes: the marker count, the description, the run and
//! the description of what ran, and no allocation or prepare.
const std::vector<std::string> KEPT_RUN {"SQLNumParams",  "SQLNumResultCols", "SQLDescribeCol", "SQLColAttribute",
                                         "SQLExecDirect", "SQLNumResultCols", "SQLDescribeCol", "SQLColAttribute"};

//! The calls a call that frees the kept statement and prepares its own starts with.
const std::vector<std::string> FREED_THEN_PREPARED {"SQLFreeHandle", "SQLAllocHandle", "SQLPrepare"};

//! How many times the fake prepared exactly `sql`.
size_t PreparesOf(const FakeDriverFixture &fixture, const std::string &sql) {
	const auto details = fixture.DetailsOf("SQLPrepare");
	return NumericCast<size_t>(std::count(details.begin(), details.end(), sql));
}

//! The first `count` calls the fake received after the first `skipped`, or all of them when fewer.
std::vector<std::string> FirstCallsAfter(const FakeDriverFixture &fixture, size_t skipped, size_t count) {
	auto calls = fixture.CallsAfter(skipped);
	calls.resize(std::min(calls.size(), count));
	return calls;
}

//! The one value of the one row `result` returns.
int32_t OnlyInteger(RedshiftResult &result) {
	RedshiftBatch batch;
	REQUIRE(result.NextBatch(batch));
	REQUIRE(batch.rows.size() == 1);
	return NumericCast<int32_t>(batch.rows[0][0].integer);
}

} // namespace

TEST_CASE("An Execute with no parameters takes the statement Describe just prepared, without preparing it again",
          "[odbc]") {
	FakeDriverFixture fixture;
	ManualClock clock;
	fixture.fake.AddResult(QUERY, {Seven()});
	auto connection = fixture.OpenConnection();
	const auto shape = connection->Describe(QUERY);
	const auto before = fixture.fake.Calls().size();

	auto result = connection->Execute(QUERY, {}, shape);
	CHECK(fixture.CallsAfter(before) == KEPT_RUN);
	CHECK(PreparesOf(fixture, QUERY) == 1);
	CHECK(OnlyInteger(*result) == 7);
}

TEST_CASE("The next Describe frees the kept statement before it prepares", "[odbc]") {
	FakeDriverFixture fixture;
	ManualClock clock;
	fixture.fake.AddResult(QUERY, {Seven()});
	fixture.fake.AddResult(OTHER, {Seven()});
	auto connection = fixture.OpenConnection();
	connection->Describe(QUERY);
	const auto before = fixture.fake.Calls().size();

	connection->Describe(OTHER);
	CHECK(FirstCallsAfter(fixture, before, 2) == std::vector<std::string> {"SQLFreeHandle", "SQLAllocHandle"});
	CHECK(fixture.fake.AllocatedHandles() == "environment, connection, statement");
}

TEST_CASE("Execute of other text frees the kept statement before it prepares its own", "[odbc]") {
	FakeDriverFixture fixture;
	ManualClock clock;
	fixture.fake.AddResult(QUERY, {Seven()});
	fixture.fake.AddResult(OTHER, {Seven()});
	auto connection = fixture.OpenConnection();
	connection->Describe(QUERY);
	const auto before = fixture.fake.Calls().size();

	auto result = redshift_test::ExecuteStatement(*connection, OTHER, {});
	CHECK(FirstCallsAfter(fixture, before, 3) == FREED_THEN_PREPARED);
	CHECK(fixture.fake.AllocatedHandles() == "environment, connection, statement");
}

TEST_CASE("A kept statement another call freed is not freed again at teardown", "[odbc]") {
	FakeDriverFixture fixture;
	ManualClock clock;
	fixture.fake.AddResult(QUERY, {Seven()});
	fixture.fake.AddResult(OTHER, {Seven()});
	auto connection = fixture.OpenConnection();
	connection->Describe(QUERY);
	redshift_test::ExecuteStatement(*connection, OTHER, {}).reset();
	const auto closing = fixture.fake.Calls().size();

	connection.reset();
	CHECK(fixture.CallsAfter(closing) == std::vector<std::string> {"SQLEndTran", "SQLDisconnect", "SQLFreeHandle"});
}

TEST_CASE("Execute a second or more after Describe's prepare prepares the statement again", "[odbc]") {
	FakeDriverFixture fixture;
	ManualClock clock;
	fixture.fake.AddResult(QUERY, {Seven()});
	auto connection = fixture.OpenConnection();

	const auto shape = connection->Describe(QUERY);
	clock.Advance(DESCRIBED_STATEMENT_REUSE - std::chrono::milliseconds(1));
	connection->Execute(QUERY, {}, shape).reset();
	CHECK(PreparesOf(fixture, QUERY) == 1);

	connection->Describe(QUERY);
	clock.Advance(DESCRIBED_STATEMENT_REUSE);
	const auto before = fixture.fake.Calls().size();
	auto prepared = connection->Execute(QUERY, {}, shape);
	CHECK(FirstCallsAfter(fixture, before, 3) == FREED_THEN_PREPARED);
	CHECK(PreparesOf(fixture, QUERY) == 3);
	CHECK(OnlyInteger(*prepared) == 7);
}

TEST_CASE("The second counts from when Describe sent its prepare, not from its answer", "[odbc]") {
	FakeDriverFixture fixture;
	// Each prepare takes the whole second.
	SetConnectClockForTesting([&fixture] {
		return std::chrono::steady_clock::time_point() +
		       DESCRIBED_STATEMENT_REUSE * NumericCast<int64_t>(fixture.CallsTo("SQLPrepare"));
	});
	fixture.fake.AddResult(QUERY, {Seven()});
	auto connection = fixture.OpenConnection();
	const auto shape = connection->Describe(QUERY);
	const auto before = fixture.fake.Calls().size();

	auto result = connection->Execute(QUERY, {}, shape);
	CHECK(FirstCallsAfter(fixture, before, 3) == FREED_THEN_PREPARED);
}

TEST_CASE("Each Describe starts the second again", "[odbc]") {
	FakeDriverFixture fixture;
	ManualClock clock;
	fixture.fake.AddResult(QUERY, {Seven()});
	auto connection = fixture.OpenConnection();
	connection->Describe(QUERY);
	clock.Advance(2 * DESCRIBED_STATEMENT_REUSE);
	const auto shape = connection->Describe(QUERY);
	const auto before = fixture.fake.Calls().size();

	auto result = connection->Execute(QUERY, {}, shape);
	CHECK(fixture.CallsAfter(before) == KEPT_RUN);
}

TEST_CASE("Execute of the text Describe just prepared, with parameters, prepares it again", "[odbc]") {
	FakeDriverFixture fixture;
	ManualClock clock;
	fixture.fake.AddResult(WITH_MARKER, {Seven()}, 1);
	auto connection = fixture.OpenConnection();
	const auto shape = connection->Describe(WITH_MARKER);
	const auto before = fixture.fake.Calls().size();

	auto result = connection->Execute(WITH_MARKER, {"7"}, shape);
	CHECK(FirstCallsAfter(fixture, before, 3) == FREED_THEN_PREPARED);
	CHECK(PreparesOf(fixture, WITH_MARKER) == 2);
	CHECK(fixture.CallsTo("SQLExecDirect") == 0);
	CHECK(OnlyInteger(*result) == 7);
}

TEST_CASE("A statement with parameters whose shape changes inside the second is refused before it runs", "[odbc]") {
	FakeDriverFixture fixture;
	ManualClock clock;
	fixture.fake.AddResult(WITH_MARKER, {Seven()}, 1);
	auto connection = fixture.OpenConnection();
	const auto bound = connection->Describe(WITH_MARKER);
	// Describe saw column a; the server now names it b, which the prepare at Execute reads.
	auto changed = Seven();
	changed.columns[0].name = "b";
	fixture.fake.AddResult(WITH_MARKER, {changed}, 1);
	const auto executes = fixture.CallsTo("SQLExecute");

	const auto error = redshift_test::CaptureError([&] { connection->Execute(WITH_MARKER, {"7"}, bound); });
	CHECK(error.Type() == ExceptionType::BINDER);
	CHECK(error.ExtraInfo() == unordered_map<string, string> {{"redshift_error", "shape_changed"}});
	CHECK(fixture.CallsTo("SQLExecute") == executes);
	CHECK(fixture.fake.AllocatedHandles() == "environment, connection");
	CHECK_FALSE(connection->SessionHoldsState());
}

TEST_CASE("The shape check refuses a bound shape that differs from the kept statement's, freeing it", "[odbc]") {
	FakeDriverFixture fixture;
	ManualClock clock;
	fixture.fake.AddResult(QUERY, {Seven()});
	auto connection = fixture.OpenConnection();
	auto bound = connection->Describe(QUERY);
	bound.columns[0].type = RedshiftTypeId::BIGINT;
	const auto runs = fixture.CallsTo("SQLExecDirect");

	const auto error = redshift_test::CaptureError([&] { connection->Execute(QUERY, {}, bound); });
	CHECK(Thrown {error.Type(), error.RawMessage()} ==
	      Thrown {ExceptionType::BINDER, "the statement's result shape is not the one it was bound with, so it was "
	                                     "not run: column 1 was bound as \"a\" BIGINT and is now \"a\" INTEGER"});
	CHECK(error.ExtraInfo() == unordered_map<string, string> {{"redshift_error", "shape_changed"}});
	CHECK(fixture.CallsTo("SQLExecDirect") == runs);
	CHECK(PreparesOf(fixture, QUERY) == 1);
	CHECK(fixture.fake.AllocatedHandles() == "environment, connection");
	CHECK_FALSE(connection->SessionHoldsState());
}

TEST_CASE("The kept statement's marker count is checked before anything is bound, freeing it", "[odbc]") {
	FakeDriverFixture fixture;
	ManualClock clock;
	fixture.fake.AddResult(WITH_MARKER, {Seven()}, 1);
	auto connection = fixture.OpenConnection();
	const auto shape = connection->Describe(WITH_MARKER);

	// No parameters are given, so Execute takes the kept statement and finds its marker.
	CHECK(redshift_test::Capture([&] { connection->Execute(WITH_MARKER, {}, shape); }) ==
	      Thrown {ExceptionType::INVALID_INPUT, "the statement has 1 parameter marker(s) but 0 parameter(s) were "
	                                            "given; pass exactly one parameter for each marker, in marker order"});
	CHECK(fixture.CallsTo("SQLBindParameter") == 0);
	CHECK(PreparesOf(fixture, WITH_MARKER) == 1);
	CHECK(fixture.fake.AllocatedHandles() == "environment, connection");
}

TEST_CASE("A failed run of the kept statement frees it, leaving the connection usable", "[odbc]") {
	FakeDriverFixture fixture;
	ManualClock clock;
	fixture.fake.AddResult(QUERY, {Seven()});
	auto connection = fixture.OpenConnection();
	const auto shape = connection->Describe(QUERY);
	fixture.FailNextCall("SQLExecDirect", SQL_ERROR, {FakeDiag {"42P01", 0, "relation \"t\" does not exist"}});

	const auto failed = redshift_test::Capture([&] { connection->Execute(QUERY, {}, shape); });
	CHECK(failed.type == ExceptionType::EXECUTOR);
	CHECK_THAT(failed.message, Catch::Contains("relation \"t\" does not exist"));
	CHECK(fixture.fake.AllocatedHandles() == "environment, connection");
	auto result = redshift_test::ExecuteStatement(*connection, QUERY, {});
	CHECK(OnlyInteger(*result) == 7);
}

TEST_CASE("A connection lost while the kept statement runs frees it once, before the connection", "[odbc]") {
	FakeDriverFixture fixture;
	ManualClock clock;
	fixture.fake.AddResult(QUERY, {Seven()});
	auto connection = fixture.OpenConnection();
	const auto shape = connection->Describe(QUERY);
	const auto frees_before = fixture.DetailsOf("SQLFreeHandle").size();
	fixture.FailNextCall("SQLExecDirect", SQL_ERROR, {FakeDiag {"08S01", 0, "server closed the connection"}});
	fixture.fake.SetConnectionDead(true);

	CHECK(redshift_test::Capture([&] { connection->Execute(QUERY, {}, shape); }).type == ExceptionType::IO);
	CHECK(fixture.fake.AllocatedHandles() == "environment");
	const auto frees = fixture.DetailsOf("SQLFreeHandle");
	CHECK(std::vector<std::string>(frees.begin() + NumericCast<std::ptrdiff_t>(frees_before), frees.end()) ==
	      std::vector<std::string> {"statement", "connection"});
	const auto closed = fixture.fake.Calls().size();
	connection.reset();
	CHECK(fixture.CallsAfter(closed).empty());
}

TEST_CASE("A kept statement whose free fails is abandoned, never freed again", "[odbc]") {
	FakeDriverFixture fixture;
	ManualClock clock;
	fixture.fake.AddResult(QUERY, {Seven()});
	fixture.fake.AddResult(OTHER, {Seven()});
	auto connection = fixture.OpenConnection();
	connection->Describe(QUERY);
	const auto before = fixture.fake.Calls().size();
	fixture.FailNextCall("SQLFreeHandle", SQL_ERROR, {});

	redshift_test::ExecuteStatement(*connection, OTHER, {}).reset();
	CHECK(FirstCallsAfter(fixture, before, 3) == FREED_THEN_PREPARED);
	connection.reset();
	CHECK(fixture.fake.MisusedAbandonedHandles().empty());
}

TEST_CASE("An Execute refused before any driver call leaves the kept statement for the next", "[odbc]") {
	FakeDriverFixture fixture;
	ManualClock clock;
	fixture.fake.AddResult(QUERY, {Seven()});
	auto connection = fixture.OpenConnection();
	const auto shape = connection->Describe(QUERY);
	const auto before = fixture.fake.Calls().size();

	CHECK(redshift_test::Capture([&] { connection->Execute(QUERY, {string("7\0", 2)}, shape); }).type ==
	      ExceptionType::INVALID_INPUT);
	CHECK(fixture.fake.Calls().size() == before);
	auto result = connection->Execute(QUERY, {}, shape);
	CHECK(fixture.CallsAfter(before) == KEPT_RUN);
}

TEST_CASE("A Describe refused before any driver call leaves the kept statement for the next Execute", "[odbc]") {
	FakeDriverFixture fixture;
	ManualClock clock;
	fixture.fake.AddResult(QUERY, {Seven()});
	auto connection = fixture.OpenConnection();
	const auto shape = connection->Describe(QUERY);
	const auto before = fixture.fake.Calls().size();

	CHECK(redshift_test::Capture([&] { connection->Describe(string("SELECT 1\0", 9)); }).type ==
	      ExceptionType::INVALID_INPUT);
	CHECK(fixture.fake.Calls().size() == before);
	auto result = connection->Execute(QUERY, {}, shape);
	CHECK(fixture.CallsAfter(before) == KEPT_RUN);
}

TEST_CASE("redshift_query prepares its statement once, at bind", "[odbc]") {
	FakeDriverFixture fixture;
	ManualClock clock;
	const std::string sql = "SELECT a FROM t";
	fixture.fake.AddResult(sql, {Seven()});
	ExtensionDatabase database;
	database.Run(ATTACH_RS);

	auto read = database.Run("SELECT * FROM redshift_query('rs', '" + sql + "')");
	CHECK(redshift_test::FirstColumn(*read) == std::vector<std::string> {"7"});
	CHECK(PreparesOf(fixture, sql) == 1);
	// The scan has no parameters, so it runs with SQLExecDirect; the setup's statements do not.
	CHECK(fixture.CallsTo("SQLExecute") == redshift_test::SessionSetupStatements().size());
	CHECK(fixture.CallsTo("SQLExecDirect") == 1);
}

TEST_CASE("redshift_query with params prepares its statement at bind and again to run it", "[odbc]") {
	FakeDriverFixture fixture;
	ManualClock clock;
	fixture.fake.AddResult(WITH_MARKER, {Seven()}, 1);
	ExtensionDatabase database;
	database.Run(ATTACH_RS);

	auto read = database.Run(std::string("SELECT * FROM redshift_query('rs', '") + WITH_MARKER + "', params := ['7'])");
	CHECK(redshift_test::FirstColumn(*read) == std::vector<std::string> {"7"});
	CHECK(PreparesOf(fixture, WITH_MARKER) == 2);
	CHECK(fixture.CallsTo("SQLExecute") == redshift_test::SessionSetupStatements().size() + 1);
}

TEST_CASE("redshift_query with no params and a DOUBLE column prepares at bind and again to run", "[odbc]") {
	FakeDriverFixture fixture;
	ManualClock clock;
	const std::string sql = "SELECT d FROM t";
	FakeColumn column;
	column.name = "d";
	column.type_name = "DOUBLE PRECISION";
	column.sql_type = SQL_DOUBLE;
	column.column_size = 15;
	fixture.fake.AddResult(sql, {redshift_test::OneColumn(column, {"0.5"})});
	ExtensionDatabase database;
	database.Run(ATTACH_RS);

	auto read = database.Run("SELECT * FROM redshift_query('rs', '" + sql + "')");
	CHECK(redshift_test::FirstColumn(*read) == std::vector<std::string> {"0.5"});
	CHECK(PreparesOf(fixture, sql) == 2);
	CHECK(fixture.CallsTo("SQLExecDirect") == 0);
}

TEST_CASE("A PREPARE with params whose shape changes inside the second is refused at EXECUTE, running nothing",
          "[odbc]") {
	FakeDriverFixture fixture;
	ManualClock clock;
	fixture.fake.AddResult(WITH_MARKER, {Seven()}, 1);
	ExtensionDatabase database;
	database.Run(ATTACH_RS);
	database.Run(std::string("PREPARE p AS SELECT * FROM redshift_query('rs', '") + WITH_MARKER +
	             "', params := ['7'])");
	auto changed = Seven();
	changed.columns[0].name = "b";
	fixture.fake.AddResult(WITH_MARKER, {changed}, 1);
	clock.Advance(DESCRIBED_STATEMENT_REUSE - std::chrono::milliseconds(1));
	const auto executes = fixture.CallsTo("SQLExecute");

	auto refused = database.con.Query("EXECUTE p");
	REQUIRE(refused->HasError());
	CHECK(refused->GetErrorObject().Type() == ExceptionType::BINDER);
	CHECK_THAT(refused->GetErrorObject().RawMessage(),
	           Catch::Contains("now returns different columns than when it was bound, so this call was not run"));
	CHECK(PreparesOf(fixture, WITH_MARKER) == 2);
	CHECK(fixture.CallsTo("SQLExecute") == executes);
}

TEST_CASE("An EXECUTE within a second of PREPARE runs on PREPARE's statement, and the server runs its text afresh",
          "[odbc]") {
	FakeDriverFixture fixture;
	ManualClock clock;
	const std::string sql = "SELECT a FROM t";
	fixture.fake.AddResult(sql, {Seven()});
	ExtensionDatabase database;
	database.Run(ATTACH_RS);
	database.Run("PREPARE p AS SELECT * FROM redshift_query('rs', '" + sql + "')");
	// A script changed now is what a server change inside the second would be. The statement has no
	// parameters, so SQLExecDirect sends its text again and the server's current version runs.
	auto changed = Seven();
	changed.rows = {{"8"}};
	fixture.fake.AddResult(sql, {changed});
	clock.Advance(DESCRIBED_STATEMENT_REUSE - std::chrono::milliseconds(1));

	auto read = database.Run("EXECUTE p");
	CHECK(redshift_test::FirstColumn(*read) == std::vector<std::string> {"8"});
	CHECK(PreparesOf(fixture, sql) == 1);
}

TEST_CASE("A kept statement with no parameters whose shape changes inside the second is not read, and may have run",
          "[odbc]") {
	FakeDriverFixture fixture;
	ManualClock clock;
	const std::string sql = "SELECT a FROM t";
	fixture.fake.AddResult(sql, {Seven()});
	auto connection = fixture.OpenConnection();
	const auto bound = connection->Describe(sql);
	// The kept statement still has column a; the server's statement for the text now names it b.
	auto changed = Seven();
	changed.columns[0].name = "b";
	fixture.fake.AddResult(sql, {changed});
	clock.Advance(DESCRIBED_STATEMENT_REUSE - std::chrono::milliseconds(1));

	const auto thrown = redshift_test::Capture([&] { connection->Execute(sql, {}, bound); });

	CHECK(thrown == Thrown {ExceptionType::EXECUTOR,
	                        "the statement's result shape changed on the server after it was checked, so its rows "
	                        "were not read; it may have run, so check before running it again: column 1 was bound "
	                        "as \"a\" INTEGER and is now \"b\" INTEGER"});
	CHECK(PreparesOf(fixture, sql) == 1);
	CHECK(fixture.fake.AllocatedHandles() == "environment, connection");
	CHECK(connection->SessionHoldsState());
}
