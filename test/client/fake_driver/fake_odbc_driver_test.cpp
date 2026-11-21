#include "fake_odbc_driver.hpp"

// catch.hpp comes before exception.hpp, because exception.hpp alone declares duckdb_base_std
// without the unique_ptr that catch.hpp then expects from it.
#include "catch.hpp"
#include "duckdb/common/exception.hpp"
#include "duckdb/main/database.hpp"
#include "fake_handles.hpp"

#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <future>
#include <limits>
#include <memory>
#include <string>
#include <thread>
#include <utility>
#include <vector>

using namespace duckdb::redshift_driver;
using namespace duckdb::redshift_driver::testing;

namespace {

//! A statement on a connection that never connected, for a case about handle bookkeeping, the call
//! log, or connecting itself. A case that runs statement work takes OpenStatement instead: what the
//! driver does with a statement call on an unconnected connection is not modelled.
struct OpenHandles : OpenConnection {
	HandleGuard statement {SQL_HANDLE_STMT, connection.value};
};

//! Stands in for a driver-specific SQLGetDiagField identifier, which no record carries unless a
//! test puts it there.
const SQLSMALLINT DETAIL_FIELD = 1001;

//! Record `number` on the handle as "SQLSTATE native message". The buffer is ODBC's largest message
//! length, so no record the fake posts is cut and the read succeeds.
std::string DiagRecord(const OdbcApi &api, SQLSMALLINT type, SQLHANDLE handle, SQLSMALLINT number) {
	SQLCHAR sqlstate[SQL_SQLSTATE_SIZE + 1] = {};
	SQLINTEGER native = -1;
	SQLCHAR message[SQL_MAX_MESSAGE_LENGTH] = {};
	REQUIRE(api.GetDiagRec(type, handle, number, sqlstate, &native, message, sizeof(message), nullptr) == SQL_SUCCESS);
	return std::string(reinterpret_cast<char *>(sqlstate)) + " " + std::to_string(native) + " " +
	       reinterpret_cast<char *>(message);
}

SQLINTEGER DiagRecordCount(const OdbcApi &api, SQLSMALLINT type, SQLHANDLE handle) {
	SQLINTEGER count = -1;
	REQUIRE(api.GetDiagField(type, handle, 0, SQL_DIAG_NUMBER, &count, 0, nullptr) == SQL_SUCCESS);
	return count;
}

//! The calls logged from first_call on, as (entry point, detail) pairs.
std::vector<std::pair<std::string, std::string>> CallsSince(const FakeOdbcDriver &fake, size_t first_call) {
	std::vector<std::pair<std::string, std::string>> logged;
	for (auto i = first_call; i < fake.Calls().size(); i++) {
		logged.emplace_back(fake.Calls()[i].entry_point, fake.Calls()[i].detail);
	}
	return logged;
}

//! The entry points logged from first_call on, for a case that pins the names alone.
std::vector<std::string> EntryPointsSince(const FakeOdbcDriver &fake, size_t first_call) {
	std::vector<std::string> logged;
	for (const auto &call : CallsSince(fake, first_call)) {
		logged.push_back(call.first);
	}
	return logged;
}

//! Prepares the default script, which SQLExecute needs before it will run.
void PrepareDefaultResult(const OpenStatement &open) {
	REQUIRE(open.api.Prepare(open.statement.value, Text("SELECT 1"), SQL_NTS) == SQL_SUCCESS);
}

//! Prepares, executes and fetches the default script's row, leaving the statement on it.
void FetchDefaultRow(const OpenStatement &open) {
	PrepareDefaultResult(open);
	REQUIRE(open.api.Execute(open.statement.value) == SQL_SUCCESS);
	REQUIRE(open.api.Fetch(open.statement.value) == SQL_SUCCESS);
}

//! The SQLSTATE of the first record the latest call left on the statement.
std::string StatementSqlstate(const OpenStatement &open) {
	return DiagRecord(open.api, SQL_HANDLE_STMT, open.statement.value, 1).substr(0, SQL_SQLSTATE_SIZE);
}

//! A column as driver 2.3.0.0 described one: its own type-name spelling, concise type, size and
//! digits.
FakeColumn Column(const char *name, const char *type_name, SQLSMALLINT sql_type, SQLULEN size, SQLSMALLINT digits) {
	FakeColumn column;
	column.name = name;
	column.type_name = type_name;
	column.sql_type = sql_type;
	column.column_size = size;
	column.decimal_digits = digits;
	return column;
}

FakeColumn Varchar(const char *name) {
	return Column(name, "CHARACTER VARYING", SQL_VARCHAR, 256, 0);
}

FakeResultSet Rows(std::vector<FakeColumn> columns, std::vector<std::vector<std::string>> rows) {
	FakeResultSet result;
	result.columns = std::move(columns);
	result.rows = std::move(rows);
	return result;
}

void PrepareAndExecute(const OpenStatement &open, const char *sql) {
	REQUIRE(open.api.Prepare(open.statement.value, Text(sql), SQL_NTS) == SQL_SUCCESS);
	REQUIRE(open.api.Execute(open.statement.value) == SQL_SUCCESS);
}

//! Scripts sql as one row of these cells, one VARCHAR column each, then executes it and fetches the
//! row.
void FetchTextRow(FakeOdbcDriver &fake, const OpenStatement &open, const char *sql, std::vector<std::string> cells) {
	const std::vector<FakeColumn> columns(cells.size(), Varchar("v"));
	fake.AddResult(sql, {Rows(columns, {std::move(cells)})});
	PrepareAndExecute(open, sql);
	REQUIRE(open.api.Fetch(open.statement.value) == SQL_SUCCESS);
}

//! The whole of a short cell on the current row, read as text.
std::string Value(const OpenStatement &open, SQLUSMALLINT column) {
	char value[64] = {};
	SQLLEN length = 0;
	REQUIRE(open.api.GetData(open.statement.value, column, SQL_C_CHAR, value, sizeof(value), &length) == SQL_SUCCESS);
	REQUIRE(length >= 0);
	REQUIRE(static_cast<size_t>(length) < sizeof(value));
	return std::string(value, static_cast<size_t>(length));
}

//! What one SQLGetData call returned: the bytes it wrote, the length it reported, and its return
//! code, which tells a cut part from the last one.
struct Part {
	std::string bytes;
	SQLLEN length;
	SQLRETURN rc;
	bool operator==(const Part &other) const {
		return bytes == other.bytes && length == other.length && rc == other.rc;
	}
};

//! Reads a cell on the current row with SQLGetData calls into a buffer of `capacity` bytes until it
//! reports no more data. A cut part fills the buffer, less its NUL for text, and leaves `cut_records`
//! records: 1 for a cut text part's 01004, 0 for a cut SQL_C_BINARY part of a non-GEOMETRY column.
std::vector<Part> ReadParts(const OpenStatement &open, SQLUSMALLINT column, SQLSMALLINT c_type, SQLLEN capacity,
                            SQLINTEGER cut_records) {
	const auto filled = static_cast<size_t>(c_type == SQL_C_CHAR ? capacity - 1 : capacity);
	std::vector<char> buffer(static_cast<size_t>(capacity));
	std::vector<Part> parts;
	for (int call = 0; call < 100; call++) {
		SQLLEN length = 0;
		const auto rc = open.api.GetData(open.statement.value, column, c_type, buffer.data(), capacity, &length);
		if (rc == SQL_NO_DATA) {
			return parts;
		}
		REQUIRE((rc == SQL_SUCCESS || rc == SQL_SUCCESS_WITH_INFO));
		const bool cut = rc == SQL_SUCCESS_WITH_INFO;
		if (cut) {
			CHECK(DiagRecordCount(open.api, SQL_HANDLE_STMT, open.statement.value) == cut_records);
		}
		if (cut && cut_records > 0) {
			CHECK(DiagRecord(open.api, SQL_HANDLE_STMT, open.statement.value, 1) ==
			      "01004 0 String data, right truncated");
		}
		if (!cut) {
			REQUIRE(length >= 0);
			REQUIRE(static_cast<size_t>(length) <= filled);
		}
		parts.push_back(Part {std::string(buffer.data(), cut ? filled : static_cast<size_t>(length)), length, rc});
	}
	FAIL("SQLGetData never reported that the cell had no more data");
	return parts;
}

//! Cell `column` on the current row read as the fixed-size C type T, which must convert, and whose
//! width the read must report.
template <class T>
T Number(const OpenStatement &open, SQLUSMALLINT column, SQLSMALLINT c_type) {
	T value {};
	SQLLEN length = 0;
	REQUIRE(open.api.GetData(open.statement.value, column, c_type, &value, sizeof(value), &length) == SQL_SUCCESS);
	CHECK(length == static_cast<SQLLEN>(sizeof(T)));
	return value;
}

//! The SQLSTATE of a read of cell `column` on the current row as the C type T, which must fail.
template <class T>
std::string ReadFailure(const OpenStatement &open, SQLUSMALLINT column, SQLSMALLINT c_type) {
	T value {};
	REQUIRE(open.api.GetData(open.statement.value, column, c_type, &value, sizeof(value), nullptr) == SQL_ERROR);
	return StatementSqlstate(open);
}

//! Binds value as text input parameter `number` with BufferLength `buffer_length` and the indicator
//! `length`, both of which the driver reads at SQLExecute.
SQLRETURN BindText(const OpenStatement &open, SQLUSMALLINT number, char *value, SQLLEN buffer_length, SQLLEN *length) {
	return open.api.BindParameter(open.statement.value, number, SQL_PARAM_INPUT, SQL_C_CHAR, SQL_VARCHAR, 64, 0, value,
	                              buffer_length, length);
}

//! Binds value as text input parameter `number`, of the length *length holds: with BufferLength 0,
//! the driver takes the length from the indicator.
SQLRETURN BindText(const OpenStatement &open, SQLUSMALLINT number, char *value, SQLLEN *length) {
	return BindText(open, number, value, 0, length);
}

//! Binds the NUL-terminated value as text input parameter `number`, with SQL_NTS in an indicator
//! that outlives every test, so the driver reads up to the NUL whatever BufferLength says.
SQLRETURN BindNulTerminated(const OpenStatement &open, SQLUSMALLINT number, char *value) {
	static SQLLEN nul_terminated = SQL_NTS;
	return BindText(open, number, value, &nul_terminated);
}

//! The detail logged for every call to entry_point, oldest first.
std::vector<std::string> DetailsOf(const FakeOdbcDriver &fake, const std::string &entry_point) {
	std::vector<std::string> details;
	for (const auto &call : fake.Calls()) {
		if (call.entry_point == entry_point) {
			details.push_back(call.detail);
		}
	}
	return details;
}

//! Runs body on thread_count threads at once. Each waits for the others to start, so the calls
//! overlap instead of the first thread finishing before the last one starts. body asserts nothing:
//! Catch2 v2's assertions are not thread-safe.
template <class BODY>
void RunConcurrently(int thread_count, BODY body) {
	std::atomic<int> ready {0};
	std::vector<std::thread> workers;
	for (int worker_index = 0; worker_index < thread_count; worker_index++) {
		workers.emplace_back([&]() {
			ready++;
			while (ready < thread_count) {
				std::this_thread::yield();
			}
			body();
		});
	}
	for (auto &worker : workers) {
		worker.join();
	}
}

//! One column per case label of the fake's DriverPrecision, with a DATE and a VARCHAR for its default
//! of 0, each at the size and precision the driver's getSize and getPrecision give (rsutil.c). The
//! SQL_DECIMAL, SQL_FLOAT, SQL_TIME and SQL_TIMESTAMP rows pin arms the driver never reports.
struct ArmColumn {
	FakeColumn column;
	SQLLEN precision;
};

std::vector<ArmColumn> EveryPrecisionArm() {
	return {{Column("numeric", "NUMERIC", SQL_NUMERIC, 10, 2), 10},
	        {Column("decimal", "NUMERIC", SQL_DECIMAL, 12, 3), 12},
	        {Column("smallint", "SMALLINT", SQL_SMALLINT, 5, 0), 5},
	        {Column("integer", "INTEGER", SQL_INTEGER, 10, 0), 10},
	        {Column("bigint", "BIGINT", SQL_BIGINT, 19, 0), 19},
	        {Column("real", "REAL", SQL_REAL, 7, 0), 7},
	        {Column("float", "DOUBLE PRECISION", SQL_FLOAT, 15, 0), 15},
	        {Column("double", "DOUBLE PRECISION", SQL_DOUBLE, 15, 0), 15},
	        {Column("time", "TIME", SQL_TYPE_TIME, 8, 0), 6},
	        {Column("time2", "TIME", SQL_TIME, 8, 0), 6},
	        {Column("timestamp", "TIMESTAMP", SQL_TYPE_TIMESTAMP, 26, 6), 6},
	        {Column("timestamp2", "TIMESTAMP", SQL_TIMESTAMP, 26, 6), 6},
	        {Column("interval", "INTERVAL DAY TO SECOND", SQL_INTERVAL_DAY_TO_SECOND, 26, 0), 6},
	        {Column("date", "DATE", SQL_TYPE_DATE, 10, 0), 0},
	        {Column("name", "CHARACTER VARYING", SQL_VARCHAR, 10, 0), 0}};
}

//! Prepares one statement whose columns are EveryPrecisionArm's.
void PrepareEveryPrecisionArm(FakeOdbcDriver &fake, const OpenStatement &open) {
	std::vector<FakeColumn> columns;
	for (const auto &arm : EveryPrecisionArm()) {
		columns.push_back(arm.column);
	}
	fake.AddResult("SELECT every type", {Rows(columns, {})});
	REQUIRE(open.api.Prepare(open.statement.value, Text("SELECT every type"), SQL_NTS) == SQL_SUCCESS);
}

//! Numeric field `field` of column `number` through SQLColAttribute, which must succeed.
SQLLEN NumericAttribute(const OpenStatement &open, SQLUSMALLINT number, SQLUSMALLINT field) {
	SQLLEN value = -1;
	REQUIRE(open.api.ColAttribute(open.statement.value, number, field, nullptr, 0, nullptr, &value) == SQL_SUCCESS);
	return value;
}

//! Scripts and fetches one row holding the NUMERIC whole-digits cases: "-123.45" in a NUMERIC(10,2)
//! and a DECIMAL(10,2) column, needing 5 bytes, "12345" in a NUMERIC(10,0), needing 6, and
//! "-123.45" again in a VARCHAR and in a NUMERIC column read as binary, which are not checked.
void FetchNumericRow(FakeOdbcDriver &fake, const OpenStatement &open) {
	fake.AddResult(
	    "SELECT n, d, w, v, b",
	    {Rows({Column("n", "NUMERIC", SQL_NUMERIC, 10, 2), Column("d", "NUMERIC", SQL_DECIMAL, 10, 2),
	           Column("w", "NUMERIC", SQL_NUMERIC, 10, 0), Varchar("v"), Column("b", "NUMERIC", SQL_NUMERIC, 10, 2)},
	          {{"-123.45", "-123.45", "12345", "-123.45", "-123.45"}})});
	PrepareAndExecute(open, "SELECT n, d, w, v, b");
	REQUIRE(open.api.Fetch(open.statement.value) == SQL_SUCCESS);
}

} // namespace

TEST_CASE("The fake's source returns one complete test-fake driver on every call", "[fake_driver]") {
	FakeOdbcDriver fake;
	const DriverSource source = &FakeOdbcDriver::Source;
	const auto first = source();
	REQUIRE(first.ok());
	CHECK(source().driver == first.driver);
	CHECK(first.driver->api.Complete());
	CHECK(first.driver->identity.origin == DriverOrigin::TEST_FAKE);
	CHECK(first.driver->identity.version == "fake");
	CHECK(first.driver->identity.platform == duckdb::DuckDB::Platform());
}

TEST_CASE("The fake's source throws when no fake is current", "[fake_driver]") {
#ifdef DUCKDB_CRASH_ON_ASSERT
	return; // InternalException aborts in this build
#endif
	CHECK_THROWS_AS(FakeOdbcDriver::Source(), duckdb::InternalException);
	CHECK_THROWS_WITH(FakeOdbcDriver::Source(), Catch::Contains("no FakeOdbcDriver is current"));
}

TEST_CASE("FailLoad makes every later source call fail with its kind and message", "[fake_driver]") {
	FakeOdbcDriver fake;
	fake.FailLoad(DriverErrorKind::REFUSED_ENVIRONMENT, "PGSERVICE is set");
	for (int call = 0; call < 2; call++) {
		const auto result = FakeOdbcDriver::Source();
		CHECK_FALSE(result.ok());
		CHECK(result.error.kind == DriverErrorKind::REFUSED_ENVIRONMENT);
		CHECK(result.error.message == "PGSERVICE is set");
	}
}

TEST_CASE("A fake constructed after another's FailLoad loads again", "[fake_driver]") {
	{
		FakeOdbcDriver failing;
		failing.FailLoad(DriverErrorKind::LOAD_FAILED, "x");
	}
	FakeOdbcDriver fake;
	CHECK(FakeOdbcDriver::Source().ok());
}

TEST_CASE("SourceCalls counts every call to the fake's source, a failed one included", "[fake_driver]") {
	FakeOdbcDriver fake;
	CHECK(fake.SourceCalls() == 0);
	REQUIRE(FakeOdbcDriver::Source().ok());
	CHECK(fake.SourceCalls() == 1);
	fake.FailLoad(DriverErrorKind::LOAD_FAILED, "no such file");
	REQUIRE_FALSE(FakeOdbcDriver::Source().ok());

	CHECK(fake.SourceCalls() == 2);
}

TEST_CASE("The fake's driver carries the environment lists the test set", "[fake_driver]") {
	FakeOdbcDriver fake;
	fake.SetRefusedEnvironment({"PGSERVICE"});
	fake.SetOverriddenEnvironment({{"PGHOST", "Host"}});
	const auto source = FakeOdbcDriver::Source();
	REQUIRE(source.ok());
	const auto &driver = *source.driver;
	CHECK(driver.refused_environment == std::vector<std::string> {"PGSERVICE"});
	REQUIRE(driver.overridden_environment.size() == 1);
	CHECK(driver.overridden_environment[0].name == "PGHOST");
	CHECK(driver.overridden_environment[0].keyword == "Host");
}

TEST_CASE("The fake's driver carries the trust directory the test set", "[fake_driver]") {
	FakeOdbcDriver fake;
	REQUIRE(FakeOdbcDriver::Source().driver->trust_directory.empty());
	fake.SetTrustDirectory("C:\\data\\trust");
	const auto source = FakeOdbcDriver::Source();
	REQUIRE(source.ok());
	CHECK(source.driver->trust_directory == "C:\\data\\trust");
}

TEST_CASE("A new fake starts with an empty call log, whatever the last one logged", "[fake_driver]") {
	{
		FakeOdbcDriver earlier;
		OpenHandles open;
		REQUIRE_FALSE(earlier.Calls().empty());
	}
	FakeOdbcDriver fake;

	CHECK(fake.Calls().empty());
}

TEST_CASE("Constructing a fake while another is current throws and leaves the first current", "[fake_driver]") {
#ifdef DUCKDB_CRASH_ON_ASSERT
	return; // InternalException aborts in this build
#endif
	FakeOdbcDriver current;
	const auto *driver = FakeOdbcDriver::Source().driver;

	CHECK_THROWS_AS(FakeOdbcDriver(), duckdb::InternalException);
	CHECK_THROWS_WITH(FakeOdbcDriver(), Catch::Contains("already current"));
	CHECK(FakeOdbcDriver::Source().driver == driver);
}

TEST_CASE("The call log lists calls oldest first", "[fake_driver]") {
	FakeOdbcDriver fake;
	OpenStatement open;
	const auto first_call = fake.Calls().size();
	SQLLEN rows = 0;
	open.api.RowCount(open.statement.value, &rows);
	open.api.Fetch(open.statement.value);

	REQUIRE(fake.Calls().size() == first_call + 2);
	CHECK(fake.Calls()[first_call].entry_point == "SQLRowCount");
	CHECK(fake.Calls()[first_call + 1].entry_point == "SQLFetch");
}

TEST_CASE("An entry point called when no fake is current returns SQL_INVALID_HANDLE", "[fake_driver]") {
	OdbcApi api;
	{
		FakeOdbcDriver fake;
		const auto source = FakeOdbcDriver::Source();
		REQUIRE(source.ok());
		api = source.driver->api;
	}
	SQLHANDLE environment = SQL_NULL_HANDLE;

	CHECK(api.AllocHandle(SQL_HANDLE_ENV, SQL_NULL_HANDLE, &environment) == SQL_INVALID_HANDLE);
	CHECK(environment == SQL_NULL_HANDLE);
}

TEST_CASE("Calls from eight threads at once are each logged once", "[fake_driver]") {
	FakeOdbcDriver fake;
	OpenStatement open;
	const auto first_call = fake.Calls().size();
	const int thread_count = 8;
	const int calls_per_thread = 100;
	RunConcurrently(thread_count, [&]() {
		SQLLEN rows = 0;
		for (int call = 0; call < calls_per_thread; call++) {
			open.api.RowCount(open.statement.value, &rows);
		}
	});

	CHECK(fake.Calls().size() == first_call + static_cast<size_t>(thread_count) * calls_per_thread);
}

TEST_CASE("A handle the fake does not hold as the given type is invalid", "[fake_driver]") {
	FakeOdbcDriver fake;
	OpenHandles open;
	CHECK(open.api.FreeHandle(SQL_HANDLE_DBC, open.statement.value) == SQL_INVALID_HANDLE);
	SQLHANDLE statement = SQL_NULL_HANDLE;
	CHECK(open.api.AllocHandle(SQL_HANDLE_STMT, open.environment.value, &statement) == SQL_INVALID_HANDLE);
	REQUIRE(open.api.FreeHandle(SQL_HANDLE_STMT, open.statement.value) == SQL_SUCCESS);
	CHECK(open.api.FreeHandle(SQL_HANDLE_STMT, open.statement.value) == SQL_INVALID_HANDLE);
}

TEST_CASE("Allocating a handle type the fake does not model fails", "[fake_driver]") {
	FakeOdbcDriver fake;
	OpenHandles open;
	SQLHANDLE descriptor = SQL_NULL_HANDLE;

	CHECK(open.api.AllocHandle(SQL_HANDLE_DESC, open.connection.value, &descriptor) == SQL_ERROR);
	CHECK(descriptor == SQL_NULL_HANDLE);
}

// The fake has no such handle to allocate, so this call ignores its script: a scripted
// SQL_INVALID_HANDLE still comes back as the SQL_ERROR that ODBC gives an invalid handle type.
TEST_CASE("FailCall cannot change what a handle type the fake does not model returns", "[fake_driver]") {
	FakeOdbcDriver fake;
	OpenHandles open; // three SQLAllocHandle calls
	fake.FailCall("SQLAllocHandle", 4, SQL_INVALID_HANDLE, {});
	SQLHANDLE descriptor = SQL_NULL_HANDLE;

	CHECK(open.api.AllocHandle(SQL_HANDLE_DESC, open.connection.value, &descriptor) == SQL_ERROR);

	CHECK(descriptor == SQL_NULL_HANDLE);
}

TEST_CASE("AllocatedHandles names the handles still allocated, oldest first", "[fake_driver]") {
	FakeOdbcDriver fake;
	OpenHandles open;

	CHECK(fake.AllocatedHandles() == "environment, connection, statement");
}

// Passes only when the destructor's leak check fails the test.
TEST_CASE("A handle left allocated when the fake is destroyed fails the test", "[fake_driver][!shouldfail]") {
	FakeOdbcDriver fake;
	SQLHANDLE environment = SQL_NULL_HANDLE;
	Api().AllocHandle(SQL_HANDLE_ENV, SQL_NULL_HANDLE, &environment);
}

// The ODBC client's teardown abandons a handle whose free failed rather than calling again, so the fake does
// not count it as leaked: the connection below is never freed and this case still passes.
TEST_CASE("A handle whose scripted SQLFreeHandle failed is left out of the leak check", "[fake_driver]") {
	FakeOdbcDriver fake;
	HandleGuard environment {SQL_HANDLE_ENV, SQL_NULL_HANDLE};
	SQLHANDLE connection = SQL_NULL_HANDLE;
	REQUIRE(environment.api.AllocHandle(SQL_HANDLE_DBC, environment.value, &connection) == SQL_SUCCESS);
	fake.FailCall("SQLFreeHandle", 1, SQL_ERROR, {{"HY000", 0, "connection is busy"}});

	CHECK(environment.api.FreeHandle(SQL_HANDLE_DBC, connection) == SQL_ERROR);

	CHECK(fake.AllocatedHandles() == "environment");
}

// The ODBC client's teardown frees a connection whose statements it has not freed, so an abandoned
// connection's statements go with it: the driver frees them inside RS_SQLFreeConnect (public source
// v2.3.0, rsconnect.cpp:1660-1669), leaving the caller no handle of its own to free.
TEST_CASE("The statements of a connection whose scripted SQLFreeHandle failed are abandoned with it", "[fake_driver]") {
	FakeOdbcDriver fake;
	HandleGuard environment {SQL_HANDLE_ENV, SQL_NULL_HANDLE};
	SQLHANDLE connection = SQL_NULL_HANDLE;
	SQLHANDLE statement = SQL_NULL_HANDLE;
	REQUIRE(environment.api.AllocHandle(SQL_HANDLE_DBC, environment.value, &connection) == SQL_SUCCESS);
	REQUIRE(environment.api.AllocHandle(SQL_HANDLE_STMT, connection, &statement) == SQL_SUCCESS);
	fake.FailCall("SQLFreeHandle", 1, SQL_ERROR, {{"HY000", 0, "connection is busy"}});

	REQUIRE(environment.api.FreeHandle(SQL_HANDLE_DBC, connection) == SQL_ERROR);

	CHECK(fake.AllocatedHandles() == "environment");
}

// Passes only when the leak check fails the test: abandoning one handle does not excuse another.
// The leaked connection is a sibling of the abandoned one, not its child, so the cascade above
// cannot account for it. It asserts nothing, so the leak is the only failure it can report.
TEST_CASE("A handle left allocated beside an abandoned one still fails the test", "[fake_driver][!shouldfail]") {
	FakeOdbcDriver fake;
	HandleGuard environment {SQL_HANDLE_ENV, SQL_NULL_HANDLE};
	SQLHANDLE abandoned = SQL_NULL_HANDLE;
	SQLHANDLE leaked = SQL_NULL_HANDLE;
	environment.api.AllocHandle(SQL_HANDLE_DBC, environment.value, &abandoned);
	environment.api.AllocHandle(SQL_HANDLE_DBC, environment.value, &leaked);
	fake.FailCall("SQLFreeHandle", 1, SQL_ERROR, {});
	environment.api.FreeHandle(SQL_HANDLE_DBC, abandoned);
}

// The ODBC client's teardown reads the records of the free it could not complete, which is why an abandoned
// handle stays readable.
TEST_CASE("An abandoned handle still reads the diagnostics its failed free left", "[fake_driver]") {
	FakeOdbcDriver fake;
	HandleGuard environment {SQL_HANDLE_ENV, SQL_NULL_HANDLE};
	SQLHANDLE connection = SQL_NULL_HANDLE;
	REQUIRE(environment.api.AllocHandle(SQL_HANDLE_DBC, environment.value, &connection) == SQL_SUCCESS);
	fake.FailCall("SQLFreeHandle", 1, SQL_ERROR, {{"HY000", 0, "connection is busy"}});
	REQUIRE(environment.api.FreeHandle(SQL_HANDLE_DBC, connection) == SQL_ERROR);

	CHECK(DiagRecord(environment.api, SQL_HANDLE_DBC, connection, 1) == "HY000 0 connection is busy");
	CHECK(DiagRecordCount(environment.api, SQL_HANDLE_DBC, connection) == 1);
}

// The ODBC client's teardown runs in destructors and on its producer thread, so the fake records the misuse
// and reports it when it is destroyed rather than throwing where the exception would end the run.
TEST_CASE("A call other than a diagnostic read on an abandoned handle is refused and recorded", "[fake_driver]") {
	FakeOdbcDriver fake;
	fake.AllowAbandonedHandleMisuse(); // this case makes the misuse on purpose
	HandleGuard environment {SQL_HANDLE_ENV, SQL_NULL_HANDLE};
	SQLHANDLE connection = SQL_NULL_HANDLE;
	REQUIRE(environment.api.AllocHandle(SQL_HANDLE_DBC, environment.value, &connection) == SQL_SUCCESS);
	fake.FailCall("SQLFreeHandle", 1, SQL_ERROR, {});
	REQUIRE(environment.api.FreeHandle(SQL_HANDLE_DBC, connection) == SQL_ERROR);

	CHECK(environment.api.Disconnect(connection) == SQL_INVALID_HANDLE);
	CHECK(environment.api.FreeHandle(SQL_HANDLE_DBC, connection) == SQL_INVALID_HANDLE);

	CHECK(fake.MisusedAbandonedHandles() == "SQLDisconnect on a connection, SQLFreeHandle on a connection");
}

// Passes only when the destructor reports the misuse. It asserts nothing, so that report is the
// only failure it can produce; the handle it abandons is left out of the leak check.
TEST_CASE("A call on an abandoned handle fails the test when the fake is destroyed", "[fake_driver][!shouldfail]") {
	FakeOdbcDriver fake;
	HandleGuard environment {SQL_HANDLE_ENV, SQL_NULL_HANDLE};
	SQLHANDLE connection = SQL_NULL_HANDLE;
	environment.api.AllocHandle(SQL_HANDLE_DBC, environment.value, &connection);
	fake.FailCall("SQLFreeHandle", 1, SQL_ERROR, {});
	environment.api.FreeHandle(SQL_HANDLE_DBC, connection);
	environment.api.Disconnect(connection);
}

TEST_CASE("A fake that received no such call reports no misuse", "[fake_driver]") {
	FakeOdbcDriver fake;
	OpenHandles open;

	CHECK(fake.MisusedAbandonedHandles().empty());
}

TEST_CASE("The call log names each handle call in order, a refused one included", "[fake_driver]") {
	FakeOdbcDriver fake;
	OpenHandles open;
	REQUIRE(open.api.FreeHandle(SQL_HANDLE_STMT, open.statement.value) == SQL_SUCCESS);
	REQUIRE(open.api.FreeHandle(SQL_HANDLE_STMT, open.statement.value) == SQL_INVALID_HANDLE);

	const auto logged = EntryPointsSince(fake, 0);
	CHECK(logged == std::vector<std::string> {"SQLAllocHandle", "SQLAllocHandle", "SQLAllocHandle", "SQLFreeHandle",
	                                          "SQLFreeHandle"});
}

TEST_CASE("Eight threads allocating and freeing handles at once get distinct handles", "[fake_driver]") {
	FakeOdbcDriver fake;
	OpenHandles open;
	const int rounds = 200;
	std::atomic<int> failures {0};
	RunConcurrently(8, [&]() {
		for (int round = 0; round < rounds; round++) {
			SQLHANDLE connection = SQL_NULL_HANDLE;
			SQLHANDLE statement = SQL_NULL_HANDLE;
			// Two threads given one handle would fail the second free of it.
			const bool ok = open.api.AllocHandle(SQL_HANDLE_DBC, open.environment.value, &connection) == SQL_SUCCESS &&
			                open.api.AllocHandle(SQL_HANDLE_STMT, connection, &statement) == SQL_SUCCESS &&
			                open.api.FreeHandle(SQL_HANDLE_STMT, statement) == SQL_SUCCESS &&
			                open.api.FreeHandle(SQL_HANDLE_DBC, connection) == SQL_SUCCESS;
			failures += ok ? 0 : 1;
		}
	});

	CHECK(failures == 0);
}

TEST_CASE("Two fakes constructed one after the other both pass the leak check", "[fake_driver]") {
	{
		FakeOdbcDriver first;
		OpenHandles open;
	}
	FakeOdbcDriver second;
	OpenHandles open;
}

TEST_CASE("A handle kept from an earlier fake is invalid in the next one", "[fake_driver]") {
	SQLHANDLE first_environment = SQL_NULL_HANDLE;
	{
		FakeOdbcDriver first;
		OpenHandles open;
		first_environment = open.environment.value;
	}
	FakeOdbcDriver second;
	OpenHandles open;

	CHECK(open.environment.value != first_environment);
	CHECK(open.api.FreeHandle(SQL_HANDLE_ENV, first_environment) == SQL_INVALID_HANDLE);
}

