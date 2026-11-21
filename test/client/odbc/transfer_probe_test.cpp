// The read-path probe: the test-only switch that sends a transfer setting the client does not,
// (Compression=off), its string rewrite, the row-array cap, and the counts and
// captures it records, on the fake driver.

// catch.hpp comes first, as in this directory's other tests.
#include "catch.hpp"

#include "fake_driver_fixture.hpp"
#include "odbc/connection_settings.hpp"
#include "transfer_probe.hpp"

#include <algorithm>
#include <stdexcept>
#include <string>
#include <vector>

using namespace duckdb;
using redshift_driver::testing::FakeColumn;
using redshift_driver::testing::FakeOdbcDriver;
using redshift_driver::testing::FakeResultSet;
using redshift_test::FakeDriverFixture;
using redshift_test::ReadBatches;
using redshift_test::TransferChanges;
using redshift_test::TransferProbe;

namespace {

const char *const QUERY = "SELECT c FROM t";

//! `count` INTEGER rows holding 0 to count - 1.
FakeResultSet Counting(size_t count) {
	FakeColumn column;
	column.name = "c";
	column.type_name = "INTEGER";
	column.sql_type = SQL_INTEGER;
	column.column_size = 10;
	std::vector<std::string> cells;
	for (size_t i = 0; i < count; i++) {
		cells.push_back(std::to_string(i));
	}
	return redshift_test::OneColumn(column, cells);
}

//! The fake's driver behind the probe, with `changes`.
void ProbeFake(TransferChanges changes) {
	SetDriverSourceForTesting(&TransferProbe::Source);
	TransferProbe::Start(&FakeOdbcDriver::Source, std::move(changes));
}

//! The connection string the client builds for the fixture's connection.
std::string ClientConnectionString(FakeDriverFixture &fixture) {
	return BuildConnectionString(redshift_test::UsEast1Config(), TrustSource {fixture.ca_bundle.path});
}

//! `text` with its one `from` replaced by `to`.
std::string ReplacedOnce(const std::string &text, const std::string &from, const std::string &to) {
	const auto at = text.find(from);
	REQUIRE(at != std::string::npos);
	REQUIRE(text.find(from, at + 1) == std::string::npos);
	return text.substr(0, at) + to + text.substr(at + from.size());
}

} // namespace

TEST_CASE("The probe with no changes passes the client's connection string through", "[odbc]") {
	FakeDriverFixture fixture;
	ProbeFake({});
	auto connection = fixture.OpenConnection();

	CHECK(fixture.fake.LastConnectionString() == ClientConnectionString(fixture));
}

TEST_CASE("The probe's compression switch changes only the Compression setting", "[odbc]") {
	FakeDriverFixture fixture;
	ProbeFake({{{"Compression", "off"}}});
	auto connection = fixture.OpenConnection();

	CHECK(fixture.fake.LastConnectionString() ==
	      ReplacedOnce(ClientConnectionString(fixture), "Compression={zstd};", "Compression={off};"));
}

TEST_CASE("The probe replaces several settings, the first and the last included", "[odbc]") {
	const std::string text = "Host={h};StreamingCursorRows={2048};UseDeclareFetch={0};LogLevel={0}";
	TransferChanges changes {{{"Host", "x"}, {"UseDeclareFetch", "1"}, {"LogLevel", "6"}}};

	CHECK(TransferProbe::Apply(text, changes) ==
	      "Host={x};StreamingCursorRows={2048};UseDeclareFetch={1};LogLevel={6}");
}

TEST_CASE("The probe refuses a setting the client does not send, a keyword inside another included", "[odbc]") {
	const std::string text = "Host={h};UseDeclareFetch={0}";

	CHECK_THROWS_WITH(TransferProbe::Apply(text, {{{"Fetch", "100"}}}), "the client sends no Fetch setting to replace");
	CHECK_THROWS_WITH(TransferProbe::Apply(text, {{{"DeclareFetch", "1"}}}),
	                  "the client sends no DeclareFetch setting to replace");
}

TEST_CASE("The probe's row-array cap limits each fetch, and every row still arrives in order", "[odbc]") {
	FakeDriverFixture fixture;
	fixture.fake.AddResult(QUERY, {Counting(10)});
	TransferChanges changes;
	changes.row_array_cap = 3;
	ProbeFake(changes);
	auto connection = fixture.OpenConnection();
	ReadBatches read(*redshift_test::ExecuteStatement(*connection, QUERY, {}));

	REQUIRE(read.rows.size() == 10);
	for (size_t row = 0; row < read.rows.size(); row++) {
		CHECK(read.rows[row][0].integer == int64_t(row));
	}
	const auto sizes = fixture.DetailsOf("SQLSetStmtAttr");
	CHECK(std::count(sizes.begin(), sizes.end(), "27=3") == 1);
	// Four arrays of 3, 3, 3 and 1 rows, then the end.
	CHECK(TransferProbe::Timing().fetch_calls == 5);
	CHECK(fixture.CallsTo("SQLFetch") == 5);
	// Not asked to, the probe makes no call of its own.
	CHECK(fixture.CallsTo("SQLSetPos") == 0);
}

