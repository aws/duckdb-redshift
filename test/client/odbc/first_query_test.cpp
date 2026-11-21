// SELECT 1 through redshift_query, traced: offline on the fake, and with the live variables on the
// embedded driver, whose calls and result must match the fake's.

// catch.hpp comes first, as in this directory's other tests.
#include "catch.hpp"

#include "call_trace.hpp"
#include "extension_database.hpp"
#include "fake_driver_fixture.hpp"
#include "live_workgroup.hpp"

#include <algorithm>
#include <string>
#include <vector>

using namespace duckdb;
using redshift_driver::DriverErrorKind;
using redshift_driver::testing::FakeOdbcDriver;
using redshift_test::CallTrace;

namespace {

//! One SELECT 1 through redshift_query on `database` after `attach`, as a line: the result's column
//! name, type and value, its row count when it is not one row, or the error of the ATTACH or the
//! query. It asserts nothing, so a failed live run still prints its trace.
std::string FirstQueryOutcome(redshift_test::ExtensionDatabase &database, const std::string &attach) {
	auto attached = database.con.Query(attach);
	if (attached->HasError()) {
		return "attach error " + attached->GetError();
	}
	auto result = database.con.Query("SELECT * FROM redshift_query('rs', 'SELECT 1')");
	if (result->HasError()) {
		return "error " + result->GetError();
	}
	std::string first_row;
	idx_t rows = 0;
	for (auto chunk = result->Fetch(); chunk; chunk = result->Fetch()) {
		if (rows == 0 && chunk->size() != 0) {
			// Through strings, so this code names none of DuckDB's value types.
			first_row = result->ColumnName(0).GetIdentifierName() + " " + chunk->GetTypes()[0].ToString() + " " +
			            chunk->GetValue(0, 0).ToString();
		}
		rows += chunk->size();
	}
	return rows == 1 ? "result " + first_row : "rows " + std::to_string(rows);
}

//! The "Redshift driver loaded: " lines in `database`'s log.
std::vector<std::string> DriverLoads(redshift_test::ExtensionDatabase &database) {
	return redshift_test::FirstColumn(
	    *database.Run("SELECT message FROM duckdb_logs WHERE message LIKE 'Redshift driver loaded: %'"));
}

//! A traced first query: the calls the driver received, oldest first, FirstQueryOutcome's line, and
//! the driver loads the client logged.
struct FirstQueryTrace {
	std::vector<std::string> calls;
	std::string outcome;
	std::vector<std::string> loads;
};

//! FirstQueryOutcome on a fresh database with debug logging, through `source`'s driver: the calls
//! that driver receives and the driver loads the client logged.
FirstQueryTrace TraceFirstQuery(redshift_driver::DriverSource source, const std::string &attach) {
	SetDriverSourceForTesting(&CallTrace::Source);
	CallTrace::Start(source);
	FirstQueryTrace trace;
	{
		// Closed before the calls are read, so the trace ends with the connection's teardown.
		redshift_test::ExtensionDatabase database;
		database.Run("CALL enable_logging(level = 'debug')");
		trace.outcome = FirstQueryOutcome(database, attach);
		trace.loads = DriverLoads(database);
	}
	trace.calls = CallTrace::Calls();
	return trace;
}

//! How many of `calls` are `call`.
size_t CountOf(const std::vector<std::string> &calls, const std::string &call) {
	return NumericCast<size_t>(std::count(calls.begin(), calls.end(), call));
}

//! Each of `calls` without its return code.
std::vector<std::string> EntryPointsOf(const std::vector<std::string> &calls) {
	std::vector<std::string> names;
	for (const auto &call : calls) {
		names.push_back(call.substr(0, call.find(' ')));
	}
	return names;
}

//! The name LongNameDriver's SQLDescribeCol writes, longer than the buffer its case passes.
const std::string LONG_NAME = "a_long_column_name";

//! SQLDescribeCol as a driver answers a buffer too short for the name: the bytes that fit before
//! the NUL, the whole name's length, and success with info.
SQLRETURN DescribeLongName(SQLHSTMT, SQLUSMALLINT, SQLCHAR *name, SQLSMALLINT capacity, SQLSMALLINT *name_length,
                           SQLSMALLINT *type, SQLULEN *size, SQLSMALLINT *digits, SQLSMALLINT *nullable) {
	const auto kept = std::min<size_t>(LONG_NAME.size(), NumericCast<size_t>(capacity) - 1);
	std::copy_n(LONG_NAME.begin(), kept, name);
	name[kept] = '\0';
	*name_length = NumericCast<SQLSMALLINT>(LONG_NAME.size());
	*type = SQL_INTEGER;
	*size = 10;
	*digits = 0;
	*nullable = SQL_NULLABLE;
	return SQL_SUCCESS_WITH_INFO;
}

//! SQLColAttribute answering every field with the number SQL_INTEGER and, where the caller asks
//! for a text length, SQL_NO_TOTAL, a negative one.
SQLRETURN NumericAttribute(SQLHSTMT, SQLUSMALLINT, SQLUSMALLINT, SQLPOINTER, SQLSMALLINT, SQLSMALLINT *text_length,
                           SQLLEN *numeric) {
	if (text_length) {
		*text_length = SQL_NO_TOTAL;
	}
	*numeric = SQL_INTEGER;
	return SQL_SUCCESS;
}

//! A DriverSource: the current fake's driver with those two descriptions, so a case calls the
//! traced entry points with buffers it chooses.
redshift_driver::DriverLoadResult LongNameDriver() {
	static redshift_driver::LoadedDriver driver;
	auto result = FakeOdbcDriver::Source();
	driver = *result.driver;
	driver.api.DescribeCol = &DescribeLongName;
	driver.api.ColAttribute = &NumericAttribute;
	result.driver = &driver;
	return result;
}

//! SQLColAttribute failing after it wrote a length, which a caller must not read.
SQLRETURN FailedAttribute(SQLHSTMT, SQLUSMALLINT, SQLUSMALLINT, SQLPOINTER, SQLSMALLINT, SQLSMALLINT *text_length,
                          SQLLEN *) {
	if (text_length) {
		*text_length = 3;
	}
	return SQL_ERROR;
}

//! A DriverSource: the current fake's driver with that failing SQLColAttribute.
redshift_driver::DriverLoadResult FailedAttributeDriver() {
	static redshift_driver::LoadedDriver driver;
	auto result = FakeOdbcDriver::Source();
	driver = *result.driver;
	driver.api.ColAttribute = &FailedAttribute;
	result.driver = &driver;
	return result;
}

} // namespace