// The driver drops a connection's statements at SQLFreeHandle, not at SQLDisconnect (the driver's
// public source, v2.3.0, rsconnect.cpp:1660-1669 in RS_SQLFreeConnect, against SQLDisconnect at
// rsconnect.cpp:1569-1611), and the ODBC client's teardown frees the connection without having freed the
// statement of a result it marked closed.
TEST_CASE("A connection's statements are freed with the connection, not at SQLDisconnect", "[fake_driver]") {
	FakeOdbcDriver fake;
	OpenHandles open;
	REQUIRE(open.api.Disconnect(open.connection.value) == SQL_SUCCESS);
	REQUIRE(fake.AllocatedHandles() == "environment, connection, statement");
	REQUIRE(open.api.FreeHandle(SQL_HANDLE_DBC, open.connection.value) == SQL_SUCCESS);

	CHECK(fake.AllocatedHandles() == "environment");
	CHECK(open.api.FreeHandle(SQL_HANDLE_STMT, open.statement.value) == SQL_INVALID_HANDLE);
}

// The interface wants a connection that was never freed reported, so this free does not cascade, although
// the driver's RS_SQLFreeEnv disconnects and frees each connection (public source v2.3.0,
// rsconnect.cpp:1791-1796): the ODBC client never relies on that.
TEST_CASE("Freeing an environment leaves its connections allocated", "[fake_driver]") {
	FakeOdbcDriver fake;
	OpenHandles open;
	REQUIRE(open.api.FreeHandle(SQL_HANDLE_STMT, open.statement.value) == SQL_SUCCESS);
	REQUIRE(open.api.FreeHandle(SQL_HANDLE_ENV, open.environment.value) == SQL_SUCCESS);

	CHECK(fake.AllocatedHandles() == "connection");
}

TEST_CASE("SQLDriverConnect succeeds until the test scripts another outcome", "[fake_driver]") {
	FakeOdbcDriver fake;
	OpenHandles open;
	CHECK(Connect(open.connection, "Host={a}") == SQL_SUCCESS);
	REQUIRE(open.api.Disconnect(open.connection.value) == SQL_SUCCESS);
	fake.SetConnectOutcome(SQL_ERROR, {{"08001", 0, "could not connect"}});
	CHECK(Connect(open.connection, "Host={a}") == SQL_ERROR);
}

// The driver's public source (v2.3.0, rsconnect.cpp:862-868 in RS_SQLDriverConnect) refuses a
// connect on a connection still open.
TEST_CASE("SQLDriverConnect on a connection still open fails with 08002", "[fake_driver]") {
	FakeOdbcDriver fake;
	OpenHandles open;
	REQUIRE(Connect(open.connection, "Host={a}") == SQL_SUCCESS);

	CHECK(Connect(open.connection, "Host={a}") == SQL_ERROR);
	CHECK(DiagRecord(open.api, SQL_HANDLE_DBC, open.connection.value, 1) == "08002 0 Connection name in use");
}

// ODBC lets a handle whose connect failed connect again. The connect retry does not rely on it: it
// frees the failed handle and allocates another.
TEST_CASE("A connection whose SQLDriverConnect failed can connect again", "[fake_driver]") {
	FakeOdbcDriver fake;
	OpenHandles open;
	fake.SetConnectOutcome(SQL_ERROR, {{"HY000", 0, "timeout expired"}});
	REQUIRE(Connect(open.connection, "Host={a}") == SQL_ERROR);
	fake.SetConnectOutcome(SQL_SUCCESS, {});

	CHECK(Connect(open.connection, "Host={a}") == SQL_SUCCESS);
}

// Measured on driver 2.3.0.0: a successful connect left no diagnostic records. An earlier driver
// release left an 01004 record on every connect, so a test must not expect one.
TEST_CASE("A default SQLDriverConnect succeeds with no diagnostic records", "[fake_driver]") {
	FakeOdbcDriver fake;
	OpenHandles open;
	REQUIRE(Connect(open.connection, "Host={a}") == SQL_SUCCESS);

	CHECK(DiagRecordCount(open.api, SQL_HANDLE_DBC, open.connection.value) == 0);
}

TEST_CASE("LastConnectionString is empty before any SQLDriverConnect", "[fake_driver]") {
	FakeOdbcDriver fake;
	OpenHandles open;

	CHECK(fake.LastConnectionString().empty());
}

TEST_CASE("LastConnectionString is the latest string SQLDriverConnect received, to its length", "[fake_driver]") {
	FakeOdbcDriver fake;
	OpenHandles open;
	REQUIRE(Connect(open.connection, "Host={a}") == SQL_SUCCESS);
	REQUIRE(open.api.Disconnect(open.connection.value) == SQL_SUCCESS);
	REQUIRE(open.api.DriverConnect(open.connection.value, nullptr, Text("Host={b};ignored"), 8, nullptr, 0, nullptr,
	                               SQL_DRIVER_NOPROMPT) == SQL_SUCCESS);

	CHECK(fake.LastConnectionString() == "Host={b}");
}

TEST_CASE("LastConnectOutput is no buffer of capacity -1 and completion -1 before any SQLDriverConnect",
          "[fake_driver]") {
	FakeOdbcDriver fake;
	OpenHandles open;

	CHECK_FALSE(fake.LastConnectOutput().has_buffer);
	CHECK(fake.LastConnectOutput().capacity == -1);
	CHECK(fake.LastConnectOutput().completion == -1);
}

TEST_CASE("LastConnectOutput records the latest SQLDriverConnect's buffer, capacity and completion", "[fake_driver]") {
	FakeOdbcDriver fake;
	OpenHandles open;
	SQLCHAR completed[16] = {};
	REQUIRE(open.api.DriverConnect(open.connection.value, nullptr, Text("Host={a}"), SQL_NTS, completed,
	                               sizeof(completed), nullptr, SQL_DRIVER_COMPLETE) == SQL_SUCCESS);

	CHECK(fake.LastConnectOutput().has_buffer);
	CHECK(fake.LastConnectOutput().capacity == 16);
	CHECK(fake.LastConnectOutput().completion == SQL_DRIVER_COMPLETE);
}

TEST_CASE("SQLFreeHandle's call records the type of the handle it was given", "[fake_driver]") {
	FakeOdbcDriver fake;
	{ OpenStatement open; }

	std::vector<std::string> freed;
	for (const auto &call : fake.Calls()) {
		if (call.entry_point == "SQLFreeHandle") {
			freed.push_back(call.detail);
		}
	}
	CHECK(freed == std::vector<std::string> {"statement", "connection", "environment"});
}

TEST_CASE("LastConnectOutput is the output buffer the latest SQLDriverConnect was given", "[fake_driver]") {
	FakeOdbcDriver fake;
	OpenHandles open;
	SQLCHAR completed[16];
	REQUIRE(open.api.DriverConnect(open.connection.value, nullptr, Text("Host={a}"), SQL_NTS, completed, 16, nullptr,
	                               SQL_DRIVER_NOPROMPT) == SQL_SUCCESS);
	CHECK(fake.LastConnectOutput().has_buffer);
	CHECK(fake.LastConnectOutput().capacity == 16);
	REQUIRE(open.api.Disconnect(open.connection.value) == SQL_SUCCESS);
	REQUIRE(Connect(open.connection, "Host={b}") == SQL_SUCCESS);

	CHECK_FALSE(fake.LastConnectOutput().has_buffer);
	CHECK(fake.LastConnectOutput().capacity == 0);
}

// Measured on driver 2.3.0.0: with a zero-capacity output buffer the driver still reported the
// completed string's required length, 276 bytes, and wrote nothing to the buffer.
TEST_CASE("A successful SQLDriverConnect reports the completed string's length", "[fake_driver]") {
	FakeOdbcDriver fake;
	OpenHandles open;
	SQLSMALLINT completed_length = -7;

	REQUIRE(open.api.DriverConnect(open.connection.value, nullptr, Text("Host={a}"), SQL_NTS, nullptr, 0,
	                               &completed_length, SQL_DRIVER_NOPROMPT) == SQL_SUCCESS);

	CHECK(completed_length == 8);
}

TEST_CASE("SQLGetConnectAttr reports a dead connection only once the test sets it", "[fake_driver]") {
	FakeOdbcDriver fake;
	OpenHandles open;
	SQLUINTEGER dead = 99;
	REQUIRE(open.api.GetConnectAttr(open.connection.value, SQL_ATTR_CONNECTION_DEAD, &dead, 0, nullptr) == SQL_SUCCESS);
	CHECK(dead == SQL_CD_FALSE);
	fake.SetConnectionDead(true);
	REQUIRE(open.api.GetConnectAttr(open.connection.value, SQL_ATTR_CONNECTION_DEAD, &dead, 0, nullptr) == SQL_SUCCESS);
	CHECK(dead == SQL_CD_TRUE);
}

TEST_CASE("SQLGetConnectAttr fails any attribute but the connection-dead flag", "[fake_driver]") {
	FakeOdbcDriver fake;
	OpenHandles open;
	SQLUINTEGER value = 99;

	CHECK(open.api.GetConnectAttr(open.connection.value, SQL_ATTR_AUTOCOMMIT, &value, 0, nullptr) == SQL_ERROR);

	CHECK(value == 99);
}

// Measured on driver 2.3.0.0: SQL_ATTR_CONNECTION_DEAD is written as a 4-byte SQLUINTEGER and the
// length out-param is left untouched. A wider write would reach the second cell.
TEST_CASE("SQLGetConnectAttr writes the dead flag as four bytes and no length", "[fake_driver]") {
	FakeOdbcDriver fake;
	OpenHandles open;
	SQLUINTEGER cells[2] = {99, 99};
	SQLINTEGER length = -7;

	REQUIRE(open.api.GetConnectAttr(open.connection.value, SQL_ATTR_CONNECTION_DEAD, cells, 0, &length) == SQL_SUCCESS);

	CHECK(cells[0] == SQL_CD_FALSE);
	CHECK(cells[1] == 99);
	CHECK(length == -7);
}

// Measured on driver 2.3.0.0: SQLGetInfo(SQL_GETDATA_EXTENSIONS) returned 0xb.
TEST_CASE("SQLGetInfo reports driver 2.3.0.0's SQL_GETDATA_EXTENSIONS before the test sets any", "[fake_driver]") {
	FakeOdbcDriver fake;
	OpenHandles open;
	SQLUINTEGER value = 0;
	REQUIRE(open.api.GetInfo(open.connection.value, SQL_GETDATA_EXTENSIONS, &value, 0, nullptr) == SQL_SUCCESS);

	// SQL_GD_ANY_COLUMN, SQL_GD_ANY_ORDER and SQL_GD_BOUND, but not SQL_GD_BLOCK.
	CHECK(value == 0xb);
}

TEST_CASE("SQLGetInfo answers an info type the test set", "[fake_driver]") {
	FakeOdbcDriver fake;
	OpenHandles open;
	fake.SetInfo(SQL_GETDATA_EXTENSIONS, SQL_GD_ANY_COLUMN);
	SQLUINTEGER value = 0;

	REQUIRE(open.api.GetInfo(open.connection.value, SQL_GETDATA_EXTENSIONS, &value, 0, nullptr) == SQL_SUCCESS);

	CHECK(value == SQL_GD_ANY_COLUMN);
}

TEST_CASE("SQLGetInfo fails an info type the test never set", "[fake_driver]") {
	FakeOdbcDriver fake;
	OpenHandles open;
	SQLUINTEGER value = 0;

	CHECK(open.api.GetInfo(open.connection.value, SQL_TXN_CAPABLE, &value, 0, nullptr) == SQL_ERROR);
}

// Measured on driver 2.3.0.0: SQLGetInfo(SQL_GETDATA_EXTENSIONS) reported length 4.
TEST_CASE("SQLGetInfo reports the width of the value it wrote", "[fake_driver]") {
	FakeOdbcDriver fake;
	OpenHandles open;
	SQLUINTEGER value = 0;
	SQLSMALLINT length = -7;

	REQUIRE(open.api.GetInfo(open.connection.value, SQL_GETDATA_EXTENSIONS, &value, 0, &length) == SQL_SUCCESS);

	CHECK(length == 4);
}

TEST_CASE("The call log records each call in order with what it set", "[fake_driver]") {
	FakeOdbcDriver fake;
	OpenHandles open;
	const auto first_call = fake.Calls().size();
	CHECK(open.api.SetEnvAttr(open.environment.value, SQL_ATTR_ODBC_VERSION, reinterpret_cast<SQLPOINTER>(SQL_OV_ODBC3),
	                          0) == SQL_SUCCESS);
	CHECK(open.api.SetConnectAttr(open.connection.value, SQL_ATTR_AUTOCOMMIT,
	                              reinterpret_cast<SQLPOINTER>(SQL_AUTOCOMMIT_ON), 0) == SQL_SUCCESS);
	CHECK(Connect(open.connection, "Host={a}") == SQL_SUCCESS);
	CHECK(open.api.Prepare(open.statement.value, Text("SELECT 1"), SQL_NTS) == SQL_SUCCESS);
	CHECK(open.api.EndTran(SQL_HANDLE_DBC, open.connection.value, SQL_ROLLBACK) == SQL_SUCCESS);
	CHECK(open.api.Disconnect(open.connection.value) == SQL_SUCCESS);
	const std::vector<std::pair<std::string, std::string>> expected {
	    {"SQLSetEnvAttr", std::to_string(SQL_ATTR_ODBC_VERSION) + "=" + std::to_string(SQL_OV_ODBC3)},
	    {"SQLSetConnectAttr", std::to_string(SQL_ATTR_AUTOCOMMIT) + "=" + std::to_string(SQL_AUTOCOMMIT_ON)},
	    {"SQLDriverConnect", "Host={a}"},
	    {"SQLPrepare", "SELECT 1"},
	    {"SQLEndTran", std::to_string(SQL_ROLLBACK)},
	    {"SQLDisconnect", ""}};

	CHECK(CallsSince(fake, first_call) == expected);
}

// The literals are the vendored ODBC inputs: SQL_ATTR_CONNECTION_DEAD is 1209 (sqlext.h:141) and
// SQL_GETDATA_EXTENSIONS is 81 (sql.h:438).
TEST_CASE("The call log records the attribute or info type a read asked for", "[fake_driver]") {
	FakeOdbcDriver fake;
	OpenHandles open;
	const auto first_call = fake.Calls().size();
	SQLUINTEGER value = 0;
	REQUIRE(open.api.GetConnectAttr(open.connection.value, SQL_ATTR_CONNECTION_DEAD, &value, 0, nullptr) ==
	        SQL_SUCCESS);
	REQUIRE(open.api.GetInfo(open.connection.value, SQL_GETDATA_EXTENSIONS, &value, 0, nullptr) == SQL_SUCCESS);

	CHECK(CallsSince(fake, first_call) ==
	      std::vector<std::pair<std::string, std::string>> {{"SQLGetConnectAttr", "1209"}, {"SQLGetInfo", "81"}});
}

// FailCall and BlockCall find their script by the name the trampoline logs, so a mistyped name would
// leave a test passing without the call it scripted.
TEST_CASE("The call log names each statement and diagnostic call", "[fake_driver]") {
	FakeOdbcDriver fake;
	OpenStatement open;
	const auto first_call = fake.Calls().size();
	SQLSMALLINT columns = 0;
	char value[8] = {};
	SQLLEN length = 0;
	SQLINTEGER count = 0;
	open.api.NumResultCols(open.statement.value, &columns);
	open.api.DescribeCol(open.statement.value, 1, nullptr, 0, nullptr, nullptr, nullptr, nullptr, nullptr);
	open.api.GetData(open.statement.value, 1, SQL_C_CHAR, value, sizeof(value), &length);
	open.api.GetDiagRec(SQL_HANDLE_STMT, open.statement.value, 1, nullptr, nullptr, nullptr, 0, nullptr);
	open.api.GetDiagField(SQL_HANDLE_STMT, open.statement.value, 0, SQL_DIAG_NUMBER, &count, 0, nullptr);

	const auto logged = EntryPointsSince(fake, first_call);
	CHECK(logged == std::vector<std::string> {"SQLNumResultCols", "SQLDescribeCol", "SQLGetData", "SQLGetDiagRec",
	                                          "SQLGetDiagField"});
}

TEST_CASE("A prepared statement describes one INTEGER column, and once executed returns one row holding 1, read once",
          "[fake_driver]") {
	FakeOdbcDriver fake;
	OpenStatement open;
	REQUIRE(open.api.Prepare(open.statement.value, Text("SELECT 1"), SQL_NTS) == SQL_SUCCESS);
	SQLSMALLINT columns = 0;
	REQUIRE(open.api.NumResultCols(open.statement.value, &columns) == SQL_SUCCESS);
	CHECK(columns == 1);
	SQLCHAR name[16];
	SQLSMALLINT type = 0;
	SQLULEN column_size = 0;
	SQLSMALLINT digits = -1;
	SQLSMALLINT nullable = -1;
	// ODBC lets a caller pass null for any output it does not want, here the name's length.
	REQUIRE(open.api.DescribeCol(open.statement.value, 1, name, sizeof(name), nullptr, &type, &column_size, &digits,
	                             &nullable) == SQL_SUCCESS);
	CHECK(std::string(reinterpret_cast<char *>(name)) == "?column?");
	CHECK(type == SQL_INTEGER);
	CHECK(column_size == 10);
	CHECK(digits == 0);
	CHECK(nullable == SQL_NULLABLE);
	REQUIRE(open.api.Execute(open.statement.value) == SQL_SUCCESS);
	REQUIRE(open.api.Fetch(open.statement.value) == SQL_SUCCESS);
	char value[8];
	SQLLEN length = 0;
	REQUIRE(open.api.GetData(open.statement.value, 1, SQL_C_CHAR, value, sizeof(value), &length) == SQL_SUCCESS);
	CHECK(std::string(value) == "1");
	CHECK(length == 1);
	CHECK(open.api.GetData(open.statement.value, 1, SQL_C_CHAR, value, sizeof(value), &length) == SQL_NO_DATA);
	CHECK(open.api.Fetch(open.statement.value) == SQL_NO_DATA);
}

TEST_CASE("The default script has no second column", "[fake_driver]") {
	FakeOdbcDriver fake;
	OpenStatement open;
	FetchDefaultRow(open);
	char value[8] = {};
	SQLLEN length = 0;

	CHECK(open.api.DescribeCol(open.statement.value, 2, nullptr, 0, nullptr, nullptr, nullptr, nullptr, nullptr) ==
	      SQL_ERROR);
	CHECK(DiagRecord(open.api, SQL_HANDLE_STMT, open.statement.value, 1) ==
	      "HY000 0 SQLDescribeCol: Invalid column number");
	CHECK(open.api.GetData(open.statement.value, 2, SQL_C_CHAR, value, sizeof(value), &length) == SQL_ERROR);
	CHECK(DiagRecord(open.api, SQL_HANDLE_STMT, open.statement.value, 1) == "07009 0 Invalid descriptor index");
}

TEST_CASE("SQLGetData before SQLFetch fails", "[fake_driver]") {
	FakeOdbcDriver fake;
	OpenStatement open;
	REQUIRE(open.api.Prepare(open.statement.value, Text("SELECT 1"), SQL_NTS) == SQL_SUCCESS);
	REQUIRE(open.api.Execute(open.statement.value) == SQL_SUCCESS);
	char value[8] = {};
	SQLLEN length = 0;

	CHECK(open.api.GetData(open.statement.value, 1, SQL_C_CHAR, value, sizeof(value), &length) == SQL_ERROR);
	CHECK(DiagRecord(open.api, SQL_HANDLE_STMT, open.statement.value, 1) == "24000 0 Invalid cursor state");
}

// The driver's SQLFetch posts 24000 when the statement has no result, and HY010 only for a closed
// statement (public source v2.3.0, rsresult.cpp:2112-2118).
TEST_CASE("SQLFetch fails on a statement prepared but never executed", "[fake_driver]") {
	FakeOdbcDriver fake;
	OpenStatement open;
	REQUIRE(open.api.Prepare(open.statement.value, Text("SELECT 1"), SQL_NTS) == SQL_SUCCESS);

	CHECK(open.api.Fetch(open.statement.value) == SQL_ERROR);

	CHECK(DiagRecord(open.api, SQL_HANDLE_STMT, open.statement.value, 1) == "24000 0 Invalid cursor state");
}

// A statement never prepared has no result either, so it gets the same record as one prepared and
// not executed, rather than the SQL_NO_DATA an exhausted result returns.
TEST_CASE("SQLFetch fails on a statement never prepared", "[fake_driver]") {
	FakeOdbcDriver fake;
	OpenStatement open;

	CHECK(open.api.Fetch(open.statement.value) == SQL_ERROR);

	CHECK(DiagRecord(open.api, SQL_HANDLE_STMT, open.statement.value, 1) == "24000 0 Invalid cursor state");
}

// The fake refuses this where the driver would send it to the server; fake_odbc_driver.cpp's
// Execute cites both sources. A fake that accepted it would let code that skips SQLPrepare pass.
TEST_CASE("SQLExecute fails on a statement that was never prepared", "[fake_driver]") {
	FakeOdbcDriver fake;
	OpenStatement open;

	CHECK(open.api.Execute(open.statement.value) == SQL_ERROR);

	CHECK(DiagRecord(open.api, SQL_HANDLE_STMT, open.statement.value, 1) == "HY010 0 Function sequence error");
}

TEST_CASE("A statement executed again returns its row again", "[fake_driver]") {
	FakeOdbcDriver fake;
	OpenStatement open;
	FetchDefaultRow(open);
	REQUIRE(open.api.Fetch(open.statement.value) == SQL_NO_DATA);

	REQUIRE(open.api.Execute(open.statement.value) == SQL_SUCCESS);

	CHECK(open.api.Fetch(open.statement.value) == SQL_SUCCESS);
}

// The driver's SQLPrepare releases an open result (public source v2.3.0, rsprepare.cpp:700, which
// calls makeItReadyForNewQueryExecution) and leaves the statement prepared, so the next SQLFetch
// posts 24000.
TEST_CASE("A statement prepared again fails SQLFetch until it is executed again", "[fake_driver]") {
	FakeOdbcDriver fake;
	OpenStatement open;
	REQUIRE(open.api.Prepare(open.statement.value, Text("SELECT 1"), SQL_NTS) == SQL_SUCCESS);
	REQUIRE(open.api.Execute(open.statement.value) == SQL_SUCCESS);
	REQUIRE(open.api.Prepare(open.statement.value, Text("SELECT 1"), SQL_NTS) == SQL_SUCCESS);

	CHECK(open.api.Fetch(open.statement.value) == SQL_ERROR);
	CHECK(DiagRecord(open.api, SQL_HANDLE_STMT, open.statement.value, 1) == "24000 0 Invalid cursor state");
}

TEST_CASE("A cut SQLGetData reports the full length, posts 01004 and leaves the value readable", "[fake_driver]") {
	FakeOdbcDriver fake;
	OpenStatement open;
	FetchDefaultRow(open);
	char value[1] = {'x'};
	SQLLEN length = 0;
	CHECK(open.api.GetData(open.statement.value, 1, SQL_C_CHAR, value, sizeof(value), &length) ==
	      SQL_SUCCESS_WITH_INFO);
	CHECK(value[0] == '\0');
	CHECK(length == 1);
	CHECK(DiagRecord(open.api, SQL_HANDLE_STMT, open.statement.value, 1) == "01004 0 String data, right truncated");

	char whole[8] = {};
	CHECK(open.api.GetData(open.statement.value, 1, SQL_C_CHAR, whole, sizeof(whole), &length) == SQL_SUCCESS);

	CHECK(std::string(whole) == "1");
}

TEST_CASE("A cut SQLDescribeCol reports the full name length and posts 01004", "[fake_driver]") {
	FakeOdbcDriver fake;
	OpenStatement open;
	REQUIRE(open.api.Prepare(open.statement.value, Text("SELECT 1"), SQL_NTS) == SQL_SUCCESS);
	SQLCHAR name[4] = {};
	SQLSMALLINT name_length = 0;

	CHECK(open.api.DescribeCol(open.statement.value, 1, name, sizeof(name), &name_length, nullptr, nullptr, nullptr,
	                           nullptr) == SQL_SUCCESS_WITH_INFO);

	CHECK(std::string(reinterpret_cast<char *>(name)) == "?co");
	CHECK(name_length == 8);
	CHECK(DiagRecord(open.api, SQL_HANDLE_STMT, open.statement.value, 1) == "01004 0 String data, right truncated");
}

TEST_CASE("SQLGetDiagRec reads a handle's records in order, then reports no more", "[fake_driver]") {
	FakeOdbcDriver fake;
	OpenHandles open;
	fake.SetConnectOutcome(SQL_ERROR,
	                       {{"08001", 1, "could not connect"}, {"28000", 2, "password authentication failed"}});
	REQUIRE(Connect(open.connection, "Host={a}") == SQL_ERROR);
	CHECK(DiagRecord(open.api, SQL_HANDLE_DBC, open.connection.value, 1) == "08001 1 could not connect");
	CHECK(DiagRecord(open.api, SQL_HANDLE_DBC, open.connection.value, 2) == "28000 2 password authentication failed");
	CHECK(open.api.GetDiagRec(SQL_HANDLE_DBC, open.connection.value, 3, nullptr, nullptr, nullptr, 0, nullptr) ==
	      SQL_NO_DATA);
}

TEST_CASE("SQLGetDiagRec refuses record 0", "[fake_driver]") {
	FakeOdbcDriver fake;
	OpenHandles open;
	fake.SetConnectOutcome(SQL_ERROR, {{"08001", 0, "could not connect"}});
	REQUIRE(Connect(open.connection, "Host={a}") == SQL_ERROR);

	// Record 0 is no record at all; indexing the vector with it would read diags[-1].
	CHECK(open.api.GetDiagRec(SQL_HANDLE_DBC, open.connection.value, 0, nullptr, nullptr, nullptr, 0, nullptr) ==
	      SQL_ERROR);
}

TEST_CASE("SQLGetDiagRec reports a message length past 32767 in 16 bits, as the driver stores it", "[fake_driver]") {
	FakeOdbcDriver fake;
	fake.SetMessageLimit(65536);
	OpenHandles open;
	fake.SetConnectOutcome(SQL_ERROR, {{"08001", 0, std::string(40000, 'a')}, {"08001", 0, std::string(32767, 'b')}});
	REQUIRE(Connect(open.connection, "Host={a}") == SQL_ERROR);
	SQLCHAR message[8] = {};
	SQLSMALLINT length = 0;

	CHECK(open.api.GetDiagRec(SQL_HANDLE_DBC, open.connection.value, 1, nullptr, nullptr, message, sizeof(message),
	                          &length) == SQL_SUCCESS_WITH_INFO);
	CHECK(length == -25536);
	CHECK(std::string(reinterpret_cast<char *>(message)) == "aaaaaaa");
	REQUIRE(open.api.GetDiagRec(SQL_HANDLE_DBC, open.connection.value, 2, nullptr, nullptr, message, sizeof(message),
	                            &length) == SQL_SUCCESS_WITH_INFO);
	CHECK(length == 32767);
}

TEST_CASE("SQLGetDiagRec reports a cut message as SQL_SUCCESS when given no length pointer", "[fake_driver]") {
	FakeOdbcDriver fake;
	OpenHandles open;
	fake.SetConnectOutcome(SQL_ERROR, {{"08001", 0, "could not connect"}});
	REQUIRE(Connect(open.connection, "Host={a}") == SQL_ERROR);
	SQLCHAR message[8] = {};

	CHECK(open.api.GetDiagRec(SQL_HANDLE_DBC, open.connection.value, 1, nullptr, nullptr, message, sizeof(message),
	                          nullptr) == SQL_SUCCESS);
	CHECK(std::string(reinterpret_cast<char *>(message)) == "could n");
}

TEST_CASE("A record keeps the first 326 bytes of its message, as the driver keeps them", "[fake_driver]") {
	FakeOdbcDriver fake;
	OpenHandles open;
	fake.SetConnectOutcome(SQL_ERROR, {{"08001", 0, std::string(326, 'a') + "bc"}});
	REQUIRE(Connect(open.connection, "Host={a}") == SQL_ERROR);
	char message[512] = {};
	SQLSMALLINT length = 0;

	REQUIRE(open.api.GetDiagRec(SQL_HANDLE_DBC, open.connection.value, 1, nullptr, nullptr,
	                            reinterpret_cast<SQLCHAR *>(message), sizeof(message), &length) == SQL_SUCCESS);
	CHECK(std::string(message) == std::string(326, 'a'));
	CHECK(length == 326);
	REQUIRE(open.api.GetDiagField(SQL_HANDLE_DBC, open.connection.value, 1, SQL_DIAG_MESSAGE_TEXT, message,
	                              sizeof(message), &length) == SQL_SUCCESS);
	CHECK(std::string(message) == std::string(326, 'a'));
}

TEST_CASE("A message limit a test sets lets a longer message through", "[fake_driver]") {
	FakeOdbcDriver fake;
	fake.SetMessageLimit(327);
	OpenHandles open;
	fake.SetConnectOutcome(SQL_ERROR, {{"08001", 0, std::string(326, 'a') + "bc"}});
	REQUIRE(Connect(open.connection, "Host={a}") == SQL_ERROR);
	char message[512] = {};

	REQUIRE(open.api.GetDiagRec(SQL_HANDLE_DBC, open.connection.value, 1, nullptr, nullptr,
	                            reinterpret_cast<SQLCHAR *>(message), sizeof(message), nullptr) == SQL_SUCCESS);
	CHECK(std::string(message) == std::string(326, 'a') + "b");
}

TEST_CASE("SQLGetDiagField counts the records the latest call on the handle left", "[fake_driver]") {
	FakeOdbcDriver fake;
	OpenHandles open;
	CHECK(DiagRecordCount(open.api, SQL_HANDLE_DBC, open.connection.value) == 0);
	fake.SetConnectOutcome(SQL_ERROR, {{"08001", 0, "first"}, {"08001", 0, "second"}});
	REQUIRE(Connect(open.connection, "Host={a}") == SQL_ERROR);
	CHECK(DiagRecordCount(open.api, SQL_HANDLE_DBC, open.connection.value) == 2);
}

TEST_CASE("SQLGetDiagField reports the record count a test set in place of the handle's own", "[fake_driver]") {
	FakeOdbcDriver fake;
	fake.SetRecordCount(-5);
	OpenHandles open;
	fake.SetConnectOutcome(SQL_ERROR, {{"08001", 0, "first"}});
	REQUIRE(Connect(open.connection, "Host={a}") == SQL_ERROR);
	CHECK(DiagRecordCount(open.api, SQL_HANDLE_DBC, open.connection.value) == -5);
}

TEST_CASE("SQLGetDiagField returns a field the record carries", "[fake_driver]") {
	FakeOdbcDriver fake;
	OpenStatement open;
	fake.FailCall("SQLPrepare", 1, SQL_ERROR, {{"42601", 0, "syntax error", {{DETAIL_FIELD, "near FROM"}}}});
	REQUIRE(open.api.Prepare(open.statement.value, Text("SELECT FROM"), SQL_NTS) == SQL_ERROR);
	char value[16];
	SQLSMALLINT length = 0;

	REQUIRE(open.api.GetDiagField(SQL_HANDLE_STMT, open.statement.value, 1, DETAIL_FIELD, value, sizeof(value),
	                              &length) == SQL_SUCCESS);

	CHECK(std::string(value) == "near FROM");
	CHECK(length == 9);
}

// Probed on driver 2.3.0.0: it answered SQL_NO_DATA for each identifier tried outside ODBC's own fields.
TEST_CASE("SQLGetDiagField answers SQL_NO_DATA for a field the record lacks", "[fake_driver]") {
	FakeOdbcDriver fake;
	OpenStatement open;
	fake.FailCall("SQLPrepare", 1, SQL_ERROR, {{"42601", 0, "syntax error", {{DETAIL_FIELD, "near FROM"}}}});
	REQUIRE(open.api.Prepare(open.statement.value, Text("SELECT FROM"), SQL_NTS) == SQL_ERROR);
	char value[16];
	SQLSMALLINT length = 0;

	CHECK(open.api.GetDiagField(SQL_HANDLE_STMT, open.statement.value, 1, DETAIL_FIELD + 1, value, sizeof(value),
	                            &length) == SQL_NO_DATA);
}

