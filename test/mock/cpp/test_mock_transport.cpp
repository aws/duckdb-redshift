// The shared mock's answers and controls that the extension's tests rely on: SessionHoldsState() follows
// the real client's latch; gates hold the next connection open or close until the test releases
// them, as the real client's connect and network teardown can take a while; an open can be made to
// fail; and the directives shape results, refusals, statement failures and connection loss as the
// real client would. Each case opens mock connections directly, and reads opens and closes from the
// lines the mock logs through its sink.

#include "catch.hpp"

#include <chrono>
#include <future>
#include <mutex>
#include <thread>
#include <tuple>

#include "duckdb.hpp"
#include "mock_transport.hpp"
#include "redshift_transport.hpp"
#include "test_util.hpp"

using namespace duckdb;
using redshift_test::Capture;

namespace {

//! Keeps every line the mock logs. The mock logs from whichever thread destroys a connection.
class RecordingLogSink : public RedshiftLogSink {
public:
	void Debug(const string &message) noexcept override {
		std::lock_guard<std::mutex> guard(lock);
		lines.push_back(message);
	}

	//! How many logged lines are exactly `message`.
	idx_t Count(const string &message) {
		std::lock_guard<std::mutex> guard(lock);
		idx_t count = 0;
		for (auto &line : lines) {
			count += line == message ? 1 : 0;
		}
		return count;
	}

private:
	std::mutex lock;
	vector<string> lines;
};

unique_ptr<RedshiftConnection> OpenMock(shared_ptr<RedshiftLogSink> log_sink, const string &region = "us-east-1") {
	return OpenRedshiftConnection(redshift_test::MockConfig(region), std::move(log_sink));
}

unique_ptr<RedshiftConnection> OpenMock() {
	return OpenMock(make_shared_ptr<RecordingLogSink>());
}

//! The mock's refusal of a configuration with no target, or one outside the enum.
constexpr const char *NO_TARGET =
    "Redshift connection requires a target: a Serverless workgroup or a provisioned cluster";

string OpenedLine(const string &region) {
	return "redshift mock: opened a connection to " + redshift_test::MockTarget(region);
}

string ClosedLine(const string &region) {
	return "redshift mock: closed a connection to " + redshift_test::MockTarget(region);
}

//! Executes `sql` directly, passing the shape the mock's Describe reports for it, as redshift_query
//! passes the shape it bound, so the shape check passes unless changed= changes the shape.
unique_ptr<RedshiftResult> ExecuteAsBound(RedshiftConnection &connection, const string &sql,
                                          const vector<string> &params = vector<string>()) {
	return connection.Execute(sql, params, MockDescribedShape(sql));
}

//! The size of each batch `result` returns until it is drained.
vector<idx_t> BatchSizes(RedshiftResult &result) {
	vector<idx_t> sizes;
	RedshiftBatch batch;
	while (result.NextBatch(batch)) {
		sizes.push_back(batch.rows.size());
	}
	return sizes;
}

} // namespace

TEST_CASE("SessionHoldsState is false until a statement runs, then true for good", "[mock]") {
	auto connection = OpenMock();
	CHECK_FALSE(connection->SessionHoldsState());
	// Describe runs nothing.
	connection->Describe("rows=1 cols=1");
	connection->Describe("norows");
	CHECK_FALSE(connection->SessionHoldsState());
	// Nor does a statement refused before it runs.
	const auto refused = Capture([&]() { ExecuteAsBound(*connection, "error=refused"); });
	CHECK(refused.type == ExceptionType::BINDER);
	CHECK_FALSE(connection->SessionHoldsState());

	ExecuteAsBound(*connection, "rows=2 cols=1");
	CHECK(connection->SessionHoldsState());
	// Nothing makes it false again: not another statement, not a refused one.
	ExecuteAsBound(*connection, "norows");
	CHECK(connection->SessionHoldsState());
	CHECK_THROWS(ExecuteAsBound(*connection, "error=refused"));
	CHECK(connection->SessionHoldsState());
}

TEST_CASE("SessionHoldsState turns true whatever the statement returns, on its own connection only", "[mock]") {
	auto with_rows = OpenMock();
	auto without_rows = OpenMock();
	auto untouched = OpenMock();

	ExecuteAsBound(*with_rows, "rows=0 cols=1");
	ExecuteAsBound(*without_rows, "norows");

	CHECK(with_rows->SessionHoldsState());
	CHECK(without_rows->SessionHoldsState());
	CHECK_FALSE(untouched->SessionHoldsState());
}

TEST_CASE("a close gate holds the next close until released, and no other close", "[mock]") {
	auto log_sink = make_shared_ptr<RecordingLogSink>();
	auto held = OpenMock(log_sink, "us-east-1");
	auto other = OpenMock(log_sink, "us-west-2");
	auto gate = make_shared_ptr<MockGate>();
	HoldNextMockClose(gate);

	std::thread closer([&]() { held.reset(); });
	const bool arrived = gate->WaitUntilHeldFor(std::chrono::seconds(10));
	const auto held_closes_while_held = log_sink->Count(ClosedLine("us-east-1"));
	// Bounded, so a close that waited for the held one fails the case instead of hanging it. The
	// release below lets such a close finish, so the future can still be destroyed.
	auto other_closed = std::async(std::launch::async, [&]() { other.reset(); });
	const auto other_status = other_closed.wait_for(std::chrono::seconds(10));
	const auto other_closes = log_sink->Count(ClosedLine("us-west-2"));
	gate->Release();
	closer.join();
	other_closed.wait();

	CHECK(arrived);
	// The held close waits before it logs.
	CHECK(held_closes_while_held == 0);
	CHECK(other_status == std::future_status::ready);
	CHECK(other_closes == 1);
	CHECK(log_sink->Count(ClosedLine("us-east-1")) == 1);
}

TEST_CASE("a close gate released before the close lets it through", "[mock]") {
	auto log_sink = make_shared_ptr<RecordingLogSink>();
	auto connection = OpenMock(log_sink);
	auto gate = make_shared_ptr<MockGate>();
	HoldNextMockClose(gate);
	gate->Release();

	connection.reset();

	CHECK(log_sink->Count(ClosedLine("us-east-1")) == 1);
	// The close still took the gate.
	CHECK(gate->WaitUntilHeldFor(std::chrono::milliseconds(0)));
}

TEST_CASE("an open gate holds the next open until released, and no other open", "[mock]") {
	auto log_sink = make_shared_ptr<RecordingLogSink>();
	auto gate = make_shared_ptr<MockGate>();
	HoldNextMockOpen(gate);

	unique_ptr<RedshiftConnection> held;
	std::thread opener([&]() { held = OpenMock(log_sink, "us-east-1"); });
	const bool arrived = gate->WaitUntilHeldFor(std::chrono::seconds(10));
	const auto held_opens_while_held = log_sink->Count(OpenedLine("us-east-1"));
	// Bounded, so an open that waited for the held one fails the case instead of hanging it. The
	// release below lets such an open finish, so the future can still be destroyed.
	unique_ptr<RedshiftConnection> other;
	auto other_opened = std::async(std::launch::async, [&]() { other = OpenMock(log_sink, "us-west-2"); });
	const auto other_status = other_opened.wait_for(std::chrono::seconds(10));
	const auto other_opens = log_sink->Count(OpenedLine("us-west-2"));
	gate->Release();
	opener.join();
	other_opened.wait();

	CHECK(arrived);
	// The held open waits before the connection exists, so nothing has logged it.
	CHECK(held_opens_while_held == 0);
	CHECK(other_status == std::future_status::ready);
	CHECK(other_opens == 1);
	CHECK(held);
	CHECK(log_sink->Count(OpenedLine("us-east-1")) == 1);
}