TEST_CASE("The call trace records each call the driver receives, in order", "[odbc]") {
	{
		// A first run, so the second shows that a new trace holds only its own calls.
		redshift_test::FakeDriverFixture first;
		TraceFirstQuery(&FakeOdbcDriver::Source, redshift_test::ATTACH_RS);
	}
	redshift_test::FakeDriverFixture fixture;
	const auto trace = TraceFirstQuery(&FakeOdbcDriver::Source, redshift_test::ATTACH_RS);

	CHECK(EntryPointsOf(trace.calls) == fixture.EntryPoints());
	CHECK(CountOf(trace.calls, "SQLDriverConnect 0") == 1);
	CHECK(CountOf(trace.calls, "SQLFetch 100") == 1);
	// SELECT 1's INTEGER is bound, so the live case compares these calls with the driver's too.
	CHECK(CountOf(trace.calls, "SQLSetStmtAttr 0") == 4);
	CHECK(CountOf(trace.calls, "SQLBindCol 0") == 1);
	// Describe and Execute's check each describe the column, and so does the check of what SQLExecDirect
	// ran; the trace keeps what the driver wrote out.
	CHECK(CountOf(trace.calls, "SQLDescribeCol 0 name ?column? type 4 size 10 digits 0 nullable -") == 3);
	CHECK(CountOf(trace.calls, "SQLColAttribute 0 field 14 INTEGER") == 3);
	CHECK(trace.outcome == "result ?column? INTEGER 1");
	// The traced driver keeps the wrapped one's identity.
	CHECK(trace.loads == std::vector<std::string> {"Redshift driver loaded: test-fake fake " + DuckDB::Platform()});
}