// Measured on driver 2.3.0.0: SQLGetDiagField answered SQL_DIAG_SQLSTATE, SQL_DIAG_NATIVE and
// SQL_DIAG_MESSAGE_TEXT from the record, among others.
TEST_CASE("SQLGetDiagField reads the SQLSTATE, native code and message from the record", "[fake_driver]") {
	FakeOdbcDriver fake;
	OpenStatement open;
	fake.FailCall("SQLPrepare", 1, SQL_ERROR, {{"42601", 7, "syntax error"}});
	REQUIRE(open.api.Prepare(open.statement.value, Text("SELECT FROM"), SQL_NTS) == SQL_ERROR);
	char sqlstate[6] = {};
	char message[16] = {};
	SQLSMALLINT length = 0;
	SQLINTEGER native = 0;

	REQUIRE(open.api.GetDiagField(SQL_HANDLE_STMT, open.statement.value, 1, SQL_DIAG_SQLSTATE, sqlstate,
	                              sizeof(sqlstate), &length) == SQL_SUCCESS);
	REQUIRE(open.api.GetDiagField(SQL_HANDLE_STMT, open.statement.value, 1, SQL_DIAG_NATIVE, &native, 0, nullptr) ==
	        SQL_SUCCESS);
	REQUIRE(open.api.GetDiagField(SQL_HANDLE_STMT, open.statement.value, 1, SQL_DIAG_MESSAGE_TEXT, message,
	                              sizeof(message), &length) == SQL_SUCCESS);

	CHECK(std::string(sqlstate) == "42601");
	CHECK(native == 7);
	CHECK(std::string(message) == "syntax error");
}

TEST_CASE("A failed diagnostic read leaves the handle's records alone", "[fake_driver]") {
	FakeOdbcDriver fake;
	OpenHandles open;
	fake.SetConnectOutcome(SQL_ERROR, {{"08001", 0, "could not connect"}});
	REQUIRE(Connect(open.connection, "Host={a}") == SQL_ERROR);
	fake.FailCall("SQLGetDiagRec", 1, SQL_ERROR, {{"HY000", 0, "read failed"}});

	CHECK(open.api.GetDiagRec(SQL_HANDLE_DBC, open.connection.value, 1, nullptr, nullptr, nullptr, 0, nullptr) ==
	      SQL_ERROR);

	CHECK(DiagRecord(open.api, SQL_HANDLE_DBC, open.connection.value, 1) == "08001 0 could not connect");
}

TEST_CASE("A handle's diagnostics last until the next call on that handle", "[fake_driver]") {
	FakeOdbcDriver fake;
	OpenStatement open;
	fake.FailCall("SQLPrepare", 1, SQL_ERROR, {{"42601", 0, "syntax error"}});
	REQUIRE(open.api.Prepare(open.statement.value, Text("SELECT FROM"), SQL_NTS) == SQL_ERROR);
	REQUIRE(open.api.Disconnect(open.connection.value) == SQL_SUCCESS);
	CHECK(DiagRecordCount(open.api, SQL_HANDLE_STMT, open.statement.value) == 1);
	REQUIRE(open.api.Prepare(open.statement.value, Text("SELECT 1"), SQL_NTS) == SQL_SUCCESS);
	CHECK(DiagRecordCount(open.api, SQL_HANDLE_STMT, open.statement.value) == 0);
}

TEST_CASE("FailCall fails only the n-th call to its entry point, with its diagnostics", "[fake_driver]") {
	FakeOdbcDriver fake;
	OpenStatement open;
	PrepareDefaultResult(open);
	fake.FailCall("SQLExecute", 2, SQL_ERROR, {{"57014", 0, "canceling statement"}});
	CHECK(open.api.Execute(open.statement.value) == SQL_SUCCESS);
	CHECK(open.api.Execute(open.statement.value) == SQL_ERROR);
	CHECK(DiagRecord(open.api, SQL_HANDLE_STMT, open.statement.value, 1) == "57014 0 canceling statement");
	CHECK(open.api.Execute(open.statement.value) == SQL_SUCCESS);
}

// The failed allocation also writes a null handle over the caller's value, as the driver does
// (rsconnect.cpp RS_SQLAllocEnv).
TEST_CASE("FailCall fails an environment allocation without allocating one", "[fake_driver]") {
	FakeOdbcDriver fake;
	fake.FailCall("SQLAllocHandle", 1, SQL_ERROR, {});
	SQLHANDLE environment = reinterpret_cast<SQLHANDLE>(uintptr_t(0xdead));
	CHECK(Api().AllocHandle(SQL_HANDLE_ENV, SQL_NULL_HANDLE, &environment) == SQL_ERROR);
	CHECK(environment == SQL_NULL_HANDLE);
}

// An environment's allocation has no handle to put records on until it succeeds, so the fake drops a warning scripted
// for it where ODBC would put it on the new handle, and the call reports its own success; FailCall's code still comes
// back, which the case above pins.
TEST_CASE("A warning scripted for an environment allocation is dropped", "[fake_driver]") {
	FakeOdbcDriver fake;
	fake.WarnCall("SQLAllocHandle", 1, {{"01004", 0, "String data, right truncation"}});
	HandleGuard environment {SQL_HANDLE_ENV, SQL_NULL_HANDLE};

	CHECK(DiagRecordCount(environment.api, SQL_HANDLE_ENV, environment.value) == 0);
}

// ODBC reads an allocation's records from its input handle, and the driver posts them there
// (rsconnect.cpp RS_SQLAllocConnect, RS_SQLAllocStmt), so a scripted warning goes there too and the
// new handle starts with none.
TEST_CASE("A warning scripted for a connection or statement allocation goes on its input handle", "[fake_driver]") {
	FakeOdbcDriver fake;
	HandleGuard environment {SQL_HANDLE_ENV, SQL_NULL_HANDLE};
	const auto &api = environment.api;
	fake.WarnCall("SQLAllocHandle", 2, {{"01000", 0, "connection allocation"}});
	fake.WarnCall("SQLAllocHandle", 3, {{"01000", 0, "statement allocation"}});
	SQLHANDLE connection = SQL_NULL_HANDLE;
	SQLHANDLE statement = SQL_NULL_HANDLE;

	CHECK(api.AllocHandle(SQL_HANDLE_DBC, environment.value, &connection) == SQL_SUCCESS_WITH_INFO);
	CHECK(DiagRecord(api, SQL_HANDLE_ENV, environment.value, 1) == "01000 0 connection allocation");
	CHECK(DiagRecordCount(api, SQL_HANDLE_DBC, connection) == 0);
	CHECK(api.AllocHandle(SQL_HANDLE_STMT, connection, &statement) == SQL_SUCCESS_WITH_INFO);
	CHECK(DiagRecord(api, SQL_HANDLE_DBC, connection, 1) == "01000 0 statement allocation");
	CHECK(DiagRecordCount(api, SQL_HANDLE_STMT, statement) == 0);
	api.FreeHandle(SQL_HANDLE_STMT, statement);
	api.FreeHandle(SQL_HANDLE_DBC, connection);
}

// The driver writes a null handle whenever a connection or statement allocation fails, a null input
// handle included (rsconnect.cpp RS_SQLAllocConnect, RS_SQLAllocStmt), so a cleanup that frees a
// handle only when it is not null never sees a stale value.
TEST_CASE("A failed allocation writes a null handle over the caller's value", "[fake_driver]") {
	const SQLHANDLE stale = reinterpret_cast<SQLHANDLE>(uintptr_t(0xdead));
	FakeOdbcDriver fake;
	HandleGuard environment {SQL_HANDLE_ENV, SQL_NULL_HANDLE};
	fake.FailCall("SQLAllocHandle", 2, SQL_ERROR, {});
	SQLHANDLE connection = stale;
	CHECK(environment.api.AllocHandle(SQL_HANDLE_DBC, environment.value, &connection) == SQL_ERROR);
	CHECK(connection == SQL_NULL_HANDLE);

	SQLHANDLE statement = stale;
	CHECK(environment.api.AllocHandle(SQL_HANDLE_STMT, SQL_NULL_HANDLE, &statement) == SQL_INVALID_HANDLE);
	CHECK(statement == SQL_NULL_HANDLE);
}

// The driver refuses a null output pointer with SQL_ERROR for each of the three handle types, once
// the input handle has passed its check (rsconnect.cpp RS_SQLAllocEnv, RS_SQLAllocConnect, RS_SQLAllocStmt).
TEST_CASE("SQLAllocHandle with a null output pointer allocates nothing", "[fake_driver]") {
	FakeOdbcDriver fake;
	OpenConnection open;

	CHECK(open.api.AllocHandle(SQL_HANDLE_ENV, SQL_NULL_HANDLE, nullptr) == SQL_ERROR);
	CHECK(open.api.AllocHandle(SQL_HANDLE_DBC, open.environment.value, nullptr) == SQL_ERROR);
	CHECK(DiagRecordCount(open.api, SQL_HANDLE_ENV, open.environment.value) == 0);
	CHECK(open.api.AllocHandle(SQL_HANDLE_STMT, open.connection.value, nullptr) == SQL_ERROR);
	CHECK(DiagRecordCount(open.api, SQL_HANDLE_DBC, open.connection.value) == 0);

	CHECK(fake.AllocatedHandles() == "environment, connection");
}

TEST_CASE("FailCall refuses a name that is not a driver entry point", "[fake_driver]") {
#ifdef DUCKDB_CRASH_ON_ASSERT
	return; // InternalException aborts in this build
#endif
	FakeOdbcDriver fake;
	// An ODBC function the driver interface leaves out.
	CHECK_THROWS_AS(fake.FailCall("SQLCancel", 1, SQL_ERROR, {}), duckdb::InternalException);
	CHECK_THROWS_WITH(fake.FailCall("SQLCancel", 1, SQL_ERROR, {}),
	                  Catch::Contains("SQLCancel is not in REDSHIFT_ODBC_ENTRY_POINTS"));
}

// A scripted success code would report that the call worked with none of its work done, which no
// driver does: SQLExecute would leave the statement unexecuted and its rows unfetchable.
TEST_CASE("FailCall refuses a success return code", "[fake_driver]") {
#ifdef DUCKDB_CRASH_ON_ASSERT
	return; // InternalException aborts in this build
#endif
	FakeOdbcDriver fake;
	CHECK_THROWS_AS(fake.FailCall("SQLExecute", 1, SQL_SUCCESS, {}), duckdb::InternalException);
	CHECK_THROWS_WITH(
	    fake.FailCall("SQLExecute", 1, SQL_SUCCESS_WITH_INFO, {{"01004", 0, "String data, right truncation"}}),
	    Catch::Contains("FailCall cannot script a success code for SQLExecute"));
	CHECK_NOTHROW(fake.FailCall("SQLExecute", 1, SQL_ERROR, {}));
}

// What FailCall refuses: a success code with records, which leaves the statement executed and its
// rows where they were. The record is one the driver posts for a cut copy (rsutil.c
// copyStrDataSmallLen); which entry point carries it is the fake's own liberty.
TEST_CASE("A warning SQLExecute still leaves its rows fetchable", "[fake_driver]") {
	FakeOdbcDriver fake;
	OpenStatement open;
	fake.AddResult("SELECT a", {Rows({Varchar("a")}, {{"1"}, {"2"}})});
	REQUIRE(open.api.Prepare(open.statement.value, Text("SELECT a"), SQL_NTS) == SQL_SUCCESS);
	fake.WarnCall("SQLExecute", 1, {{"01004", 0, "String data, right truncation"}});

	CHECK(open.api.Execute(open.statement.value) == SQL_SUCCESS_WITH_INFO);

	CHECK(DiagRecord(open.api, SQL_HANDLE_STMT, open.statement.value, 1) == "01004 0 String data, right truncation");
	REQUIRE(open.api.Fetch(open.statement.value) == SQL_SUCCESS);
	CHECK(Value(open, 1) == "1");
	REQUIRE(open.api.Fetch(open.statement.value) == SQL_SUCCESS);
	CHECK(Value(open, 1) == "2");
}

// A warned fetch still advanced the cursor, which the ODBC client needs: it reports the warning and then reads
// the row it reached. No column is bound, so the read below proves the cursor moved.
TEST_CASE("A warning SQLFetch still moves onto its row", "[fake_driver]") {
	FakeOdbcDriver fake;
	OpenStatement open;
	fake.AddResult("SELECT a", {Rows({Varchar("a")}, {{"value"}})});
	PrepareAndExecute(open, "SELECT a");
	fake.WarnCall("SQLFetch", 1, {{"01004", 0, "String data, right truncated"}});

	CHECK(open.api.Fetch(open.statement.value) == SQL_SUCCESS_WITH_INFO);

	CHECK(StatementSqlstate(open) == "01004");
	CHECK(Value(open, 1) == "value");
}

// A warning adds to the records the call posted and comes first: the driver puts each new record at
// the head of the handle's list and counts from there (rsutil.c addError, getNextError).
TEST_CASE("A warning keeps the records the call posted and is read before them", "[fake_driver]") {
	FakeOdbcDriver fake;
	OpenStatement open;
	fake.AddResult("SELECT a", {Rows({Varchar("a")}, {{"abc"}})});
	PrepareAndExecute(open, "SELECT a");
	REQUIRE(open.api.Fetch(open.statement.value) == SQL_SUCCESS);
	fake.WarnCall("SQLGetData", 1, {{"01S07", 0, "Fractional truncation"}});
	char value[2] = {};
	SQLLEN length = 0;

	// The buffer cuts the cell, so SQLGetData posts 01004 of its own first.
	CHECK(open.api.GetData(open.statement.value, 1, SQL_C_CHAR, value, sizeof(value), &length) ==
	      SQL_SUCCESS_WITH_INFO);

	CHECK(DiagRecord(open.api, SQL_HANDLE_STMT, open.statement.value, 1) == "01S07 0 Fractional truncation");
	CHECK(DiagRecord(open.api, SQL_HANDLE_STMT, open.statement.value, 2) == "01004 0 String data, right truncated");
}

// A failed call did not warn, so its own answer and records stand.
TEST_CASE("A call scripted to fail and to warn keeps its failure", "[fake_driver]") {
	FakeOdbcDriver fake;
	OpenStatement open;
	PrepareDefaultResult(open);
	fake.FailCall("SQLExecute", 1, SQL_ERROR, {{"57014", 0, "canceling statement"}});
	fake.WarnCall("SQLExecute", 1, {{"01004", 0, "String data, right truncation"}});

	CHECK(open.api.Execute(open.statement.value) == SQL_ERROR);

	CHECK(DiagRecord(open.api, SQL_HANDLE_STMT, open.statement.value, 1) == "57014 0 canceling statement");
	CHECK(DiagRecordCount(open.api, SQL_HANDLE_STMT, open.statement.value) == 1);
}

// Only a call that succeeded warned, and a fetch past the last row did not succeed: ODBC's
// SQL_NO_DATA is not a success code, so the records a test scripted do not arrive and the fetch
// keeps its own answer.
TEST_CASE("A warned SQLFetch past the last row still reports no data and no records", "[fake_driver]") {
	FakeOdbcDriver fake;
	OpenStatement open;
	fake.AddResult("SELECT a", {Rows({Varchar("a")}, {{"1"}})});
	PrepareAndExecute(open, "SELECT a");
	REQUIRE(open.api.Fetch(open.statement.value) == SQL_SUCCESS);
	fake.WarnCall("SQLFetch", 2, {{"01004", 0, "String data, right truncated"}});

	CHECK(open.api.Fetch(open.statement.value) == SQL_NO_DATA);

	CHECK(DiagRecordCount(open.api, SQL_HANDLE_STMT, open.statement.value) == 0);
}

// The driver marks a column read only when the read did not report SQL_SUCCESS_WITH_INFO
// (rsresult.cpp RS_SQLGetData), and its text copy restarts after a whole copy (rsutil.c
// copyStrDataBigLen), so a warned read of a whole cell leaves it to be read again.
TEST_CASE("A warned SQLGetData that returned its whole cell returns it again on the next read", "[fake_driver]") {
	FakeOdbcDriver fake;
	OpenStatement open;
	FetchTextRow(fake, open, "SELECT a", {"abc"});
	fake.WarnCall("SQLGetData", 1, {{"01S07", 0, "Fractional truncation"}});
	char value[8] = {};
	SQLLEN length = 0;
	REQUIRE(open.api.GetData(open.statement.value, 1, SQL_C_CHAR, value, sizeof(value), &length) ==
	        SQL_SUCCESS_WITH_INFO);

	CHECK(Value(open, 1) == "abc");

	CHECK(open.api.GetData(open.statement.value, 1, SQL_C_CHAR, value, sizeof(value), &length) == SQL_NO_DATA);
}

// Only a read that returned the rest of its cell starts the cell again: a warned read that cut it
// still reported SQL_SUCCESS_WITH_INFO of its own, and the next read goes on from where it stopped.
TEST_CASE("A warned SQLGetData that cut its cell goes on from where it stopped", "[fake_driver]") {
	FakeOdbcDriver fake;
	OpenStatement open;
	FetchTextRow(fake, open, "SELECT a", {"abc"});
	fake.WarnCall("SQLGetData", 1, {{"01S07", 0, "Fractional truncation"}});
	char value[2] = {};
	SQLLEN length = 0;
	REQUIRE(open.api.GetData(open.statement.value, 1, SQL_C_CHAR, value, sizeof(value), &length) ==
	        SQL_SUCCESS_WITH_INFO);

	CHECK(Value(open, 1) == "bc");
}

// The scripted list reads as the records the driver would report, so the fake keeps its order.
TEST_CASE("A warning's records keep the order the test gave them", "[fake_driver]") {
	FakeOdbcDriver fake;
	OpenStatement open;
	FetchTextRow(fake, open, "SELECT a", {"abc"});
	fake.WarnCall("SQLGetData", 1, {{"01S07", 0, "first"}, {"01004", 0, "second"}});
	char value[8] = {};
	SQLLEN length = 0;

	REQUIRE(open.api.GetData(open.statement.value, 1, SQL_C_CHAR, value, sizeof(value), &length) ==
	        SQL_SUCCESS_WITH_INFO);

	CHECK(DiagRecord(open.api, SQL_HANDLE_STMT, open.statement.value, 1) == "01S07 0 first");
	CHECK(DiagRecord(open.api, SQL_HANDLE_STMT, open.statement.value, 2) == "01004 0 second");
}

TEST_CASE("WarnCall refuses a warning with no records", "[fake_driver]") {
#ifdef DUCKDB_CRASH_ON_ASSERT
	return; // InternalException aborts in this build
#endif
	FakeOdbcDriver fake;
	CHECK_THROWS_AS(fake.WarnCall("SQLFetch", 1, {}), duckdb::InternalException);
	CHECK_THROWS_WITH(fake.WarnCall("SQLFetch", 1, {}), Catch::Contains("WarnCall needs a record for SQLFetch"));
	CHECK_NOTHROW(fake.WarnCall("SQLFetch", 1, {{"01004", 0, "String data, right truncation"}}));
}

// A successful free erases the handle the records would sit on, and a diagnostic read leaves the
// records it reads alone, so either script would never fire; WarnCall's comment gives the source.
TEST_CASE("WarnCall refuses the free and the two diagnostic reads", "[fake_driver]") {
#ifdef DUCKDB_CRASH_ON_ASSERT
	return; // InternalException aborts in this build
#endif
	FakeOdbcDriver fake;
	const std::vector<FakeDiag> records {{"01004", 0, "String data, right truncation"}};
	CHECK_THROWS_AS(fake.WarnCall("SQLFreeHandle", 1, records), duckdb::InternalException);
	CHECK_THROWS_WITH(fake.WarnCall("SQLFreeHandle", 1, records),
	                  Catch::Contains("WarnCall cannot warn SQLFreeHandle"));
	CHECK_THROWS_WITH(fake.WarnCall("SQLGetDiagRec", 1, records),
	                  Catch::Contains("WarnCall cannot warn SQLGetDiagRec"));
	CHECK_THROWS_WITH(fake.WarnCall("SQLGetDiagField", 1, records),
	                  Catch::Contains("WarnCall cannot warn SQLGetDiagField"));
	CHECK_NOTHROW(fake.WarnCall("SQLFetch", 1, records));
}

// A misspelt name would leave the warning never firing and the test passing without it.
TEST_CASE("WarnCall refuses a name that is not a driver entry point", "[fake_driver]") {
#ifdef DUCKDB_CRASH_ON_ASSERT
	return; // InternalException aborts in this build
#endif
	FakeOdbcDriver fake;
	const std::vector<FakeDiag> records {{"01004", 0, "String data, right truncation"}};
	CHECK_THROWS_AS(fake.WarnCall("SQLCancel", 1, records), duckdb::InternalException);
	CHECK_THROWS_WITH(fake.WarnCall("SQLCancel", 1, records),
	                  Catch::Contains("SQLCancel is not in REDSHIFT_ODBC_ENTRY_POINTS"));
}

// A warning for a call already made would never fire either.
TEST_CASE("WarnCall refuses a call the entry point has already had", "[fake_driver]") {
#ifdef DUCKDB_CRASH_ON_ASSERT
	return; // InternalException aborts in this build
#endif
	FakeOdbcDriver fake;
	OpenHandles open; // three SQLAllocHandle calls
	const std::vector<FakeDiag> records {{"01004", 0, "String data, right truncation"}};
	CHECK_THROWS_AS(fake.WarnCall("SQLAllocHandle", 3, records), duckdb::InternalException);
	CHECK_THROWS_WITH(fake.WarnCall("SQLAllocHandle", 3, records), Catch::Contains("SQLAllocHandle has had 3 calls"));
	CHECK_NOTHROW(fake.WarnCall("SQLAllocHandle", 4, records));
}

// The scripted code replaces the read, so neither the target nor the indicator is written.
TEST_CASE("A scripted SQLGetData failure replaces the read", "[fake_driver]") {
	FakeOdbcDriver fake;
	OpenStatement open;
	FetchTextRow(fake, open, "SELECT a", {"abc"});
	fake.FailCall("SQLGetData", 1, SQL_ERROR, {{"08S01", 0, "communication link failure"}});
	char value[8] = {'z'};
	SQLLEN length = -7;

	CHECK(open.api.GetData(open.statement.value, 1, SQL_C_CHAR, value, sizeof(value), &length) == SQL_ERROR);

	CHECK(DiagRecord(open.api, SQL_HANDLE_STMT, open.statement.value, 1) == "08S01 0 communication link failure");
	CHECK(value[0] == 'z');
	CHECK(length == -7);
}

TEST_CASE("BlockCall refuses a call the entry point has already had", "[fake_driver]") {
#ifdef DUCKDB_CRASH_ON_ASSERT
	return; // InternalException aborts in this build
#endif
	FakeOdbcDriver fake;
	OpenHandles open; // three SQLAllocHandle calls
	CHECK_THROWS_AS(fake.BlockCall("SQLAllocHandle", 3, std::make_shared<FakeGate>()), duckdb::InternalException);
	CHECK_THROWS_WITH(fake.BlockCall("SQLAllocHandle", 3, std::make_shared<FakeGate>()),
	                  Catch::Contains("SQLAllocHandle has had 3 calls"));
	CHECK_NOTHROW(fake.BlockCall("SQLAllocHandle", 4, std::make_shared<FakeGate>()));
}

TEST_CASE("A held call lets other threads call the fake and completes once released", "[fake_driver]") {
	FakeOdbcDriver fake;
	OpenStatement open;
	PrepareDefaultResult(open);
	SQLHANDLE other = SQL_NULL_HANDLE;
	REQUIRE(open.api.AllocHandle(SQL_HANDLE_STMT, open.connection.value, &other) == SQL_SUCCESS);
	auto gate = std::make_shared<FakeGate>();
	fake.BlockCall("SQLExecute", 1, gate);
	std::atomic<bool> returned {false};
	SQLRETURN held_rc = SQL_ERROR;
	std::thread caller([&]() {
		held_rc = open.api.Execute(open.statement.value);
		returned = true;
	});
	const bool held = gate->WaitUntilHeldFor(std::chrono::seconds(10));
	auto freed = std::async(std::launch::async, [&]() { return open.api.FreeHandle(SQL_HANDLE_STMT, other); });
	// A held call that kept the fake's mutex would stall the free until the release below.
	const auto freed_status = freed.wait_for(std::chrono::seconds(10));
	const bool returned_while_held = returned;
	// Released and joined before the assertions, so a call a regression stops making ends the case
	// instead of leaving the binary waiting with no case name in its output.
	gate->Release();
	caller.join();

	CHECK(held);
	CHECK(freed_status == std::future_status::ready);
	CHECK_FALSE(returned_while_held);
	CHECK(freed.get() == SQL_SUCCESS);
	CHECK(held_rc == SQL_SUCCESS);
}

TEST_CASE("A held call runs only once released, so a handle freed meanwhile is invalid to it", "[fake_driver]") {
	FakeOdbcDriver fake;
	OpenStatement open;
	PrepareDefaultResult(open);
	auto gate = std::make_shared<FakeGate>();
	fake.BlockCall("SQLExecute", 1, gate);
	SQLRETURN held_rc = SQL_SUCCESS;
	std::thread caller([&]() { held_rc = open.api.Execute(open.statement.value); });
	const bool held = gate->WaitUntilHeldFor(std::chrono::seconds(10));
	const SQLRETURN freed_rc = open.api.FreeHandle(SQL_HANDLE_STMT, open.statement.value);
	gate->Release();
	caller.join();

	CHECK(held);
	CHECK(freed_rc == SQL_SUCCESS);
	CHECK(held_rc == SQL_INVALID_HANDLE);
}

TEST_CASE("A held call whose fake was destroyed and replaced returns SQL_INVALID_HANDLE", "[fake_driver]") {
	auto gate = std::make_shared<FakeGate>();
	OdbcApi api;
	std::thread caller;
	SQLRETURN held_rc = SQL_SUCCESS;
	SQLHANDLE environment = SQL_NULL_HANDLE;
	bool held = false;
	{
		FakeOdbcDriver fake;
		fake.BlockCall("SQLAllocHandle", 1, gate);
		// By value: the function table outlives the fake, the state behind it does not.
		api = Api();
		caller = std::thread([&]() { held_rc = api.AllocHandle(SQL_HANDLE_ENV, SQL_NULL_HANDLE, &environment); });
		held = gate->WaitUntilHeldFor(std::chrono::seconds(10));
	}
	// Whether this fake takes the destroyed one's stack slot is the compiler's choice; the generation
	// tells them apart either way.
	FakeOdbcDriver replacement;
	gate->Release();
	caller.join();

	CHECK(held);
	CHECK(held_rc == SQL_INVALID_HANDLE);
	CHECK(environment == SQL_NULL_HANDLE);
}

TEST_CASE("A call that reaches a gate already released runs at once", "[fake_driver]") {
	FakeOdbcDriver fake;
	OpenStatement open;
	PrepareDefaultResult(open);
	auto gate = std::make_shared<FakeGate>();
	fake.BlockCall("SQLExecute", 1, gate);
	gate->Release();
	auto executed = std::async(std::launch::async, [&]() { return open.api.Execute(open.statement.value); });

	const auto status = executed.wait_for(std::chrono::seconds(10));
	// Wakes a call that waited anyway, so a failure here does not hang the binary.
	gate->Release();
	CHECK(status == std::future_status::ready);
	CHECK(executed.get() == SQL_SUCCESS);
}

// Every other gate in these cases is on call 1, so a BlockCall that ignored nth_call would still
// pass them. Call 1 runs through a future with a bound because that mutation would hold it: made
// synchronously it would never return, and the case that catches the mutation would hang on it.
TEST_CASE("BlockCall holds the call its number names and lets the earlier one through", "[fake_driver]") {
	FakeOdbcDriver fake;
	OpenStatement open;
	PrepareDefaultResult(open);
	auto gate = std::make_shared<FakeGate>();
	fake.BlockCall("SQLExecute", 2, gate);
	auto first = std::async(std::launch::async, [&]() { return open.api.Execute(open.statement.value); });
	const auto first_status = first.wait_for(std::chrono::seconds(10));
	const bool held_first_call = gate->WaitUntilHeldFor(std::chrono::milliseconds(0));
	SQLRETURN second_rc = SQL_ERROR;
	std::thread caller([&]() { second_rc = open.api.Execute(open.statement.value); });
	const bool held_second_call = gate->WaitUntilHeldFor(std::chrono::seconds(10));
	gate->Release();
	caller.join();

	CHECK(first_status == std::future_status::ready);
	CHECK(first.get() == SQL_SUCCESS);
	CHECK_FALSE(held_first_call);
	CHECK(held_second_call);
	CHECK(second_rc == SQL_SUCCESS);
}

TEST_CASE("SQLPrepare fails for SQL text the test did not script, naming AddResult", "[fake_driver]") {
	FakeOdbcDriver fake;
	OpenStatement open;

	CHECK(open.api.Prepare(open.statement.value, Text("SELECT 2"), SQL_NTS) == SQL_ERROR);

	CHECK(DiagRecord(open.api, SQL_HANDLE_STMT, open.statement.value, 1) ==
	      "HY000 0 the fake driver has no result for \"SELECT 2\"; script one with AddResult");
}

TEST_CASE("SQLExecDirect runs its own text's script in place of the statement the handle prepared", "[fake_driver]") {
	FakeOdbcDriver fake;
	OpenStatement open;
	fake.AddResult("SELECT a", {Rows({Varchar("a")}, {{"prepared"}})});
	fake.AddResult("SELECT b", {Rows({Varchar("b")}, {{"direct"}})});
	REQUIRE(open.api.Prepare(open.statement.value, Text("SELECT a"), SQL_NTS) == SQL_SUCCESS);

	CHECK(open.api.ExecDirect(open.statement.value, Text("SELECT b"), SQL_NTS) == SQL_SUCCESS);

	CHECK(DetailsOf(fake, "SQLExecDirect") == std::vector<std::string> {"SELECT b"});
	REQUIRE(open.api.Fetch(open.statement.value) == SQL_SUCCESS);
	CHECK(Value(open, 1) == "direct");
}

TEST_CASE("SQLExecDirect fails for SQL text the test did not script, naming AddResult", "[fake_driver]") {
	FakeOdbcDriver fake;
	OpenStatement open;

	CHECK(open.api.ExecDirect(open.statement.value, Text("SELECT 2"), SQL_NTS) == SQL_ERROR);

	CHECK(DiagRecord(open.api, SQL_HANDLE_STMT, open.statement.value, 1) ==
	      "HY000 0 the fake driver has no result for \"SELECT 2\"; script one with AddResult");
}

// The direct run reports a statement that changed no row as SQLExecute does (rslibpq.c
// setResultInStmt), which the ODBC client reads as a statement that ran.
TEST_CASE("SQLExecDirect reports no data for a statement that affected no rows", "[fake_driver]") {
	FakeOdbcDriver fake;
	OpenStatement open;
	FakeResultSet affected_none;
	affected_none.row_count = 0;
	fake.AddResult("DELETE FROM t WHERE a = 0", {affected_none});

	CHECK(open.api.ExecDirect(open.statement.value, Text("DELETE FROM t WHERE a = 0"), SQL_NTS) == SQL_NO_DATA);
}

// A text with no script replaces what the handle held with nothing, so the statement prepared
// before it cannot be run by mistake: SQLExecute then finds no prepared statement.
TEST_CASE("SQLPrepare or SQLExecDirect of unscripted text leaves no earlier statement to run", "[fake_driver]") {
	const std::string entry_point = GENERATE("SQLPrepare", "SQLExecDirect");
	INFO(entry_point);
	FakeOdbcDriver fake;
	OpenStatement open;
	PrepareDefaultResult(open);

	const auto rc = entry_point == "SQLPrepare" ? open.api.Prepare(open.statement.value, Text("SELECT 2"), SQL_NTS)
	                                            : open.api.ExecDirect(open.statement.value, Text("SELECT 2"), SQL_NTS);
	REQUIRE(rc == SQL_ERROR);

	CHECK(open.api.Execute(open.statement.value) == SQL_ERROR);
	CHECK(StatementSqlstate(open) == "HY010");
}