TEST_CASE("The probe's row-array cap leaves a smaller row array as the client set it", "[odbc]") {
	FakeDriverFixture fixture;
	fixture.fake.AddResult(QUERY, {Counting(10)});
	TransferChanges changes;
	changes.row_array_cap = 4096;
	ProbeFake(changes);
	auto connection = fixture.OpenConnection();
	ReadBatches read(*redshift_test::ExecuteStatement(*connection, QUERY, {}));

	CHECK(read.rows.size() == 10);
	// The client's own row array, as bulk_fetch_test.cpp's bounded case sets it.
	CHECK(fixture.DetailsOf("SQLSetStmtAttr") == std::vector<std::string> {"27=2048", "26", "25", "23"});
}

TEST_CASE("The probe counts each fetch and SQLGetData call the client makes", "[odbc]") {
	FakeDriverFixture fixture;
	FakeResultSet result;
	result.columns.push_back(redshift_test::LongVarcharColumn("c"));
	result.rows = {{"a"}, {"b"}};
	fixture.fake.AddResult(QUERY, {result});
	ProbeFake({});
	auto connection = fixture.OpenConnection();
	ReadBatches read(*redshift_test::ExecuteStatement(*connection, QUERY, {}));

	REQUIRE(read.rows.size() == 2);
	const auto timing = TransferProbe::Timing();
	CHECK(timing.fetch_calls == fixture.CallsTo("SQLFetch"));
	CHECK(timing.get_data_calls == fixture.CallsTo("SQLGetData"));
	CHECK(timing.get_data_calls == 2);
	CHECK(timing.driver_time.count() > 0);
	// The session setup's statements and the query's, each timed.
	CHECK(timing.prepares.size() == fixture.CallsTo("SQLPrepare"));
}

TEST_CASE("Starting the probe again clears what it recorded", "[odbc]") {
	FakeDriverFixture fixture;
	fixture.fake.AddResult(QUERY, {Counting(2)});
	TransferChanges changes;
	changes.probe_position = true;
	ProbeFake(changes);
	{
		auto connection = fixture.OpenConnection();
		ReadBatches read(*redshift_test::ExecuteStatement(*connection, QUERY, {}));
		REQUIRE_FALSE(TransferProbe::PositionAnswer().empty());
	}
	TransferProbe::Start(&FakeOdbcDriver::Source, {});

	const auto timing = TransferProbe::Timing();
	CHECK(timing.fetch_calls == 0);
	CHECK(timing.get_data_calls == 0);
	CHECK(timing.driver_time.count() == 0);
	CHECK(timing.prepares.empty());
	CHECK(TransferProbe::Described().empty());
	CHECK(TransferProbe::PositionAnswer().empty());
}

TEST_CASE("The probe records the described columns and the driver's SQLSetPos answer, once", "[odbc]") {
	FakeDriverFixture fixture;
	fixture.fake.AddResult(QUERY, {Counting(2)});
	TransferChanges changes;
	changes.probe_position = true;
	// One row a fetch, so two fetches return rows and only the first asks.
	changes.row_array_cap = 1;
	ProbeFake(changes);
	auto connection = fixture.OpenConnection();
	ReadBatches read(*redshift_test::ExecuteStatement(*connection, QUERY, {}));

	REQUIRE(read.rows.size() == 2);
	const auto described = TransferProbe::Described();
	REQUIRE_FALSE(described.empty());
	CHECK(described.back().name == "c");
	CHECK(described.back().data_type == SQL_INTEGER);
	CHECK(described.back().column_size == 10);
	CHECK(fixture.CallsTo("SQLSetPos") == 1);
	// The fake answers as rsresult.cpp's SQLSetPos does on a forward-only cursor.
	CHECK(TransferProbe::PositionAnswer() == "-1 HY106");
}

TEST_CASE("The probe leaves a fetch with records alone, so the client still reads them", "[odbc]") {
	FakeDriverFixture fixture;
	fixture.fake.AddResult(QUERY, {Counting(2)});
	TransferChanges changes;
	changes.probe_position = true;
	ProbeFake(changes);
	auto connection = fixture.OpenConnection();
	auto result = redshift_test::ExecuteStatement(*connection, QUERY, {});
	fixture.fake.WarnCall("SQLFetch", NumericCast<int>(fixture.CallsTo("SQLFetch")) + 1,
	                      {{"01000", 0, "General warning", {}}});
	ReadBatches read(*result);

	CHECK(read.rows.size() == 2);
	CHECK(fixture.CallsTo("SQLSetPos") == 0);
	CHECK(TransferProbe::PositionAnswer().empty());
}

TEST_CASE("The probe passes a failed driver load through unchanged", "[odbc]") {
	FakeDriverFixture fixture;
	fixture.fake.FailLoad(redshift_driver::DriverErrorKind::LOAD_FAILED, "the platform loader refused the file");
	ProbeFake({});

	CHECK_THROWS_WITH(fixture.OpenConnection(),
	                  Catch::Contains("the Redshift driver could not be loaded: the platform loader refused"));
}

TEST_CASE("The probe records no column a failed SQLDescribeCol wrote nothing for", "[odbc]") {
	FakeDriverFixture fixture;
	fixture.fake.AddResult(QUERY, {Counting(2)});
	ProbeFake({});
	auto connection = fixture.OpenConnection();
	fixture.FailNextCall("SQLDescribeCol", SQL_ERROR, {{"HY000", 0, "describe failed", {}}});

	CHECK_THROWS_WITH(connection->Describe(QUERY),
	                  Catch::Contains("the Redshift driver failed while describing the statement's result columns"));
	CHECK(TransferProbe::Described().empty());
}