TEST_CASE("The call trace records a failed call's return code and its diagnostic read", "[odbc]") {
	redshift_test::FakeDriverFixture fixture;
	fixture.FailNextCall("SQLDriverConnect", SQL_ERROR, {{"HY000", 0, "No AWS profile found: no-such-profile"}});
	const auto trace = TraceFirstQuery(&FakeOdbcDriver::Source, redshift_test::ATTACH_RS);

	CHECK(EntryPointsOf(trace.calls) == fixture.EntryPoints());
	CHECK(CountOf(trace.calls, "SQLDriverConnect -1") == 1);
	CHECK(CountOf(EntryPointsOf(trace.calls), "SQLGetDiagRec") != 0);
	// The connect is the ATTACH's, so its failure is the ATTACH's.
	CHECK_THAT(trace.outcome,
	           Catch::StartsWith("attach error ") && Catch::Contains("no usable AWS credentials were found"));
}

TEST_CASE("The call trace records a failed description without out-values", "[odbc]") {
	// A failed call wrote nothing, so the trace must not read its buffers.
	redshift_test::FakeDriverFixture fixture;
	fixture.FailNextCall("SQLDescribeCol", SQL_ERROR, {{"HY000", 0, "description failed"}});
	const auto trace = TraceFirstQuery(&FakeOdbcDriver::Source, redshift_test::ATTACH_RS);

	CHECK(CountOf(trace.calls, "SQLDescribeCol -1") == 1);
	CHECK_THAT(trace.outcome, Catch::StartsWith("error "));
}

TEST_CASE("The call trace keeps a cut name to the bytes before its NUL", "[odbc]") {
	redshift_test::FakeDriverFixture fixture;
	CallTrace::Start(&LongNameDriver);
	// Longer than the reported length, so a trace that ignored the capacity would read the filler.
	std::vector<SQLCHAR> name(32, 'x');
	SQLSMALLINT name_length = 0;
	SQLSMALLINT type = 0;
	SQLULEN size = 0;
	SQLSMALLINT digits = 0;
	SQLSMALLINT nullable = 0;
	CallTrace::Source().driver->api.DescribeCol(nullptr, 1, name.data(), 5, &name_length, &type, &size, &digits,
	                                            &nullable);

	CHECK(CallTrace::Calls() ==
	      std::vector<std::string> {"SQLDescribeCol 1 name a_lo type 4 size 10 digits 0 nullable 1"});
}

TEST_CASE("The call trace records the number a numeric attribute wrote", "[odbc]") {
	redshift_test::FakeDriverFixture fixture;
	CallTrace::Start(&LongNameDriver);
	SQLLEN numeric = 0;
	CallTrace::Source().driver->api.ColAttribute(nullptr, 1, SQL_DESC_CONCISE_TYPE, nullptr, 0, nullptr, &numeric);

	CHECK(CallTrace::Calls() == std::vector<std::string> {"SQLColAttribute 0 field 2 4"});
}

TEST_CASE("The call trace records a failed attribute call without its text", "[odbc]") {
	redshift_test::FakeDriverFixture fixture;
	CallTrace::Start(&FailedAttributeDriver);
	// Filler a trace that read a failed call's buffers would record.
	std::vector<SQLCHAR> text(8, 'x');
	SQLSMALLINT text_length = 0;
	SQLLEN numeric = 0;
	CallTrace::Source().driver->api.ColAttribute(nullptr, 1, SQL_DESC_NAME, text.data(), 8, &text_length, &numeric);

	CHECK(CallTrace::Calls() == std::vector<std::string> {"SQLColAttribute -1"});
}

TEST_CASE("The call trace records no text where the driver reports a negative length", "[odbc]") {
	redshift_test::FakeDriverFixture fixture;
	CallTrace::Start(&LongNameDriver);
	// Filler a trace without the lower bound would read, or fail to count.
	std::vector<SQLCHAR> text(8, 'x');
	SQLSMALLINT text_length = 0;
	SQLLEN numeric = 0;
	CallTrace::Source().driver->api.ColAttribute(nullptr, 1, SQL_DESC_NAME, text.data(), 8, &text_length, &numeric);

	CHECK(text_length == SQL_NO_TOTAL);
	CHECK(CallTrace::Calls() == std::vector<std::string> {"SQLColAttribute 0 field 1011 "});
}