// The driver releases the prepared statement before it sends the text (rsutil.c
// makeItReadyForNewQueryExecution), so a failed SQLExecDirect leaves nothing to run again.
TEST_CASE("A failed SQLExecDirect leaves no prepared statement on the handle", "[fake_driver]") {
	FakeOdbcDriver fake;
	OpenStatement open;
	PrepareDefaultResult(open);
	fake.FailCall("SQLExecDirect", 1, SQL_ERROR, {});

	REQUIRE(open.api.ExecDirect(open.statement.value, Text("SELECT 1"), SQL_NTS) == SQL_ERROR);

	CHECK(open.api.Execute(open.statement.value) == SQL_ERROR);
	CHECK(StatementSqlstate(open) == "HY010");
}

// The ODBC client prepares the same text twice, once to describe the result and once to run it.
TEST_CASE("One script answers every SQLPrepare of its text", "[fake_driver]") {
	FakeOdbcDriver fake;
	OpenStatement open;
	fake.AddResult("SELECT a", {Rows({Varchar("a")}, {{"1"}})});
	PrepareAndExecute(open, "SELECT a");

	REQUIRE(open.api.Prepare(open.statement.value, Text("SELECT a"), SQL_NTS) == SQL_SUCCESS);

	REQUIRE(open.api.Execute(open.statement.value) == SQL_SUCCESS);
	REQUIRE(open.api.Fetch(open.statement.value) == SQL_SUCCESS);
	CHECK(Value(open, 1) == "1");
}

TEST_CASE("A second AddResult for one SQL text replaces the first", "[fake_driver]") {
	FakeOdbcDriver fake;
	OpenStatement open;
	fake.AddResult("SELECT a", {Rows({Varchar("a")}, {{"first"}})});
	fake.AddResult("SELECT a", {Rows({Varchar("a")}, {{"second"}})});
	PrepareAndExecute(open, "SELECT a");
	REQUIRE(open.api.Fetch(open.statement.value) == SQL_SUCCESS);

	CHECK(Value(open, 1) == "second");
}

TEST_CASE("AddResult refuses a script with no result set at all", "[fake_driver]") {
#ifdef DUCKDB_CRASH_ON_ASSERT
	return; // InternalException aborts in this build
#endif
	FakeOdbcDriver fake;

	CHECK_THROWS_AS(fake.AddResult("COMMIT", {}), duckdb::InternalException);
	CHECK_THROWS_WITH(fake.AddResult("COMMIT", {}), Catch::Contains("has no result set"));
}

TEST_CASE("AddResult refuses a result set whose rows, nulls or row status do not fit it", "[fake_driver]") {
#ifdef DUCKDB_CRASH_ON_ASSERT
	return; // InternalException aborts in this build
#endif
	FakeOdbcDriver fake;
	auto missing_cell = Rows({Varchar("a"), Varchar("b")}, {{"1"}});
	auto missing_null = Rows({Varchar("a")}, {{"1"}, {"2"}});
	missing_null.nulls = {{false}};
	auto extra_null = Rows({Varchar("a")}, {{"1"}});
	extra_null.nulls = {{false}, {false}};
	auto narrow_null = Rows({Varchar("a"), Varchar("b")}, {{"1", "2"}});
	narrow_null.nulls = {{false}};
	auto missing_status = Rows({Varchar("a")}, {{"1"}, {"2"}});
	missing_status.row_status = {SQL_ROW_SUCCESS};

	CHECK_THROWS_AS(fake.AddResult("SELECT a, b", {missing_cell}), duckdb::InternalException);
	CHECK_THROWS_WITH(fake.AddResult("SELECT a, b", {missing_cell}), Catch::Contains("one cell per column"));
	CHECK_THROWS_WITH(fake.AddResult("SELECT a", {missing_null}), Catch::Contains("one cell per column"));
	CHECK_THROWS_WITH(fake.AddResult("SELECT a", {extra_null}), Catch::Contains("one cell per column"));
	CHECK_THROWS_WITH(fake.AddResult("SELECT a, b", {narrow_null}), Catch::Contains("one cell per column"));
	CHECK_THROWS_WITH(fake.AddResult("SELECT a", {missing_status}), Catch::Contains("one cell per column"));
}

// A scripted row succeeds or fails, with the diagnostics to post. SQL_ROW_SUCCESS_WITH_INFO comes
// only from a cut bound cell, so a script that names it is refused even with the diagnostics a fetch
// would post.
TEST_CASE("AddResult refuses a row status the fake cannot answer", "[fake_driver]") {
#ifdef DUCKDB_CRASH_ON_ASSERT
	return; // InternalException aborts in this build
#endif
	FakeOdbcDriver fake;
	auto with_info = Rows({Varchar("a")}, {{"1"}});
	with_info.row_status = {SQL_ROW_SUCCESS_WITH_INFO};
	with_info.row_diags[0] = {{"01S07", 0, "Fractional truncation"}};
	auto silent_error = Rows({Varchar("a")}, {{"1"}});
	silent_error.row_status = {SQL_ROW_ERROR};

	CHECK_THROWS_WITH(fake.AddResult("SELECT a", {with_info}),
	                  Catch::Contains("SQL_ROW_SUCCESS or SQL_ROW_ERROR only"));
	CHECK_THROWS_WITH(fake.AddResult("SELECT a", {silent_error}), Catch::Contains("its diagnostics in row_diags"));
}

// A fetch posts a row's diagnostics only when that row is in error, so an entry for any other row,
// or for one past the last, would be silently ignored.
TEST_CASE("AddResult refuses row diagnostics for a row not scripted SQL_ROW_ERROR", "[fake_driver]") {
#ifdef DUCKDB_CRASH_ON_ASSERT
	return; // InternalException aborts in this build
#endif
	FakeOdbcDriver fake;
	const std::vector<FakeDiag> diags {{"22003", 0, "Numeric value out of range"}};
	auto succeeded = Rows({Varchar("a")}, {{"1"}});
	succeeded.row_status = {SQL_ROW_SUCCESS};
	succeeded.row_diags[0] = diags;
	auto no_status = Rows({Varchar("a")}, {{"1"}});
	no_status.row_diags[0] = diags;
	auto past_last = Rows({Varchar("a")}, {{"1"}});
	past_last.row_status = {SQL_ROW_ERROR};
	past_last.row_diags[0] = diags;
	past_last.row_diags[1] = diags;
	auto in_error = Rows({Varchar("a")}, {{"1"}});
	in_error.row_status = {SQL_ROW_ERROR};
	in_error.row_diags[0] = diags;

	CHECK_THROWS_AS(fake.AddResult("SELECT a", {succeeded}), duckdb::InternalException);
	CHECK_THROWS_WITH(fake.AddResult("SELECT a", {succeeded}), Catch::Contains("for a row not scripted SQL_ROW_ERROR"));
	CHECK_THROWS_WITH(fake.AddResult("SELECT a", {no_status}), Catch::Contains("for a row not scripted SQL_ROW_ERROR"));
	CHECK_THROWS_WITH(fake.AddResult("SELECT a", {past_last}), Catch::Contains("for a row not scripted SQL_ROW_ERROR"));
	CHECK_NOTHROW(fake.AddResult("SELECT a", {in_error}));
}

// A statement that returns no result set has no rows for SQLFetch to step to.
TEST_CASE("AddResult refuses rows in a set with no columns", "[fake_driver]") {
#ifdef DUCKDB_CRASH_ON_ASSERT
	return; // InternalException aborts in this build
#endif
	FakeOdbcDriver fake;
	FakeResultSet set_rows;
	set_rows.rows = {{}, {}};
	set_rows.row_count = 0;
	FakeResultSet set_nothing;
	set_nothing.row_count = 0;

	CHECK_THROWS_AS(fake.AddResult("SET a TO 1", {set_rows}), duckdb::InternalException);
	CHECK_THROWS_WITH(fake.AddResult("SET a TO 1", {set_rows}), Catch::Contains("has rows but no columns"));
	CHECK_NOTHROW(fake.AddResult("SET a TO 1", {set_nothing}));
}

// The server reports a row count for every command whose tag carries one, and the count is 0 for a
// SET or a CREATE, whose tag carries none, so -1 is a count no executed statement without columns
// ever reports (public source v2.3.0, rslibpq.c, and pgclient fe-exec.c, PQcmdTuples).
TEST_CASE("AddResult refuses a set with no columns and no row count", "[fake_driver]") {
#ifdef DUCKDB_CRASH_ON_ASSERT
	return; // InternalException aborts in this build
#endif
	FakeOdbcDriver fake;

	CHECK_THROWS_WITH(fake.AddResult("SET a = 1", {FakeResultSet()}), Catch::Contains("script row_count, not -1"));
}

TEST_CASE("AddResult takes an empty query's set with no columns and no row count", "[fake_driver]") {
	FakeOdbcDriver fake;

	CHECK_NOTHROW(fake.AddResult(" ;", {FakeResultSet()}));
}

// The driver answers SQLNumResultCols with 0 for a statement with no result set rather than refusing
// the call (public source v2.3.0, rsresult.cpp).
TEST_CASE("SQLNumResultCols reports no columns for a statement not yet prepared", "[fake_driver]") {
	FakeOdbcDriver fake;
	OpenStatement open;
	SQLSMALLINT columns = -1;

	CHECK(open.api.NumResultCols(open.statement.value, &columns) == SQL_SUCCESS);

	CHECK(columns == 0);
	CHECK(DiagRecordCount(open.api, SQL_HANDLE_STMT, open.statement.value) == 0);
}

TEST_CASE("SQLNumResultCols fails a null count pointer", "[fake_driver]") {
	FakeOdbcDriver fake;
	OpenStatement open;

	CHECK(open.api.NumResultCols(open.statement.value, nullptr) == SQL_ERROR);

	CHECK(DiagRecord(open.api, SQL_HANDLE_STMT, open.statement.value, 1) == "HY009 0 Invalid use of null pointer");
}

// The driver posts HY000 "No result found" from both calls whenever it has no result set, whether
// the statement was never prepared, has been closed or has had its results drained (public source
// v2.3.0, rsresult.cpp).
TEST_CASE("Describing a column with no result set fails with HY000, whichever call asks", "[fake_driver]") {
	FakeOdbcDriver fake;
	OpenStatement open;
	char type_name[16] = {};

	CHECK(open.api.DescribeCol(open.statement.value, 1, nullptr, 0, nullptr, nullptr, nullptr, nullptr, nullptr) ==
	      SQL_ERROR);
	CHECK(DiagRecord(open.api, SQL_HANDLE_STMT, open.statement.value, 1) == "HY000 0 No result found");
	CHECK(open.api.ColAttribute(open.statement.value, 1, SQL_DESC_TYPE_NAME, type_name, sizeof(type_name), nullptr,
	                            nullptr) == SQL_ERROR);
	CHECK(DiagRecord(open.api, SQL_HANDLE_STMT, open.statement.value, 1) == "HY000 0 No result found");
}

// A closed statement holds no result either, and the driver answers it the same way (public source
// v2.3.0, rsresult.cpp), so the columns a closed statement once had are no longer describable.
TEST_CASE("Describing a column after SQLCloseCursor fails with HY000", "[fake_driver]") {
	FakeOdbcDriver fake;
	OpenStatement open;
	PrepareAndExecute(open, "SELECT 1");
	REQUIRE(open.api.CloseCursor(open.statement.value) == SQL_SUCCESS);

	CHECK(open.api.DescribeCol(open.statement.value, 1, nullptr, 0, nullptr, nullptr, nullptr, nullptr, nullptr) ==
	      SQL_ERROR);

	CHECK(DiagRecord(open.api, SQL_HANDLE_STMT, open.statement.value, 1) == "HY000 0 No result found");
}

// SQLPrepare keeps a result to describe only when the server sent a row description, which an INSERT
// has none of, so describing its columns before it runs reaches the no-result answer rather than
// 07005 (public source v2.3.0, rslibpq.c, libpqPrepareOnThread, and rsresult.cpp).
TEST_CASE("Describing a prepared statement that returns no rows fails with HY000", "[fake_driver]") {
	FakeOdbcDriver fake;
	OpenStatement open;
	FakeResultSet inserted;
	inserted.row_count = 1;
	fake.AddResult("INSERT INTO t VALUES (1)", {inserted});
	REQUIRE(open.api.Prepare(open.statement.value, Text("INSERT INTO t VALUES (1)"), SQL_NTS) == SQL_SUCCESS);

	CHECK(open.api.DescribeCol(open.statement.value, 1, nullptr, 0, nullptr, nullptr, nullptr, nullptr, nullptr) ==
	      SQL_ERROR);

	CHECK(DiagRecord(open.api, SQL_HANDLE_STMT, open.statement.value, 1) == "HY000 0 No result found");
}

// The driver posts 07005 for an executed result set that has no columns to describe (public source
// v2.3.0, rsresult.cpp), which an INSERT scripted with one column-less set reaches once it has run.
TEST_CASE("Describing a column of an executed result set with no columns fails with 07005", "[fake_driver]") {
	FakeOdbcDriver fake;
	OpenStatement open;
	FakeResultSet inserted;
	inserted.row_count = 1;
	fake.AddResult("INSERT INTO t VALUES (1)", {inserted});
	PrepareAndExecute(open, "INSERT INTO t VALUES (1)");

	CHECK(open.api.DescribeCol(open.statement.value, 1, nullptr, 0, nullptr, nullptr, nullptr, nullptr, nullptr) ==
	      SQL_ERROR);

	CHECK(DiagRecord(open.api, SQL_HANDLE_STMT, open.statement.value, 1) == "07005 0 No columns found");
}

// The driver answers a column number outside the result set with HY000 and its own message, which
// column 0 reaches, bookmarks being unsupported (public source v2.3.0, rsresult.cpp).
TEST_CASE("SQLDescribeCol of column 0 fails as an invalid column number", "[fake_driver]") {
	FakeOdbcDriver fake;
	OpenStatement open;
	REQUIRE(open.api.Prepare(open.statement.value, Text("SELECT 1"), SQL_NTS) == SQL_SUCCESS);

	CHECK(open.api.DescribeCol(open.statement.value, 0, nullptr, 0, nullptr, nullptr, nullptr, nullptr, nullptr) ==
	      SQL_ERROR);

	CHECK(DiagRecord(open.api, SQL_HANDLE_STMT, open.statement.value, 1) ==
	      "HY000 0 SQLDescribeCol: Invalid column number");
}

// The driver answers the count only between SQLPrepare and SQLExecute (public source v2.3.0,
// rsparameter.cpp).
TEST_CASE("SQLNumParams before SQLPrepare fails with HY010", "[fake_driver]") {
	FakeOdbcDriver fake;
	OpenStatement open;
	SQLSMALLINT count = -1;

	CHECK(open.api.NumParams(open.statement.value, &count) == SQL_ERROR);

	CHECK(StatementSqlstate(open) == "HY010");
	CHECK(count == 0);
}

// Measured on driver 2.3.0.0: SQLNumParams reported 2 for "SELECT $2::varchar || $1::varchar" and 2
// for the same statement written with two question marks, so a count the fake took from the SQL text
// would be a guess either way.
TEST_CASE("SQLNumParams reports the count AddResult was given, not one counted from the SQL", "[fake_driver]") {
	FakeOdbcDriver fake;
	OpenStatement open;
	const char *const sql = "SELECT $1::varchar || $1::varchar";
	fake.AddResult(sql, {Rows({Varchar("a")}, {})}, 1);
	REQUIRE(open.api.Prepare(open.statement.value, Text(sql), SQL_NTS) == SQL_SUCCESS);
	SQLSMALLINT count = -1;

	REQUIRE(open.api.NumParams(open.statement.value, &count) == SQL_SUCCESS);

	CHECK(count == 1);
}

// The driver answers the count only between SQLPrepare and SQLExecute (public source v2.3.0,
// rsparameter.cpp), and the ODBC client reads it there, before it binds.
TEST_CASE("SQLNumParams fails once the statement has been executed", "[fake_driver]") {
	FakeOdbcDriver fake;
	OpenStatement open;
	PrepareAndExecute(open, "SELECT 1");
	SQLSMALLINT count = -1;

	CHECK(open.api.NumParams(open.statement.value, &count) == SQL_ERROR);

	CHECK(StatementSqlstate(open) == "HY010");
	CHECK(count == 0);
}

TEST_CASE("SQLNumParams fails a null count pointer", "[fake_driver]") {
	FakeOdbcDriver fake;
	OpenStatement open;
	REQUIRE(open.api.Prepare(open.statement.value, Text("SELECT 1"), SQL_NTS) == SQL_SUCCESS);

	CHECK(open.api.NumParams(open.statement.value, nullptr) == SQL_ERROR);

	CHECK(StatementSqlstate(open) == "HY009");
}

TEST_CASE("SQLNumResultCols and SQLDescribeCol report each scripted column", "[fake_driver]") {
	FakeOdbcDriver fake;
	OpenStatement open;
	auto price = Column("price", "NUMERIC", SQL_NUMERIC, 38, 9);
	price.nullable = SQL_NO_NULLS;
	fake.AddResult("SELECT id, price", {Rows({Varchar("id"), price}, {})});
	REQUIRE(open.api.Prepare(open.statement.value, Text("SELECT id, price"), SQL_NTS) == SQL_SUCCESS);
	SQLSMALLINT columns = 0;
	REQUIRE(open.api.NumResultCols(open.statement.value, &columns) == SQL_SUCCESS);
	CHECK(columns == 2);
	SQLCHAR name[16] = {};
	SQLSMALLINT name_length = 0;
	SQLSMALLINT type = 0;
	SQLULEN size = 0;
	SQLSMALLINT digits = -1;
	SQLSMALLINT nullable = -1;

	REQUIRE(open.api.DescribeCol(open.statement.value, 2, name, sizeof(name), &name_length, &type, &size, &digits,
	                             &nullable) == SQL_SUCCESS);

	CHECK(std::string(reinterpret_cast<char *>(name)) == "price");
	CHECK(name_length == 5);
	CHECK(type == SQL_NUMERIC);
	CHECK(size == 38);
	CHECK(digits == 9);
	CHECK(nullable == SQL_NO_NULLS);
}

// The driver's public source (v2.3.0, rsresult.cpp) posts a different record per call for a column
// the result does not have.
TEST_CASE("A column the result set does not have fails each call with its own SQLSTATE", "[fake_driver]") {
	FakeOdbcDriver fake;
	OpenStatement open;
	fake.AddResult("SELECT a", {Rows({Varchar("a")}, {{"1"}})});
	PrepareAndExecute(open, "SELECT a");
	REQUIRE(open.api.Fetch(open.statement.value) == SQL_SUCCESS);
	char value[8] = {};
	SQLLEN length = 0;

	CHECK(open.api.DescribeCol(open.statement.value, 2, nullptr, 0, nullptr, nullptr, nullptr, nullptr, nullptr) ==
	      SQL_ERROR);
	CHECK(DiagRecord(open.api, SQL_HANDLE_STMT, open.statement.value, 1) ==
	      "HY000 0 SQLDescribeCol: Invalid column number");
	CHECK(open.api.ColAttribute(open.statement.value, 2, SQL_DESC_TYPE_NAME, value, sizeof(value), nullptr, nullptr) ==
	      SQL_ERROR);
	CHECK(DiagRecord(open.api, SQL_HANDLE_STMT, open.statement.value, 1) ==
	      "07009 0 SQLColAttribute: Invalid column number");
	CHECK(open.api.GetData(open.statement.value, 2, SQL_C_CHAR, value, sizeof(value), &length) == SQL_ERROR);
	CHECK(DiagRecord(open.api, SQL_HANDLE_STMT, open.statement.value, 1) == "07009 0 Invalid descriptor index");
}

// Measured on driver 2.3.0.0: a numeric(38,9) column reports the type name "NUMERIC", which the ODBC client
// classifies by, SQLDescribeCol reporting no name of its own.
TEST_CASE("SQLColAttribute reports a column's type name", "[fake_driver]") {
	FakeOdbcDriver fake;
	OpenStatement open;
	fake.AddResult("SELECT price", {Rows({Column("price", "NUMERIC", SQL_NUMERIC, 38, 9)}, {})});
	REQUIRE(open.api.Prepare(open.statement.value, Text("SELECT price"), SQL_NTS) == SQL_SUCCESS);
	char type_name[16] = {};
	SQLSMALLINT length = 0;

	REQUIRE(open.api.ColAttribute(open.statement.value, 1, SQL_DESC_TYPE_NAME, type_name, sizeof(type_name), &length,
	                              nullptr) == SQL_SUCCESS);

	CHECK(std::string(type_name) == "NUMERIC");
	CHECK(length == 7);
}

TEST_CASE("A cut SQLColAttribute reports the full type-name length and posts 01004", "[fake_driver]") {
	FakeOdbcDriver fake;
	OpenStatement open;
	fake.AddResult("SELECT price", {Rows({Column("price", "NUMERIC", SQL_NUMERIC, 38, 9)}, {})});
	REQUIRE(open.api.Prepare(open.statement.value, Text("SELECT price"), SQL_NTS) == SQL_SUCCESS);
	char type_name[4] = {};
	SQLSMALLINT length = 0;

	CHECK(open.api.ColAttribute(open.statement.value, 1, SQL_DESC_TYPE_NAME, type_name, sizeof(type_name), &length,
	                            nullptr) == SQL_SUCCESS_WITH_INFO);

	CHECK(std::string(type_name) == "NUM");
	CHECK(length == 7);
	CHECK(DiagRecord(open.api, SQL_HANDLE_STMT, open.statement.value, 1) == "01004 0 String data, right truncated");
}

// The driver's copyStrDataSmallLen, which copies both the column name and SQL_DESC_TYPE_NAME, posts
// 01004 for a cut copy but answers SQL_SUCCESS when there is no length pointer (rsutil.c).
TEST_CASE("A cut name or type name with no length pointer succeeds with 01004 posted", "[fake_driver]") {
	FakeOdbcDriver fake;
	OpenStatement open;
	fake.AddResult("SELECT price", {Rows({Column("price", "NUMERIC", SQL_NUMERIC, 38, 9)}, {})});
	REQUIRE(open.api.Prepare(open.statement.value, Text("SELECT price"), SQL_NTS) == SQL_SUCCESS);
	SQLCHAR name[4] = {};
	char type_name[4] = {};

	CHECK(open.api.DescribeCol(open.statement.value, 1, name, sizeof(name), nullptr, nullptr, nullptr, nullptr,
	                           nullptr) == SQL_SUCCESS);
	CHECK(DiagRecord(open.api, SQL_HANDLE_STMT, open.statement.value, 1) == "01004 0 String data, right truncated");
	CHECK(open.api.ColAttribute(open.statement.value, 1, SQL_DESC_TYPE_NAME, type_name, sizeof(type_name), nullptr,
	                            nullptr) == SQL_SUCCESS);

	CHECK(std::string(type_name) == "NUM");
	CHECK(DiagRecord(open.api, SQL_HANDLE_STMT, open.statement.value, 1) == "01004 0 String data, right truncated");
}

TEST_CASE("SQLColAttribute reports the numeric fields that classify a column and size its type", "[fake_driver]") {
	FakeOdbcDriver fake;
	OpenStatement open;
	auto price = Column("price", "NUMERIC", SQL_NUMERIC, 38, 9);
	price.nullable = SQL_NO_NULLS;
	fake.AddResult("SELECT price", {Rows({price}, {})});
	REQUIRE(open.api.Prepare(open.statement.value, Text("SELECT price"), SQL_NTS) == SQL_SUCCESS);
	SQLLEN concise = 0;
	SQLLEN precision = 0;
	SQLLEN scale = -1;
	SQLLEN column_length = 0;
	SQLLEN nullable = -1;

	REQUIRE(open.api.ColAttribute(open.statement.value, 1, SQL_DESC_CONCISE_TYPE, nullptr, 0, nullptr, &concise) ==
	        SQL_SUCCESS);
	REQUIRE(open.api.ColAttribute(open.statement.value, 1, SQL_DESC_PRECISION, nullptr, 0, nullptr, &precision) ==
	        SQL_SUCCESS);
	REQUIRE(open.api.ColAttribute(open.statement.value, 1, SQL_DESC_SCALE, nullptr, 0, nullptr, &scale) == SQL_SUCCESS);
	REQUIRE(open.api.ColAttribute(open.statement.value, 1, SQL_DESC_LENGTH, nullptr, 0, nullptr, &column_length) ==
	        SQL_SUCCESS);
	REQUIRE(open.api.ColAttribute(open.statement.value, 1, SQL_DESC_NULLABLE, nullptr, 0, nullptr, &nullable) ==
	        SQL_SUCCESS);

	CHECK(concise == SQL_NUMERIC);
	CHECK(precision == 38);
	CHECK(scale == 9);
	CHECK(column_length == 38);
	CHECK(nullable == SQL_NO_NULLS);
}

TEST_CASE("SQL_DESC_PRECISION reports the precision the driver derives, not the column size", "[fake_driver]") {
	FakeOdbcDriver fake;
	OpenStatement open;
	PrepareEveryPrecisionArm(fake, open);
	const auto arms = EveryPrecisionArm();

	for (SQLUSMALLINT number = 1; number <= arms.size(); number++) {
		INFO("column " << arms[number - 1].column.name);
		CHECK(NumericAttribute(open, number, SQL_DESC_PRECISION) == arms[number - 1].precision);
	}
}

// The driver reports SQL_DESC_LENGTH as the size SQLDescribeCol gives (rsresult.cpp
// RS_SQLColAttribute), so a column whose precision differs from its size still reports the size.
TEST_CASE("SQL_DESC_LENGTH reports the column size for every type, not the precision", "[fake_driver]") {
	FakeOdbcDriver fake;
	OpenStatement open;
	PrepareEveryPrecisionArm(fake, open);
	const auto arms = EveryPrecisionArm();

	for (SQLUSMALLINT number = 1; number <= arms.size(); number++) {
		INFO("column " << arms[number - 1].column.name);
		CHECK(NumericAttribute(open, number, SQL_DESC_LENGTH) ==
		      static_cast<SQLLEN>(arms[number - 1].column.column_size));
	}
}

// The driver answers HY091 for a field it does not know (public source v2.3.0, rsresult.cpp), and
// the fake answers it for one it does not model, so a test that needs that field says so.
TEST_CASE("SQLColAttribute fails a field the fake does not model", "[fake_driver]") {
	FakeOdbcDriver fake;
	OpenStatement open;
	REQUIRE(open.api.Prepare(open.statement.value, Text("SELECT 1"), SQL_NTS) == SQL_SUCCESS);
	SQLLEN value = 0;

	CHECK(open.api.ColAttribute(open.statement.value, 1, SQL_DESC_SEARCHABLE, nullptr, 0, nullptr, &value) ==
	      SQL_ERROR);

	CHECK(DiagRecord(open.api, SQL_HANDLE_STMT, open.statement.value, 1) ==
	      "HY091 0 Invalid descriptor field identifier");
	CHECK(value == 0);
}

TEST_CASE("SQLExecute logs each bound value with its number", "[fake_driver]") {
	FakeOdbcDriver fake;
	OpenStatement open;
	const char *const sql = "SELECT a FROM t WHERE a = ? AND b = ?";
	fake.AddResult(sql, {Rows({Varchar("a")}, {})}, 2);
	REQUIRE(open.api.Prepare(open.statement.value, Text(sql), SQL_NTS) == SQL_SUCCESS);
	const std::string first = "it's; DROP TABLE t; --";
	auto first_buffer = first + "past its length";
	auto first_length = static_cast<SQLLEN>(first.size());
	char second[] = "$1 ?";
	REQUIRE(BindText(open, 1, &first_buffer[0], &first_length) == SQL_SUCCESS);
	REQUIRE(BindNulTerminated(open, 2, second) == SQL_SUCCESS);

	REQUIRE(open.api.Execute(open.statement.value) == SQL_SUCCESS);

	CHECK(DetailsOf(fake, "SQLExecute") == std::vector<std::string> {"1=" + first + "\n2=$1 ?"});
}

TEST_CASE("SQLPrepare logs the SQL text unchanged", "[fake_driver]") {
	FakeOdbcDriver fake;
	OpenStatement open;
	const char *const sql = "SELECT a FROM t WHERE a = ? AND b = ?";
	fake.AddResult(sql, {Rows({Varchar("a")}, {})}, 2);

	REQUIRE(open.api.Prepare(open.statement.value, Text(sql), SQL_NTS) == SQL_SUCCESS);

	CHECK(DetailsOf(fake, "SQLPrepare") == std::vector<std::string> {sql});
}

// The ODBC client must read a VARBYTE column as SQL_C_BINARY, because the driver answers hex for SQL_C_CHAR
// (rsutil.c convertSQLDataToCData) while the fake answers with the fixture's own bytes, so only the
// logged C type tells the right read from the wrong one.
TEST_CASE("SQLGetData logs the column it read and the C type it read it as", "[fake_driver]") {
	FakeOdbcDriver fake;
	OpenStatement open;
	fake.AddResult("SELECT b, a",
	               {Rows({Column("b", "VARBYTE", SQL_LONGVARBINARY, 64000, 0), Varchar("a")}, {{"\xff", "x"}})});
	PrepareAndExecute(open, "SELECT b, a");
	REQUIRE(open.api.Fetch(open.statement.value) == SQL_SUCCESS);
	char bytes[8] = {};
	SQLLEN length = 0;
	REQUIRE(open.api.GetData(open.statement.value, 1, SQL_C_BINARY, bytes, sizeof(bytes), &length) == SQL_SUCCESS);
	REQUIRE(Value(open, 2) == "x");

	CHECK(DetailsOf(fake, "SQLGetData") == std::vector<std::string> {"1 -2", "2 1"});
}

TEST_CASE("SQLExecute sends what the bound buffers hold when it runs, not when they were bound", "[fake_driver]") {
	FakeOdbcDriver fake;
	OpenStatement open;
	fake.AddResult("SELECT ?", {Rows({Varchar("a")}, {})}, 1);
	REQUIRE(open.api.Prepare(open.statement.value, Text("SELECT ?"), SQL_NTS) == SQL_SUCCESS);
	char value[] = "old";
	SQLLEN length = 0;
	REQUIRE(BindText(open, 1, value, &length) == SQL_SUCCESS);
	memcpy(value, "new", 3);
	length = 3;

	REQUIRE(open.api.Execute(open.statement.value) == SQL_SUCCESS);

	CHECK(DetailsOf(fake, "SQLExecute") == std::vector<std::string> {"1=new"});
}

// ODBC binds a parameter to the statement handle, not to the prepared text, so the ODBC client's second
// prepare of one statement keeps what it bound before. Once the binding is dropped the statement has
// fewer parameters than markers, which the driver refuses.
TEST_CASE("A bound parameter lasts across SQLPrepare until SQLFreeStmt(SQL_RESET_PARAMS)", "[fake_driver]") {
	FakeOdbcDriver fake;
	OpenStatement open;
	FakeResultSet set_nothing;
	set_nothing.row_count = 0;
	fake.AddResult("SET a = ?", {set_nothing}, 1);
	char value[] = "x";
	REQUIRE(BindNulTerminated(open, 1, value) == SQL_SUCCESS);
	PrepareAndExecute(open, "SET a = ?");
	PrepareAndExecute(open, "SET a = ?");
	REQUIRE(open.api.FreeStmt(open.statement.value, SQL_RESET_PARAMS) == SQL_SUCCESS);

	CHECK(open.api.Execute(open.statement.value) == SQL_ERROR);

	CHECK(DetailsOf(fake, "SQLExecute") == std::vector<std::string> {"1=x", "1=x", ""});
}

// The driver refuses a statement with more parameter markers than bound parameters (public source
// v2.3.0, rsexecute.cpp), which is why the ODBC client reads SQLNumParams before it binds.
TEST_CASE("SQLExecute refuses a statement with fewer bound parameters than markers", "[fake_driver]") {
	FakeOdbcDriver fake;
	OpenStatement open;
	fake.AddResult("SELECT ?, ?", {Rows({Varchar("a")}, {})}, 2);
	REQUIRE(open.api.Prepare(open.statement.value, Text("SELECT ?, ?"), SQL_NTS) == SQL_SUCCESS);
	char value[] = "x";
	REQUIRE(BindNulTerminated(open, 1, value) == SQL_SUCCESS);

	CHECK(open.api.Execute(open.statement.value) == SQL_ERROR);

	CHECK(DiagRecord(open.api, SQL_HANDLE_STMT, open.statement.value, 1) ==
	      "HY000 0 Number of parameters in query is more than number of bind parameters.");
}