TEST_CASE("a close gate holds no open, and an open gate no close", "[mock]") {
	auto log_sink = make_shared_ptr<RecordingLogSink>();
	auto first = OpenMock(log_sink, "us-east-1");

	// An open runs past an armed close gate, which then holds the next close. Bounded, so an open
	// the gate held fails the case instead of hanging it; the release lets such an open finish.
	auto close_gate = make_shared_ptr<MockGate>();
	HoldNextMockClose(close_gate);
	unique_ptr<RedshiftConnection> second;
	auto opened = std::async(std::launch::async, [&]() { second = OpenMock(log_sink, "us-west-2"); });
	const auto open_status = opened.wait_for(std::chrono::seconds(10));
	const bool close_gate_held_open = close_gate->WaitUntilHeldFor(std::chrono::milliseconds(0));
	close_gate->Release();
	opened.wait();
	CHECK(open_status == std::future_status::ready);
	CHECK_FALSE(close_gate_held_open);
	first.reset();
	CHECK(close_gate->WaitUntilHeldFor(std::chrono::milliseconds(0)));

	// A close runs past an armed open gate, which then holds the next open.
	auto open_gate = make_shared_ptr<MockGate>();
	HoldNextMockOpen(open_gate);
	auto closed = std::async(std::launch::async, [&]() { second.reset(); });
	const auto close_status = closed.wait_for(std::chrono::seconds(10));
	const bool open_gate_held_close = open_gate->WaitUntilHeldFor(std::chrono::milliseconds(0));
	open_gate->Release();
	closed.wait();
	CHECK(close_status == std::future_status::ready);
	CHECK_FALSE(open_gate_held_close);
	auto third = OpenMock(log_sink);
	CHECK(open_gate->WaitUntilHeldFor(std::chrono::milliseconds(0)));
	CHECK(log_sink->Count(ClosedLine("us-east-1")) == 1);
	CHECK(log_sink->Count(ClosedLine("us-west-2")) == 1);
}

TEST_CASE("an open gate released before the open lets it through", "[mock]") {
	auto log_sink = make_shared_ptr<RecordingLogSink>();
	auto gate = make_shared_ptr<MockGate>();
	HoldNextMockOpen(gate);
	gate->Release();

	auto connection = OpenMock(log_sink);

	CHECK(log_sink->Count(OpenedLine("us-east-1")) == 1);
	// The open still took the gate.
	CHECK(gate->WaitUntilHeldFor(std::chrono::milliseconds(0)));
}

TEST_CASE("the mock logs a Describe and an execution only once each gets past its refusals", "[mock]") {
	auto log_sink = make_shared_ptr<RecordingLogSink>();
	auto connection = OpenMock(log_sink);
	const string described = "redshift mock: described a statement on a connection to " + redshift_test::MockTarget();
	const string executed = "redshift mock: executed a statement on a connection to " + redshift_test::MockTarget();

	connection->Describe("rows=1 cols=1");
	CHECK(log_sink->Count(described) == 1);
	CHECK(log_sink->Count(executed) == 0);

	CHECK(Capture([&]() { connection->Describe("error=refused"); }).type == ExceptionType::BINDER);
	CHECK(Capture([&]() { ExecuteAsBound(*connection, "error=refused"); }).type == ExceptionType::BINDER);
	CHECK(log_sink->Count(described) == 1);
	CHECK(log_sink->Count(executed) == 0);

	ExecuteAsBound(*connection, "norows");
	CHECK(log_sink->Count(described) == 1);
	CHECK(log_sink->Count(executed) == 1);
}

TEST_CASE("the mock refuses a multi-statement string at Describe and at Execute, before it runs", "[mock]") {
	auto connection = OpenMock();
	// The last one's malformed directive is never read: the refusal comes first, as Redshift's would.
	for (auto &sql : {"norows; norows", "SELECT 1;SELECT 2", "SELECT ';' ; x", "SELECT 1; -- c\nSELECT 2",
	                  "SELECT $a$;$a$; SELECT 2", "SELECT 1 /* ; */; SELECT 2", "SELECT 'a\\\\'; SELECT 2",
	                  "SELECT 1 AS a$b$c; SELECT 2", "SELECT 1 AS a_$b$c; SELECT 2", "SELECT 1 AS a$$b$; SELECT 2",
	                  "SELECT $1, $2; SELECT 3", "SELECT \"a\\\"; SELECT 2", "rows=x; SELECT 1"}) {
		INFO(sql);
		auto described = Capture([&]() { connection->Describe(sql); });
		CHECK(described.type == ExceptionType::BINDER);
		CHECK(described.message ==
		      "Redshift rejected the statement: cannot insert multiple commands into a prepared statement");
		// Refused before the shape check, so any bound shape meets the same refusal.
		auto executed = Capture([&]() { connection->Execute(sql, vector<string>(), RedshiftStatementShape()); });
		CHECK(executed.type == ExceptionType::BINDER);
		CHECK(executed.message == described.message);
	}
	// A refusal runs nothing, so the latch stays false.
	CHECK_FALSE(connection->SessionHoldsState());
	// One statement, whatever its quotes and trailing ';' hold.
	for (auto &sql :
	     {"SELECT 1;", "SELECT 1 ; ;\n", "SELECT ';x'", "SELECT \"a;b\"", "SELECT 'it''s;'", "SELECT 'it\\'s;'",
	      "SELECT 1 -- ; SELECT 2", "SELECT 1 /* ; /* ; */ ; */", "SELECT $$;$$", "SELECT $q$ ; $q$", "SELECT $1, $2;",
	      "SELECT 1 AS a$b$c;", "SELECT 1; -- done", "SELECT 1; /* done */", "SELECT 'unterminated ;",
	      "SELECT $q$ ; SELECT 2", "SELECT 1 /* ; SELECT 2", "SELECT \"a ; SELECT 2", "SELECT $a_b$ ; $a_b$"}) {
		INFO(sql);
		CHECK(connection->Describe(sql).returns_rows);
	}
}

TEST_CASE("the mock reads directives only outside quotes and comments", "[mock]") {
	// Each is an ordinary statement, which the mock echoes, as Redshift reads the directive-like words
	// as text.
	auto connection = OpenMock();
	for (auto &sql : {"SELECT 'rows=5'", "SELECT \"norows\"", "SELECT 1 -- rows=0", "SELECT 1 /* error=refused */",
	                  "SELECT $$ error=refused $$", "SELECT $t$ types=bogus $t$"}) {
		INFO(sql);
		auto shape = connection->Describe(sql);
		REQUIRE(shape.returns_rows);
		REQUIRE(shape.columns.size() == 1);
		auto result = ExecuteAsBound(*connection, sql);
		RedshiftBatch batch;
		REQUIRE(result->NextBatch(batch));
		REQUIRE(batch.rows.size() == 1);
		CHECK(batch.rows[0][0].text == sql);
		CHECK_FALSE(result->NextBatch(batch));
	}
}