TEST_CASE("The call trace hands a warning back unchanged, so the client reads its diagnostics", "[odbc]") {
	// FailNextCall refuses success codes, so the connect's own outcome is scripted, as in
	// connection_test.cpp's warned connect.
	redshift_test::FakeDriverFixture fixture;
	fixture.fake.SetConnectOutcome(SQL_SUCCESS_WITH_INFO, {{"01004", 0, "String data, right truncated", {}}});
	const auto trace = TraceFirstQuery(&FakeOdbcDriver::Source, redshift_test::ATTACH_RS);

	CHECK(EntryPointsOf(trace.calls) == fixture.EntryPoints());
	CHECK(CountOf(trace.calls, "SQLDriverConnect 1") == 1);
	CHECK(CountOf(EntryPointsOf(trace.calls), "SQLGetDiagRec") != 0);
	CHECK_THAT(trace.outcome, Catch::StartsWith("result "));
}

TEST_CASE("The call trace passes a failed driver load through unchanged", "[odbc]") {
	redshift_test::FakeDriverFixture fixture;
	fixture.fake.FailLoad(DriverErrorKind::LOAD_FAILED, "the platform loader refused the file");
	const auto trace = TraceFirstQuery(&FakeOdbcDriver::Source, redshift_test::ATTACH_RS);

	CHECK(trace.calls.empty());
	// The load is the ATTACH's connect's, so its failure is the ATTACH's.
	CHECK_THAT(trace.outcome,
	           Catch::StartsWith("attach error ") &&
	               Catch::Contains("the Redshift driver could not be loaded: the platform loader refused"));
}

TEST_CASE("The call trace reports a refused ATTACH as an attach error, with no calls", "[odbc]") {
	// REGION has no default, so every build refuses an ATTACH without one before loading the driver.
	redshift_test::FakeDriverFixture fixture;
	const auto trace = TraceFirstQuery(&FakeOdbcDriver::Source, "ATTACH '' AS rs (TYPE redshift, "
	                                                            "CREDENTIAL_PROVIDER aws_default_chain, WORKGROUP "
	                                                            "'analytics', ACCOUNT_ID '012345678901')");

	CHECK(trace.calls.empty());
	CHECK(fixture.fake.SourceCalls() == 0);
	CHECK_THAT(trace.outcome, Catch::StartsWith("attach error ") &&
	                              Catch::Contains("Cannot attach rs: Missing required parameter 'REGION'"));
}

TEST_CASE("Live: SELECT 1 on the embedded driver returns INTEGER 1 through the calls the fake receives",
          "[odbc][live]") {
	SKIP_UNLESS_LIVE(redshift_driver::LoadEmbeddedDriver());
	const auto load = redshift_driver::LoadEmbeddedDriver();
	const auto attach = redshift_test::LiveWorkgroupAttach();
	// A reachable endpoint that is not the derived one can record the trace: the calls do not depend
	// on the host.
	const bool through_test_endpoints = redshift_test::ThroughTestEndpoints();
	// Where the trace came from, so a trace kept beside the fake's script names its driver.
	const auto provenance = (load.ok() ? load.driver->identity.Describe() + ", set " + load.driver->identity.set_sha256
	                                   : "load error " + load.error.message) +
	                        (through_test_endpoints ? ", through the test endpoints" : "");
	// The fake's own SELECT 1 script, so a difference in the calls or the result is one to fix it for.
	FirstQueryTrace fake_trace;
	{
		redshift_test::FakeDriverFixture fixture;
		fake_trace = TraceFirstQuery(&FakeOdbcDriver::Source, attach);
	}
	redshift_test::ResetOnExit reset;
	redshift_test::LiveEndpoints endpoints;
	const auto driver_trace = TraceFirstQuery(&redshift_driver::LoadEmbeddedDriver, attach);

	std::string recorded;
	for (const auto &call : driver_trace.calls) {
		recorded += call + "\n";
	}
	WARN("the embedded driver's trace (" << provenance << "):\n" << recorded << driver_trace.outcome);
	// The result first: two runs refused alike before any driver call, as for a mistyped region,
	// would otherwise pass on equal calls.
	REQUIRE(driver_trace.outcome == "result ?column? INTEGER 1");
	CHECK(driver_trace.calls == fake_trace.calls);
	CHECK(driver_trace.outcome == fake_trace.outcome);
}