// The driver looks each marker's parameter up by position and fails when one is missing (rslibpq.c
// libpqExecuteDirectOrPreparedOnThread), so a bind loop that skipped a position fails here as it
// would live, rather than running on whichever bindings it holds.
TEST_CASE("SQLExecute refuses a marker position with nothing bound to it", "[fake_driver]") {
	FakeOdbcDriver fake;
	OpenStatement open;
	fake.AddResult("SELECT ?, ?", {Rows({Varchar("a")}, {})}, 2);
	REQUIRE(open.api.Prepare(open.statement.value, Text("SELECT ?, ?"), SQL_NTS) == SQL_SUCCESS);
	char first[] = "x";
	char third[] = "z";
	REQUIRE(BindNulTerminated(open, 1, first) == SQL_SUCCESS);
	REQUIRE(BindNulTerminated(open, 3, third) == SQL_SUCCESS);

	CHECK(open.api.Execute(open.statement.value) == SQL_ERROR);

	CHECK(DiagRecord(open.api, SQL_HANDLE_STMT, open.statement.value, 1) == "HY000 0 Bind parameter not found.");
}

// The driver's SQLBindParameter accepts parameter number 0, refusing only a number negative as a
// short (rsparameter.cpp RS_SQLBindParameter), so a bind loop that counted from 0 binds without
// complaint and fails only at execution.
TEST_CASE("SQLExecute refuses parameters bound from 0 instead of from 1", "[fake_driver]") {
	FakeOdbcDriver fake;
	OpenStatement open;
	fake.AddResult("SELECT ?, ?", {Rows({Varchar("a")}, {})}, 2);
	REQUIRE(open.api.Prepare(open.statement.value, Text("SELECT ?, ?"), SQL_NTS) == SQL_SUCCESS);
	char first[] = "x";
	char second[] = "y";
	REQUIRE(BindNulTerminated(open, 0, first) == SQL_SUCCESS);
	REQUIRE(BindNulTerminated(open, 1, second) == SQL_SUCCESS);

	CHECK(open.api.Execute(open.statement.value) == SQL_ERROR);

	CHECK(DiagRecord(open.api, SQL_HANDLE_STMT, open.statement.value, 1) == "HY000 0 Bind parameter not found.");
}

// The driver sends the first param_count bindings and ignores the rest (rslibpq.c
// libpqExecuteDirectOrPreparedOnThread), so the log holds what it would have sent and no more.
TEST_CASE("SQLExecute logs only the parameters the query has markers for", "[fake_driver]") {
	FakeOdbcDriver fake;
	OpenStatement open;
	fake.AddResult("SELECT ?, ?", {Rows({Varchar("a")}, {})}, 2);
	REQUIRE(open.api.Prepare(open.statement.value, Text("SELECT ?, ?"), SQL_NTS) == SQL_SUCCESS);
	char first[] = "x";
	char second[] = "y";
	char third[] = "z";
	REQUIRE(BindNulTerminated(open, 1, first) == SQL_SUCCESS);
	REQUIRE(BindNulTerminated(open, 2, second) == SQL_SUCCESS);
	REQUIRE(BindNulTerminated(open, 3, third) == SQL_SUCCESS);

	REQUIRE(open.api.Execute(open.statement.value) == SQL_SUCCESS);

	CHECK(DetailsOf(fake, "SQLExecute") == std::vector<std::string> {"1=x\n2=y"});
}

// A binding the query has no marker for is never read, so one the fake could not send does not
// refuse the statement either.
TEST_CASE("SQLExecute ignores a NULL parameter bound above the marker count", "[fake_driver]") {
	FakeOdbcDriver fake;
	OpenStatement open;
	fake.AddResult("SELECT ?, ?", {Rows({Varchar("a")}, {})}, 2);
	REQUIRE(open.api.Prepare(open.statement.value, Text("SELECT ?, ?"), SQL_NTS) == SQL_SUCCESS);
	char first[] = "x";
	char second[] = "y";
	char third[] = "z";
	SQLLEN third_is_null = SQL_NULL_DATA;
	REQUIRE(BindNulTerminated(open, 1, first) == SQL_SUCCESS);
	REQUIRE(BindNulTerminated(open, 2, second) == SQL_SUCCESS);
	REQUIRE(BindText(open, 3, third, &third_is_null) == SQL_SUCCESS);

	CHECK(open.api.Execute(open.statement.value) == SQL_SUCCESS);

	CHECK(DetailsOf(fake, "SQLExecute") == std::vector<std::string> {"1=x\n2=y"});
}

TEST_CASE("SQLBindParameter refuses a parameter that is not a text input", "[fake_driver]") {
	FakeOdbcDriver fake;
	OpenStatement open;
	SQLINTEGER number = 7;
	char text[] = "x";

	CHECK(open.api.BindParameter(open.statement.value, 1, SQL_PARAM_INPUT, SQL_C_SLONG, SQL_VARCHAR, 0, 0, &number, 0,
	                             nullptr) == SQL_ERROR);

	CHECK(DiagRecord(open.api, SQL_HANDLE_STMT, open.statement.value, 1) ==
	      "HYC00 0 the fake driver binds only SQL_C_CHAR input parameters as SQL_VARCHAR; bind the value as text");
	CHECK(open.api.BindParameter(open.statement.value, 1, SQL_PARAM_OUTPUT, SQL_C_CHAR, SQL_VARCHAR, 64, 0, text, 0,
	                             nullptr) == SQL_ERROR);
	CHECK(StatementSqlstate(open) == "HYC00");
	CHECK(open.api.BindParameter(open.statement.value, 1, SQL_PARAM_INPUT, SQL_C_CHAR, SQL_INTEGER, 64, 0, text, 0,
	                             nullptr) == SQL_ERROR);
	CHECK(StatementSqlstate(open) == "HYC00");
}

// The ODBC client binds no NULL parameter, so a NULL indicator is a mistake the fake reports rather than
// sends.
TEST_CASE("SQLExecute refuses a parameter bound with a NULL indicator", "[fake_driver]") {
	FakeOdbcDriver fake;
	OpenStatement open;
	fake.AddResult("SELECT ?", {Rows({Varchar("a")}, {})}, 1);
	REQUIRE(open.api.Prepare(open.statement.value, Text("SELECT ?"), SQL_NTS) == SQL_SUCCESS);
	char value[] = "x";
	SQLLEN indicator = SQL_NULL_DATA;
	REQUIRE(BindText(open, 1, value, &indicator) == SQL_SUCCESS);

	CHECK(open.api.Execute(open.statement.value) == SQL_ERROR);

	CHECK(DiagRecord(open.api, SQL_HANDLE_STMT, open.statement.value, 1) ==
	      "HYC00 0 the fake driver sends only non-NULL text parameters, counted or NUL-terminated; bind a value with "
	      "a length or SQL_NTS");
}

TEST_CASE("SQLExecute refuses a parameter bound with no buffer at all", "[fake_driver]") {
	FakeOdbcDriver fake;
	OpenStatement open;
	fake.AddResult("SELECT ?", {Rows({Varchar("a")}, {})}, 1);
	REQUIRE(open.api.Prepare(open.statement.value, Text("SELECT ?"), SQL_NTS) == SQL_SUCCESS);
	SQLLEN length = 1;
	REQUIRE(BindText(open, 1, nullptr, &length) == SQL_SUCCESS);

	CHECK(open.api.Execute(open.statement.value) == SQL_ERROR);

	CHECK(StatementSqlstate(open) == "HYC00");
}

// With no indicator the driver sends BufferLength bytes, so a parameter bound the way ODBC reads as
// NUL-terminated text elsewhere runs live with an empty value (rsutil.c getParamVal).
TEST_CASE("A parameter bound with no indicator sends BufferLength bytes, none for 0", "[fake_driver]") {
	FakeOdbcDriver fake;
	OpenStatement open;
	fake.AddResult("SELECT ?", {Rows({Varchar("a")}, {})}, 1);
	REQUIRE(open.api.Prepare(open.statement.value, Text("SELECT ?"), SQL_NTS) == SQL_SUCCESS);
	char value[] = "abc";
	REQUIRE(BindText(open, 1, value, 0, nullptr) == SQL_SUCCESS);
	REQUIRE(open.api.Execute(open.statement.value) == SQL_SUCCESS);
	REQUIRE(BindText(open, 1, value, 2, nullptr) == SQL_SUCCESS);

	REQUIRE(open.api.Execute(open.statement.value) == SQL_SUCCESS);

	CHECK(DetailsOf(fake, "SQLExecute") == std::vector<std::string> {"1=", "1=ab"});
}

// An indicator of 0 or more replaces a BufferLength of 0 and caps a larger one, and SQL_NTS reads up
// to the NUL whatever BufferLength says, a negative one included (rsutil.c getParamVal).
TEST_CASE("SQLExecute takes a parameter's length from its indicator, capped by a BufferLength above 0",
          "[fake_driver]") {
	FakeOdbcDriver fake;
	OpenStatement open;
	fake.AddResult("SELECT ?", {Rows({Varchar("a")}, {})}, 1);
	REQUIRE(open.api.Prepare(open.statement.value, Text("SELECT ?"), SQL_NTS) == SQL_SUCCESS);
	char value[] = "abcd";
	struct Binding {
		SQLLEN buffer_length;
		SQLLEN indicator;
	};
	const std::vector<Binding> bindings {{0, 3}, {3, 2}, {2, 3}, {2, SQL_NTS}, {SQL_NTS, SQL_NTS}, {0, 0}};
	for (const auto &binding : bindings) {
		SQLLEN indicator = binding.indicator;
		REQUIRE(BindText(open, 1, value, binding.buffer_length, &indicator) == SQL_SUCCESS);
		REQUIRE(open.api.Execute(open.statement.value) == SQL_SUCCESS);
	}

	CHECK(DetailsOf(fake, "SQLExecute") ==
	      std::vector<std::string> {"1=abc", "1=ab", "1=ab", "1=abcd", "1=abcd", "1="});
}

// A negative BufferLength that no SQL_NTS indicator overrides sends NULL (rsutil.c getParamVal), and
// the ODBC client binds no NULL, so the fake refuses it; the case above pins the SQL_NTS exception.
TEST_CASE("SQLExecute refuses a negative BufferLength unless the indicator is SQL_NTS, because the driver sends such a "
          "parameter as NULL",
          "[fake_driver]") {
	FakeOdbcDriver fake;
	OpenStatement open;
	fake.AddResult("SELECT ?", {Rows({Varchar("a")}, {})}, 1);
	REQUIRE(open.api.Prepare(open.statement.value, Text("SELECT ?"), SQL_NTS) == SQL_SUCCESS);
	char value[] = "abc";
	SQLLEN length = 3;
	const std::string refusal = "HYC00 0 the fake driver sends only non-NULL text parameters, counted or "
	                            "NUL-terminated; bind a value with a length or SQL_NTS";
	REQUIRE(BindText(open, 1, value, SQL_NTS, nullptr) == SQL_SUCCESS);
	CHECK(open.api.Execute(open.statement.value) == SQL_ERROR);
	CHECK(DiagRecord(open.api, SQL_HANDLE_STMT, open.statement.value, 1) == refusal);
	REQUIRE(BindText(open, 1, value, SQL_NTS, &length) == SQL_SUCCESS);

	CHECK(open.api.Execute(open.statement.value) == SQL_ERROR);

	CHECK(DiagRecord(open.api, SQL_HANDLE_STMT, open.statement.value, 1) == refusal);
}

// The driver sends every parameter as text with no lengths, which libpq measures with strlen
// (rslibpq.c libpqExecuteDirectOrPreparedOnThread, fe-exec.c PQsendQueryGuts), so a length that
// takes in a NUL still sends only the text before it.
TEST_CASE("SQLExecute sends a parameter's text only up to its first NUL", "[fake_driver]") {
	FakeOdbcDriver fake;
	OpenStatement open;
	fake.AddResult("SELECT ?", {Rows({Varchar("a")}, {})}, 1);
	REQUIRE(open.api.Prepare(open.statement.value, Text("SELECT ?"), SQL_NTS) == SQL_SUCCESS);
	char value[] = "abc";
	char embedded[] = {'a', '\0', 'b'};
	SQLLEN length = sizeof(embedded);
	REQUIRE(BindText(open, 1, value, sizeof(value), nullptr) == SQL_SUCCESS);
	REQUIRE(open.api.Execute(open.statement.value) == SQL_SUCCESS);
	REQUIRE(BindText(open, 1, embedded, &length) == SQL_SUCCESS);

	REQUIRE(open.api.Execute(open.statement.value) == SQL_SUCCESS);

	CHECK(DetailsOf(fake, "SQLExecute") == std::vector<std::string> {"1=abc", "1=a"});
}

// The driver unbinds a parameter given neither a buffer nor an indicator (rsparameter.cpp
// RS_SQLBindParameter), so the statement then has fewer bound parameters than markers, which it
// refuses (rsutil.c countBindParams, rsexecute.cpp RS_SQLExecDirect).
TEST_CASE("Binding a null buffer with a null indicator unbinds the parameter", "[fake_driver]") {
	FakeOdbcDriver fake;
	OpenStatement open;
	fake.AddResult("SELECT ?", {Rows({Varchar("a")}, {})}, 1);
	REQUIRE(open.api.Prepare(open.statement.value, Text("SELECT ?"), SQL_NTS) == SQL_SUCCESS);
	char value[] = "x";
	REQUIRE(BindNulTerminated(open, 1, value) == SQL_SUCCESS);
	REQUIRE(BindText(open, 1, nullptr, nullptr) == SQL_SUCCESS);

	CHECK(open.api.Execute(open.statement.value) == SQL_ERROR);

	CHECK(DiagRecord(open.api, SQL_HANDLE_STMT, open.statement.value, 1) ==
	      "HY000 0 Number of parameters in query is more than number of bind parameters.");
}

// The driver returns SQL_NO_DATA when an UPDATE, INSERT or DELETE affected no rows (public source
// v2.3.0, rslibpq.c), not checked against a live server.
TEST_CASE("SQLExecute reports no data for a statement that affected no rows", "[fake_driver]") {
	FakeOdbcDriver fake;
	OpenStatement open;
	FakeResultSet deleted;
	deleted.row_count = 0;
	fake.AddResult("DELETE FROM t WHERE a = 0", {deleted});
	REQUIRE(open.api.Prepare(open.statement.value, Text("DELETE FROM t WHERE a = 0"), SQL_NTS) == SQL_SUCCESS);
	SQLLEN rows = -7;

	CHECK(open.api.Execute(open.statement.value) == SQL_NO_DATA);

	REQUIRE(open.api.RowCount(open.statement.value, &rows) == SQL_SUCCESS);
	CHECK(rows == 0);
}

// The driver returns SQL_NO_DATA for an UPDATE or INSERT tag reporting 0 rows as it does for a
// DELETE (rslibpq.c setResultInStmt).
TEST_CASE("SQLExecute reports no data for an UPDATE or an INSERT that affected no rows", "[fake_driver]") {
	FakeOdbcDriver fake;
	OpenStatement open;
	FakeResultSet affected_none;
	affected_none.row_count = 0;
	fake.AddResult("UPDATE t SET a = 1 WHERE a = 0", {affected_none});
	fake.AddResult("INSERT INTO t SELECT a FROM t WHERE a = 0", {affected_none});

	REQUIRE(open.api.Prepare(open.statement.value, Text("UPDATE t SET a = 1 WHERE a = 0"), SQL_NTS) == SQL_SUCCESS);
	CHECK(open.api.Execute(open.statement.value) == SQL_NO_DATA);
	REQUIRE(open.api.Prepare(open.statement.value, Text("INSERT INTO t SELECT a FROM t WHERE a = 0"), SQL_NTS) ==
	        SQL_SUCCESS);
	CHECK(open.api.Execute(open.statement.value) == SQL_NO_DATA);
}

// The server's command tag does not depend on how the text was written, so the fake's reader skips
// the leading blanks and compares the word without case.
TEST_CASE("SQLExecute reports no data for an indented lower-case DELETE that affected no rows", "[fake_driver]") {
	FakeOdbcDriver fake;
	OpenStatement open;
	FakeResultSet deleted;
	deleted.row_count = 0;
	fake.AddResult("  delete from t where a = 0", {deleted});
	REQUIRE(open.api.Prepare(open.statement.value, Text("  delete from t where a = 0"), SQL_NTS) == SQL_SUCCESS);

	CHECK(open.api.Execute(open.statement.value) == SQL_NO_DATA);
}

// The driver takes the count from the server's command tag and returns SQL_NO_DATA only for an
// UPDATE, INSERT or DELETE tag, so a SET, which affects no rows and reports 0, succeeds (public
// source v2.3.0, rslibpq.c). The ODBC client runs its session settings through SQLPrepare and SQLExecute.
TEST_CASE("SQLExecute succeeds for a statement that is not DML and reports 0 rows", "[fake_driver]") {
	FakeOdbcDriver fake;
	OpenStatement open;
	FakeResultSet applied;
	applied.row_count = 0;
	fake.AddResult("SET extra_float_digits TO 2", {applied});
	REQUIRE(open.api.Prepare(open.statement.value, Text("SET extra_float_digits TO 2"), SQL_NTS) == SQL_SUCCESS);
	SQLLEN rows = -7;

	CHECK(open.api.Execute(open.statement.value) == SQL_SUCCESS);

	REQUIRE(open.api.RowCount(open.statement.value, &rows) == SQL_SUCCESS);
	CHECK(rows == 0);
}

// The driver reads the tag only of a command that returned no rows (rslibpq.c setResultInStmt), so a
// result set with columns never reports no data, whatever its text's leading word.
TEST_CASE("SQLExecute succeeds for DML text scripted with a result set of no rows", "[fake_driver]") {
	FakeOdbcDriver fake;
	OpenStatement open;
	auto returned = Rows({Varchar("a")}, {});
	returned.row_count = 0;
	fake.AddResult("DELETE FROM t", {returned});
	REQUIRE(open.api.Prepare(open.statement.value, Text("DELETE FROM t"), SQL_NTS) == SQL_SUCCESS);

	CHECK(open.api.Execute(open.statement.value) == SQL_SUCCESS);
}

// The driver writes -1 and succeeds whatever state the statement is in (public source v2.3.0,
// rsresult.cpp), so a count read too early is -1 rather than an error.
TEST_CASE("SQLRowCount reports -1 until the statement has run", "[fake_driver]") {
	FakeOdbcDriver fake;
	OpenStatement open;
	SQLLEN rows = -7;
	REQUIRE(open.api.RowCount(open.statement.value, &rows) == SQL_SUCCESS);
	CHECK(rows == -1);
	auto counted = Rows({Varchar("a")}, {{"1"}, {"2"}});
	counted.row_count = 2;
	fake.AddResult("SELECT a", {counted});
	PrepareAndExecute(open, "SELECT a");

	REQUIRE(open.api.RowCount(open.statement.value, &rows) == SQL_SUCCESS);

	CHECK(rows == 2);
}

TEST_CASE("SQLRowCount reports -1 for a statement prepared but not yet run", "[fake_driver]") {
	FakeOdbcDriver fake;
	OpenStatement open;
	auto counted = Rows({Varchar("a")}, {{"1"}, {"2"}});
	counted.row_count = 2;
	fake.AddResult("SELECT a", {counted});
	REQUIRE(open.api.Prepare(open.statement.value, Text("SELECT a"), SQL_NTS) == SQL_SUCCESS);
	SQLLEN rows = -7;

	REQUIRE(open.api.RowCount(open.statement.value, &rows) == SQL_SUCCESS);

	CHECK(rows == -1);
}

TEST_CASE("SQLRowCount fails a null count pointer", "[fake_driver]") {
	FakeOdbcDriver fake;
	OpenStatement open;

	CHECK(open.api.RowCount(open.statement.value, nullptr) == SQL_ERROR);

	CHECK(StatementSqlstate(open) == "HY009");
}

TEST_CASE("SQLMoreResults steps to the next result set, which has its own columns and count", "[fake_driver]") {
	FakeOdbcDriver fake;
	OpenStatement open;
	const char *const sql = "INSERT INTO t VALUES (1); SELECT a FROM t";
	FakeResultSet inserted;
	inserted.row_count = 3;
	auto selected = Rows({Varchar("a")}, {{"1"}});
	selected.row_count = 1;
	fake.AddResult(sql, {inserted, selected});
	PrepareAndExecute(open, sql);
	SQLSMALLINT columns = -1;
	SQLLEN rows = -7;
	REQUIRE(open.api.NumResultCols(open.statement.value, &columns) == SQL_SUCCESS);
	CHECK(columns == 0);
	REQUIRE(open.api.RowCount(open.statement.value, &rows) == SQL_SUCCESS);
	CHECK(rows == 3);

	REQUIRE(open.api.MoreResults(open.statement.value) == SQL_SUCCESS);

	REQUIRE(open.api.NumResultCols(open.statement.value, &columns) == SQL_SUCCESS);
	CHECK(columns == 1);
	REQUIRE(open.api.RowCount(open.statement.value, &rows) == SQL_SUCCESS);
	CHECK(rows == 1);
	REQUIRE(open.api.Fetch(open.statement.value) == SQL_SUCCESS);
	CHECK(Value(open, 1) == "1");
}

// The driver's SQLMoreResults releases the last result without touching the statement's status, so
// the statement stays executed with no result to fetch from and SQLFetch posts 24000, HY010 being
// for a closed cursor alone (public source v2.3.0, rsresult.cpp and rsprepare.cpp).
TEST_CASE("SQLMoreResults past the last result set reports no more and leaves nothing to fetch", "[fake_driver]") {
	FakeOdbcDriver fake;
	OpenStatement open;
	fake.AddResult("SELECT a", {Rows({Varchar("a")}, {{"1"}})});
	PrepareAndExecute(open, "SELECT a");

	CHECK(open.api.MoreResults(open.statement.value) == SQL_NO_DATA);

	CHECK(open.api.Fetch(open.statement.value) == SQL_ERROR);
	CHECK(DiagRecord(open.api, SQL_HANDLE_STMT, open.statement.value, 1) == "24000 0 Invalid cursor state");
}

// The driver reports no columns and no row count once it holds no result (public source v2.3.0,
// rsresult.cpp).
TEST_CASE("A statement whose results are drained reports no columns and no row count", "[fake_driver]") {
	FakeOdbcDriver fake;
	OpenStatement open;
	auto counted = Rows({Varchar("a")}, {{"1"}});
	counted.row_count = 1;
	fake.AddResult("SELECT a", {counted});
	PrepareAndExecute(open, "SELECT a");
	REQUIRE(open.api.MoreResults(open.statement.value) == SQL_NO_DATA);
	SQLSMALLINT columns = -1;
	SQLLEN rows = -7;

	REQUIRE(open.api.NumResultCols(open.statement.value, &columns) == SQL_SUCCESS);
	REQUIRE(open.api.RowCount(open.statement.value, &rows) == SQL_SUCCESS);

	CHECK(columns == 0);
	CHECK(rows == -1);
}

// Once the driver holds no result, both describing calls find nothing to describe (rsresult.cpp
// RS_SQLDescribeCol, RS_SQLColAttribute).
TEST_CASE("Describing a column of a statement whose results are drained fails with HY000", "[fake_driver]") {
	FakeOdbcDriver fake;
	OpenStatement open;
	fake.AddResult("SELECT a", {Rows({Varchar("a")}, {{"1"}})});
	PrepareAndExecute(open, "SELECT a");
	REQUIRE(open.api.MoreResults(open.statement.value) == SQL_NO_DATA);
	SQLLEN concise = 0;

	CHECK(open.api.DescribeCol(open.statement.value, 1, nullptr, 0, nullptr, nullptr, nullptr, nullptr, nullptr) ==
	      SQL_ERROR);
	CHECK(DiagRecord(open.api, SQL_HANDLE_STMT, open.statement.value, 1) == "HY000 0 No result found");
	CHECK(open.api.ColAttribute(open.statement.value, 1, SQL_DESC_CONCISE_TYPE, nullptr, 0, nullptr, &concise) ==
	      SQL_ERROR);
	CHECK(DiagRecord(open.api, SQL_HANDLE_STMT, open.statement.value, 1) == "HY000 0 No result found");
}

// The ODBC client's end of data drains the results and then closes the cursor, which the driver accepts: only
// a statement still allocated or already closed is refused (public source v2.3.0, rsprepare.cpp).
TEST_CASE("SQLCloseCursor succeeds once SQLMoreResults has drained every result set", "[fake_driver]") {
	FakeOdbcDriver fake;
	OpenStatement open;
	fake.AddResult("SELECT a", {Rows({Varchar("a")}, {{"1"}})});
	PrepareAndExecute(open, "SELECT a");
	REQUIRE(open.api.MoreResults(open.statement.value) == SQL_NO_DATA);

	CHECK(open.api.CloseCursor(open.statement.value) == SQL_SUCCESS);

	CHECK(DiagRecordCount(open.api, SQL_HANDLE_STMT, open.statement.value) == 0);
}

// The driver's SQLGetData posts 24000 whenever it has no result, whatever the statement's state, and
// never HY010 (public source v2.3.0, rsresult.cpp), so the row a drained statement stood on is no
// longer readable.
TEST_CASE("SQLGetData after SQLMoreResults has drained fails with 24000", "[fake_driver]") {
	FakeOdbcDriver fake;
	OpenStatement open;
	FetchDefaultRow(open);
	REQUIRE(open.api.MoreResults(open.statement.value) == SQL_NO_DATA);
	char value[8] = {};
	SQLLEN length = 0;

	CHECK(open.api.GetData(open.statement.value, 1, SQL_C_CHAR, value, sizeof(value), &length) == SQL_ERROR);

	CHECK(DiagRecord(open.api, SQL_HANDLE_STMT, open.statement.value, 1) == "24000 0 Invalid cursor state");
}

// The driver's SQLGetData posts 24000 whenever it has no result, whatever the statement's state, and
// never HY010 (public source v2.3.0, rsresult.cpp).
TEST_CASE("SQLGetData after SQLCloseCursor fails with 24000", "[fake_driver]") {
	FakeOdbcDriver fake;
	OpenStatement open;
	FetchDefaultRow(open);
	REQUIRE(open.api.CloseCursor(open.statement.value) == SQL_SUCCESS);
	char value[8] = {};
	SQLLEN length = 0;

	CHECK(open.api.GetData(open.statement.value, 1, SQL_C_CHAR, value, sizeof(value), &length) == SQL_ERROR);

	CHECK(DiagRecord(open.api, SQL_HANDLE_STMT, open.statement.value, 1) == "24000 0 Invalid cursor state");
}

TEST_CASE("SQLMoreResults reports no more for a statement that was never executed", "[fake_driver]") {
	FakeOdbcDriver fake;
	OpenStatement open;
	fake.AddResult("SELECT a; SELECT b", {Rows({Varchar("a")}, {{"1"}}), Rows({Varchar("b")}, {{"2"}})});
	REQUIRE(open.api.Prepare(open.statement.value, Text("SELECT a; SELECT b"), SQL_NTS) == SQL_SUCCESS);

	CHECK(open.api.MoreResults(open.statement.value) == SQL_NO_DATA);
}

// A failed SQLExecute releases every result set, the later ones included.
TEST_CASE("SQLMoreResults reports no more after a scripted SQLExecute failure", "[fake_driver]") {
	FakeOdbcDriver fake;
	OpenStatement open;
	fake.AddResult("SELECT a; SELECT b", {Rows({Varchar("a")}, {{"1"}}), Rows({Varchar("b")}, {{"2"}})});
	PrepareAndExecute(open, "SELECT a; SELECT b");
	fake.FailCall("SQLExecute", 2, SQL_ERROR, {{"57014", 0, "Query cancelled"}});
	REQUIRE(open.api.Execute(open.statement.value) == SQL_ERROR);

	CHECK(open.api.MoreResults(open.statement.value) == SQL_NO_DATA);
}

// Each result set starts at its first row, so a fetch on the set before does not carry over.
TEST_CASE("SQLMoreResults between two sets of rows starts the next from its first row", "[fake_driver]") {
	FakeOdbcDriver fake;
	OpenStatement open;
	const char *const sql = "SELECT a FROM t; SELECT b FROM u";
	fake.AddResult(sql, {Rows({Varchar("a")}, {{"a1"}, {"a2"}}), Rows({Varchar("b")}, {{"b1"}})});
	PrepareAndExecute(open, sql);
	REQUIRE(open.api.Fetch(open.statement.value) == SQL_SUCCESS);
	REQUIRE(Value(open, 1) == "a1");

	REQUIRE(open.api.MoreResults(open.statement.value) == SQL_SUCCESS);

	REQUIRE(open.api.Fetch(open.statement.value) == SQL_SUCCESS);
	CHECK(Value(open, 1) == "b1");
	CHECK(open.api.Fetch(open.statement.value) == SQL_NO_DATA);
	CHECK(open.api.MoreResults(open.statement.value) == SQL_NO_DATA);
}

// Measured on driver 2.3.0.0: SQL_GETDATA_EXTENSIONS is 0xb, which includes SQL_GD_ANY_ORDER, so a
// caller may read a row's columns in any order.
TEST_CASE("SQLFetch steps through the scripted rows, whose cells SQLGetData reads in any order", "[fake_driver]") {
	FakeOdbcDriver fake;
	OpenStatement open;
	fake.AddResult("SELECT a, b", {Rows({Varchar("a"), Varchar("b")}, {{"a1", "b1"}, {"a2", ""}})});
	PrepareAndExecute(open, "SELECT a, b");

	REQUIRE(open.api.Fetch(open.statement.value) == SQL_SUCCESS);
	CHECK(Value(open, 2) == "b1");
	CHECK(Value(open, 1) == "a1");
	REQUIRE(open.api.Fetch(open.statement.value) == SQL_SUCCESS);
	CHECK(Value(open, 2) == "");
	CHECK(Value(open, 1) == "a2");

	CHECK(open.api.Fetch(open.statement.value) == SQL_NO_DATA);
	CHECK(open.api.Fetch(open.statement.value) == SQL_NO_DATA);
}

// ODBC returns SQL_ERROR when every row of the rowset is in error, and the driver's public source
// (v2.3.0, rsresult.cpp) says so for a rowset of 1, which is how the fake fetches.
TEST_CASE("SQLFetch fails a row scripted in error and leaves that row's diagnostics", "[fake_driver]") {
	FakeOdbcDriver fake;
	OpenStatement open;
	auto result = Rows({Varchar("a")}, {{"1"}, {""}});
	result.row_status = {SQL_ROW_SUCCESS, SQL_ROW_ERROR};
	result.row_diags[1] = {{"22003", 0, "numeric value out of range"}};
	fake.AddResult("SELECT a", {result});
	PrepareAndExecute(open, "SELECT a");
	REQUIRE(open.api.Fetch(open.statement.value) == SQL_SUCCESS);

	CHECK(open.api.Fetch(open.statement.value) == SQL_ERROR);

	CHECK(DiagRecord(open.api, SQL_HANDLE_STMT, open.statement.value, 1) == "22003 0 numeric value out of range");
}

// The driver's SQLFetch finds a result with no rows and returns SQL_NO_DATA rather than failing
// (public source v2.3.0, rsresult.cpp), where a statement never executed posts 24000.
TEST_CASE("SQLFetch on a statement that returned no rows reports no data", "[fake_driver]") {
	FakeOdbcDriver fake;
	OpenStatement open;
	FakeResultSet inserted;
	inserted.row_count = 1;
	fake.AddResult("INSERT INTO t VALUES (1)", {inserted});
	PrepareAndExecute(open, "INSERT INTO t VALUES (1)");

	CHECK(open.api.Fetch(open.statement.value) == SQL_NO_DATA);
}

TEST_CASE("SQLGetData returns a long cell in parts, each with the length still to come", "[fake_driver]") {
	FakeOdbcDriver fake;
	OpenStatement open;
	FetchTextRow(fake, open, "SELECT v", {"abcdefg"});

	CHECK(ReadParts(open, 1, SQL_C_CHAR, 4, 1) == std::vector<Part> {{"abc", 7, SQL_SUCCESS_WITH_INFO},
	                                                                 {"def", 4, SQL_SUCCESS_WITH_INFO},
	                                                                 {"g", 1, SQL_SUCCESS}});
}