TEST_CASE("types= classifies each column as the ODBC client does", "[mock]") {
	auto connection = OpenMock();
	auto shape =
	    connection->Describe("types=int2,numeric:38:38,numeric:12:2,varbyte,geometry,hllsketch,geography,char");
	REQUIRE(shape.returns_rows);
	REQUIRE(shape.columns.size() == 8);
	CHECK(shape.columns[0].type == RedshiftTypeId::SMALLINT);
	CHECK(shape.columns[1].type == RedshiftTypeId::DECIMAL);
	CHECK(shape.columns[1].precision == 38);
	CHECK(shape.columns[1].scale == 38);
	CHECK(shape.columns[2].precision == 12);
	CHECK(shape.columns[2].scale == 2);
	CHECK(shape.columns[3].type == RedshiftTypeId::BLOB);
	CHECK(shape.columns[4].type == RedshiftTypeId::UNKNOWN);
	CHECK(shape.columns[5].type == RedshiftTypeId::UNKNOWN);
	CHECK(shape.columns[6].type == RedshiftTypeId::UNKNOWN);
	CHECK(shape.columns[7].type == RedshiftTypeId::CHAR);
	for (idx_t c = 0; c < shape.columns.size(); c++) {
		CHECK(shape.columns[c].name == "c" + std::to_string(c));
	}
	// An empty types= leaves cols=.
	auto untyped = connection->Describe("types= cols=2");
	REQUIRE(untyped.columns.size() == 2);
	CHECK(untyped.columns[1].type == RedshiftTypeId::VARCHAR);
	// Metadata DuckDB cannot represent arrives as UNKNOWN, never as DECIMAL.
	for (auto &type : {"numeric:39:0", "numeric:0:0", "numeric:5:6", "numeric:-5:2", "numeric:5:-1"}) {
		INFO(type);
		auto column = connection->Describe(string("types=") + type).columns[0];
		CHECK(column.type == RedshiftTypeId::UNKNOWN);
		CHECK(column.precision == 0);
		CHECK(column.scale == 0);
	}
}

TEST_CASE("a CHAR column's cells arrive padded, as a CHAR(n) wider than the value pads them", "[mock]") {
	auto connection = OpenMock();
	auto result = ExecuteAsBound(*connection, "rows=2 types=char,varchar");
	RedshiftBatch batch;
	REQUIRE(result->NextBatch(batch));
	REQUIRE(batch.rows.size() == 2);
	CHECK(batch.rows[1][0].kind == RedshiftValueKind::TEXT);
	CHECK(batch.rows[1][0].text == "r1c0   ");
	// VARCHAR arrives as it is.
	CHECK(batch.rows[1][1].text == "r1c1");
}

TEST_CASE("an UNKNOWN column executed directly reads TEXT cells", "[mock]") {
	// redshift_query refuses an UNKNOWN column at bind, so only a test like this one reads its cells.
	// The ODBC client does not specify their carrier; the mock sends TEXT, as for VARCHAR.
	auto connection = OpenMock();
	auto result = ExecuteAsBound(*connection, "rows=1 types=geometry,varchar");
	RedshiftBatch batch;
	REQUIRE(result->NextBatch(batch));
	REQUIRE(batch.rows.size() == 1);
	REQUIRE(batch.rows[0].size() == 2);
	CHECK(batch.rows[0][0].kind == RedshiftValueKind::TEXT);
	CHECK(batch.rows[0][0].text == "r0c0");
	CHECK(batch.rows[0][1].text == "r0c1");
}

TEST_CASE("names= names the columns in order, a name repeated if need be", "[mock]") {
	auto connection = OpenMock();
	auto shape = connection->Describe("types=int4,int4,varchar names=id,id,Name");
	REQUIRE(shape.columns.size() == 3);
	CHECK(shape.columns[0].name == "id");
	CHECK(shape.columns[1].name == "id");
	CHECK(shape.columns[2].name == "Name");
	CHECK(shape.columns[2].type == RedshiftTypeId::VARCHAR);
}

TEST_CASE("a trailing ';' ends the last directive", "[mock]") {
	auto connection = OpenMock();
	CHECK_FALSE(connection->Describe("norows;").returns_rows);
	auto shape = connection->Describe("rows=2 cols=1;");
	REQUIRE(shape.columns.size() == 1);
	auto result = ExecuteAsBound(*connection, "rows=2 cols=1;");
	RedshiftBatch batch;
	REQUIRE(result->NextBatch(batch));
	CHECK(batch.rows.size() == 2);
}

TEST_CASE("the mock refuses a malformed types=, cols= or names= directive", "[mock]") {
	auto connection = OpenMock();
	const std::pair<const char *, const char *> cases[] = {
	    {"types=bogus", "redshift mock: unknown type \"bogus\" in types="},
	    {"types=numeric:10", "redshift mock: unknown type \"numeric:10\" in types="},
	    {"types=numeric:x:2", "redshift mock: numeric precision must be a non-negative integer, got \"x\""},
	    {"cols=0", "redshift mock: cols must be at least 1; use norows for a statement with no result set"},
	    {"cols=2 names=a", "redshift mock: names= gives 1 names for 2 columns"},
	    {"norows names=a", "redshift mock: names= names result columns, and norows has none"},
	    {"cols=3 names=a,,b", "redshift mock: names= has an empty name"},
	};
	for (auto &c : cases) {
		INFO(c.first);
		auto thrown = Capture([&]() { connection->Describe(c.first); });
		CHECK(thrown.type == ExceptionType::INVALID_INPUT);
		CHECK(thrown.message == c.second);
	}
}

TEST_CASE("the mock refuses a directive or combination the real client cannot produce", "[mock]") {
	auto connection = OpenMock();
	const std::pair<const char *, const char *> refused[] = {
	    {"rows=1 batch=0", "redshift mock: batch= needs a size above 0, or the result never ends"},
	    {"rows=1 batch=0,0", "redshift mock: batch= needs a size above 0, or the result never ends"},
	    {"rows=3 lose=fetch:4", "redshift mock: fetch:4 is past the 3 rows the result has"},
	    {"rows=3 fail=fetch:4", "redshift mock: fetch:4 is past the 3 rows the result has"},
	    {"rows=1 sqlstates=22012", "redshift mock: sqlstates= needs error=, fail= or lose="},
	    {"rows=1 batch=", "redshift mock: batch needs a value"},
	    {"norows lose=fetch:0", "redshift mock: fetch: needs a statement with a result set"},
	    {"norows fail=fetch:0", "redshift mock: fetch: needs a statement with a result set"},
	    {"norows changed=int4 lose=fetch:0", "redshift mock: fetch: needs a statement with a result set"},
	    {"norows rowcount=1 changed=int4", "redshift mock: rowcount= and changed= cannot be combined"},
	    {"rows=1 unchecked", "redshift mock: unchecked needs changed="},
	    {"rows=1 unusable", "redshift mock: unusable needs error= or fail="},
	    {"error=x sqlstates=42601 unusable", "redshift mock: unusable needs XX000 among sqlstates="},
	    {"rows=1 fail=execute sqlstates=HY000,22012 unusable", "redshift mock: unusable needs XX000 among sqlstates="},
	    {"error=x lose=execute", "redshift mock: error= and that lose= or fail= cannot both fire"},
	    {"error=x lose=prepare", "redshift mock: error= and that lose= or fail= cannot both fire"},
	    {"error=x lose=describe:any", "redshift mock: error= and that lose= or fail= cannot both fire"},
	    {"error=x fail=fetch:1 rows=3", "redshift mock: error= and that lose= or fail= cannot both fire"},
	    {"error=x error_at=execute fail=execute", "redshift mock: error= and that lose= or fail= cannot both fire"},
	    {"error=x error_at=execute lose=prepare", "redshift mock: error= and that lose= or fail= cannot both fire"},
	    {"rows=1 rowcount=1", "redshift mock: rowcount= needs norows"},
	    {"rows=1 error_at=execute", "redshift mock: error_at= needs error="},
	    {"error=x error_at=describe", "redshift mock: error_at must be execute, got \"describe\""},
	    {"fail=prepare", "redshift mock: fail must be execute or fetch:<rows>, got \"prepare\""},
	    {"lose=network", "redshift mock: lose must be describe, describe:any, prepare, execute or fetch:<rows>, got "
	                     "\"network\""},
	    {"lose=execute fail=execute", "redshift mock: lose= and fail= cannot be combined"},
	    {"fail=execute sqlstates=25P02", "redshift mock: fail= cannot carry 25P02, which Redshift gives when it "
	                                     "refuses to prepare a statement; use error="},
	    {"rows=3 fail=fetch:1 sqlstates=HY000,25P02", "redshift mock: fail= cannot carry 25P02, which Redshift "
	                                                  "gives when it refuses to prepare a statement; use error="},
	    {"error=x sqlstates=08006", "redshift mock: a class 08 record is a lost connection; use lose="},
	    {"rows=1 fail=execute sqlstates=HY000,08S01",
	     "redshift mock: a class 08 record is a lost connection; use lose="},
	    // Any record, not only the one the classifier acts on.
	    {"rows=1 fail=execute sqlstates=22012,08S01",
	     "redshift mock: a class 08 record is a lost connection; use lose="},
	    {"error=x sqlstates=", "redshift mock: sqlstates needs a value"},
	    {"error=x sqlstates=2201", "redshift mock: sqlstates= needs SQLSTATEs of five digits or capital letters, got "
	                               "\"2201\""},
	    {"error=x sqlstates=HY000,,22012", "redshift mock: sqlstates= needs SQLSTATEs of five digits or capital "
	                                       "letters, got \"\""},
	    {"error=x sqlstates=42p01", "redshift mock: sqlstates= needs SQLSTATEs of five digits or capital letters, "
	                                "got \"42p01\""},
	};
	for (auto &c : refused) {
		INFO(c.first);
		auto thrown = Capture([&]() { connection->Describe(c.first); });
		CHECK(thrown.type == ExceptionType::INVALID_INPUT);
		CHECK(thrown.message == c.second);
	}
}

