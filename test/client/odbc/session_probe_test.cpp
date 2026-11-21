// SessionProbe on the fake driver: it keeps the connection the client opened and reads the driver's
// own answers on it, so a [live] case's record is the driver's, not the client's, and runs a statement
// of its own on it.

// catch.hpp comes first, as in this directory's other tests.
#include "catch.hpp"

#include "fake_driver_fixture.hpp"
#include "session_probe.hpp"
#include "thrown.hpp"

#include <algorithm>
#include <stdexcept>
#include <string>
#include <vector>

using namespace duckdb;
using redshift_driver::testing::FakeColumn;
using redshift_driver::testing::FakeDiag;
using redshift_driver::testing::FakeOdbcDriver;
using redshift_driver::testing::FakeResultSet;
using redshift_test::FakeDriverFixture;
using redshift_test::SessionProbe;

namespace {

//! A fixture whose connections go through SessionProbe to the fake.
struct ProbedFakeFixture : FakeDriverFixture {
	ProbedFakeFixture() {
		SetDriverSourceForTesting(&SessionProbe::Source);
		SessionProbe::Start(&FakeOdbcDriver::Source);
	}
};

//! Cells longer than the probe's read buffer, so they arrive in parts.
const std::string LONG_CELL = std::string(5000, 'x') + "end";
const std::string LONG_BYTES = std::string("\x00\xff", 2) + std::string(5000, '\x01');

//! A NULL, a VARBYTE holding a zero byte, and two cells read in parts, one reporting no total.
FakeResultSet ProbedCells() {
	FakeColumn bytes {"bytes", "VARBYTE", SQL_LONGVARBINARY, 64000};
	auto no_total = redshift_test::LongVarcharColumn("no_total");
	no_total.report_no_total = true;
	FakeResultSet result;
	result.columns = {redshift_test::VarcharColumn("missing", 10), bytes, redshift_test::LongVarcharColumn("long"),
	                  no_total};
	result.rows = {{"", LONG_BYTES, LONG_CELL, LONG_CELL}};
	result.nulls = {{true, false, false, false}};
	return result;
}

std::string What(const std::exception &error) {
	return error.what();
}

//! What SessionProbe throws when it holds no connection to read on.
std::string NoConnectionRefusal() {
	try {
		SessionProbe::Rows("SELECT 1");
	} catch (const std::runtime_error &error) {
		return What(error);
	}
	return "no failure";
}

const std::string NO_CONNECTION = "no connection was kept: open one through SessionProbe::Source first";

} // namespace

TEST_CASE("SessionProbe reads the columns a statement describes once prepared, without running it", "[odbc]") {
	ProbedFakeFixture fixture;
	auto connection = fixture.OpenConnection();
	const FakeColumn number {"n", "NUMERIC", SQL_NUMERIC, 38, 10, SQL_NO_NULLS};
	FakeResultSet result;
	result.columns = {number};
	fixture.fake.AddResult("SELECT n", {result});

	const auto columns = SessionProbe::Columns("SELECT n");

	REQUIRE(columns.size() == 1);
	CHECK(columns[0].name == "n");
	CHECK(columns[0].type_name == "NUMERIC");
	CHECK(columns[0].sql_type == SQL_NUMERIC);
	CHECK(columns[0].column_size == 38);
	CHECK(columns[0].decimal_digits == 10);
	CHECK(columns[0].nullable == SQL_NO_NULLS);
	CHECK(fixture.CallsTo("SQLExecute") == redshift_test::SessionSetupStatements().size());
}

TEST_CASE("SessionProbe reads each cell whole: a NULL as null, a VARBYTE as its bytes", "[odbc]") {
	ProbedFakeFixture fixture;
	auto connection = fixture.OpenConnection();
	fixture.fake.AddResult("SELECT cells", {ProbedCells()});

	const auto rows = SessionProbe::Rows("SELECT cells");

	REQUIRE(rows.size() == 1);
	CHECK(rows[0].nulls == std::vector<bool> {true, false, false, false});
	CHECK(rows[0].cells == std::vector<std::string> {"", LONG_BYTES, LONG_CELL, LONG_CELL});
	const auto reads = fixture.DetailsOf("SQLGetData");
	CHECK(std::count(reads.begin(), reads.end(), "2 " + std::to_string(SQL_C_BINARY)) == 2);
	CHECK(std::count(reads.begin(), reads.end(), "3 " + std::to_string(SQL_C_CHAR)) == 2);
}