// A text part holds the buffer's size less its NUL, so 10,000 bytes need three 4,096-byte reads.
TEST_CASE("A 10,000-byte cell read with a 4,096-byte buffer comes in three parts", "[fake_driver]") {
	FakeOdbcDriver fake;
	OpenStatement open;
	FetchTextRow(fake, open, "SELECT v", {std::string(10000, 'x')});

	const auto parts = ReadParts(open, 1, SQL_C_CHAR, 4096, 1);

	REQUIRE(parts.size() == 3);
	CHECK(parts[0].length == 10000);
	CHECK(parts[1].length == 5905);
	CHECK(parts[2].length == 1810);
}

TEST_CASE("Only a column scripted to report no total gives SQL_NO_TOTAL, for each part but the last", "[fake_driver]") {
	FakeOdbcDriver fake;
	OpenStatement open;
	auto streamed = Varchar("streamed");
	streamed.report_no_total = true;
	fake.AddResult("SELECT streamed, counted", {Rows({streamed, Varchar("counted")}, {{"abcdefg", "abcdefg"}})});
	PrepareAndExecute(open, "SELECT streamed, counted");
	REQUIRE(open.api.Fetch(open.statement.value) == SQL_SUCCESS);

	CHECK(ReadParts(open, 1, SQL_C_CHAR, 4, 1) == std::vector<Part> {{"abc", SQL_NO_TOTAL, SQL_SUCCESS_WITH_INFO},
	                                                                 {"def", SQL_NO_TOTAL, SQL_SUCCESS_WITH_INFO},
	                                                                 {"g", 1, SQL_SUCCESS}});
	CHECK(ReadParts(open, 2, SQL_C_CHAR, 4, 1) == std::vector<Part> {{"abc", 7, SQL_SUCCESS_WITH_INFO},
	                                                                 {"def", 4, SQL_SUCCESS_WITH_INFO},
	                                                                 {"g", 1, SQL_SUCCESS}});
}

// The ODBC client reads a VARBYTE as SQL_C_BINARY, because read as text the driver sends hex (measured on
// 2.3.0.0). A cut binary part succeeds with info and posts no record, which ReadParts checks; that
// follows the driver's source (rsutil.c copyHexToBinaryDataBigLen), as no live test checks it.
TEST_CASE("SQLGetData returns binary zeros as bytes, in unterminated SQL_C_BINARY parts", "[fake_driver]") {
	FakeOdbcDriver fake;
	OpenStatement open;
	const std::string bytes("a\0b\0\0c", 6);
	fake.AddResult("SELECT b", {Rows({Column("b", "VARBYTE", SQL_LONGVARBINARY, 64000, 0)}, {{bytes}})});
	PrepareAndExecute(open, "SELECT b");
	REQUIRE(open.api.Fetch(open.statement.value) == SQL_SUCCESS);

	CHECK(ReadParts(open, 1, SQL_C_BINARY, 4, 0) ==
	      std::vector<Part> {{std::string("a\0b\0", 4), 6, SQL_SUCCESS_WITH_INFO},
	                         {std::string("\0c", 2), 2, SQL_SUCCESS}});
}

// A GEOMETRY cell takes the driver's other binary path, copyVariableToCBinary, which posts 01004 for
// a part it cut, where the hex-to-binary conversion of every other binary column posts no record
// (public source v2.3.0, rsutil.c).
TEST_CASE("A cut SQL_C_BINARY part of a GEOMETRY cell posts 01004", "[fake_driver]") {
	FakeOdbcDriver fake;
	OpenStatement open;
	fake.AddResult("SELECT g", {Rows({Column("g", "GEOMETRY", SQL_LONGVARBINARY, 64000, 0)}, {{"abcdefg"}})});
	PrepareAndExecute(open, "SELECT g");
	REQUIRE(open.api.Fetch(open.statement.value) == SQL_SUCCESS);

	CHECK(ReadParts(open, 1, SQL_C_BINARY, 4, 1) ==
	      std::vector<Part> {{"abcd", 7, SQL_SUCCESS_WITH_INFO}, {"efg", 3, SQL_SUCCESS}});
}

// The driver converts a binary cell read as SQL_C_CHAR with a function that keeps no offset, so
// every call returns its first part again and a reader never finishes (public source v2.3.0,
// rsutil.c, copyBinaryDataBigLen and, for VARBYTE among others, copyBinaryToHexDataBigLen). The
// fake does so twice, then refuses the read.
TEST_CASE("A binary cell too long for the buffer returns its first part twice as text, then is refused",
          "[fake_driver]") {
	FakeOdbcDriver fake;
	OpenStatement open;
	fake.AddResult("SELECT b", {Rows({Column("b", "VARBYTE", SQL_LONGVARBINARY, 64000, 0)}, {{"abcdefg"}})});
	PrepareAndExecute(open, "SELECT b");
	REQUIRE(open.api.Fetch(open.statement.value) == SQL_SUCCESS);
	char value[4] = {};
	SQLLEN length = 0;

	for (int call = 0; call < 2; call++) {
		CHECK(open.api.GetData(open.statement.value, 1, SQL_C_CHAR, value, sizeof(value), &length) ==
		      SQL_SUCCESS_WITH_INFO);
		CHECK(std::string(value) == "abc");
		CHECK(length == 7);
		CHECK(DiagRecord(open.api, SQL_HANDLE_STMT, open.statement.value, 1) == "01004 0 String data, right truncated");
	}
	CHECK(open.api.GetData(open.statement.value, 1, SQL_C_CHAR, value, sizeof(value), &length) == SQL_ERROR);
	CHECK(DiagRecord(open.api, SQL_HANDLE_STMT, open.statement.value, 1) ==
	      "HYC00 0 the fake driver returns the first part of a binary cell that does not fit the buffer only twice "
	      "as SQL_C_CHAR, because the driver returns it on every such call; read it as SQL_C_BINARY");
}

// Read as text, a binary cell needs room for the NUL as well, so the repeat starts at a cell exactly
// as long as the buffer.
TEST_CASE("A binary cell read as text repeats from the length of the buffer up", "[fake_driver]") {
	FakeOdbcDriver fake;
	OpenStatement open;
	fake.AddResult("SELECT b", {Rows({Column("b", "VARBYTE", SQL_LONGVARBINARY, 64000, 0)}, {{"abc"}, {"abc"}})});
	PrepareAndExecute(open, "SELECT b");
	REQUIRE(open.api.Fetch(open.statement.value) == SQL_SUCCESS);
	char value[4] = {};
	SQLLEN length = 0;

	CHECK(open.api.GetData(open.statement.value, 1, SQL_C_CHAR, value, 4, &length) == SQL_SUCCESS);
	CHECK(std::string(value) == "abc");
	REQUIRE(open.api.Fetch(open.statement.value) == SQL_SUCCESS);

	CHECK(open.api.GetData(open.statement.value, 1, SQL_C_CHAR, value, 3, &length) == SQL_SUCCESS_WITH_INFO);
	CHECK(std::string(value) == "ab");
	CHECK(length == 3);
	// Read again, the cell repeats its first part, where an ordinary long cell would return the rest.
	CHECK(open.api.GetData(open.statement.value, 1, SQL_C_CHAR, value, 3, &length) == SQL_SUCCESS_WITH_INFO);
	CHECK(std::string(value) == "ab");
	CHECK(length == 3);
}

TEST_CASE("A cell holding a NUL reports its full length when read as text", "[fake_driver]") {
	FakeOdbcDriver fake;
	OpenStatement open;
	FetchTextRow(fake, open, "SELECT v", {std::string("a\0b", 3)});
	char value[8] = {};
	SQLLEN length = 0;

	REQUIRE(open.api.GetData(open.statement.value, 1, SQL_C_CHAR, value, sizeof(value), &length) == SQL_SUCCESS);

	CHECK(length == 3);
	CHECK(std::string(value, 3) == std::string("a\0b", 3));
}

// The ODBC client joins the parts before it validates UTF-8, because no part is valid on its own.
TEST_CASE("SQLGetData cuts a 4-byte UTF-8 character at a part boundary, and the parts rejoin it", "[fake_driver]") {
	FakeOdbcDriver fake;
	OpenStatement open;
	const std::string text = "a\xF0\x9F\x98\x80"
	                         "b"; // U+1F600 between two ASCII letters
	FetchTextRow(fake, open, "SELECT t", {text});

	const auto parts = ReadParts(open, 1, SQL_C_CHAR, 4, 1);

	REQUIRE(parts.size() == 2);
	CHECK(parts[0].bytes == "a\xF0\x9F");
	CHECK(parts[0].bytes + parts[1].bytes == text);
}

TEST_CASE("SQLGetData with no room reports the length and copies nothing", "[fake_driver]") {
	FakeOdbcDriver fake;
	OpenStatement open;
	FetchTextRow(fake, open, "SELECT v", {"abcdefg"});
	char value[8] = {'z'};
	SQLLEN length = 0;

	CHECK(open.api.GetData(open.statement.value, 1, SQL_C_CHAR, value, 0, &length) == SQL_SUCCESS_WITH_INFO);

	CHECK(length == 7);
	CHECK(value[0] == 'z');
	CHECK(Value(open, 1) == "abcdefg");
}

TEST_CASE("SQLGetData reports a NULL cell as SQL_NULL_DATA, then no more data", "[fake_driver]") {
	FakeOdbcDriver fake;
	OpenStatement open;
	auto result = Rows({Varchar("a")}, {{""}});
	result.nulls = {{true}};
	fake.AddResult("SELECT NULL", {result});
	PrepareAndExecute(open, "SELECT NULL");
	REQUIRE(open.api.Fetch(open.statement.value) == SQL_SUCCESS);
	char value[8] = {'z'};
	SQLLEN length = 0;

	REQUIRE(open.api.GetData(open.statement.value, 1, SQL_C_CHAR, value, sizeof(value), &length) == SQL_SUCCESS);

	CHECK(length == SQL_NULL_DATA);
	// The driver writes a NUL into the target's first byte for a NULL cell when the buffer has room
	// (public source v2.3.0, rsresult.cpp).
	CHECK(value[0] == '\0');
	CHECK(open.api.GetData(open.statement.value, 1, SQL_C_CHAR, value, sizeof(value), &length) == SQL_NO_DATA);
}

TEST_CASE("SQLGetData needs an indicator to report a NULL cell in", "[fake_driver]") {
	FakeOdbcDriver fake;
	OpenStatement open;
	auto result = Rows({Varchar("a")}, {{""}});
	result.nulls = {{true}};
	fake.AddResult("SELECT NULL", {result});
	PrepareAndExecute(open, "SELECT NULL");
	REQUIRE(open.api.Fetch(open.statement.value) == SQL_SUCCESS);
	char value[8] = {};

	CHECK(open.api.GetData(open.statement.value, 1, SQL_C_CHAR, value, sizeof(value), nullptr) == SQL_ERROR);

	CHECK(StatementSqlstate(open) == "22002");
}

// The driver writes the NUL only when the buffer has room for it (rsresult.cpp:865, in
// RS_SQLGetData), so a zero-length read leaves the caller's byte alone.
TEST_CASE("A NULL cell read into a buffer with no room writes nothing to it", "[fake_driver]") {
	FakeOdbcDriver fake;
	OpenStatement open;
	auto result = Rows({Varchar("a")}, {{""}});
	result.nulls = {{true}};
	fake.AddResult("SELECT NULL", {result});
	PrepareAndExecute(open, "SELECT NULL");
	REQUIRE(open.api.Fetch(open.statement.value) == SQL_SUCCESS);
	char value[1] = {'z'};
	SQLLEN length = 0;

	REQUIRE(open.api.GetData(open.statement.value, 1, SQL_C_CHAR, value, 0, &length) == SQL_SUCCESS);

	CHECK(length == SQL_NULL_DATA);
	CHECK(value[0] == 'z');
}

// Each nulls entry belongs to one row and one column, so a NULL is reported for that cell alone. The
// one NULL sits off the diagonal, so reading the entries by column and then row, or always from the
// first row or column, reports a different cell.
TEST_CASE("SQLGetData reports NULL for the cell the nulls entry names and no other", "[fake_driver]") {
	FakeOdbcDriver fake;
	OpenStatement open;
	auto result = Rows({Varchar("a"), Varchar("b")}, {{"a1", ""}, {"a2", "b2"}});
	result.nulls = {{false, true}, {false, false}};
	fake.AddResult("SELECT a, b", {result});
	PrepareAndExecute(open, "SELECT a, b");
	char value[8] = {};
	SQLLEN length = 0;
	REQUIRE(open.api.Fetch(open.statement.value) == SQL_SUCCESS);

	CHECK(Value(open, 1) == "a1");
	REQUIRE(open.api.GetData(open.statement.value, 2, SQL_C_CHAR, value, sizeof(value), &length) == SQL_SUCCESS);
	CHECK(length == SQL_NULL_DATA);
	REQUIRE(open.api.Fetch(open.statement.value) == SQL_SUCCESS);
	CHECK(Value(open, 1) == "a2");
	CHECK(Value(open, 2) == "b2");
}

// The driver refuses a buffer that cannot hold a NUMERIC or DECIMAL cell's whole digits, sign and
// NUL rather than cutting them (rsutil.c validateNumericBufferSize, calculateMinNumericBufferLength):
// "-123.45" needs 5 bytes, and "12345", which has no point, 6.
TEST_CASE("A NUMERIC or DECIMAL cell read as text into a buffer too small for its whole digits fails with 22003",
          "[fake_driver]") {
	FakeOdbcDriver fake;
	OpenStatement open;
	FetchNumericRow(fake, open);
	char value[8] = {};
	SQLLEN length = 0;
	const std::string refusal = "22003 0 Buffer too small for numeric value";

	CHECK(open.api.GetData(open.statement.value, 1, SQL_C_CHAR, value, 4, &length) == SQL_ERROR);
	CHECK(DiagRecord(open.api, SQL_HANDLE_STMT, open.statement.value, 1) == refusal);
	CHECK(open.api.GetData(open.statement.value, 2, SQL_C_CHAR, value, 4, &length) == SQL_ERROR);
	CHECK(DiagRecord(open.api, SQL_HANDLE_STMT, open.statement.value, 1) == refusal);
	CHECK(open.api.GetData(open.statement.value, 3, SQL_C_CHAR, value, 5, &length) == SQL_ERROR);
	CHECK(DiagRecord(open.api, SQL_HANDLE_STMT, open.statement.value, 1) == refusal);
}

// The driver writes the cell's length to the indicator before it converts, and the refusal leaves
// it there (rsresult.cpp RS_SQLGetData).
TEST_CASE("A NUMERIC read refused for its whole digits leaves the cell's length in the indicator", "[fake_driver]") {
	FakeOdbcDriver fake;
	OpenStatement open;
	FetchNumericRow(fake, open);
	char value[4] = {};
	SQLLEN length = 0;

	REQUIRE(open.api.GetData(open.statement.value, 1, SQL_C_CHAR, value, sizeof(value), &length) == SQL_ERROR);

	CHECK(length == 7);
}

// The driver marks a column read when its read fails (rsresult.cpp RS_SQLGetData's error label).
TEST_CASE("A NUMERIC read refused for its whole digits reports no more data next", "[fake_driver]") {
	FakeOdbcDriver fake;
	OpenStatement open;
	FetchNumericRow(fake, open);
	char value[4] = {};
	SQLLEN length = 0;
	REQUIRE(open.api.GetData(open.statement.value, 1, SQL_C_CHAR, value, sizeof(value), &length) == SQL_ERROR);

	CHECK(open.api.GetData(open.statement.value, 1, SQL_C_CHAR, value, sizeof(value), &length) == SQL_NO_DATA);
}

// Only the fraction is ever cut, and a buffer that holds the whole digits, sign and NUL exactly is
// enough.
TEST_CASE("A NUMERIC cell whose whole digits fit is cut only in its fraction", "[fake_driver]") {
	FakeOdbcDriver fake;
	OpenStatement open;
	FetchNumericRow(fake, open);

	CHECK(ReadParts(open, 1, SQL_C_CHAR, 5, 1) ==
	      std::vector<Part> {{"-123", 7, SQL_SUCCESS_WITH_INFO}, {".45", 3, SQL_SUCCESS}});
	CHECK(ReadParts(open, 3, SQL_C_CHAR, 6, 1) == std::vector<Part> {{"12345", 5, SQL_SUCCESS}});
}

// The look-alikes the whole-digits check must pass: a character column holding the same text, and a
// NUMERIC cell read as SQL_C_BINARY, both of which come in parts as any other cell does.
TEST_CASE("Only a NUMERIC or DECIMAL cell read as text is checked for its whole digits", "[fake_driver]") {
	FakeOdbcDriver fake;
	OpenStatement open;
	FetchNumericRow(fake, open);

	CHECK(ReadParts(open, 4, SQL_C_CHAR, 4, 1).size() == 3);
	CHECK(ReadParts(open, 5, SQL_C_BINARY, 4, 1).size() == 2);
}

// The fake's rule: a cell read in full is done with, in whatever order. Measured on 2.3.0.0 for an
// immediate second read (rc 100); going back to an earlier column the driver returns the value
// again (rsresult.cpp, iPrevhCol), so the fake is stricter and a double read fails here, not live.
TEST_CASE("Going back to a column already read in full reports no more data", "[fake_driver]") {
	FakeOdbcDriver fake;
	OpenStatement open;
	fake.AddResult("SELECT a, b", {Rows({Varchar("a"), Varchar("b")}, {{"a1", "b1"}, {"a2", "b2"}})});
	PrepareAndExecute(open, "SELECT a, b");
	REQUIRE(open.api.Fetch(open.statement.value) == SQL_SUCCESS);
	REQUIRE(Value(open, 1) == "a1");
	REQUIRE(Value(open, 2) == "b1");
	char value[8] = {};
	SQLLEN length = 0;

	CHECK(open.api.GetData(open.statement.value, 1, SQL_C_CHAR, value, sizeof(value), &length) == SQL_NO_DATA);

	REQUIRE(open.api.Fetch(open.statement.value) == SQL_SUCCESS);
	CHECK(Value(open, 1) == "a2");
}

// The ODBC client carries every integer width in one INTEGER and both float widths in one REAL,
// so these three C types and SQL_C_BINARY are the native carriers it reads.
TEST_CASE("SQLGetData converts a cell to the native C types the ODBC client reads", "[fake_driver]") {
	FakeOdbcDriver fake;
	OpenStatement open;
	// The driver converts the text the server sent, and for a character column range-checks the
	// result, so these cells are read from character columns as the fixture's helper builds them.
	FetchTextRow(fake, open, "SELECT carriers",
	             {"1", "0", "-9223372036854775808", "1.7976931348623157e+308", "-Infinity", "NaN"});

	CHECK(Number<SQLCHAR>(open, 1, SQL_C_BIT) == 1);
	CHECK(Number<SQLCHAR>(open, 2, SQL_C_BIT) == 0);
	CHECK(Number<SQLBIGINT>(open, 3, SQL_C_SBIGINT) == std::numeric_limits<SQLBIGINT>::min());
	CHECK(Number<SQLDOUBLE>(open, 4, SQL_C_DOUBLE) == std::numeric_limits<SQLDOUBLE>::max());
	CHECK(Number<SQLDOUBLE>(open, 5, SQL_C_DOUBLE) == -std::numeric_limits<SQLDOUBLE>::infinity());
	CHECK(std::isnan(Number<SQLDOUBLE>(open, 6, SQL_C_DOUBLE)));
}

TEST_CASE("A cell read as a native C type arrives whole, so the next read of it reports no more data",
          "[fake_driver]") {
	FakeOdbcDriver fake;
	OpenStatement open;
	FetchDefaultRow(open);
	REQUIRE(Number<SQLBIGINT>(open, 1, SQL_C_SBIGINT) == 1);
	SQLBIGINT value = 0;

	CHECK(open.api.GetData(open.statement.value, 1, SQL_C_SBIGINT, &value, sizeof(value), nullptr) == SQL_NO_DATA);
}

// Measured on driver 2.3.0.0: a stored DOUBLE PRECISION "1.79769313486232e+308" read natively as
// +infinity and succeeded, as the driver range-checks only a character column's text (rsutil.c).
// The ODBC client's native binding cannot tell the overflow from a real infinity, which this pins.
TEST_CASE("An overflowing DOUBLE PRECISION cell reads natively as infinity", "[fake_driver]") {
	FakeOdbcDriver fake;
	OpenStatement open;
	fake.AddResult("SELECT d",
	               {Rows({Column("d", "DOUBLE PRECISION", SQL_DOUBLE, 15, 0)}, {{"1.79769313486232e+308"}})});
	PrepareAndExecute(open, "SELECT d");
	REQUIRE(open.api.Fetch(open.statement.value) == SQL_SUCCESS);

	CHECK(Number<SQLDOUBLE>(open, 1, SQL_C_DOUBLE) == std::numeric_limits<SQLDOUBLE>::infinity());
}

// Measured on driver 2.3.0.0 with extra_float_digits 2: stored REALs "0.1" and "3.4028235e+38" read
// natively as 0.10000000149011612 and 3.4028234663852886e+38, float precision widened to double, as
// the driver's (float)atof gives (rsutil.c getRsVal, convertSQLDataToCData).
TEST_CASE("A REAL cell reads natively at float precision, as measured on 2.3.0.0", "[fake_driver]") {
	FakeOdbcDriver fake;
	OpenStatement open;
	fake.AddResult("SELECT r, r_max",
	               {Rows({Column("r", "REAL", SQL_REAL, 7, 0), Column("r_max", "REAL", SQL_REAL, 7, 0)},
	                     {{"0.1", "3.4028235e+38"}})});
	PrepareAndExecute(open, "SELECT r, r_max");
	REQUIRE(open.api.Fetch(open.statement.value) == SQL_SUCCESS);

	CHECK(Number<SQLDOUBLE>(open, 1, SQL_C_DOUBLE) == static_cast<double>(0.1f));
	CHECK(Number<SQLDOUBLE>(open, 2, SQL_C_DOUBLE) == static_cast<double>(std::numeric_limits<float>::max()));
}

// Each conversion of a character column runs the driver's format check first, which refuses text
// that is no number with one text whatever the target type; the driver names the target type only in
// a double's range failure (public source v2.3.0, rsutil.c).
TEST_CASE("SQLGetData fails 22018 for a cell that is no number at all", "[fake_driver]") {
	FakeOdbcDriver fake;
	OpenStatement open;
	FetchTextRow(fake, open, "SELECT carriers", {"12x", "true", "12x"});

	CHECK(ReadFailure<SQLBIGINT>(open, 1, SQL_C_SBIGINT) == "22018");
	CHECK(DiagRecord(open.api, SQL_HANDLE_STMT, open.statement.value, 1) == "22018 0 Invalid numeric format");
	CHECK(ReadFailure<SQLCHAR>(open, 2, SQL_C_BIT) == "22018");
	CHECK(DiagRecord(open.api, SQL_HANDLE_STMT, open.statement.value, 1) == "22018 0 Invalid numeric format");
	CHECK(ReadFailure<SQLDOUBLE>(open, 3, SQL_C_DOUBLE) == "22018");
	CHECK(DiagRecord(open.api, SQL_HANDLE_STMT, open.statement.value, 1) == "22018 0 Invalid numeric format");
}

TEST_CASE("SQLGetData fails 22018 for an empty cell read as a number", "[fake_driver]") {
	FakeOdbcDriver fake;
	OpenStatement open;
	FetchTextRow(fake, open, "SELECT empty", {"", ""});

	CHECK(ReadFailure<SQLBIGINT>(open, 1, SQL_C_SBIGINT) == "22018");
	CHECK(ReadFailure<SQLDOUBLE>(open, 2, SQL_C_DOUBLE) == "22018");
}

// The server sends no such text, and the fake refuses it where the driver's atof reads 0 (rsutil.c
// getRsVal).
TEST_CASE("SQLGetData fails 22018 for a float column's cell that is no number", "[fake_driver]") {
	FakeOdbcDriver fake;
	OpenStatement open;
	fake.AddResult("SELECT d, r",
	               {Rows({Column("d", "DOUBLE PRECISION", SQL_DOUBLE, 15, 0), Column("r", "REAL", SQL_REAL, 7, 0)},
	                     {{"x", "x"}})});
	PrepareAndExecute(open, "SELECT d, r");
	REQUIRE(open.api.Fetch(open.statement.value) == SQL_SUCCESS);

	CHECK(ReadFailure<SQLDOUBLE>(open, 1, SQL_C_DOUBLE) == "22018");
	CHECK(ReadFailure<SQLDOUBLE>(open, 2, SQL_C_DOUBLE) == "22018");
}

// A character column is range-checked on its way out: 22003 for a number the C type cannot hold,
// 22018 for text that is no number (rsutil.c prepareStringForNumericConversion). The fake departs
// for a fraction or exponent, which the driver truncates or applies (rsutil.c parseAndBuildInteger).
TEST_CASE("SQLGetData fails 22003 for a number a character column's C type cannot hold", "[fake_driver]") {
	FakeOdbcDriver fake;
	OpenStatement open;
	FetchTextRow(fake, open, "SELECT carriers", {"2", "9223372036854775808", "1e400", "1.7", "1.7", "NaN"});

	CHECK(ReadFailure<SQLCHAR>(open, 1, SQL_C_BIT) == "22003");       // neither 0 nor 1
	CHECK(ReadFailure<SQLBIGINT>(open, 2, SQL_C_SBIGINT) == "22003"); // beyond BIGINT
	CHECK(DiagRecord(open.api, SQL_HANDLE_STMT, open.statement.value, 1) == "22003 0 Numeric value out of range");
	CHECK(ReadFailure<SQLDOUBLE>(open, 3, SQL_C_DOUBLE) == "22003"); // beyond DOUBLE
	CHECK(DiagRecord(open.api, SQL_HANDLE_STMT, open.statement.value, 1) ==
	      "22003 0 Numeric value out of range for DOUBLE");
	CHECK(ReadFailure<SQLCHAR>(open, 4, SQL_C_BIT) == "22003");       // a fraction, not 0 or 1
	CHECK(ReadFailure<SQLBIGINT>(open, 5, SQL_C_SBIGINT) == "22003"); // a fraction the driver truncates
	CHECK(ReadFailure<SQLBIGINT>(open, 6, SQL_C_SBIGINT) == "22003"); // no integer holds NaN
}

// strtod reports ERANGE for an underflow as well as an overflow, and only the overflow fails: the
// fake reads a text below DOUBLE's range as 0, where the driver's strtold posts 22003 for one below
// long double's, as this is (rsutil.c convertStringNumericToFloatCType).
TEST_CASE("A character cell below DOUBLE's range reads as SQL_C_DOUBLE 0", "[fake_driver]") {
	FakeOdbcDriver fake;
	OpenStatement open;
	FetchTextRow(fake, open, "SELECT v", {"1e-5000"});

	CHECK(Number<SQLDOUBLE>(open, 1, SQL_C_DOUBLE) == 0);
}

// The driver marks a column read at its error label, so its next SQLGetData of that column reports
// no more data whatever the first read failed with (public source v2.3.0, rsresult.cpp, iPrevhCol).
TEST_CASE("A column whose SQLGetData failed reports no more data", "[fake_driver]") {
	FakeOdbcDriver fake;
	OpenStatement open;
	FetchTextRow(fake, open, "SELECT v", {"1e400"});
	REQUIRE(ReadFailure<SQLDOUBLE>(open, 1, SQL_C_DOUBLE) == "22003");
	char value[64] = {};
	SQLLEN length = 0;

	CHECK(open.api.GetData(open.statement.value, 1, SQL_C_CHAR, value, sizeof(value), &length) == SQL_NO_DATA);
}

// The driver writes the cell's own length to the indicator before it converts, so a failed
// conversion leaves that length rather than the zero it started from (public source v2.3.0,
// rsresult.cpp, iDataLen).
TEST_CASE("A failed conversion leaves the cell's length in the indicator", "[fake_driver]") {
	FakeOdbcDriver fake;
	OpenStatement open;
	FetchTextRow(fake, open, "SELECT v", {"1e400"});
	SQLDOUBLE value = 0;
	SQLLEN length = 0;

	CHECK(open.api.GetData(open.statement.value, 1, SQL_C_DOUBLE, &value, sizeof(value), &length) == SQL_ERROR);

	CHECK(StatementSqlstate(open) == "22003");
	CHECK(length == 5);
}

// A NULL cell is done with once its read fails for want of an indicator: the driver marks the column
// read at its error label too (public source v2.3.0, rsresult.cpp, iPrevhCol at the error label).
TEST_CASE("A NULL cell read without an indicator reports no more data next", "[fake_driver]") {
	FakeOdbcDriver fake;
	OpenStatement open;
	auto result = Rows({Varchar("a")}, {{""}});
	result.nulls = {{true}};
	fake.AddResult("SELECT NULL", {result});
	PrepareAndExecute(open, "SELECT NULL");
	REQUIRE(open.api.Fetch(open.statement.value) == SQL_SUCCESS);
	char value[8] = {};
	REQUIRE(open.api.GetData(open.statement.value, 1, SQL_C_CHAR, value, sizeof(value), nullptr) == SQL_ERROR);
	REQUIRE(StatementSqlstate(open) == "22002");
	SQLLEN length = 0;

	CHECK(open.api.GetData(open.statement.value, 1, SQL_C_CHAR, value, sizeof(value), &length) == SQL_NO_DATA);
}

TEST_CASE("SQLGetData refuses a C type the fake does not convert to", "[fake_driver]") {
	FakeOdbcDriver fake;
	OpenStatement open;
	FetchDefaultRow(open);

	CHECK(ReadFailure<SQL_DATE_STRUCT>(open, 1, SQL_C_TYPE_DATE) == "HYC00");
	CHECK(DiagRecord(open.api, SQL_HANDLE_STMT, open.statement.value, 1) ==
	      "HYC00 0 the fake driver returns a cell only as SQL_C_CHAR, SQL_C_BINARY, SQL_C_BIT, SQL_C_SBIGINT or "
	      "SQL_C_DOUBLE; read it as one of those");
}

TEST_CASE("SQLGetData reports a NULL cell as SQL_NULL_DATA in a native C type too", "[fake_driver]") {
	FakeOdbcDriver fake;
	OpenStatement open;
	auto result = Rows({Varchar("i")}, {{""}});
	result.nulls = {{true}};
	fake.AddResult("SELECT NULL", {result});
	PrepareAndExecute(open, "SELECT NULL");
	REQUIRE(open.api.Fetch(open.statement.value) == SQL_SUCCESS);
	SQLBIGINT value = 7;
	SQLLEN length = 0;

	REQUIRE(open.api.GetData(open.statement.value, 1, SQL_C_SBIGINT, &value, sizeof(value), &length) == SQL_SUCCESS);

	CHECK(length == SQL_NULL_DATA);
}

// The driver checks the buffer before anything else, HY090 for a negative length and HY009 for a
// null target, so no caller can probe a length with no buffer; it zeroes the indicator before either
// check, so a caller reading it after the failure sees 0 (rsresult.cpp RS_SQLGetData).
TEST_CASE("SQLGetData fails HY090 for a negative buffer length and zeroes the indicator", "[fake_driver]") {
	FakeOdbcDriver fake;
	OpenStatement open;
	FetchTextRow(fake, open, "SELECT v", {"abcdefg"});
	char value[8] = {};
	SQLLEN length = -7;

	CHECK(open.api.GetData(open.statement.value, 1, SQL_C_CHAR, value, -1, &length) == SQL_ERROR);

	CHECK(DiagRecord(open.api, SQL_HANDLE_STMT, open.statement.value, 1) == "HY090 0 Invalid string or buffer length");
	CHECK(length == 0);
}

TEST_CASE("SQLGetData fails HY009 for a null target buffer and zeroes the indicator", "[fake_driver]") {
	FakeOdbcDriver fake;
	OpenStatement open;
	FetchTextRow(fake, open, "SELECT v", {"abcdefg"});
	SQLLEN length = -7;

	CHECK(open.api.GetData(open.statement.value, 1, SQL_C_CHAR, nullptr, 0, &length) == SQL_ERROR);

	CHECK(DiagRecord(open.api, SQL_HANDLE_STMT, open.statement.value, 1) == "HY009 0 Invalid use of null pointer");
	CHECK(length == 0);
}