TEST_CASE("the mock refuses Describe and Execute while a result is open, as the real client does", "[mock]") {
	auto log_sink = make_shared_ptr<RecordingLogSink>();
	auto connection = OpenMock(log_sink);
	auto open = ExecuteAsBound(*connection, "rows=3 cols=1");
	for (auto &call : {"Describe", "Execute"}) {
		INFO(call);
		auto refused = Capture([&]() {
			if (string(call) == "Describe") {
				connection->Describe("rows=1 cols=1");
			} else {
				ExecuteAsBound(*connection, "rows=1 cols=1");
			}
		});
		CHECK(refused.type == ExceptionType::INVALID_INPUT);
		CHECK(refused.message == string("redshift mock: ") + call + " while a result on the connection to " +
		                             redshift_test::MockTarget() + " is still open");
	}
	// Nothing reached the server, and the open result still returns every row.
	CHECK(log_sink->Count("redshift mock: executed a statement on a connection to " + redshift_test::MockTarget()) ==
	      1);
	CHECK(BatchSizes(*open) == vector<idx_t> {2, 1});

	// Drained, it no longer counts as open, though it still exists.
	connection->Describe("rows=1 cols=1");
	auto without_rows = ExecuteAsBound(*connection, "norows");
	// A result without rows is never fetched, so it is open until destroyed, and destroying the
	// drained one first does not end its turn.
	open.reset();
	CHECK(Capture([&]() { connection->Describe("rows=1 cols=1"); }).type == ExceptionType::INVALID_INPUT);
	without_rows.reset();
	connection->Describe("rows=1 cols=1");
}

TEST_CASE("a result whose connection is gone fails rather than reach into it", "[mock]") {
	auto connection = OpenMock();
	auto result = ExecuteAsBound(*connection, "rows=3 cols=1");
	connection.reset();
	RedshiftBatch batch;
	auto failed = Capture([&]() { result->NextBatch(batch); });
	CHECK(failed.type == ExceptionType::IO);
	CHECK(failed.message ==
	      "redshift mock: NextBatch on a result whose connection to " + redshift_test::MockTarget() + " was closed");
}

TEST_CASE("batch= and rowcount= shape a result as the real client can", "[mock]") {
	auto connection = OpenMock();
	CHECK(BatchSizes(*ExecuteAsBound(*connection, "rows=10 batch=4 cols=1")) == vector<idx_t> {4, 4, 2});
	CHECK(BatchSizes(*ExecuteAsBound(*connection, "rows=3 cols=1")) == vector<idx_t> {2, 1});
	// Sizes in turn, the list repeating, as buffer sets that end early make them.
	CHECK(BatchSizes(*ExecuteAsBound(*connection, "rows=12 batch=3,1,4 cols=1")) == vector<idx_t> {3, 1, 4, 3, 1});
	CHECK(BatchSizes(*ExecuteAsBound(*connection, "rows=0 cols=1")).empty());
	// A size of 0 is a true, empty batch, which the transport header allows.
	CHECK(BatchSizes(*ExecuteAsBound(*connection, "rows=5 batch=2,0 cols=1")) == vector<idx_t> {2, 0, 2, 0, 1});

	// append adds each batch's rows to those already in the batch it is given, and a drained result
	// leaves the batch empty all the same.
	auto appending = ExecuteAsBound(*connection, "rows=3 batch=1 append cols=1");
	RedshiftBatch batch;
	vector<idx_t> held;
	while (appending->NextBatch(batch)) {
		held.push_back(batch.rows.size());
	}
	CHECK(held == vector<idx_t> {1, 2, 3});
	CHECK(batch.rows.empty());
	appending.reset();

	CHECK(ExecuteAsBound(*connection, "norows rowcount=5")->RowCount() == 5);
	CHECK(ExecuteAsBound(*connection, "norows rowcount=none")->RowCount() == -1);
	CHECK(ExecuteAsBound(*connection, "norows")->RowCount() == 0);
	CHECK(ExecuteAsBound(*connection, "rows=1 cols=1")->RowCount() == -1);
}

