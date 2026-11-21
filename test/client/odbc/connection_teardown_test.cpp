// A connection's teardown when the driver refuses a step: the remaining steps still run, each
// failure is logged at debug level by its SQLSTATEs alone, the destructor does not throw, and a
// handle whose free failed is not passed to the driver again.

// catch.hpp comes first, as in this directory's other tests.
#include "catch.hpp"

#include "duckdb/main/database.hpp"
#include "fake_driver_fixture.hpp"

#include <string>
#include <vector>

using namespace duckdb;
using redshift_driver::testing::FakeDiag;
using redshift_test::FakeDriverFixture;

namespace {

//! A record carrying a host, an address and a key ID, none of which may reach the log.
const FakeDiag LOST_SERVER {
    "08S01", 0, "server at redshift.internal.example (10.1.2.3) closed AKIAIOSFODNN7EXAMPLE", {}};

//! The line for a failed free of the connection handle that left LOST_SERVER.
const char *const FREE_FAILED =
    "Redshift connection teardown: freeing the connection handle failed, SQLSTATE 08S01; the handle is abandoned";

//! The lines logged after the driver's identity, which the first connect logs first.
std::vector<std::string> TeardownLines(const FakeDriverFixture &fixture) {
	const auto lines = fixture.log_sink->Lines();
	REQUIRE(!lines.empty());
	REQUIRE(lines.front() == "Redshift driver loaded: test-fake fake " + DuckDB::Platform());
	return std::vector<std::string>(lines.begin() + 1, lines.end());
}

} // namespace

TEST_CASE("A failed rollback at teardown is logged by its SQLSTATE alone", "[odbc]") {
	FakeDriverFixture fixture;
	auto connection = fixture.OpenConnection();
	fixture.fake.FailCall("SQLEndTran", 1, SQL_ERROR, {LOST_SERVER});

	connection.reset();
	CHECK(TeardownLines(fixture) == std::vector<std::string> {"Redshift connection teardown: the rollback failed, "
	                                                          "SQLSTATE 08S01; teardown continues"});
}

TEST_CASE("A failed rollback at teardown does not stop the disconnect or the free", "[odbc]") {
	FakeDriverFixture fixture;
	auto connection = fixture.OpenConnection();
	const auto opened = fixture.fake.Calls().size();
	fixture.fake.FailCall("SQLEndTran", 1, SQL_ERROR, {LOST_SERVER});

	connection.reset();
	CHECK(fixture.CallsAfter(opened) == std::vector<std::string> {"SQLEndTran", "SQLDisconnect", "SQLFreeHandle"});
	CHECK(fixture.fake.AllocatedHandles() == "environment");
}

TEST_CASE("A failed disconnect at teardown is logged by the SQLSTATE of each record", "[odbc]") {
	FakeDriverFixture fixture;
	auto connection = fixture.OpenConnection();
	fixture.fake.FailCall("SQLDisconnect", 1, SQL_ERROR, {LOST_SERVER, FakeDiag {"HY000", 0, "general error", {}}});

	connection.reset();
	CHECK(TeardownLines(fixture) == std::vector<std::string> {"Redshift connection teardown: the disconnect failed, "
	                                                          "SQLSTATE 08S01,HY000; teardown continues"});
}

TEST_CASE("A failed disconnect at teardown does not stop the free", "[odbc]") {
	FakeDriverFixture fixture;
	auto connection = fixture.OpenConnection();
	fixture.fake.FailCall("SQLDisconnect", 1, SQL_ERROR, {LOST_SERVER});

	connection.reset();
	CHECK(fixture.EntryPoints().back() == "SQLFreeHandle");
	CHECK(fixture.fake.AllocatedHandles() == "environment");
}

TEST_CASE("A failed teardown step that left no record is logged as having none", "[odbc]") {
	FakeDriverFixture fixture;
	auto connection = fixture.OpenConnection();
	fixture.fake.FailCall("SQLEndTran", 1, SQL_ERROR, {});

	connection.reset();
	CHECK(TeardownLines(fixture) == std::vector<std::string> {"Redshift connection teardown: the rollback failed, "
	                                                          "with no diagnostic record; teardown continues"});
}

TEST_CASE("A connection handle whose free fails at teardown is logged as abandoned, by its SQLSTATE", "[odbc]") {
	FakeDriverFixture fixture;
	auto connection = fixture.OpenConnection();
	fixture.FailNextCall("SQLFreeHandle", SQL_ERROR, {LOST_SERVER});

	connection.reset();
	CHECK(TeardownLines(fixture) == std::vector<std::string> {FREE_FAILED});
}

TEST_CASE("A connection handle whose free fails with no record is logged as having none", "[odbc]") {
	FakeDriverFixture fixture;
	auto connection = fixture.OpenConnection();
	fixture.FailNextCall("SQLFreeHandle", SQL_ERROR, {});

	connection.reset();
	CHECK(TeardownLines(fixture) == std::vector<std::string> {"Redshift connection teardown: freeing the connection "
	                                                          "handle failed, with no diagnostic record; the handle "
	                                                          "is abandoned"});
}

TEST_CASE("A connection handle whose free fails at teardown is only read for that free's records", "[odbc]") {
	FakeDriverFixture fixture;
	auto connection = fixture.OpenConnection();
	const auto opened = fixture.fake.Calls().size();
	fixture.FailNextCall("SQLFreeHandle", SQL_ERROR, {LOST_SERVER});

	connection.reset();
	CHECK(fixture.CallsAfter(opened) == std::vector<std::string> {"SQLEndTran", "SQLDisconnect", "SQLFreeHandle"});
	CHECK(fixture.fake.MisusedAbandonedHandles().empty());
}

TEST_CASE("Teardown with the rollback, the disconnect and the free all failing does not throw", "[odbc]") {
	FakeDriverFixture fixture;
	auto connection = fixture.OpenConnection();
	fixture.fake.FailCall("SQLEndTran", 1, SQL_ERROR, {LOST_SERVER});
	fixture.fake.FailCall("SQLDisconnect", 1, SQL_ERROR, {LOST_SERVER});
	fixture.FailNextCall("SQLFreeHandle", SQL_ERROR, {LOST_SERVER});

	CHECK_NOTHROW(connection.reset());
	CHECK(TeardownLines(fixture) ==
	      std::vector<std::string> {
	          "Redshift connection teardown: the rollback failed, SQLSTATE 08S01; teardown continues",
	          "Redshift connection teardown: the disconnect failed, SQLSTATE 08S01; teardown "
	          "continues",
	          FREE_FAILED});
	CHECK(fixture.fake.MisusedAbandonedHandles().empty());
}