// The driver answers the bookmark column with 07009, bookmarks being unsupported (public source
// v2.3.0, rsresult.cpp).
TEST_CASE("SQLGetData of column 0 fails as an unsupported bookmark", "[fake_driver]") {
	FakeOdbcDriver fake;
	OpenStatement open;
	FetchDefaultRow(open);
	char value[8] = {};
	SQLLEN length = 0;

	CHECK(open.api.GetData(open.statement.value, 0, SQL_C_CHAR, value, sizeof(value), &length) == SQL_ERROR);

	CHECK(DiagRecord(open.api, SQL_HANDLE_STMT, open.statement.value, 1) ==
	      "07009 0 SQLGetData: Bookmark column is not supported");
}

TEST_CASE("Closing the cursor ends its rows, and executing again starts from the first", "[fake_driver]") {
	FakeOdbcDriver fake;
	OpenStatement open;
	fake.AddResult("SELECT a", {Rows({Varchar("a")}, {{"1"}, {"2"}})});
	PrepareAndExecute(open, "SELECT a");
	REQUIRE(open.api.Fetch(open.statement.value) == SQL_SUCCESS);
	REQUIRE(open.api.CloseCursor(open.statement.value) == SQL_SUCCESS);
	CHECK(open.api.Fetch(open.statement.value) == SQL_ERROR);
	CHECK(StatementSqlstate(open) == "HY010");

	REQUIRE(open.api.Execute(open.statement.value) == SQL_SUCCESS);

	REQUIRE(open.api.Fetch(open.statement.value) == SQL_SUCCESS);
	CHECK(Value(open, 1) == "1");
}

// SQLFreeStmt(SQL_CLOSE) leaves the script in place, so the statement can be executed again, which
// is what the driver's own does: it releases the results and keeps the prepared statement (public
// source v2.3.0, rsconnect.cpp, RS_SQLFreeStmt).
TEST_CASE("SQLFreeStmt(SQL_CLOSE) closes the cursor and keeps the statement prepared", "[fake_driver]") {
	FakeOdbcDriver fake;
	OpenStatement open;
	fake.AddResult("SELECT a", {Rows({Varchar("a")}, {{"1"}, {"2"}})});
	PrepareAndExecute(open, "SELECT a");
	REQUIRE(open.api.Fetch(open.statement.value) == SQL_SUCCESS);

	REQUIRE(open.api.FreeStmt(open.statement.value, SQL_CLOSE) == SQL_SUCCESS);

	CHECK(open.api.Fetch(open.statement.value) == SQL_ERROR);
	CHECK(StatementSqlstate(open) == "HY010");
	CHECK(open.api.Execute(open.statement.value) == SQL_SUCCESS);
	CHECK(open.api.Fetch(open.statement.value) == SQL_SUCCESS);
}

// The driver refuses SQLCloseCursor for a statement still allocated or already closed, and accepts
// SQLFreeStmt(SQL_CLOSE) in both (public source v2.3.0, rsprepare.cpp and rsconnect.cpp). The ODBC client's
// teardown reaches both calls.
TEST_CASE("SQLCloseCursor without a cursor to close fails with 24000, where SQLFreeStmt does not", "[fake_driver]") {
	FakeOdbcDriver fake;
	OpenStatement open;

	CHECK(open.api.CloseCursor(open.statement.value) == SQL_ERROR);
	CHECK(StatementSqlstate(open) == "24000");
	CHECK(open.api.FreeStmt(open.statement.value, SQL_CLOSE) == SQL_SUCCESS);

	PrepareAndExecute(open, "SELECT 1");
	REQUIRE(open.api.CloseCursor(open.statement.value) == SQL_SUCCESS);
	CHECK(open.api.CloseCursor(open.statement.value) == SQL_ERROR);
	CHECK(StatementSqlstate(open) == "24000");
}

// The fake leaves a statement never prepared as it was, where the driver marks it closed and its
// SQLFetch then posts HY010 (rsconnect.cpp RS_SQLFreeStmt).
TEST_CASE("SQLFreeStmt(SQL_CLOSE) leaves a statement never prepared as it was", "[fake_driver]") {
	FakeOdbcDriver fake;
	OpenStatement open;
	REQUIRE(open.api.FreeStmt(open.statement.value, SQL_CLOSE) == SQL_SUCCESS);

	CHECK(open.api.Fetch(open.statement.value) == SQL_ERROR);
	CHECK(StatementSqlstate(open) == "24000");
	CHECK(open.api.Execute(open.statement.value) == SQL_ERROR);
	CHECK(StatementSqlstate(open) == "HY010");
}

// The driver refuses only a statement still allocated or already closed, so one merely prepared is
// accepted (public source v2.3.0, rsprepare.cpp, SQLCloseCursor).
TEST_CASE("SQLCloseCursor accepts a statement that was only prepared", "[fake_driver]") {
	FakeOdbcDriver fake;
	OpenStatement open;
	REQUIRE(open.api.Prepare(open.statement.value, Text("SELECT 1"), SQL_NTS) == SQL_SUCCESS);

	CHECK(open.api.CloseCursor(open.statement.value) == SQL_SUCCESS);

	CHECK(DiagRecordCount(open.api, SQL_HANDLE_STMT, open.statement.value) == 0);
}

// A failed SQLPrepare leaves the statement as it was allocated, so the result it held before is
// gone, which the driver's own SQLPrepare does too by releasing the open result (public source
// v2.3.0, rsprepare.cpp, makeItReadyForNewQueryExecution).
TEST_CASE("SQLPrepare of text with no script leaves an executed statement with nothing to fetch", "[fake_driver]") {
	FakeOdbcDriver fake;
	OpenStatement open;
	PrepareAndExecute(open, "SELECT 1");
	REQUIRE(open.api.Prepare(open.statement.value, Text("SELECT 2"), SQL_NTS) == SQL_ERROR);

	CHECK(open.api.Fetch(open.statement.value) == SQL_ERROR);

	CHECK(DiagRecord(open.api, SQL_HANDLE_STMT, open.statement.value, 1) == "24000 0 Invalid cursor state");
}

// A scripted failure stands for one the server reported, which the driver reaches only after it has
// released the statement's results (rsexecute.cpp RS_SQLExecDirect), so a caller that missed the
// failure cannot read row 2 of the result it was re-running.
TEST_CASE("A scripted SQLExecute failure leaves the statement with nothing to fetch", "[fake_driver]") {
	FakeOdbcDriver fake;
	OpenStatement open;
	fake.AddResult("SELECT a FROM t", {Rows({Varchar("a")}, {{"first"}, {"second"}})});
	PrepareAndExecute(open, "SELECT a FROM t");
	REQUIRE(open.api.Fetch(open.statement.value) == SQL_SUCCESS);
	fake.FailCall("SQLExecute", 2, SQL_ERROR, {{"57014", 0, "Query cancelled"}});
	REQUIRE(open.api.Execute(open.statement.value) == SQL_ERROR);

	CHECK(open.api.Fetch(open.statement.value) == SQL_ERROR);

	CHECK(DiagRecord(open.api, SQL_HANDLE_STMT, open.statement.value, 1) == "24000 0 Invalid cursor state");
}

// The driver's SQLPrepare releases the prepare and the results before it prepares again
// (rsprepare.cpp RS_SQLPrepare), so a scripted failure leaves nothing to read either.
TEST_CASE("A scripted SQLPrepare failure leaves an executed statement with nothing to fetch", "[fake_driver]") {
	FakeOdbcDriver fake;
	OpenStatement open;
	fake.AddResult("SELECT a FROM t", {Rows({Varchar("a")}, {{"first"}, {"second"}})});
	PrepareAndExecute(open, "SELECT a FROM t");
	REQUIRE(open.api.Fetch(open.statement.value) == SQL_SUCCESS);
	fake.FailCall("SQLPrepare", 2, SQL_ERROR, {{"42601", 0, "syntax error"}});
	REQUIRE(open.api.Prepare(open.statement.value, Text("SELECT a FROM t"), SQL_NTS) == SQL_ERROR);

	CHECK(open.api.Fetch(open.statement.value) == SQL_ERROR);

	CHECK(DiagRecord(open.api, SQL_HANDLE_STMT, open.statement.value, 1) == "24000 0 Invalid cursor state");
}

// The driver releases the prepared statement too, so there is nothing left to run; the record is the
// fake's own stricter refusal of a statement never prepared, which Execute explains.
TEST_CASE("A scripted SQLPrepare failure leaves nothing to execute either", "[fake_driver]") {
	FakeOdbcDriver fake;
	OpenStatement open;
	fake.AddResult("SELECT a FROM t", {Rows({Varchar("a")}, {{"first"}, {"second"}})});
	PrepareAndExecute(open, "SELECT a FROM t");
	fake.FailCall("SQLPrepare", 2, SQL_ERROR, {{"42601", 0, "syntax error"}});
	REQUIRE(open.api.Prepare(open.statement.value, Text("SELECT a FROM t"), SQL_NTS) == SQL_ERROR);

	CHECK(open.api.Execute(open.statement.value) == SQL_ERROR);

	CHECK(DiagRecord(open.api, SQL_HANDLE_STMT, open.statement.value, 1) == "HY010 0 Function sequence error");
}

// The driver releases the previous results before it looks each marker's parameter up (rsexecute.cpp
// RS_SQLExecDirect, then rslibpq.c libpqExecuteDirectOrPreparedOnThread), so nothing is left to read.
TEST_CASE("A refused marker position leaves the statement with nothing to fetch", "[fake_driver]") {
	FakeOdbcDriver fake;
	OpenStatement open;
	fake.AddResult("SELECT ?, ?", {Rows({Varchar("a")}, {{"first"}, {"second"}})}, 2);
	REQUIRE(open.api.Prepare(open.statement.value, Text("SELECT ?, ?"), SQL_NTS) == SQL_SUCCESS);
	char value[] = "x";
	REQUIRE(BindNulTerminated(open, 1, value) == SQL_SUCCESS);
	REQUIRE(BindNulTerminated(open, 2, value) == SQL_SUCCESS);
	REQUIRE(open.api.Execute(open.statement.value) == SQL_SUCCESS);
	REQUIRE(open.api.Fetch(open.statement.value) == SQL_SUCCESS);
	REQUIRE(open.api.FreeStmt(open.statement.value, SQL_RESET_PARAMS) == SQL_SUCCESS);
	REQUIRE(BindNulTerminated(open, 1, value) == SQL_SUCCESS);
	REQUIRE(BindNulTerminated(open, 3, value) == SQL_SUCCESS);
	REQUIRE(open.api.Execute(open.statement.value) == SQL_ERROR);

	CHECK(open.api.Fetch(open.statement.value) == SQL_ERROR);

	CHECK(DiagRecord(open.api, SQL_HANDLE_STMT, open.statement.value, 1) == "24000 0 Invalid cursor state");
}

// The driver counts the bindings before it releases the results (rsexecute.cpp RS_SQLExecDirect),
// so that one refusal leaves the previous result where it was.
TEST_CASE("SQLExecute refusing too few bound parameters keeps the previous result readable", "[fake_driver]") {
	FakeOdbcDriver fake;
	OpenStatement open;
	fake.AddResult("SELECT ?, ?", {Rows({Varchar("a")}, {{"first"}, {"second"}})}, 2);
	REQUIRE(open.api.Prepare(open.statement.value, Text("SELECT ?, ?"), SQL_NTS) == SQL_SUCCESS);
	char value[] = "x";
	REQUIRE(BindNulTerminated(open, 1, value) == SQL_SUCCESS);
	REQUIRE(BindNulTerminated(open, 2, value) == SQL_SUCCESS);
	REQUIRE(open.api.Execute(open.statement.value) == SQL_SUCCESS);
	REQUIRE(open.api.Fetch(open.statement.value) == SQL_SUCCESS);
	REQUIRE(open.api.FreeStmt(open.statement.value, SQL_RESET_PARAMS) == SQL_SUCCESS);
	REQUIRE(BindNulTerminated(open, 1, value) == SQL_SUCCESS);
	REQUIRE(open.api.Execute(open.statement.value) == SQL_ERROR);

	CHECK(open.api.Fetch(open.statement.value) == SQL_SUCCESS);

	CHECK(Value(open, 1) == "second");
}

TEST_CASE("SQLFreeStmt accepts SQL_UNBIND and SQL_RESET_PARAMS", "[fake_driver]") {
	FakeOdbcDriver fake;
	OpenStatement open;

	CHECK(open.api.FreeStmt(open.statement.value, SQL_UNBIND) == SQL_SUCCESS);
	CHECK(open.api.FreeStmt(open.statement.value, SQL_RESET_PARAMS) == SQL_SUCCESS);

	CHECK(DiagRecordCount(open.api, SQL_HANDLE_STMT, open.statement.value) == 0);
}

// The driver ignores an option it does not know and frees the statement for SQL_DROP (public source
// v2.3.0, rsconnect.cpp, RS_SQLFreeStmt); the fake refuses both, so a test that needs either fails
// visibly rather than diverging quietly, and the statement is still there to free.
TEST_CASE("SQLFreeStmt refuses an option the fake does not model, SQL_DROP included", "[fake_driver]") {
	FakeOdbcDriver fake;
	OpenStatement open;

	CHECK(open.api.FreeStmt(open.statement.value, SQL_DROP) == SQL_ERROR);

	CHECK(DiagRecord(open.api, SQL_HANDLE_STMT, open.statement.value, 1) ==
	      "HY092 0 Invalid attribute/option identifier");
	CHECK(fake.AllocatedHandles() == "environment, connection, statement");
}

// FailCall and BlockCall find their script by the name the trampoline logs, and SQLFreeStmt's detail
// is the option it was given, so a mistyped name or a dropped detail would leave a test passing
// without what it scripted.
TEST_CASE("The call log names each statement call and the option SQLFreeStmt was given", "[fake_driver]") {
	FakeOdbcDriver fake;
	OpenStatement open;
	fake.AddResult("SELECT a", {Rows({Varchar("a")}, {{"1"}})}, 1);
	REQUIRE(open.api.Prepare(open.statement.value, Text("SELECT a"), SQL_NTS) == SQL_SUCCESS);
	const auto first_call = fake.Calls().size();
	SQLSMALLINT count = 0;
	SQLLEN numeric = 0;
	char value[] = "x";
	open.api.NumParams(open.statement.value, &count);
	open.api.ColAttribute(open.statement.value, 1, SQL_DESC_CONCISE_TYPE, nullptr, 0, nullptr, &numeric);
	BindNulTerminated(open, 1, value);
	open.api.Execute(open.statement.value);
	open.api.MoreResults(open.statement.value);
	open.api.CloseCursor(open.statement.value);
	open.api.FreeStmt(open.statement.value, SQL_CLOSE);

	CHECK(CallsSince(fake, first_call) == std::vector<std::pair<std::string, std::string>> {{"SQLNumParams", ""},
	                                                                                        {"SQLColAttribute", ""},
	                                                                                        {"SQLBindParameter", ""},
	                                                                                        {"SQLExecute", "1=x"},
	                                                                                        {"SQLMoreResults", ""},
	                                                                                        {"SQLCloseCursor", ""},
	                                                                                        {"SQLFreeStmt", "0"}});
}

// The ODBC client's producer thread fetches a result its caller's thread executed, which the one mutex makes
// safe.
TEST_CASE("A statement executed on one thread can be fetched on another", "[fake_driver]") {
	FakeOdbcDriver fake;
	OpenStatement open;
	fake.AddResult("SELECT a", {Rows({Varchar("a")}, {{"1"}, {"2"}})});
	PrepareAndExecute(open, "SELECT a");

	// No Catch assertion runs off the test's own thread, so a length outside the buffer ends the loop
	// and the values it collected fail the check below.
	auto fetched = std::async(std::launch::async, [&]() -> std::vector<std::string> {
		std::vector<std::string> values;
		char value[8] = {};
		SQLLEN length = 0;
		while (open.api.Fetch(open.statement.value) == SQL_SUCCESS &&
		       open.api.GetData(open.statement.value, 1, SQL_C_CHAR, value, sizeof(value), &length) == SQL_SUCCESS) {
			if (length < 0 || static_cast<size_t>(length) >= sizeof(value)) {
				return values;
			}
			values.emplace_back(value, static_cast<size_t>(length));
		}
		return values;
	});

	CHECK(fetched.get() == std::vector<std::string> {"1", "2"});
}

namespace {

void SetAttribute(const OpenStatement &open, SQLINTEGER attribute, SQLPOINTER value) {
	REQUIRE(open.api.SetStmtAttr(open.statement.value, attribute, value, 0) == SQL_SUCCESS);
}

//! `size` as SQLSetStmtAttr takes an integer attribute, in the pointer itself.
SQLPOINTER AsAttribute(SQLULEN size) {
	return reinterpret_cast<SQLPOINTER>(static_cast<uintptr_t>(size));
}

//! A row status no fetch writes, so an element left unwritten shows. Not 0xffff, the value the fake
//! uses inside for a stopped fetch, so writing that would show too.
constexpr SQLUSMALLINT UNWRITTEN = 0x5a5a;

//! A row array of `size` rows, with the rows-fetched and row-status buffers the statement is
//! pointed at. The status starts UNWRITTEN, and has one element more, which no fetch may write.
struct RowArray {
	RowArray(const OpenStatement &open, SQLULEN size) : statuses(size + 1, UNWRITTEN) {
		SetAttribute(open, SQL_ATTR_ROW_ARRAY_SIZE, AsAttribute(size));
		SetAttribute(open, SQL_ATTR_ROWS_FETCHED_PTR, &fetched);
		SetAttribute(open, SQL_ATTR_ROW_STATUS_PTR, statuses.data());
	}
	RowArray(const RowArray &) = delete;
	RowArray &operator=(const RowArray &) = delete;

	//! Each row's status, without the element past the array.
	std::vector<SQLUSMALLINT> Status() const {
		return std::vector<SQLUSMALLINT>(statuses.begin(), statuses.end() - 1);
	}
	//! Whether the element past the array still holds its first value.
	bool PastEndUntouched() const {
		return statuses.back() == UNWRITTEN;
	}

	SQLULEN fetched = 99;
	std::vector<SQLUSMALLINT> statuses;
};

//! Column `number` bound column-wise as SQL_C_CHAR, or SQL_C_BINARY, for `rows` rows of `width`
//! bytes each. The bytes and indicators start at values no fetch writes, so an element left unwritten
//! shows.
struct ByteColumn {
	ByteColumn(const OpenStatement &open, SQLUSMALLINT number, size_t rows, SQLLEN width_p,
	           SQLSMALLINT c_type = SQL_C_CHAR)
	    : width(width_p), bytes(rows * static_cast<size_t>(width_p), '#'), indicators(rows, -99) {
		REQUIRE(open.api.BindCol(open.statement.value, number, c_type, bytes.data(), width, indicators.data()) ==
		        SQL_SUCCESS);
	}
	ByteColumn(const ByteColumn &) = delete;
	ByteColumn &operator=(const ByteColumn &) = delete;

	//! Element `row` up to the NUL the fetch wrote inside it.
	std::string Element(size_t row) const {
		const auto *start = bytes.data() + row * static_cast<size_t>(width);
		const auto *end = static_cast<const char *>(memchr(start, '\0', static_cast<size_t>(width)));
		REQUIRE(end);
		return std::string(start, end);
	}
	//! Whether element `row` still holds the bytes it started with.
	bool Untouched(size_t row) const {
		const auto *start = bytes.data() + row * static_cast<size_t>(width);
		return std::string(start, static_cast<size_t>(width)) == std::string(static_cast<size_t>(width), '#');
	}

	SQLLEN width;
	std::vector<char> bytes;
	std::vector<SQLLEN> indicators;
};

//! Executes "SELECT a" over the one `column`, holding `cells`, one row each.
void ExecuteColumn(FakeOdbcDriver &fake, const OpenStatement &open, FakeColumn column,
                   const std::vector<std::string> &cells) {
	std::vector<std::vector<std::string>> rows;
	for (const auto &cell : cells) {
		rows.push_back({cell});
	}
	fake.AddResult("SELECT a", {Rows({std::move(column)}, rows)});
	PrepareAndExecute(open, "SELECT a");
}

//! The three rows "a1" to "a3" of one VARCHAR column, executed as "SELECT a".
void ExecuteThreeRows(FakeOdbcDriver &fake, const OpenStatement &open) {
	ExecuteColumn(fake, open, Varchar("a"), {"a1", "a2", "a3"});
}

//! Executes "SELECT a, b" over `result`, scripted for it.
void ExecuteTwoColumns(FakeOdbcDriver &fake, const OpenStatement &open, FakeResultSet result) {
	fake.AddResult("SELECT a, b", {std::move(result)});
	PrepareAndExecute(open, "SELECT a, b");
}

SQLINTEGER StatementRecordCount(const OpenStatement &open) {
	return DiagRecordCount(open.api, SQL_HANDLE_STMT, open.statement.value);
}

std::string StatementRecord(const OpenStatement &open, SQLSMALLINT number) {
	return DiagRecord(open.api, SQL_HANDLE_STMT, open.statement.value, number);
}

} // namespace

TEST_CASE("SQLFetch fills one array element per row of each bound column, up to the row-array size", "[fake_driver]") {
	FakeOdbcDriver fake;
	OpenStatement open;
	ExecuteThreeRows(fake, open);
	RowArray rows(open, 2);
	ByteColumn column(open, 1, 2, 8);

	REQUIRE(open.api.Fetch(open.statement.value) == SQL_SUCCESS);

	CHECK(rows.fetched == 2);
	CHECK(rows.Status() == std::vector<SQLUSMALLINT> {SQL_ROW_SUCCESS, SQL_ROW_SUCCESS});
	CHECK(rows.PastEndUntouched());
	CHECK(column.Element(0) == "a1");
	CHECK(column.Element(1) == "a2");
	CHECK(column.indicators == std::vector<SQLLEN> {2, 2});
}

TEST_CASE("The last rowset holds the rows left, with SQL_ROW_NOROW past them", "[fake_driver]") {
	FakeOdbcDriver fake;
	OpenStatement open;
	ExecuteThreeRows(fake, open);
	RowArray rows(open, 2);
	ByteColumn column(open, 1, 2, 8);
	REQUIRE(open.api.Fetch(open.statement.value) == SQL_SUCCESS);

	REQUIRE(open.api.Fetch(open.statement.value) == SQL_SUCCESS);

	CHECK(rows.fetched == 1);
	CHECK(rows.Status() == std::vector<SQLUSMALLINT> {SQL_ROW_SUCCESS, SQL_ROW_NOROW});
	CHECK(column.Element(0) == "a3");
}

// The driver writes SQL_ROW_NOROW into the first element past the rows and stops there, so the
// elements after it keep what they held (rsresult.cpp RS_SQLFetchScroll).
TEST_CASE("A row-array fetch past the last row reports no data, with SQL_ROW_NOROW in the first element only",
          "[fake_driver]") {
	FakeOdbcDriver fake;
	OpenStatement open;
	ExecuteThreeRows(fake, open);
	RowArray rows(open, 3);
	REQUIRE(open.api.Fetch(open.statement.value) == SQL_SUCCESS);

	CHECK(open.api.Fetch(open.statement.value) == SQL_NO_DATA);

	CHECK(rows.fetched == 0);
	CHECK(rows.Status() == std::vector<SQLUSMALLINT> {SQL_ROW_NOROW, SQL_ROW_SUCCESS, SQL_ROW_SUCCESS});
}

// The cursor stands past the last row once a fetch reports no data, so a missed fetch fails
// visibly rather than reading the last row again.
TEST_CASE("SQLGetData after a row-array fetch that reported no data fails with 24000", "[fake_driver]") {
	FakeOdbcDriver fake;
	OpenStatement open;
	ExecuteThreeRows(fake, open);
	RowArray rows(open, 3);
	REQUIRE(open.api.Fetch(open.statement.value) == SQL_SUCCESS);
	REQUIRE(open.api.Fetch(open.statement.value) == SQL_NO_DATA);
	SetAttribute(open, SQL_ATTR_ROW_ARRAY_SIZE, AsAttribute(1));
	char value[8] = {};
	SQLLEN length = 0;

	CHECK(open.api.GetData(open.statement.value, 1, SQL_C_CHAR, value, sizeof(value), &length) == SQL_ERROR);

	CHECK(StatementSqlstate(open) == "24000");
}

// ODBC returns SQL_SUCCESS_WITH_INFO when some rows of a rowset fail, and fetches the rows after.
// The driver reads each bound cell with its own RS_SQLGetData call, which zeroes the indicator and
// clears the records first, so a later row's read leaves none of the failed row's.
TEST_CASE("A row in error in a rowset gives SQL_SUCCESS_WITH_INFO, its own value unwritten", "[fake_driver]") {
	FakeOdbcDriver fake;
	OpenStatement open;
	auto result = Rows({Varchar("a")}, {{"1"}, {"2"}, {"3"}});
	result.row_status = {SQL_ROW_SUCCESS, SQL_ROW_ERROR, SQL_ROW_SUCCESS};
	result.row_diags[1] = {{"22003", 0, "numeric value out of range"}};
	fake.AddResult("SELECT a", {result});
	PrepareAndExecute(open, "SELECT a");
	RowArray rows(open, 3);
	ByteColumn column(open, 1, 3, 8);

	CHECK(open.api.Fetch(open.statement.value) == SQL_SUCCESS_WITH_INFO);

	CHECK(rows.Status() == std::vector<SQLUSMALLINT> {SQL_ROW_SUCCESS, SQL_ROW_ERROR, SQL_ROW_SUCCESS});
	CHECK(column.Element(0) == "1");
	CHECK(column.Untouched(1));
	CHECK(column.indicators[1] == 0);
	CHECK(column.Element(2) == "3");
	CHECK(StatementRecordCount(open) == 0);
}

TEST_CASE("A rowset whose every row is in error fails, keeping the last row's records", "[fake_driver]") {
	FakeOdbcDriver fake;
	OpenStatement open;
	auto result = Rows({Varchar("a")}, {{"1"}, {"2"}});
	result.row_status = {SQL_ROW_ERROR, SQL_ROW_ERROR};
	result.row_diags[0] = {{"22003", 0, "first"}};
	result.row_diags[1] = {{"22012", 0, "second"}};
	fake.AddResult("SELECT a", {result});
	PrepareAndExecute(open, "SELECT a");
	RowArray rows(open, 2);

	CHECK(open.api.Fetch(open.statement.value) == SQL_ERROR);

	CHECK(rows.Status() == std::vector<SQLUSMALLINT> {SQL_ROW_ERROR, SQL_ROW_ERROR});
	CHECK(StatementRecordCount(open) == 1);
	CHECK(StatementRecord(open, 1) == "22012 0 second");
}

// ODBC's truncation of character data: the bytes that fit, NUL-terminated, the cell's full length
// in the indicator, and SQL_SUCCESS_WITH_INFO with 01004.
TEST_CASE("A bound cell one byte longer than its buffer is cut as ODBC specifies, where one that fits is not",
          "[fake_driver]") {
	FakeOdbcDriver fake;
	OpenStatement open;
	ExecuteColumn(fake, open, Varchar("a"), {"abc", "abcd"});
	RowArray rows(open, 2);
	ByteColumn column(open, 1, 2, 4);

	CHECK(open.api.Fetch(open.statement.value) == SQL_SUCCESS_WITH_INFO);

	CHECK(rows.Status() == std::vector<SQLUSMALLINT> {SQL_ROW_SUCCESS, SQL_ROW_SUCCESS_WITH_INFO});
	CHECK(column.Element(0) == "abc");
	CHECK(column.Element(1) == "abc");
	CHECK(column.indicators == std::vector<SQLLEN> {3, 4});
	CHECK(StatementRecordCount(open) == 1);
	CHECK(StatementRecord(open, 1) == "01004 0 String data, right truncated");
}

// The ODBC client cannot count on a 01004 record for every cut cell: only the last cell's read keeps its
// records, so the length indicator is what shows a cut.
TEST_CASE("A cut cell's 01004 is gone once a later row's cell is read", "[fake_driver]") {
	FakeOdbcDriver fake;
	OpenStatement open;
	ExecuteColumn(fake, open, Varchar("a"), {"abcd", "abc"});
	RowArray rows(open, 2);
	ByteColumn column(open, 1, 2, 4);

	CHECK(open.api.Fetch(open.statement.value) == SQL_SUCCESS_WITH_INFO);

	CHECK(rows.Status() == std::vector<SQLUSMALLINT> {SQL_ROW_SUCCESS_WITH_INFO, SQL_ROW_SUCCESS});
	CHECK(column.indicators == std::vector<SQLLEN> {4, 3});
	CHECK(StatementRecordCount(open) == 0);
}

TEST_CASE("A cut cell's 01004 is gone once a later column's cell is read", "[fake_driver]") {
	FakeOdbcDriver fake;
	OpenStatement open;
	ExecuteTwoColumns(fake, open, Rows({Varchar("a"), Varchar("b")}, {{"abcd", "b"}}));
	RowArray rows(open, 1);
	ByteColumn first(open, 1, 1, 4);
	ByteColumn second(open, 2, 1, 4);

	CHECK(open.api.Fetch(open.statement.value) == SQL_SUCCESS_WITH_INFO);

	CHECK(rows.Status() == std::vector<SQLUSMALLINT> {SQL_ROW_SUCCESS_WITH_INFO});
	CHECK(first.indicators[0] == 4);
	CHECK(second.Element(0) == "b");
	CHECK(StatementRecordCount(open) == 0);
}

TEST_CASE("A 4-byte UTF-8 character cut at a bound buffer's end leaves its first bytes, NUL-terminated",
          "[fake_driver]") {
	FakeOdbcDriver fake;
	OpenStatement open;
	ExecuteColumn(fake, open, Varchar("a"), {"ab\xF0\x9F\x98\x80"});
	RowArray rows(open, 1);
	ByteColumn column(open, 1, 1, 5);

	CHECK(open.api.Fetch(open.statement.value) == SQL_SUCCESS_WITH_INFO);

	CHECK(column.Element(0) == "ab\xF0\x9F");
	CHECK(column.indicators[0] == 6);
	CHECK(StatementRecord(open, 1) == "01004 0 String data, right truncated");
}

// The driver posts no record for a cut binary copy, so neither does a bound VARBYTE cell. Here the
// fake follows the driver rather than ODBC, which asks for 01004.
TEST_CASE("A cut SQL_C_BINARY bound cell of a VARBYTE column posts no record", "[fake_driver]") {
	FakeOdbcDriver fake;
	OpenStatement open;
	ExecuteColumn(fake, open, Column("v", "VARBYTE", SQL_LONGVARBINARY, 64000, 0),
	              {std::string("\x00\xff\x10\x01\x02", 5)});
	RowArray rows(open, 1);
	ByteColumn column(open, 1, 1, 4, SQL_C_BINARY);

	CHECK(open.api.Fetch(open.statement.value) == SQL_SUCCESS_WITH_INFO);

	CHECK(rows.Status() == std::vector<SQLUSMALLINT> {SQL_ROW_SUCCESS_WITH_INFO});
	CHECK(std::string(column.bytes.data(), 4) == std::string("\x00\xff\x10\x01", 4));
	CHECK(column.indicators[0] == 5);
	CHECK(StatementRecordCount(open) == 0);
}

TEST_CASE("A cut bound cell of a column scripted to report no total gives SQL_NO_TOTAL", "[fake_driver]") {
	FakeOdbcDriver fake;
	OpenStatement open;
	auto varchar = Varchar("a");
	varchar.report_no_total = true;
	ExecuteColumn(fake, open, varchar, {"abcdef", "ab"});
	RowArray rows(open, 2);
	ByteColumn column(open, 1, 2, 4);

	CHECK(open.api.Fetch(open.statement.value) == SQL_SUCCESS_WITH_INFO);

	CHECK(column.Element(0) == "abc");
	CHECK(column.indicators == std::vector<SQLLEN> {SQL_NO_TOTAL, 2});
}

TEST_CASE("A NULL bound cell reports SQL_NULL_DATA, with the driver's NUL in its element", "[fake_driver]") {
	FakeOdbcDriver fake;
	OpenStatement open;
	auto result = Rows({Varchar("a")}, {{"x"}, {"y"}});
	result.nulls = {{true}, {false}};
	fake.AddResult("SELECT a", {result});
	PrepareAndExecute(open, "SELECT a");
	RowArray rows(open, 2);
	ByteColumn column(open, 1, 2, 8);

	CHECK(open.api.Fetch(open.statement.value) == SQL_SUCCESS);

	CHECK(column.indicators == std::vector<SQLLEN> {SQL_NULL_DATA, 1});
	CHECK(column.Element(0) == "");
	CHECK(column.Element(1) == "y");
}