TEST_CASE("Execute refuses a statement whose shape is not the bound one, before it runs", "[mock]") {
	auto log_sink = make_shared_ptr<RecordingLogSink>();
	auto connection = OpenMock(log_sink);
	const string executed = "redshift mock: executed a statement on a connection to " + redshift_test::MockTarget();
	const string refused = "redshift mock: the statement's result shape is not the one it was bound with, so it was "
	                       "not run: ";
	// Each pair is the statement bound and the one executed; changed= makes them the same SQL with
	// another shape when it runs, as a table replaced between bind and execution does.
	const std::tuple<string, string, string> cases[] = {
	    {"rows=2 types=int4 changed=varchar", "rows=2 types=int4 changed=varchar",
	     "column 1 was bound as \"c0\" INTEGER and is now \"c0\" VARCHAR"},
	    {"rows=2 types=int4 changed=int4,int4", "rows=2 types=int4 changed=int4,int4",
	     "it was bound with 1 column and now has 2"},
	    {"rows=2 types=int4,int4 changed=int4", "rows=2 types=int4,int4 changed=int4",
	     "it was bound with 2 columns and now has 1"},
	    {"norows changed=int4", "norows changed=int4",
	     "it was bound as a statement without a result set and now returns rows"},
	    {"rows=1 cols=1", "norows", "it was bound as returning rows and now has no result set"},
	    {"types=int4 names=a", "types=int4 names=b", "column 1 was bound as \"a\" INTEGER and is now \"b\" INTEGER"},
	    {"types=numeric:12:2", "types=numeric:12:3",
	     "column 1 was bound as \"c0\" DECIMAL(12,2) and is now \"c0\" DECIMAL(12,3)"},
	    {"types=numeric:12:2", "types=numeric:14:2",
	     "column 1 was bound as \"c0\" DECIMAL(12,2) and is now \"c0\" DECIMAL(14,2)"},
	    {"types=timestamp", "types=timestamptz",
	     "column 1 was bound as \"c0\" TIMESTAMP and is now \"c0\" TIMESTAMPTZ"},
	    // Every column is compared, not only the first, and the first difference is named.
	    {"types=int4,int4,int4", "types=int4,int8,int8",
	     "column 2 was bound as \"c1\" INTEGER and is now \"c1\" BIGINT"},
	    // Names compare exactly, letter case included.
	    {"types=int4 names=a", "types=int4 names=A", "column 1 was bound as \"a\" INTEGER and is now \"A\" INTEGER"},
	    // CHAR to VARCHAR is a change: the two land differently.
	    {"types=char", "types=varchar", "column 1 was bound as \"c0\" CHAR and is now \"c0\" VARCHAR"},
	};
	for (auto &c : cases) {
		INFO(std::get<1>(c));
		auto bound = connection->Describe(std::get<0>(c));
		auto thrown = Capture([&]() { connection->Execute(std::get<1>(c), vector<string>(), bound); });
		CHECK(thrown.type == ExceptionType::BINDER);
		CHECK(thrown.message == refused + std::get<2>(c));
		CHECK(thrown.Extra("redshift_error") == "shape_changed");
		CHECK(thrown.Extra("sqlstate") == "<none>");
		CHECK(thrown.Extra("sqlstates") == "<none>");
		// Nothing ran, and no result is open.
		CHECK(log_sink->Count(executed) == 0);
		CHECK_FALSE(connection->SessionHoldsState());
	}
	// The connection stays usable.
	CHECK(BatchSizes(*ExecuteAsBound(*connection, "rows=1 cols=1")) == vector<idx_t> {1});
	CHECK(log_sink->Count(executed) == 1);
}

TEST_CASE("Execute's refusal names each type as the mock classifies it", "[mock]") {
	auto connection = OpenMock();
	const string refused = "redshift mock: the statement's result shape is not the one it was bound with, so it was "
	                       "not run: column 1 was bound as \"c0\" ";
	// Each type, bound as INTEGER and executed as the type, so the message names both; and INTEGER
	// itself, bound as VARCHAR.
	const std::pair<string, string> types[] = {
	    {"bool", "BOOLEAN"},    {"int2", "SMALLINT"},       {"int8", "BIGINT"},
	    {"float4", "FLOAT"},    {"float8", "DOUBLE"},       {"numeric:12:2", "DECIMAL(12,2)"},
	    {"varchar", "VARCHAR"}, {"date", "DATE"},           {"time", "TIME"},
	    {"timetz", "TIMETZ"},   {"timestamp", "TIMESTAMP"}, {"timestamptz", "TIMESTAMPTZ"},
	    {"varbyte", "BLOB"},    {"geometry", "UNKNOWN"},    {"char", "CHAR"},
	};
	for (auto &type : types) {
		INFO(type.first);
		const string sql = "types=int4 changed=" + type.first;
		auto thrown = Capture([&]() { connection->Execute(sql, vector<string>(), connection->Describe(sql)); });
		CHECK(thrown.message == refused + "INTEGER and is now \"c0\" " + type.second);
	}
	const string sql = "types=varchar changed=int4";
	auto thrown = Capture([&]() { connection->Execute(sql, vector<string>(), connection->Describe(sql)); });
	CHECK(thrown.message == refused + "VARCHAR and is now \"c0\" INTEGER");
}

TEST_CASE("Execute compares DECIMAL precision and scale only, and runs a statement whose shape matches", "[mock]") {
	auto connection = OpenMock();
	// Precision and scale are meaningful only for DECIMAL (the transport header's RedshiftColumn), so
	// only a DECIMAL's are compared.
	auto bound = connection->Describe("rows=1 types=varchar");
	bound.columns[0].precision = 256;
	CHECK(BatchSizes(*connection->Execute("rows=1 types=varchar", vector<string>(), bound)) == vector<idx_t> {1});

	// A caller that bound the shape changed= gives the statement when it runs reads cells in its
	// types' carriers and widths.
	RedshiftStatementShape now;
	now.returns_rows = true;
	now.columns = {RedshiftColumn {"c0", RedshiftTypeId::VARCHAR, 0, 0},
	               RedshiftColumn {"c1", RedshiftTypeId::BIGINT, 0, 0}};
	auto result = connection->Execute("rows=2 types=int4 changed=varchar,int8", vector<string>(), now);
	RedshiftBatch batch;
	REQUIRE(result->NextBatch(batch));
	REQUIRE(batch.rows.size() == 2);
	REQUIRE(batch.rows[1].size() == 2);
	CHECK(batch.rows[1][0].kind == RedshiftValueKind::TEXT);
	CHECK(batch.rows[1][0].text == "r1c0");
	CHECK(batch.rows[1][1].kind == RedshiftValueKind::INTEGER);
	CHECK(batch.rows[1][1].integer == 1);
}

TEST_CASE("unchecked runs a changed statement against the bound shape, as a client that does not compare", "[mock]") {
	auto log_sink = make_shared_ptr<RecordingLogSink>();
	auto connection = OpenMock(log_sink);
	const string executed = "redshift mock: executed a statement on a connection to " + redshift_test::MockTarget();
	const string sql = "rows=2 types=int4 changed=varchar unchecked";
	auto result = connection->Execute(sql, vector<string>(), connection->Describe(sql));
	CHECK(log_sink->Count(executed) == 1);
	CHECK(connection->SessionHoldsState());
	// The cells are the changed shape's, which the scan's own checks must refuse.
	RedshiftBatch batch;
	REQUIRE(result->NextBatch(batch));
	REQUIRE(batch.rows.size() == 2);
	CHECK(batch.rows[0][0].kind == RedshiftValueKind::TEXT);
	CHECK(batch.rows[0][0].text == "r0c0");
	result.reset();

	// A statement bound without a result set that returns rows when it runs gives a result of rows,
	// with no count.
	const string now_rows = "norows changed=int4 unchecked";
	auto rows = connection->Execute(now_rows, vector<string>(), connection->Describe(now_rows));
	CHECK(rows->RowCount() == -1);
	CHECK(BatchSizes(*rows) == vector<idx_t> {1});
}

TEST_CASE("a refusal at Execute's prepare comes before the shape check", "[mock]") {
	auto connection = OpenMock();
	auto thrown = Capture([&]() {
		connection->Execute("error=refused error_at=execute changed=int4", vector<string>(), RedshiftStatementShape());
	});
	CHECK(thrown.type == ExceptionType::BINDER);
	CHECK(thrown.message == "Redshift rejected the statement: refused");
	CHECK(thrown.Extra("redshift_error") == "<none>");
	auto lost = Capture(
	    [&]() { connection->Execute("lose=prepare changed=int4", vector<string>(), RedshiftStatementShape()); });
	CHECK(lost.type == ExceptionType::IO);
}