TEST_CASE("SessionProbe fails rather than loops on a long binary cell the driver repeats as text", "[odbc]") {
	ProbedFakeFixture fixture;
	auto connection = fixture.OpenConnection();
	FakeResultSet result;
	result.columns = {FakeColumn {"shape", "GEOMETRY", SQL_LONGVARBINARY, 1024000}};
	result.rows = {{std::string(5000, 'A')}};
	fixture.fake.AddResult("SELECT shape", {result});

	try {
		SessionProbe::Rows("SELECT shape");
		FAIL("no failure");
	} catch (const std::runtime_error &error) {
		CHECK(What(error) == "SQLGetData of column 1 returned the same part again; read the column as SQL_C_BINARY");
	}
}

TEST_CASE("SessionProbe reads on the connection the client opened, freeing its statement after", "[odbc]") {
	ProbedFakeFixture fixture;
	auto connection = fixture.OpenConnection();

	CHECK(SessionProbe::Rows("SELECT 1").size() == 1);
	CHECK(fixture.fake.AllocatedHandles() == "environment, connection");
}

TEST_CASE("SessionProbe runs a statement on a handle of its own, freeing it after, or names its failure", "[odbc]") {
	ProbedFakeFixture fixture;
	auto connection = fixture.OpenConnection();
	FakeResultSet no_rows;
	no_rows.row_count = 0;
	fixture.fake.AddResult("ALTER TABLE t ADD COLUMN b INTEGER", {no_rows});

	SessionProbe::RunStatement("ALTER TABLE t ADD COLUMN b INTEGER");
	CHECK(fixture.DetailsOf("SQLExecDirect") == std::vector<std::string> {"ALTER TABLE t ADD COLUMN b INTEGER"});
	CHECK(fixture.fake.AllocatedHandles() == "environment, connection");

	fixture.FailNextCall("SQLExecDirect", SQL_ERROR, {FakeDiag {"42P01", 0, "relation does not exist", {}}});
	try {
		SessionProbe::RunStatement("ALTER TABLE t ADD COLUMN b INTEGER");
		FAIL("no failure");
	} catch (const std::runtime_error &error) {
		CHECK(What(error) ==
		      "SQLExecDirect of ALTER TABLE t ADD COLUMN b INTEGER failed: 42P01 relation does not exist");
	}
	CHECK(fixture.fake.AllocatedHandles() == "environment, connection");
}

TEST_CASE("SessionProbe names the call that failed and the driver's first record", "[odbc]") {
	ProbedFakeFixture fixture;
	auto connection = fixture.OpenConnection();
	std::string expected;
	SECTION("a statement handle the driver refuses") {
		fixture.FailNextCall("SQLAllocHandle", SQL_ERROR, {});
		expected = "SQLAllocHandle failed on the kept connection";
	}
	SECTION("a statement the driver refuses to prepare") {
		fixture.FailNextCall("SQLPrepare", SQL_ERROR, {FakeDiag {"42601", 0, "syntax error", {}}});
		expected = "SQLPrepare of SELECT 1 failed: 42601 syntax error";
	}

	try {
		SessionProbe::Columns("SELECT 1");
		FAIL("no failure");
	} catch (const std::runtime_error &error) {
		CHECK(What(error) == expected);
	}
	CHECK(fixture.fake.AllocatedHandles() == "environment, connection");
}

TEST_CASE("SessionProbe passes a failed driver load through unchanged", "[odbc]") {
	ProbedFakeFixture fixture;
	fixture.fake.FailLoad(redshift_driver::DriverErrorKind::LOAD_FAILED, "the platform loader refused the file");

	const auto load = SessionProbe::Source();

	CHECK_FALSE(load.ok());
	CHECK(load.error.kind == redshift_driver::DriverErrorKind::LOAD_FAILED);
}

TEST_CASE("SessionProbe keeps only a connection that succeeded, until it is freed or the probe starts again",
          "[odbc]") {
	ProbedFakeFixture fixture;
	SECTION("a failed connect") {
		fixture.FailNextCall("SQLDriverConnect", SQL_ERROR, {});
		CHECK(redshift_test::Capture([&] { fixture.OpenConnection(); }) ==
		      redshift_test::Thrown {ExceptionType::IO, "Could not connect to Redshift at " +
		                                                    redshift_test::ServerlessTarget("us-east-1") +
		                                                    ": the connection attempt failed.\nCheck the region "
		                                                    "and that this host can reach the workgroup, then retry." +
		                                                    redshift_test::DEFAULT_PORT_HINT});
		CHECK(NoConnectionRefusal() == NO_CONNECTION);
	}
	SECTION("a closed connection") {
		fixture.OpenConnection().reset();
		CHECK(NoConnectionRefusal() == NO_CONNECTION);
	}
	SECTION("a new start") {
		auto connection = fixture.OpenConnection();
		SessionProbe::Start(&FakeOdbcDriver::Source);
		CHECK(NoConnectionRefusal() == NO_CONNECTION);
	}
}