TEST_CASE("A NULL bound cell with no indicator fails its row with 22002", "[fake_driver]") {
	FakeOdbcDriver fake;
	OpenStatement open;
	auto result = Rows({Varchar("a")}, {{"x"}});
	result.nulls = {{true}};
	fake.AddResult("SELECT a", {result});
	PrepareAndExecute(open, "SELECT a");
	RowArray rows(open, 1);
	char value[8] = {};
	REQUIRE(open.api.BindCol(open.statement.value, 1, SQL_C_CHAR, value, sizeof(value), nullptr) == SQL_SUCCESS);

	CHECK(open.api.Fetch(open.statement.value) == SQL_ERROR);

	CHECK(rows.Status() == std::vector<SQLUSMALLINT> {SQL_ROW_ERROR});
	CHECK(StatementSqlstate(open) == "22002");
}

// With no indicator bound, each element still lands at its own address, the bind offset included.
TEST_CASE("A column bound with no indicator fills each element at its offset address", "[fake_driver]") {
	FakeOdbcDriver fake;
	OpenStatement open;
	ExecuteThreeRows(fake, open);
	RowArray rows(open, 2);
	std::vector<char> bytes(24, '#');
	SQLULEN offset = 8;
	SetAttribute(open, SQL_ATTR_ROW_BIND_OFFSET_PTR, &offset);
	REQUIRE(open.api.BindCol(open.statement.value, 1, SQL_C_CHAR, bytes.data(), 8, nullptr) == SQL_SUCCESS);

	REQUIRE(open.api.Fetch(open.statement.value) == SQL_SUCCESS);

	CHECK(std::string(bytes.data(), 8) == std::string(8, '#'));
	CHECK(std::string(bytes.data() + 8) == "a1");
	CHECK(std::string(bytes.data() + 16) == "a2");
}

// A fixed-size C type's array element is that type's width, whatever BufferLength the caller gave.
TEST_CASE("A column bound as a native C type gets one value per element at that type's width", "[fake_driver]") {
	FakeOdbcDriver fake;
	OpenStatement open;
	fake.AddResult("SELECT a, b, c",
	               {Rows({Column("a", "BOOL", SQL_BIT, 1, 0), Column("b", "BIGINT", SQL_BIGINT, 19, 0),
	                      Column("c", "DOUBLE PRECISION", SQL_DOUBLE, 15, 0)},
	                     {{"1", "-9223372036854775808", "0.5"}, {"0", "9223372036854775807", "-2.25"}})});
	PrepareAndExecute(open, "SELECT a, b, c");
	RowArray rows(open, 2);
	SQLCHAR bits[2] = {9, 9};
	SQLBIGINT integers[2] = {};
	SQLDOUBLE doubles[2] = {};
	std::vector<SQLLEN> bit_lengths(2, -99);
	std::vector<SQLLEN> integer_lengths(2, -99);
	std::vector<SQLLEN> double_lengths(2, -99);
	REQUIRE(open.api.BindCol(open.statement.value, 1, SQL_C_BIT, bits, 0, bit_lengths.data()) == SQL_SUCCESS);
	REQUIRE(open.api.BindCol(open.statement.value, 2, SQL_C_SBIGINT, integers, 0, integer_lengths.data()) ==
	        SQL_SUCCESS);
	REQUIRE(open.api.BindCol(open.statement.value, 3, SQL_C_DOUBLE, doubles, 0, double_lengths.data()) == SQL_SUCCESS);

	REQUIRE(open.api.Fetch(open.statement.value) == SQL_SUCCESS);

	CHECK(bits[0] == 1);
	CHECK(bits[1] == 0);
	CHECK(integers[0] == std::numeric_limits<SQLBIGINT>::min());
	CHECK(integers[1] == std::numeric_limits<SQLBIGINT>::max());
	CHECK(doubles[0] == 0.5);
	CHECK(doubles[1] == -2.25);
	CHECK(bit_lengths == std::vector<SQLLEN> {1, 1});
	CHECK(integer_lengths == std::vector<SQLLEN> {8, 8});
	CHECK(double_lengths == std::vector<SQLLEN> {8, 8});
}

// The driver writes the cell's length before it converts, so a failed conversion leaves it there.
TEST_CASE("A bound cell that does not convert fails its row with the conversion's record", "[fake_driver]") {
	FakeOdbcDriver fake;
	OpenStatement open;
	ExecuteColumn(fake, open, Column("a", "BIGINT", SQL_BIGINT, 19, 0), {"1", "x"});
	RowArray rows(open, 2);
	SQLBIGINT integers[2] = {};
	std::vector<SQLLEN> lengths(2, -99);
	REQUIRE(open.api.BindCol(open.statement.value, 1, SQL_C_SBIGINT, integers, 0, lengths.data()) == SQL_SUCCESS);

	CHECK(open.api.Fetch(open.statement.value) == SQL_SUCCESS_WITH_INFO);

	CHECK(rows.Status() == std::vector<SQLUSMALLINT> {SQL_ROW_SUCCESS, SQL_ROW_ERROR});
	CHECK(integers[0] == 1);
	CHECK(lengths[1] == 1);
	CHECK(StatementRecord(open, 1) == "22018 0 Invalid numeric format");
}

TEST_CASE("A cell that fails in column 1 leaves column 2 of its row unread", "[fake_driver]") {
	FakeOdbcDriver fake;
	OpenStatement open;
	ExecuteTwoColumns(fake, open, Rows({Column("a", "BIGINT", SQL_BIGINT, 19, 0), Varchar("b")}, {{"x", "text"}}));
	RowArray rows(open, 1);
	SQLBIGINT integer = 0;
	SQLLEN length = -99;
	REQUIRE(open.api.BindCol(open.statement.value, 1, SQL_C_SBIGINT, &integer, 0, &length) == SQL_SUCCESS);
	ByteColumn second(open, 2, 1, 8);

	CHECK(open.api.Fetch(open.statement.value) == SQL_ERROR);

	CHECK(rows.Status() == std::vector<SQLUSMALLINT> {SQL_ROW_ERROR});
	CHECK(StatementRecordCount(open) == 1);
	CHECK(StatementRecord(open, 1) == "22018 0 Invalid numeric format");
	CHECK(second.Untouched(0));
	CHECK(second.indicators[0] == -99);
}

// SQLFetch reads a bound cell through SQLGetData's own checks, so a DECIMAL cut in its whole digits
// is a row error, not a cut value (rsutil.c validateNumericBufferSize).
TEST_CASE("A bound NUMERIC cell too long for its buffer's whole digits fails its row with 22003", "[fake_driver]") {
	FakeOdbcDriver fake;
	OpenStatement open;
	ExecuteColumn(fake, open, Column("a", "NUMERIC", SQL_NUMERIC, 10, 1), {"12345.6"});
	RowArray rows(open, 1);
	ByteColumn column(open, 1, 1, 3);

	CHECK(open.api.Fetch(open.statement.value) == SQL_ERROR);

	CHECK(rows.Status() == std::vector<SQLUSMALLINT> {SQL_ROW_ERROR});
	CHECK(column.indicators[0] == 7);
	CHECK(StatementRecord(open, 1) == "22003 0 Buffer too small for numeric value");
}

// The bind offset moves every bound buffer and indicator by the same bytes, which is how a ring of
// buffer sets selects one per fetch.
TEST_CASE("Two fetches into different bind offsets land at the base addresses plus each offset", "[fake_driver]") {
	FakeOdbcDriver fake;
	OpenStatement open;
	ExecuteThreeRows(fake, open);
	RowArray rows(open, 1);
	// Room for two sets of one row each; an offset of 8 bytes moves the 8-byte text element and its
	// indicator by one element each.
	ByteColumn column(open, 1, 2, 8);
	SQLULEN offset = 0;
	SetAttribute(open, SQL_ATTR_ROW_BIND_OFFSET_PTR, &offset);
	REQUIRE(open.api.Fetch(open.statement.value) == SQL_SUCCESS);

	offset = 8;
	REQUIRE(open.api.Fetch(open.statement.value) == SQL_SUCCESS);

	CHECK(column.Element(0) == "a1");
	CHECK(column.Element(1) == "a2");
	CHECK(column.indicators == std::vector<SQLLEN> {2, 2});
}

// The driver reads the size as an int, refuses one not above 0 before storing it, and posts its HY024
// on the ARD, which a statement's SQLGetDiagRec does not read (rsdesc.cpp RsDesc::RS_SQLSetDescField,
// rserror.cpp RS_SQLError).
TEST_CASE("SQLSetStmtAttr refuses a row-array size not above 0 as an int, keeping the size it had", "[fake_driver]") {
	FakeOdbcDriver fake;
	OpenStatement open;
	ExecuteThreeRows(fake, open);
	RowArray rows(open, 2);
	SQLULEN size = 0;
	SECTION("0") {
	}
	SECTION("2^31, negative as an int") {
		size = SQLULEN(1) << 31;
	}
	SECTION("2^32 - 1, -1 as an int") {
		size = (SQLULEN(1) << 32) - 1;
	}

	CHECK(open.api.SetStmtAttr(open.statement.value, SQL_ATTR_ROW_ARRAY_SIZE, AsAttribute(size), 0) == SQL_ERROR);

	CHECK(StatementRecordCount(open) == 0);
	REQUIRE(open.api.Fetch(open.statement.value) == SQL_SUCCESS);
	CHECK(rows.fetched == 2);
}

// The largest int is the largest size the driver takes; nothing is fetched at it.
TEST_CASE("SQLSetStmtAttr accepts a row-array size of 2^31 - 1, the largest int", "[fake_driver]") {
	FakeOdbcDriver fake;
	OpenStatement open;

	CHECK(open.api.SetStmtAttr(open.statement.value, SQL_ATTR_ROW_ARRAY_SIZE, AsAttribute((SQLULEN(1) << 31) - 1), 0) ==
	      SQL_SUCCESS);
	CHECK(StatementRecordCount(open) == 0);
}

TEST_CASE("A row-array size past 32 bits is its low 32 bits, as the driver reads an int", "[fake_driver]") {
	FakeOdbcDriver fake;
	OpenStatement open;
	ExecuteThreeRows(fake, open);
	RowArray rows(open, 3);
	SetAttribute(open, SQL_ATTR_ROW_ARRAY_SIZE, AsAttribute((SQLULEN(1) << 32) + 2));

	REQUIRE(open.api.Fetch(open.statement.value) == SQL_SUCCESS);

	CHECK(rows.fetched == 2);
	CHECK(rows.Status() == std::vector<SQLUSMALLINT> {SQL_ROW_SUCCESS, SQL_ROW_SUCCESS, UNWRITTEN});
}

TEST_CASE("SQLSetStmtAttr refuses an attribute the fake does not model with HYC00", "[fake_driver]") {
	FakeOdbcDriver fake;
	OpenStatement open;

	CHECK(open.api.SetStmtAttr(open.statement.value, SQL_ATTR_MAX_ROWS, AsAttribute(10), 0) == SQL_ERROR);

	CHECK(StatementSqlstate(open) == "HYC00");
}

TEST_CASE("The call log shows the row-array size's value, a pointer attribute alone, and each binding",
          "[fake_driver]") {
	FakeOdbcDriver fake;
	OpenStatement open;
	const auto first_call = fake.Calls().size();
	RowArray rows(open, 2048);
	ByteColumn column(open, 3, 1, 301);

	// SQL_ATTR_ROW_ARRAY_SIZE is 27, SQL_ATTR_ROWS_FETCHED_PTR 26 and SQL_ATTR_ROW_STATUS_PTR 25.
	CHECK(CallsSince(fake, first_call) ==
	      std::vector<std::pair<std::string, std::string>> {{"SQLSetStmtAttr", "27=2048"},
	                                                        {"SQLSetStmtAttr", "26"},
	                                                        {"SQLSetStmtAttr", "25"},
	                                                        {"SQLBindCol", "3 1 301"}});
}

TEST_CASE("SQLBindCol refuses column 0 with the driver's HY000", "[fake_driver]") {
	FakeOdbcDriver fake;
	OpenStatement open;
	char value[8] = {};
	SQLLEN length = 0;

	CHECK(open.api.BindCol(open.statement.value, 0, SQL_C_CHAR, value, sizeof(value), &length) == SQL_ERROR);

	CHECK(StatementRecord(open, 1) == "HY000 0 SQLBindCol: Invalid column number");
}

// The driver binds any BufferLength, and its read of each cell refuses a negative one.
TEST_CASE("A negative buffer length binds but fails each row's read with HY090", "[fake_driver]") {
	FakeOdbcDriver fake;
	OpenStatement open;
	ExecuteThreeRows(fake, open);
	RowArray rows(open, 1);
	char value[8] = {};
	SQLLEN length = -99;
	REQUIRE(open.api.BindCol(open.statement.value, 1, SQL_C_CHAR, value, -1, &length) == SQL_SUCCESS);

	CHECK(open.api.Fetch(open.statement.value) == SQL_ERROR);

	CHECK(rows.Status() == std::vector<SQLUSMALLINT> {SQL_ROW_ERROR});
	CHECK(StatementRecord(open, 1) == "HY090 0 Invalid string or buffer length");
}

// The driver binds an indicator with no buffer, and its read converts nothing into the null target.
// The driver offsets the null pointer by the element and the bind offset and writes through it; the
// fake keeps the target null, a departure.
TEST_CASE("A column bound with an indicator and no buffer gets each cell's length alone", "[fake_driver]") {
	FakeOdbcDriver fake;
	OpenStatement open;
	auto result = Rows({Varchar("a")}, {{"a1"}, {"x"}});
	result.nulls = {{false}, {true}};
	fake.AddResult("SELECT a", {result});
	PrepareAndExecute(open, "SELECT a");
	RowArray rows(open, 2);
	SQLLEN lengths[2] = {-99, -99};
	REQUIRE(open.api.BindCol(open.statement.value, 1, SQL_C_CHAR, nullptr, 8, lengths) == SQL_SUCCESS);

	CHECK(open.api.Fetch(open.statement.value) == SQL_SUCCESS);

	CHECK(lengths[0] == 2);
	CHECK(lengths[1] == SQL_NULL_DATA);
}

TEST_CASE("SQLBindCol refuses a C type the fake does not return, with HYC00", "[fake_driver]") {
	FakeOdbcDriver fake;
	OpenStatement open;
	SQLINTEGER value = 0;
	SQLLEN length = 0;

	CHECK(open.api.BindCol(open.statement.value, 1, SQL_C_SLONG, &value, sizeof(value), &length) == SQL_ERROR);

	CHECK(StatementSqlstate(open) == "HYC00");
}

// A NULL cell shows the column is unbound: bound with no indicator, it would fail with 22002.
TEST_CASE("Binding a null buffer with a null indicator unbinds the column", "[fake_driver]") {
	FakeOdbcDriver fake;
	OpenStatement open;
	auto result = Rows({Varchar("a")}, {{"x"}});
	result.nulls = {{true}};
	fake.AddResult("SELECT a", {result});
	PrepareAndExecute(open, "SELECT a");
	ByteColumn column(open, 1, 1, 8);
	// The driver unbinds whatever the C type, so the fake unbinds before refusing a type it does not
	// return.
	SQLSMALLINT c_type = SQL_C_CHAR;
	SECTION("as SQL_C_CHAR") {
	}
	SECTION("as a C type the fake does not return") {
		c_type = SQL_C_SLONG;
	}

	REQUIRE(open.api.BindCol(open.statement.value, 1, c_type, nullptr, 0, nullptr) == SQL_SUCCESS);

	REQUIRE(open.api.Fetch(open.statement.value) == SQL_SUCCESS);
	CHECK(column.bytes == std::vector<char>(8, '#'));
	CHECK(column.indicators[0] == -99);
}

// The driver's rebind overwrites the buffer, length and indicator it keeps (rsresult.cpp
// RS_STMT_INFO::RS_SQLBindCol), so a statement reused with fresh buffers never writes the old ones.
TEST_CASE("A rebind to new buffers moves the column's writes there, leaving the old ones", "[fake_driver]") {
	FakeOdbcDriver fake;
	OpenStatement open;
	ExecuteThreeRows(fake, open);
	RowArray rows(open, 1);
	ByteColumn old_buffers(open, 1, 1, 8);
	ByteColumn new_buffers(open, 1, 1, 8);

	REQUIRE(open.api.Fetch(open.statement.value) == SQL_SUCCESS);

	CHECK(new_buffers.Element(0) == "a1");
	CHECK(new_buffers.indicators[0] == 2);
	CHECK(old_buffers.Untouched(0));
	CHECK(old_buffers.indicators[0] == -99);
}

TEST_CASE("SQLFreeStmt(SQL_UNBIND) unbinds every column", "[fake_driver]") {
	FakeOdbcDriver fake;
	OpenStatement open;
	ExecuteTwoColumns(fake, open, Rows({Varchar("a"), Varchar("b")}, {{"a1", "b1"}}));
	ByteColumn first(open, 1, 1, 8);
	ByteColumn second(open, 2, 1, 8);

	REQUIRE(open.api.FreeStmt(open.statement.value, SQL_UNBIND) == SQL_SUCCESS);

	REQUIRE(open.api.Fetch(open.statement.value) == SQL_SUCCESS);
	CHECK(first.indicators[0] == -99);
	CHECK(second.indicators[0] == -99);
}

// The driver's read of each cell finds the column missing, so each row fails and the cursor moves
// past them, as for any row error (rsresult.cpp RS_SQLGetData, RS_SQLFetchScroll).
TEST_CASE("A column bound past the result's columns fails every row of the rowset with 07009", "[fake_driver]") {
	FakeOdbcDriver fake;
	OpenStatement open;
	ExecuteThreeRows(fake, open);
	RowArray rows(open, 2);
	ByteColumn column(open, 2, 2, 8);

	CHECK(open.api.Fetch(open.statement.value) == SQL_ERROR);

	CHECK(rows.fetched == 2);
	CHECK(rows.Status() == std::vector<SQLUSMALLINT> {SQL_ROW_ERROR, SQL_ROW_ERROR});
	CHECK(StatementRecord(open, 1) == "07009 0 Invalid descriptor index");
	REQUIRE(open.api.FreeStmt(open.statement.value, SQL_UNBIND) == SQL_SUCCESS);
	SetAttribute(open, SQL_ATTR_ROW_ARRAY_SIZE, AsAttribute(1));
	REQUIRE(open.api.Fetch(open.statement.value) == SQL_SUCCESS);
	CHECK(Value(open, 1) == "a3");
}

// The driver strides a column by getOctetLenUsingCType, which gives no length for SQL_C_BINARY or
// for text bound with a BufferLength of 0. It checks each column's length as its column loop reaches
// it, after the row's earlier cells, and puts its HY000 ahead of their records (rsutil.c,
// rsresult.cpp RS_SQLFetchScroll).
TEST_CASE("A column with no array element length fails a row-array fetch with the driver's HY000", "[fake_driver]") {
	FakeOdbcDriver fake;
	OpenStatement open;
	FakeResultSet result;
	result.columns = {Varchar("a"), Column("b", "VARBYTE", SQL_LONGVARBINARY, 64000, 0), Varchar("c")};
	result.rows = {{"abcd", "b1", "c1"}, {"a2", "b2", "c2"}};
	fake.AddResult("SELECT a, b, c", {result});
	PrepareAndExecute(open, "SELECT a, b, c");
	RowArray rows(open, 2);
	ByteColumn first(open, 1, 2, 4);
	char bytes[16] = {};
	SQLLEN lengths[2] = {};
	SECTION("SQL_C_BINARY") {
		REQUIRE(open.api.BindCol(open.statement.value, 2, SQL_C_BINARY, bytes, 8, lengths) == SQL_SUCCESS);
	}
	SECTION("SQL_C_CHAR with a BufferLength of 0") {
		REQUIRE(open.api.BindCol(open.statement.value, 2, SQL_C_CHAR, bytes, 0, lengths) == SQL_SUCCESS);
	}
	ByteColumn third(open, 3, 2, 8);

	CHECK(open.api.Fetch(open.statement.value) == SQL_ERROR);

	CHECK(StatementRecordCount(open) == 2);
	CHECK(StatementRecord(open, 1) == "HY000 0 Array element length is zero.");
	CHECK(StatementRecord(open, 2) == "01004 0 String data, right truncated");
	CHECK(first.Element(0) == "abc");
	CHECK(third.Untouched(0));
	CHECK(third.indicators[0] == -99);
	CHECK(rows.fetched == 99);
	CHECK(rows.Status() == std::vector<SQLUSMALLINT>(2, UNWRITTEN));
	// The stopped fetch leaves no part-read cell, so the cut "abcd" is read again from its start.
	SetAttribute(open, SQL_ATTR_ROW_ARRAY_SIZE, AsAttribute(1));
	CHECK(Value(open, 1) == "abcd");
}

// The driver's column loop breaks at the first failed cell, so only the first bound column's
// element length is checked before a row fails.
TEST_CASE("A row scripted in error checks only its first bound column's element length", "[fake_driver]") {
	FakeOdbcDriver fake;
	OpenStatement open;
	auto result =
	    Rows({Varchar("a"), Column("b", "VARBYTE", SQL_LONGVARBINARY, 64000, 0)}, {{"a1", "b1"}, {"a2", "b2"}});
	result.row_status = {SQL_ROW_ERROR, SQL_ROW_ERROR};
	result.row_diags[0] = {{"22003", 0, "first"}};
	result.row_diags[1] = {{"22012", 0, "second"}};
	ExecuteTwoColumns(fake, open, result);
	RowArray rows(open, 2);
	ByteColumn text(open, 1, 2, 8);
	char bytes[16] = {};
	SQLLEN byte_lengths[2] = {-99, -99};
	REQUIRE(open.api.BindCol(open.statement.value, 2, SQL_C_BINARY, bytes, 8, byte_lengths) == SQL_SUCCESS);

	CHECK(open.api.Fetch(open.statement.value) == SQL_ERROR);

	CHECK(rows.fetched == 2);
	CHECK(rows.Status() == std::vector<SQLUSMALLINT> {SQL_ROW_ERROR, SQL_ROW_ERROR});
	CHECK(StatementRecord(open, 1) == "22012 0 second");
	CHECK(text.indicators == std::vector<SQLLEN> {0, 0});
	CHECK(byte_lengths[0] == -99);
}

// A row scripted in error stands for a failed first cell, which the driver never reads when that
// cell's column has no length: the check comes first.
TEST_CASE("A row scripted in error whose first column has no length gets the driver's HY000", "[fake_driver]") {
	FakeOdbcDriver fake;
	OpenStatement open;
	auto result = Rows({Column("b", "VARBYTE", SQL_LONGVARBINARY, 64000, 0)}, {{"b1"}, {"b2"}});
	result.row_status = {SQL_ROW_ERROR, SQL_ROW_SUCCESS};
	result.row_diags[0] = {{"22003", 0, "numeric value out of range"}};
	fake.AddResult("SELECT b", {result});
	PrepareAndExecute(open, "SELECT b");
	RowArray rows(open, 2);
	char bytes[16] = {};
	SQLLEN lengths[2] = {-99, -99};
	REQUIRE(open.api.BindCol(open.statement.value, 1, SQL_C_BINARY, bytes, 8, lengths) == SQL_SUCCESS);

	CHECK(open.api.Fetch(open.statement.value) == SQL_ERROR);

	CHECK(StatementRecordCount(open) == 1);
	CHECK(StatementRecord(open, 1) == "HY000 0 Array element length is zero.");
	CHECK(rows.fetched == 99);
	CHECK(rows.Status() == std::vector<SQLUSMALLINT>(2, UNWRITTEN));
	CHECK(lengths[0] == -99);
	CHECK(lengths[1] == -99);
}

// A cell that fails ends its row before the loop reaches the column with no length, so that row
// fails as a row and the fetch goes on to the next.
TEST_CASE("A row whose earlier cell fails is a row error before the HY000 of a later row", "[fake_driver]") {
	FakeOdbcDriver fake;
	OpenStatement open;
	ExecuteTwoColumns(
	    fake, open,
	    Rows({Column("a", "BIGINT", SQL_BIGINT, 19, 0), Column("b", "VARBYTE", SQL_LONGVARBINARY, 64000, 0)},
	         {{"x", "b1"}, {"1", "b2"}}));
	RowArray rows(open, 2);
	SQLBIGINT integers[2] = {};
	SQLLEN integer_lengths[2] = {};
	char bytes[16] = {};
	SQLLEN byte_lengths[2] = {};
	REQUIRE(open.api.BindCol(open.statement.value, 1, SQL_C_SBIGINT, integers, 0, integer_lengths) == SQL_SUCCESS);
	REQUIRE(open.api.BindCol(open.statement.value, 2, SQL_C_BINARY, bytes, 8, byte_lengths) == SQL_SUCCESS);

	CHECK(open.api.Fetch(open.statement.value) == SQL_ERROR);

	CHECK(rows.Status() == std::vector<SQLUSMALLINT> {SQL_ROW_ERROR, UNWRITTEN});
	CHECK(StatementRecord(open, 1) == "HY000 0 Array element length is zero.");
	CHECK(rows.fetched == 99);
}

TEST_CASE("A rowset whose every row fails before a column with no length gets no HY000", "[fake_driver]") {
	FakeOdbcDriver fake;
	OpenStatement open;
	auto result = Rows({Varchar("a"), Column("b", "VARBYTE", SQL_LONGVARBINARY, 64000, 0)}, {{"x", "b1"}, {"y", "b2"}});
	result.nulls = {{true, false}, {true, false}};
	ExecuteTwoColumns(fake, open, result);
	RowArray rows(open, 2);
	char text[16] = {};
	char bytes[16] = {};
	SQLLEN byte_lengths[2] = {};
	REQUIRE(open.api.BindCol(open.statement.value, 1, SQL_C_CHAR, text, 8, nullptr) == SQL_SUCCESS);
	REQUIRE(open.api.BindCol(open.statement.value, 2, SQL_C_BINARY, bytes, 8, byte_lengths) == SQL_SUCCESS);

	CHECK(open.api.Fetch(open.statement.value) == SQL_ERROR);

	CHECK(rows.fetched == 2);
	CHECK(rows.Status() == std::vector<SQLUSMALLINT> {SQL_ROW_ERROR, SQL_ROW_ERROR});
	CHECK(StatementRecordCount(open) == 1);
	CHECK(StatementRecord(open, 1) == "22002 0 Indicator variable required but not supplied");
}

// The driver reads bound columns in the order they were first bound (rsutil.c addDescriptorRec,
// rsresult.cpp RS_SQLFetchScroll), so the column bound last is read last and keeps its records.
TEST_CASE("SQLFetch reads bound columns in bind order, so the last bound keeps its records", "[fake_driver]") {
	FakeOdbcDriver fake;
	OpenStatement open;
	ExecuteTwoColumns(fake, open, Rows({Varchar("a"), Varchar("b")}, {{"abcd", "b"}}));
	RowArray rows(open, 1);
	ByteColumn second(open, 2, 1, 4);
	ByteColumn first(open, 1, 1, 4);
	// A rebind keeps the column's place, so column 1 is still read last.
	REQUIRE(open.api.BindCol(open.statement.value, 2, SQL_C_CHAR, second.bytes.data(), 4, second.indicators.data()) ==
	        SQL_SUCCESS);

	CHECK(open.api.Fetch(open.statement.value) == SQL_SUCCESS_WITH_INFO);

	CHECK(StatementRecordCount(open) == 1);
	CHECK(StatementRecord(open, 1) == "01004 0 String data, right truncated");
}

// The ODBC client's producer thread fetches a row array its caller's thread set up: the size, the bound
// columns and the bind offset, which points into the caller's memory.
TEST_CASE("A row array set up on one thread can be fetched on another", "[fake_driver]") {
	FakeOdbcDriver fake;
	OpenStatement open;
	ExecuteThreeRows(fake, open);
	RowArray rows(open, 2);
	// Room for three rows, with an offset of one element, so the fetch lands in elements 1 and 2.
	ByteColumn column(open, 1, 3, 8);
	SQLULEN offset = 8;
	SetAttribute(open, SQL_ATTR_ROW_BIND_OFFSET_PTR, &offset);

	auto fetched = std::async(std::launch::async, [&]() { return open.api.Fetch(open.statement.value); });

	CHECK(fetched.get() == SQL_SUCCESS);
	CHECK(rows.fetched == 2);
	CHECK(rows.Status() == std::vector<SQLUSMALLINT> {SQL_ROW_SUCCESS, SQL_ROW_SUCCESS});
	CHECK(column.Untouched(0));
	CHECK(column.Element(1) == "a1");
	CHECK(column.Element(2) == "a2");
	CHECK(column.indicators == std::vector<SQLLEN> {-99, 2, 2});
}

// The driver resets a bound column's read offset after the fetch reads it (rsresult.cpp
// RS_SQLFetchScroll), so SQLGetData reads the cell from its start.
TEST_CASE("SQLGetData after a fetch reads a bound cell from its start", "[fake_driver]") {
	FakeOdbcDriver fake;
	OpenStatement open;
	ExecuteColumn(fake, open, Varchar("a"), {"abcd"});
	RowArray rows(open, 1);
	ByteColumn column(open, 1, 1, 4);
	REQUIRE(open.api.Fetch(open.statement.value) == SQL_SUCCESS_WITH_INFO);
	char value[8] = {};
	SQLLEN length = 0;

	CHECK(open.api.GetData(open.statement.value, 1, SQL_C_CHAR, value, sizeof(value), &length) == SQL_SUCCESS);

	CHECK(std::string(value) == "abcd");
	CHECK(length == 4);
}

// The driver reports no SQL_GD_BLOCK, so ODBC does not allow SQLGetData over a rowset of several
// rows.
TEST_CASE("SQLGetData with a row array above 1 fails with HYC00", "[fake_driver]") {
	FakeOdbcDriver fake;
	OpenStatement open;
	ExecuteThreeRows(fake, open);
	RowArray rows(open, 2);
	REQUIRE(open.api.Fetch(open.statement.value) == SQL_SUCCESS);
	char value[8] = {};
	SQLLEN length = 0;

	CHECK(open.api.GetData(open.statement.value, 1, SQL_C_CHAR, value, sizeof(value), &length) == SQL_ERROR);

	CHECK(StatementSqlstate(open) == "HYC00");
}

// The driver's streaming cursor is forward-only, so it cannot position within a rowset.
TEST_CASE("SQLSetPos(SQL_POSITION) fails with the driver's HY106", "[fake_driver]") {
	FakeOdbcDriver fake;
	OpenStatement open;
	ExecuteThreeRows(fake, open);
	RowArray rows(open, 2);
	REQUIRE(open.api.Fetch(open.statement.value) == SQL_SUCCESS);
	const auto first_call = fake.Calls().size();

	CHECK(open.api.SetPos(open.statement.value, 1, SQL_POSITION, SQL_LOCK_NO_CHANGE) == SQL_ERROR);

	CHECK(StatementRecord(open, 1) == "HY106 0 Cursor is forward only or non-scrollable");
	// SQL_POSITION is 0.
	CHECK(CallsSince(fake, first_call).front() == std::make_pair(std::string("SQLSetPos"), std::string("0")));
}

TEST_CASE("SQLSetPos refuses any other operation or lock with the driver's HYC00", "[fake_driver]") {
	FakeOdbcDriver fake;
	OpenStatement open;
	ExecuteThreeRows(fake, open);
	REQUIRE(open.api.Fetch(open.statement.value) == SQL_SUCCESS);

	CHECK(open.api.SetPos(open.statement.value, 1, SQL_REFRESH, SQL_LOCK_NO_CHANGE) == SQL_ERROR);
	CHECK(StatementRecord(open, 1) == "HYC00 0 Optional feature not implemented: hOperation");
	CHECK(open.api.SetPos(open.statement.value, 1, SQL_POSITION, SQL_LOCK_EXCLUSIVE) == SQL_ERROR);
	CHECK(StatementRecord(open, 1) == "HYC00 0 Optional feature not implemented-SQL_POSITION");
}