TEST_CASE("an open result and a lost connection are refused before the shape check", "[mock]") {
	// Either would otherwise read as shape_changed, which says this call did not run.
	// The bound INTEGER shape differs from the VARCHAR one the statement has when it runs.
	const string sql = "rows=1 types=int4 changed=varchar";
	const auto bound = MockDescribedShape(sql);
	auto connection = OpenMock();
	auto open = ExecuteAsBound(*connection, "rows=3 cols=1");
	auto refused = Capture([&]() { connection->Execute(sql, vector<string>(), bound); });
	CHECK(refused.type == ExceptionType::INVALID_INPUT);
	CHECK(refused.Extra("redshift_error") == "<none>");
	open.reset();

	CHECK(Capture([&]() { ExecuteAsBound(*connection, "lose=execute rows=1"); }).type == ExceptionType::IO);
	auto lost = Capture([&]() { connection->Execute(sql, vector<string>(), bound); });
	CHECK(lost.type == ExceptionType::IO);
	CHECK(lost.Extra("redshift_error") == "<none>");
}

TEST_CASE("an injected refusal or loss is the class the transport header names", "[mock]") {
	auto connection = OpenMock();
	CHECK_THROWS_AS(connection->Describe("error=refused sqlstates=42601"), BinderException);
	CHECK_THROWS_AS(ExecuteAsBound(*connection, "error=refused error_at=execute"), BinderException);
	CHECK_THROWS_AS(ExecuteAsBound(*connection, "lose=execute rows=1"), IOException);
}

TEST_CASE("sqlstates= carries the client's error format", "[mock]") {
	auto connection = OpenMock();
	auto refused = Capture([&]() { connection->Describe("error=refused sqlstates=HY000,25P02,42601"); });
	CHECK(refused.type == ExceptionType::BINDER);
	CHECK(refused.Extra("sqlstate") == "25P02");
	CHECK(refused.Extra("sqlstates") == "HY000,25P02,42601");
	CHECK(refused.message == "Redshift rejected the statement: refused\nSQLSTATE: 25P02");

	// A success or warning state counts as HY000, and when every record's
	// state counts as HY000, the state acted on is HY000.
	auto after_warning = Capture([&]() { connection->Describe("error=refused sqlstates=01000,00000,25P02"); });
	CHECK(after_warning.Extra("sqlstate") == "25P02");
	CHECK(after_warning.Extra("sqlstates") == "01000,00000,25P02");
	auto generic = Capture([&]() { connection->Describe("error=refused sqlstates=HY000,HY000"); });
	CHECK(generic.Extra("sqlstate") == "HY000");
	auto only_warnings = Capture([&]() { connection->Describe("error=refused sqlstates=01000,01P01"); });
	CHECK(only_warnings.Extra("sqlstate") == "HY000");
	CHECK(only_warnings.Extra("sqlstates") == "01000,01P01");
	CHECK(only_warnings.message == "Redshift rejected the statement: refused\nSQLSTATE: HY000");

	// Without sqlstates=, a refusal carries none.
	auto bare = Capture([&]() { connection->Describe("error=refused"); });
	CHECK(bare.Extra("sqlstate") == "<none>");
	CHECK(bare.Extra("sqlstates") == "<none>");
	CHECK(bare.message == "Redshift rejected the statement: refused");
}

TEST_CASE("error_at=execute and fail= refuse and fail where the real client does, on a connection that stays",
          "[mock]") {
	auto log_sink = make_shared_ptr<RecordingLogSink>();
	auto connection = OpenMock(log_sink);
	const string executed = "redshift mock: executed a statement on a connection to " + redshift_test::MockTarget();

	// Refused at Execute's prepare, after Describe accepted it: nothing ran.
	connection->Describe("error=refused error_at=execute");
	auto refused = Capture([&]() { ExecuteAsBound(*connection, "error=refused error_at=execute"); });
	CHECK(refused.type == ExceptionType::BINDER);
	CHECK_FALSE(connection->SessionHoldsState());
	CHECK(log_sink->Count(executed) == 0);

	// Failed while it ran: it was sent, so the latch is set.
	auto failed = Capture([&]() { ExecuteAsBound(*connection, "fail=execute sqlstates=22012"); });
	CHECK(failed.type == ExceptionType::EXECUTOR);
	CHECK(failed.Extra("sqlstate") == "22012");
	CHECK(failed.message == "redshift mock: the statement failed while it ran\nSQLSTATE: 22012");
	CHECK(connection->SessionHoldsState());
	CHECK(log_sink->Count(executed) == 1);

	// After N rows, and again on every later call; the result stays open until destroyed.
	auto result = ExecuteAsBound(*connection, "fail=fetch:3 sqlstates=57014 rows=10");
	RedshiftBatch batch;
	REQUIRE(result->NextBatch(batch));
	CHECK(batch.rows.size() == 2);
	REQUIRE(result->NextBatch(batch));
	CHECK(batch.rows.size() == 1);
	for (idx_t attempt = 0; attempt < 2; attempt++) {
		auto at_fetch = Capture([&]() { result->NextBatch(batch); });
		CHECK(at_fetch.type == ExceptionType::EXECUTOR);
		CHECK(at_fetch.Extra("sqlstate") == "57014");
	}
	CHECK(Capture([&]() { connection->Describe("rows=1 cols=1"); }).type == ExceptionType::INVALID_INPUT);
	result.reset();
	CHECK(BatchSizes(*ExecuteAsBound(*connection, "rows=1 cols=1")) == vector<idx_t> {1});
}

TEST_CASE("unusable marks a refusal or a failure session_unusable and loses the connection, as the real client does",
          "[mock]") {
	auto refusing = OpenMock();
	auto refused = Capture([&]() { refusing->Describe("error=x sqlstates=XX000 unusable"); });
	CHECK(refused.type == ExceptionType::BINDER);
	CHECK(refused.Extra("sqlstate") == "XX000");
	CHECK(refused.Extra("redshift_error") == "session_unusable");
	CHECK(Capture([&]() { refusing->Describe("rows=1 cols=1"); }).type == ExceptionType::IO);

	auto failing = OpenMock();
	auto failed = Capture([&]() { ExecuteAsBound(*failing, "fail=execute sqlstates=XX000 unusable"); });
	CHECK(failed.type == ExceptionType::EXECUTOR);
	CHECK(failed.Extra("redshift_error") == "session_unusable");
	CHECK(Capture([&]() { failing->Describe("rows=1 cols=1"); }).type == ExceptionType::IO);

	// Without it, the same refusal keeps the connection.
	auto keeping = OpenMock();
	CHECK(Capture([&]() { keeping->Describe("error=x sqlstates=XX000"); }).Extra("redshift_error") == "<none>");
	CHECK(keeping->Describe("rows=1 cols=1").returns_rows);
}

TEST_CASE("lose= loses the connection where it says, and the connection fails every call after", "[mock]") {
	auto log_sink = make_shared_ptr<RecordingLogSink>();
	const string described = "redshift mock: described a statement on a connection to " + redshift_test::MockTarget();
	const string executed = "redshift mock: executed a statement on a connection to " + redshift_test::MockTarget();

	// A connection opened for the call has not sat idle, so lose=describe describes as usual on it.
	auto fresh = OpenMock(log_sink);
	CHECK(fresh->Describe("lose=describe rows=1 cols=1").returns_rows);
	// Once it has served a call, Describe finds it lost, and it stays lost, whatever the next call
	// asks, without reaching the server; the latch keeps its answer.
	auto lost = Capture([&]() { fresh->Describe("lose=describe sqlstates=08006 rows=1 cols=1"); });
	CHECK(lost.type == ExceptionType::IO);
	CHECK(lost.message ==
	      "redshift mock: lost the connection to " + redshift_test::MockTarget() + " during Describe\nSQLSTATE: 08006");
	CHECK(lost.Extra("sqlstate") == "08006");
	auto describe_after = Capture([&]() { fresh->Describe("rows=1 cols=1"); });
	auto execute_after = Capture([&]() { ExecuteAsBound(*fresh, "rows=1 cols=1"); });
	CHECK(describe_after.type == ExceptionType::IO);
	CHECK(execute_after.type == ExceptionType::IO);
	CHECK(execute_after.message == "redshift mock: the connection to " + redshift_test::MockTarget() + " was lost");
	CHECK(execute_after.Extra("sqlstate") == "<none>");
	CHECK_FALSE(fresh->SessionHoldsState());
	CHECK(log_sink->Count(described) == 1);
	CHECK(log_sink->Count(executed) == 0);

	// lose=describe:any loses a connection opened for the call too.
	auto any = OpenMock();
	CHECK(Capture([&]() { any->Describe("lose=describe:any rows=1 cols=1"); }).type == ExceptionType::IO);

	// Lost while Execute prepares the statement: nothing was sent, and the latch is unchanged.
	auto preparing = OpenMock(log_sink, "us-west-2");
	CHECK(Capture([&]() { ExecuteAsBound(*preparing, "lose=prepare rows=1"); }).type == ExceptionType::IO);
	CHECK_FALSE(preparing->SessionHoldsState());

	// Lost after Execute sent it: the statement may have run, so the latch is set.
	auto running = OpenMock(log_sink);
	CHECK(Capture([&]() { ExecuteAsBound(*running, "lose=execute rows=1"); }).type == ExceptionType::IO);
	CHECK(running->SessionHoldsState());
	CHECK(log_sink->Count(executed) == 1);
	CHECK(Capture([&]() { running->Describe("rows=1 cols=1"); }).type == ExceptionType::IO);

	// Lost after N rows: those come first, then the result and the connection fail every call.
	auto reading = OpenMock();
	auto result = ExecuteAsBound(*reading, "lose=fetch:3 rows=10 batch=2");
	RedshiftBatch batch;
	REQUIRE(result->NextBatch(batch));
	CHECK(batch.rows.size() == 2);
	REQUIRE(result->NextBatch(batch));
	CHECK(batch.rows.size() == 1);
	CHECK(Capture([&]() { result->NextBatch(batch); }).message ==
	      "redshift mock: lost the connection to " + redshift_test::MockTarget() + " while reading the result");
	CHECK(Capture([&]() { result->NextBatch(batch); }).message ==
	      "redshift mock: the connection to " + redshift_test::MockTarget() + " was lost");
	CHECK(Capture([&]() { reading->Describe("rows=1 cols=1"); }).type == ExceptionType::IO);
	CHECK(reading->SessionHoldsState());
}

TEST_CASE("an armed open failure fails the next open only, and logs it", "[mock]") {
	auto log_sink = make_shared_ptr<RecordingLogSink>();
	FailNextMockOpen("redshift mock test: unreachable");
	auto failed = Capture([&]() { OpenMock(log_sink); });
	CHECK(failed.type == ExceptionType::IO);
	CHECK(failed.message == "redshift mock test: unreachable");
	CHECK(log_sink->Count("redshift mock: could not open a connection to " + redshift_test::MockTarget()) == 1);
	CHECK(log_sink->Count(OpenedLine("us-east-1")) == 0);

	auto opened = OpenMock(log_sink);
	CHECK(log_sink->Count(OpenedLine("us-east-1")) == 1);
}

TEST_CASE("the mock logs a caller that reads past the end, or destroys a connection before its result", "[mock]") {
	auto log_sink = make_shared_ptr<RecordingLogSink>();
	const string read_past =
	    "redshift mock: NextBatch was called after the result ended, on a connection to " + redshift_test::MockTarget();
	const string closed_early = "redshift mock: a result outlived its connection to " + redshift_test::MockTarget();

	auto connection = OpenMock(log_sink);
	auto result = ExecuteAsBound(*connection, "rows=1 cols=1");
	CHECK(BatchSizes(*result) == vector<idx_t> {1});
	CHECK(log_sink->Count(read_past) == 0);
	// Asked again after it returned false, the answer is still false, and the line is logged.
	RedshiftBatch batch;
	CHECK_FALSE(result->NextBatch(batch));
	CHECK(batch.rows.empty());
	CHECK(log_sink->Count(read_past) == 1);

	// Destroyed in the right order, the connection logs only its close.
	result.reset();
	connection.reset();
	CHECK(log_sink->Count(closed_early) == 0);
	CHECK(log_sink->Count(ClosedLine("us-east-1")) == 1);

	// Destroyed before a result of it, it logs that too.
	auto early = OpenMock(log_sink);
	auto kept = ExecuteAsBound(*early, "norows");
	early.reset();
	CHECK(log_sink->Count(closed_early) == 1);
}

TEST_CASE("an Execute counts as a served call, and a lost connection fails a drained result too", "[mock]") {
	// A connection whose only call was Execute, as a prepared statement's after a loss is, has served
	// a call, so lose=describe finds it lost.
	auto executed_only = OpenMock();
	ExecuteAsBound(*executed_only, "norows");
	CHECK(Capture([&]() { executed_only->Describe("lose=describe rows=1 cols=1"); }).type == ExceptionType::IO);

	// Once the connection is lost, a result of it fails too, even one that had already ended.
	auto connection = OpenMock();
	auto drained = ExecuteAsBound(*connection, "rows=1 cols=1");
	CHECK(BatchSizes(*drained) == vector<idx_t> {1});
	CHECK(Capture([&]() { ExecuteAsBound(*connection, "lose=execute rows=1"); }).type == ExceptionType::IO);
	RedshiftBatch batch;
	CHECK(Capture([&]() { drained->NextBatch(batch); }).type == ExceptionType::IO);
}

TEST_CASE("a second open failure armed before the next open replaces the first", "[mock]") {
	auto log_sink = make_shared_ptr<RecordingLogSink>();
	FailNextMockOpen("redshift mock test: first");
	FailNextMockOpen("redshift mock test: second");
	auto failed = Capture([&]() { OpenMock(log_sink); });
	CHECK(failed.message == "redshift mock test: second");
	auto opened = OpenMock(log_sink);
	CHECK(log_sink->Count("redshift mock: could not open a connection to " + redshift_test::MockTarget()) == 1);
	CHECK(log_sink->Count(OpenedLine("us-east-1")) == 1);
}

TEST_CASE("Describe names the target and its identifiers, and nothing for UNSPECIFIED", "[mock][config]") {
	RedshiftConfig config;
	config.region = "us-east-1";
	CHECK(config.Describe() == "region us-east-1");
	config.profile = "analytics";
	CHECK(config.Describe() == "region us-east-1, profile analytics");

	config.target = RedshiftTarget::SERVERLESS;
	config.workgroup = "test-wg";
	CHECK(config.Describe() == "region us-east-1, profile analytics, workgroup test-wg");
	config.account_id = "012345678901";
	config.database = "sales";
	CHECK(config.Describe() == "region us-east-1, profile analytics, workgroup test-wg, account 012345678901, "
	                           "database sales");

	RedshiftConfig cluster;
	cluster.region = "eu-west-1";
	cluster.target = RedshiftTarget::PROVISIONED;
	cluster.cluster = "test-cluster";
	CHECK(cluster.Describe() == "region eu-west-1, cluster test-cluster");
	cluster.database = "sales";
	CHECK(cluster.Describe() == "region eu-west-1, cluster test-cluster, database sales");
}

TEST_CASE("Describe shows only the region and profile when the target is UNSPECIFIED", "[mock][config]") {
	// Fields set without a target are not shown, so the text names no target it would not reach.
	auto config = redshift_test::MockConfig();
	config.profile = "analytics";
	config.database = "sales";
	config.cluster = "test-cluster";
	config.target = RedshiftTarget::UNSPECIFIED;
	CHECK(config.Describe() == "region us-east-1, profile analytics");
}

TEST_CASE("Describe names a target value outside the enum as unknown", "[mock][config]") {
	RedshiftConfig config;
	config.region = "eu-west-1";
	config.target = static_cast<RedshiftTarget>(3);
	config.cluster = "test-cluster";
	config.database = "sales";
	CHECK(config.Describe() == "region eu-west-1, unknown target, database sales");
}

TEST_CASE("the mock refuses an UNSPECIFIED target before the open is held or failed", "[mock][config]") {
	auto log_sink = make_shared_ptr<RecordingLogSink>();
	auto config = redshift_test::MockConfig();
	config.target = RedshiftTarget::UNSPECIFIED;
	auto gate = make_shared_ptr<MockGate>();
	HoldNextMockOpen(gate);
	// Released first, so an open that took the gate passes it instead of hanging the case.
	gate->Release();
	FailNextMockOpen("redshift mock test: unreachable");

	auto refused = Capture([&]() { OpenRedshiftConnection(config, log_sink); });
	CHECK(refused.type == ExceptionType::INVALID_INPUT);
	CHECK(refused.message == NO_TARGET);
	CHECK_FALSE(gate->WaitUntilHeldFor(std::chrono::milliseconds(0)));
	// The line the refused configuration would log had it opened.
	CHECK(log_sink->Count("redshift mock: opened a connection to " + config.Describe()) == 0);

	// The refusal used up neither the gate nor the armed failure: the next open takes both.
	auto failed = Capture([&]() { OpenMock(log_sink); });
	CHECK(gate->WaitUntilHeldFor(std::chrono::milliseconds(0)));
	CHECK(failed.message == "redshift mock test: unreachable");
}

TEST_CASE("the mock refuses the AWS credential and port combinations the real client refuses, before the open is "
          "held or failed",
          "[mock][config]") {
	// Each as the real client refuses it.
	RedshiftAwsCredentials credentials {"k", "s", ""};
	auto with_secret = redshift_test::MockConfig();
	with_secret.aws_secret = "creds";
	auto with_secret_and_profile = with_secret;
	with_secret_and_profile.profile = "p";
	RedshiftConfig cluster_with_port;
	cluster_with_port.region = "us-east-1";
	cluster_with_port.target = RedshiftTarget::PROVISIONED;
	cluster_with_port.cluster = "test-cluster";
	cluster_with_port.endpoint_port = 5439;
	RedshiftConfig accountless_with_port;
	accountless_with_port.region = "us-east-1";
	accountless_with_port.target = RedshiftTarget::SERVERLESS;
	accountless_with_port.workgroup = "test-wg";
	accountless_with_port.endpoint_port = 5439;
	const std::pair<RedshiftConfig, optional_ptr<const RedshiftAwsCredentials>> refused_cases[] = {
	    {redshift_test::MockConfig(), &credentials},
	    {with_secret, nullptr},
	    {with_secret_and_profile, &credentials},
	    {cluster_with_port, nullptr},
	    {accountless_with_port, nullptr},
	};
	for (auto &entry : refused_cases) {
		INFO(entry.first.Describe());
		auto log_sink = make_shared_ptr<RecordingLogSink>();
		auto gate = make_shared_ptr<MockGate>();
		HoldNextMockOpen(gate);
		// Released first, so an open that took the gate passes it instead of hanging the case.
		gate->Release();
		FailNextMockOpen("redshift mock test: unreachable");

		auto refused = Capture([&]() { OpenRedshiftConnection(entry.first, entry.second, log_sink); });
		CHECK(refused.type == ExceptionType::INVALID_INPUT);
		CHECK_FALSE(gate->WaitUntilHeldFor(std::chrono::milliseconds(0)));
		CHECK(log_sink->Count("redshift mock: opened a connection to " + entry.first.Describe()) == 0);
		CHECK(log_sink->Count("redshift mock: opened a connection to " + entry.first.Describe() +
		                      " (with AWS credentials)") == 0);
		// The refusal used up neither the gate nor the armed failure.
		auto failed = Capture([&]() { OpenMock(log_sink); });
		CHECK(gate->WaitUntilHeldFor(std::chrono::milliseconds(0)));
		CHECK(failed.message == "redshift mock test: unreachable");
	}

	// Their valid forms open, and record that credentials arrived, never them.
	auto log_sink = make_shared_ptr<RecordingLogSink>();
	auto workgroup_with_port = redshift_test::MockConfig();
	workgroup_with_port.endpoint_port = 5440;
	auto labelled_with_port = accountless_with_port;
	labelled_with_port.endpoint_label = "lbl";
	auto from_secret = OpenRedshiftConnection(with_secret, &credentials, log_sink);
	auto on_port = OpenRedshiftConnection(workgroup_with_port, log_sink);
	auto labelled = OpenRedshiftConnection(labelled_with_port, log_sink);
	CHECK(log_sink->Count("redshift mock: opened a connection to region us-east-1, credentials from AWS secret creds, "
	                      "workgroup test-wg, account 012345678901 (with AWS credentials)") == 1);
	CHECK(log_sink->Count(OpenedLine("us-east-1") + ", port 5440") == 1);
	CHECK(log_sink->Count("redshift mock: opened a connection to " + labelled_with_port.Describe()) == 1);
}

TEST_CASE("the mock refuses a target value outside the enum", "[mock][config]") {
	auto log_sink = make_shared_ptr<RecordingLogSink>();
	auto config = redshift_test::MockConfig();
	config.target = static_cast<RedshiftTarget>(3);

	auto refused = Capture([&]() { OpenRedshiftConnection(config, log_sink); });
	CHECK(refused.type == ExceptionType::INVALID_INPUT);
	CHECK(refused.message == NO_TARGET);
	CHECK(log_sink->Count("redshift mock: opened a connection to " + config.Describe()) == 0);
}

TEST_CASE("the mock opens each target and records its fields", "[mock][config]") {
	auto log_sink = make_shared_ptr<RecordingLogSink>();
	RedshiftConfig cluster;
	cluster.region = "us-east-1";
	cluster.target = RedshiftTarget::PROVISIONED;
	cluster.cluster = "test-cluster";
	cluster.database = "sales";
	RedshiftConfig accountless;
	accountless.region = "us-east-1";
	accountless.target = RedshiftTarget::SERVERLESS;
	accountless.workgroup = "test-wg";

	auto serverless_connection = OpenRedshiftConnection(redshift_test::MockConfig(), log_sink);
	auto cluster_connection = OpenRedshiftConnection(cluster, log_sink);
	auto accountless_connection = OpenRedshiftConnection(accountless, log_sink);

	CHECK(log_sink->Count(OpenedLine("us-east-1")) == 1);
	CHECK(log_sink->Count("redshift mock: opened a connection to region us-east-1, cluster test-cluster, "
	                      "database sales") == 1);
	CHECK(log_sink->Count("redshift mock: opened a connection to region us-east-1, workgroup test-wg") == 1);
	CHECK(cluster_connection->Target() == "mock (region us-east-1, cluster test-cluster, database sales)");

	auto full = redshift_test::MockConfig();
	full.profile = "analytics";
	full.database = "sales";
	auto full_connection = OpenRedshiftConnection(full, log_sink);
	CHECK(full_connection->Target() ==
	      "mock (region us-east-1, profile analytics, workgroup test-wg, account 012345678901, database sales)");
}
