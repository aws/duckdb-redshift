// DETACH against redshift_query in flight on other DuckDB connections: what closes when, that a
// pin taken before DETACH keeps its connection until it ends, that one asked for after DETACH is
// refused, and that each Redshift connection closes exactly once and is never used after its close;
// (the [detach] cases); and what a bystander's next statement gets
// afterwards, races with a re-attach included (the [reattach] cases). Opens,
// closes and calls are read from the lines the mock logs.

#include "catch.hpp"

#include <atomic>
#include <condition_variable>
#include <future>
#include <mutex>

#include "duckdb.hpp"
#include "duckdb/common/string_util.hpp"
#include "duckdb/main/attached_database.hpp"
#include "duckdb/main/database_manager.hpp"
#include "duckdb/main/query_result_stream.hpp"
#include "duckdb/transaction/meta_transaction.hpp"
#include "mock_transport.hpp"
#include "storage/redshift_catalog.hpp"
#include "storage/redshift_client_state.hpp"
#include "storage/redshift_connection_ownership.hpp"
#include "test_log_storage.hpp"
#include "test_util.hpp"

using namespace duckdb;
using redshift_test::BOUND;
using redshift_test::ErrorOf;
using redshift_test::PauseHold;
using redshift_test::Run;

namespace {

using Fixture = redshift_test::LoggedDatabase;

string Use(const string &handle) {
	return "SELECT * FROM redshift_query('" + handle + "', 'rows=1 cols=1')";
}

//! Runs `sql` on `con` on another thread, so the caller can bound how long it takes and release
//! whatever it waits on before joining it.
std::future<string> Start(Connection &con, const string &sql) {
	return std::async(std::launch::async, [&con, sql]() { return ErrorOf(con, sql); });
}

//! Whether `operation` finished within BOUND. The caller releases any gate or hold and then joins it,
//! so an operation that wrongly waits on one fails the case instead of hanging it.
bool FinishesInBound(std::future<string> &operation) {
	return operation.wait_for(BOUND) == std::future_status::ready;
}

//! The position of the first line starting with `prefix` at or after `from`, or the line count.
idx_t FirstLine(const vector<string> &lines, const string &prefix, idx_t from = 0) {
	for (idx_t i = from; i < lines.size(); i++) {
		if (StringUtil::StartsWith(lines[i], prefix)) {
			return i;
		}
	}
	return lines.size();
}

const char *const CLOSED = "redshift mock: closed a connection to ";
//! Execution's refusals of a handle gone since bind: nothing has the name now, or another handle does.
const char *const DETACHED_AFTER_BIND =
    "attach handle \"rs\" was detached, or an ATTACH under its name was rolled back, after this statement was bound";
const char *const REPLACED_AFTER_BIND =
    "attach handle \"rs\" was detached or replaced, or an ATTACH under its name was rolled back, after this statement "
    "was bound, and another handle has the name now";
const char *const OUTLIVED = "redshift mock: a result outlived its connection to ";

} // namespace

TEST_CASE("DETACH closes idle sessions at once, and a streaming one when its stream ends, each once", "[detach]") {
	Fixture f;
	Connection streaming(f.db);
	Connection idle(f.db);
	Connection detaching(f.db);
	Run(idle, Use("rs"));
	Run(detaching, Use("rs"));
	// More rows than one DuckDB chunk, so the first fetch leaves the query, and its pin, open.
	auto stream =
	    make_uniq<QueryResultStream<>>(streaming.Submit("SELECT * FROM redshift_query('rs', 'rows=5000 cols=1')"));
	REQUIRE_FALSE(stream->HasError());
	auto chunk = stream->Fetch();
	REQUIRE(chunk);
	idx_t rows = chunk->size();
	REQUIRE(rows < 5000);
	REQUIRE(f.lines->Opened() == 3);
	const auto executed = f.lines->Executed();
	// A transaction that looks the name up as a catalog keeps the catalog alive past the stream's end,
	// so the close there is the last pin's, not the catalog's destruction.
	Connection holder(f.db);
	Run(holder, "BEGIN");
	CHECK(StringUtil::Contains(ErrorOf(holder, "SELECT * FROM rs.some_table"), "cannot be browsed as a catalog"));

	string removal = "DETACH rs";
	// The replacing ATTACH opens the detaching DuckDB connection's session on the new handle.
	idx_t replacement_opens = 0;
	SECTION("DETACH") {
	}
	SECTION("ATTACH OR REPLACE") {
		removal = redshift_test::AttachOrReplace("rs");
		replacement_opens = 1;
	}
	// DETACH does not wait for the stream's pin. Were it to, the stream's end would let it go, so the
	// case fails rather than hangs.
	auto detach = Start(detaching, removal);
	const bool detached_in_bound = FinishesInBound(detach);
	if (!detached_in_bound) {
		stream.reset();
	}
	REQUIRE(detached_in_bound);
	CHECK(detach.get().empty());
	// The detaching DuckDB connection's session and the idle bystander's close now, though the stream
	// still holds the old handle.
	CHECK(f.lines->Closed() == 2);

	// The stream finishes on its own connection, which nothing has closed until the last chunk.
	chunk = stream->Fetch();
	REQUIRE(chunk);
	rows += chunk->size();
	CHECK(f.lines->Closed() == 2);
	while ((chunk = stream->Fetch())) {
		rows += chunk->size();
	}
	CHECK(rows == 5000);
	// The query ends with its last chunk, which destroys the result and then releases the pin, which
	// closes the connection.
	CHECK(f.lines->Closed() == 3);
	stream.reset();
	Run(holder, "ROLLBACK");
	CHECK(f.lines->Closed() == 3);
	CHECK(f.lines->Opened() == 3 + replacement_opens);
	CHECK(f.lines->Executed() == executed);
	CHECK(f.lines->CountStartingWith(OUTLIVED) == 0);
	// No call follows the first close: each session that closed had nothing left to do.
	auto lines = f.lines->Lines();
	const auto first_close = FirstLine(lines, CLOSED);
	CHECK(FirstLine(lines, "redshift mock: described ", first_close) == lines.size());
	CHECK(FirstLine(lines, "redshift mock: executed ", first_close) == lines.size());
}

TEST_CASE("a bind that found the handle before DETACH, and asks for its pin after, gets the detached error and "
          "makes no transport call",
          "[detach]") {
	Fixture f;
	Connection bystander(f.db);
	Connection detaching(f.db);
	Run(bystander, Use("rs"));
	const auto described = f.lines->Described();
	PauseHold hold;
	hold.Arm();

	// The bystander's bind resolves the handle, then waits before asking for its pin.
	auto use = Start(bystander, Use("rs"));
	const bool held = hold.WaitUntilHeldFor(BOUND);
	// DETACH does not wait for it: the bystander holds no pin, so its idle session closes now.
	auto detach = Start(detaching, "DETACH rs");
	const bool detached_in_bound = FinishesInBound(detach);
	const auto closed_at_detach = f.lines->Closed();
	hold.Release();
	const auto detach_error = detach.get();
	REQUIRE(FinishesInBound(use));
	const auto use_error = use.get();

	CHECK(held);
	CHECK(detached_in_bound);
	CHECK(detach_error.empty());
	CHECK(closed_at_detach == 1);
	// The pin is refused, and the bind gets what a statement bound after the DETACH gets,
	// without opening or describing anything.
	CHECK(StringUtil::Contains(use_error, "no attached database is named \"rs\""));
	CHECK(f.lines->Described() == described);
	CHECK(f.lines->Opened() == 1);
	CHECK(f.lines->Closed() == 1);
}

TEST_CASE("an execution that checked the handle before DETACH, and asks for its pin after, is refused and runs "
          "nothing",
          "[detach]") {
	Fixture f;
	Connection bystander(f.db);
	Connection detaching(f.db);
	Run(bystander, "PREPARE bound AS " + Use("rs"));
	Run(bystander, "EXECUTE bound");
	const auto executed = f.lines->Executed();
	string removal = "DETACH rs";
	string refusal = DETACHED_AFTER_BIND;
	// The replacing ATTACH opens the detaching DuckDB connection's session on the new handle.
	idx_t replacement_opens = 0;
	SECTION("DETACH") {
	}
	SECTION("ATTACH OR REPLACE") {
		// A handle has the name again, so the refusal says to prepare the statement again, not to attach.
		removal = redshift_test::AttachOrReplace("rs");
		refusal = REPLACED_AFTER_BIND;
		replacement_opens = 1;
	}
	PauseHold hold;
	hold.Arm();

	// The execution finds the handle bind used still attached, then waits before asking for its pin:
	// the window between the identity check and Execute.
	auto run = Start(bystander, "EXECUTE bound");
	const bool held = hold.WaitUntilHeldFor(BOUND);
	auto detach = Start(detaching, removal);
	const bool detached_in_bound = FinishesInBound(detach);
	const auto closed_at_detach = f.lines->Closed();
	hold.Release();
	const auto detach_error = detach.get();
	REQUIRE(FinishesInBound(run));
	const auto run_error = run.get();

	CHECK(held);
	CHECK(detached_in_bound);
	CHECK(detach_error.empty());
	CHECK(closed_at_detach == 1);
	// Refused as a statement bound to a detached or replaced handle is: it does not run on the earlier
	// handle, and opens no connection on it.
	INFO(run_error);
	CHECK(StringUtil::Contains(run_error, refusal));
	CHECK(f.lines->Executed() == executed);
	CHECK(f.lines->Opened() == 1 + replacement_opens);
	CHECK(f.lines->Closed() == 1);
}

TEST_CASE("a bind whose pin comes before DETACH uses no connection that finished opening after it", "[detach]") {
	Fixture f;
	Connection bystander(f.db);
	Connection detaching(f.db);
	Run(detaching, Use("rs"));
	auto gate = make_shared_ptr<MockGate>();
	HoldNextMockOpen(gate);

	// The bystander's first use pins its pair, then opens its connection, which waits at the gate:
	// the pin is held, and no connection exists yet.
	auto use = Start(bystander, Use("rs"));
	const bool held = gate->WaitUntilHeldFor(BOUND);
	// DETACH closes the detaching DuckDB connection's idle session, and leaves the pinned pair to its
	// pin.
	auto detach = Start(detaching, "DETACH rs");
	const bool detached_in_bound = FinishesInBound(detach);
	const auto closed_at_detach = f.lines->Closed();
	gate->Release();
	const auto detach_error = detach.get();
	REQUIRE(FinishesInBound(use));
	const auto use_error = use.get();

	CHECK(held);
	CHECK(detached_in_bound);
	CHECK(detach_error.empty());
	CHECK(closed_at_detach == 1);
	// The open finishes after the DETACH, so its connection is closed unused, and the bind gets what a
	// statement bound after the DETACH gets: the pin lets work on an open connection finish, never
	// start on a new one.
	CHECK(StringUtil::Contains(use_error, "no attached database is named \"rs\""));
	CHECK(f.lines->Opened() == 2);
	CHECK(f.lines->Closed() == 2);
	CHECK(f.lines->Described() == 1);
	CHECK(f.lines->Executed() == 1);
}

TEST_CASE("a bind whose Describe lost its connection uses no replacement that finished opening after DETACH",
          "[detach]") {
	Fixture f;
	Connection bystander(f.db);
	Connection detaching(f.db);
	// A bind-only DESCRIBE opens the bystander's connection and runs no statement, so the session holds
	// no state, and a lost Describe on it would be tried again on a replacement.
	Run(bystander, "DESCRIBE " + Use("rs"));
	REQUIRE(f.lines->Opened() == 1);
	auto gate = make_shared_ptr<MockGate>();
	HoldNextMockOpen(gate);

	// The bind's Describe loses the connection, which is discarded, and the replacement waits at the gate
	// with the bind's pin held.
	auto use = Start(bystander, "SELECT * FROM redshift_query('rs', 'lose=describe rows=1 cols=1')");
	const bool held = gate->WaitUntilHeldFor(BOUND);
	const auto closed_while_held = f.lines->Closed();
	auto detach = Start(detaching, "DETACH rs");
	const bool detached_in_bound = FinishesInBound(detach);
	gate->Release();
	const auto detach_error = detach.get();
	REQUIRE(FinishesInBound(use));
	const auto use_error = use.get();

	CHECK(held);
	CHECK(closed_while_held == 1);
	CHECK(detached_in_bound);
	CHECK(detach_error.empty());
	// The replacement opened after the DETACH, so it is closed unused, and nothing is described on it.
	// The bind starts again from the name, which nothing has now; the lost session held no state, so
	// the error says nothing about it.
	CHECK(StringUtil::Contains(use_error, "no attached database is named \"rs\""));
	CHECK(f.lines->Opened() == 2);
	CHECK(f.lines->Closed() == 2);
	CHECK(f.lines->Described() == 1);
	CHECK(f.lines->Executed() == 0);
}

TEST_CASE("a pin on a retired handle's pair with no connection opens none", "[detach]") {
	// Driven on the map directly: through SQL, the gap between a pin and the open is too short to put
	// a DETACH in without a pause point of its own.
	Fixture f;
	Connection con(f.db);
	auto &context = *con.context;
	RedshiftConnectionMap map;
	const auto config = redshift_test::MockConfig();
	const auto connection_id = context.GetConnectionId();
	const auto handle_id = map.NewHandleId();

	REQUIRE(map.Pin(connection_id, handle_id));
	map.RetireHandle(handle_id, Identifier("rs"), DConstants::INVALID_INDEX);
	CHECK_FALSE(map.GetConnection(connection_id, handle_id, config, context));
	CHECK_FALSE(map.Pin(connection_id, handle_id));
	map.Unpin(connection_id, handle_id);
	CHECK(f.lines->Opened() == 0);
	CHECK(f.lines->Closed() == 0);
	map.CloseHandle(handle_id);
}

TEST_CASE("concurrent DETACHes and pins on many DuckDB connections close each connection once", "[detach]") {
	// Each round attaches the handle, gives every DuckDB connection a session on it, and then detaches
	// it while each connection uses it again. Whichever comes first for each, the counts must add up:
	// every opened connection closes exactly once, and no result outlives its connection.
	Fixture f;
	constexpr idx_t CONNECTIONS = 4;
	constexpr idx_t ROUNDS = 20;
	vector<unique_ptr<Connection>> users;
	for (idx_t i = 0; i < CONNECTIONS; i++) {
		users.push_back(make_uniq<Connection>(f.db));
	}
	Connection detaching(f.db);
	Run(detaching, "DETACH rs");
	for (idx_t round = 0; round < ROUNDS; round++) {
		Run(detaching, redshift_test::Attach("rs"));
		for (auto &user : users) {
			// A user whose session on the last round's handle ran a statement, and that has not been told
			// yet, is told once that its state may be lost, and then runs.
			const auto first = ErrorOf(*user, Use("rs"));
			INFO(first);
			CHECK((first.empty() || StringUtil::Contains(first, "a new handle is attached under the name "
			                                                    "now")));
			if (!first.empty()) {
				Run(*user, Use("rs"));
			}
		}
		vector<std::future<string>> uses;
		for (auto &user : users) {
			auto &con = *user;
			uses.push_back(std::async(std::launch::async, [&con]() { return ErrorOf(con, Use("rs")); }));
		}
		auto detach = Start(detaching, "DETACH rs");
		REQUIRE(FinishesInBound(detach));
		CHECK(detach.get().empty());
		for (auto &use : uses) {
			REQUIRE(use.wait_for(BOUND) == std::future_status::ready);
			const auto error = use.get();
			// Either it ran before the DETACH, or it found the handle gone at bind or at execution.
			const bool expected = error.empty() || StringUtil::Contains(error, "no attached database is named") ||
			                      StringUtil::Contains(error, DETACHED_AFTER_BIND);
			INFO(error);
			CHECK(expected);
		}
	}
	users.clear();
	CHECK(f.lines->Opened() == f.lines->Closed());
	CHECK(f.lines->CountStartingWith(OUTLIVED) == 0);
}

TEST_CASE("an execution pinned before DETACH, whose open finishes after it, runs nothing", "[detach]") {
	Fixture f;
	Connection bystander(f.db);
	Connection detaching(f.db);
	Run(bystander, "PREPARE bound AS " + Use("rs"));
	// An execution that loses the connection leaves the pair with none, so the prepared statement's
	// execution opens one after its pin.
	CHECK(StringUtil::Contains(ErrorOf(bystander, "SELECT * FROM redshift_query('rs', 'lose=execute rows=1 cols=1')"),
	                           "was lost while executing"));
	const auto executed = f.lines->Executed();
	const auto opened = f.lines->Opened();
	string removal = "DETACH rs";
	string refusal = DETACHED_AFTER_BIND;
	// The replacing ATTACH opens the detaching DuckDB connection's session on the new handle, which
	// stays open.
	idx_t replacement_opens = 0;
	SECTION("DETACH") {
	}
	SECTION("ATTACH OR REPLACE") {
		// A handle has the name again by the refusal, so execution's second check of the name says so.
		removal = redshift_test::AttachOrReplace("rs");
		refusal = REPLACED_AFTER_BIND;
		replacement_opens = 1;
	}
	auto gate = make_shared_ptr<MockGate>();
	HoldNextMockOpen(gate);

	auto run = Start(bystander, "EXECUTE bound");
	const bool held = gate->WaitUntilHeldFor(BOUND);
	auto detach = Start(detaching, removal);
	const bool detached_in_bound = FinishesInBound(detach);
	gate->Release();
	const auto detach_error = detach.get();
	REQUIRE(FinishesInBound(run));
	const auto run_error = run.get();

	CHECK(held);
	CHECK(detached_in_bound);
	CHECK(detach_error.empty());
	// The connection that finished opening after the removal is closed unused, and the execution is
	// refused as one bound to a detached or replaced handle is.
	INFO(run_error);
	CHECK(StringUtil::Contains(run_error, refusal));
	CHECK(f.lines->Executed() == executed);
	CHECK(f.lines->Opened() == opened + 1 + replacement_opens);
	CHECK(f.lines->Opened() == f.lines->Closed() + replacement_opens);
}

TEST_CASE("the last pin's end closes a detached handle's connection outside the map's lock", "[detach]") {
	Fixture f;
	Connection streaming(f.db);
	Connection other(f.db);
	Run(other, redshift_test::Attach("rs2", "eu-west-1"));
	Run(other, Use("rs2"));
	auto stream =
	    make_uniq<QueryResultStream<>>(streaming.Submit("SELECT * FROM redshift_query('rs', 'rows=5000 cols=1')"));
	REQUIRE_FALSE(stream->HasError());
	REQUIRE(stream->Fetch());
	Run(other, "DETACH rs");
	REQUIRE(f.lines->Closed() == 0);
	auto gate = make_shared_ptr<MockGate>();
	HoldNextMockClose(gate);

	// The stream's end releases its pin, and the pin's end closes the connection, which waits at the
	// gate.
	auto ended = std::async(std::launch::async, [&]() { stream.reset(); });
	const bool held = gate->WaitUntilHeldFor(BOUND);
	// Another DuckDB connection's call on another handle takes the map's lock, which a close made
	// under the lock would still hold.
	auto use = Start(other, Use("rs2"));
	const bool used_in_bound = FinishesInBound(use);
	gate->Release();
	ended.wait();

	CHECK(held);
	CHECK(used_in_bound);
	CHECK(use.get().empty());
	CHECK(f.lines->Closed() == 1);
}

TEST_CASE("a pair whose Describe recovered on a replacement connection still closes at DETACH", "[detach]") {
	// The recovery discards the lost connection while the bind's pin holds the pair, and the pair must
	// keep its pin count through that, or DETACH would take it for pinned and leave it open.
	Fixture f;
	Connection bystander(f.db);
	Connection detaching(f.db);
	Run(bystander, "DESCRIBE " + Use("rs"));
	Run(bystander, "SELECT * FROM redshift_query('rs', 'lose=describe rows=1 cols=1')");
	REQUIRE(f.lines->Opened() == 2);
	REQUIRE(f.lines->Closed() == 1);
	// A transaction that looks the name up as a catalog keeps the catalog alive past the DETACH, so its
	// destruction, which closes every connection left, cannot close the pair instead of the DETACH.
	Connection holder(f.db);
	Run(holder, "BEGIN");
	CHECK(StringUtil::Contains(ErrorOf(holder, "SELECT * FROM rs.some_table"), "cannot be browsed as a catalog"));

	Run(detaching, "DETACH rs");
	CHECK(f.lines->Closed() == 2);
	Run(holder, "ROLLBACK");
	CHECK(f.lines->Closed() == 2);
}

TEST_CASE("a pinned pair keeps its open connection through DETACH until its last pin ends", "[detach]") {
	// Driven on the map directly, with two pins on one pair, which SQL never takes.
	Fixture f;
	Connection con(f.db);
	auto &context = *con.context;
	RedshiftConnectionMap map;
	const auto config = redshift_test::MockConfig();
	const auto connection_id = context.GetConnectionId();
	const auto handle_id = map.NewHandleId();

	REQUIRE(map.Pin(connection_id, handle_id));
	REQUIRE(map.Pin(connection_id, handle_id));
	auto connection = map.GetConnection(connection_id, handle_id, config, context);
	REQUIRE(connection);
	map.RetireHandle(handle_id, Identifier("rs"), DConstants::INVALID_INDEX);
	CHECK(f.lines->Closed() == 0);
	// Work on the open connection finishes: the pinned pair gets the same connection, and opens none.
	CHECK(map.GetConnection(connection_id, handle_id, config, context).get() == connection.get());
	CHECK(f.lines->Opened() == 1);
	map.Unpin(connection_id, handle_id);
	CHECK(f.lines->Closed() == 0);
	map.Unpin(connection_id, handle_id);
	CHECK(f.lines->Closed() == 1);
	map.CloseHandle(handle_id);
	CHECK(f.lines->Closed() == 1);
}

TEST_CASE("a pin that ends after its DuckDB connection's close finds nothing left to close", "[detach]") {
	// Driven on the map directly: SQL never closes a DuckDB connection while one of its queries runs.
	Fixture f;
	Connection con(f.db);
	auto &context = *con.context;
	RedshiftConnectionMap map;
	const auto config = redshift_test::MockConfig();
	const auto connection_id = context.GetConnectionId();
	const auto handle_id = map.NewHandleId();

	REQUIRE(map.Pin(connection_id, handle_id));
	REQUIRE(map.GetConnection(connection_id, handle_id, config, context));
	map.CloseConnection(connection_id);
	CHECK(f.lines->Closed() == 1);
	map.Unpin(connection_id, handle_id);
	CHECK(f.lines->Closed() == 1);
	CHECK(f.lines->Opened() == 1);
}

TEST_CASE("a session a stream holds through DETACH is reported to its DuckDB connection's next statement",
          "[reattach]") {
	// The stream's pin keeps the connection until the stream ends, so its SessionHoldsState() answer is
	// recorded then, not at DETACH. DuckDB refuses a new statement on a DuckDB connection with an open
	// result, so the stream ends, and the record is made, before the next statement binds.
	Fixture f;
	Connection streaming(f.db);
	Connection detaching(f.db);
	auto stream =
	    make_uniq<QueryResultStream<>>(streaming.Submit("SELECT * FROM redshift_query('rs', 'rows=5000 cols=1')"));
	REQUIRE_FALSE(stream->HasError());
	REQUIRE(stream->Fetch());
	auto detach = Start(detaching, "DETACH rs");
	const bool detached_in_bound = FinishesInBound(detach);
	if (!detached_in_bound) {
		stream.reset();
	}
	REQUIRE(detached_in_bound);
	CHECK(detach.get().empty());
	Run(detaching, redshift_test::Attach("rs"));
	CHECK(f.lines->Closed() == 0);
	stream.reset();
	CHECK(f.lines->Closed() == 1);

	const auto next = ErrorOf(streaming, Use("rs"));
	INFO(next);
	CHECK(StringUtil::Contains(next, "a new handle is attached under the name now"));
	// The stream's, and the ATTACH's own on the new handle.
	CHECK(f.lines->Opened() == 2);
	// Told once: the statement after runs on a fresh session on the new handle.
	CHECK(ErrorOf(streaming, Use("rs")).empty());
	CHECK(f.lines->Opened() == 3);
}

TEST_CASE("a bind held before its pin while the handle is detached and attached again follows the matrix",
          "[reattach]") {
	// The bystander resolves the old handle and waits before its pin; another DuckDB connection detaches
	// the handle and attaches a new one under the name. The pin is refused, and the bind starts again
	// from the name, now the new handle's.
	for (const bool held_state : {true, false}) {
		INFO("the bystander's session ran a statement: " << held_state);
		Fixture f;
		Connection bystander(f.db);
		Connection detaching(f.db);
		// A statement run sets the mock's latch; a bind-only DESCRIBE does not.
		Run(bystander, held_state ? Use("rs") : "DESCRIBE " + Use("rs"));
		const auto described = f.lines->Described();
		PauseHold hold;
		hold.Arm();

		auto use = Start(bystander, Use("rs"));
		const bool held = hold.WaitUntilHeldFor(BOUND);
		auto reattach = Start(detaching, "DETACH rs; " + redshift_test::Attach("rs"));
		const bool reattached_in_bound = FinishesInBound(reattach);
		hold.Release();
		const auto reattach_error = reattach.get();
		REQUIRE(FinishesInBound(use));
		const auto use_error = use.get();

		CHECK(held);
		CHECK(reattached_in_bound);
		CHECK(reattach_error.empty());
		CHECK(f.lines->Closed() == 1);
		if (held_state) {
			// Told once, before the bystander opens anything on the new handle: the second open is the
			// ATTACH's own.
			CHECK(StringUtil::Contains(use_error, "a new handle is attached under the name now"));
			CHECK(f.lines->Opened() == 2);
			CHECK(f.lines->Described() == described);
		} else {
			// Nothing was lost, so the statement runs on a fresh session on the new handle, silently.
			CHECK(use_error.empty());
			CHECK(f.lines->Opened() == 3);
			CHECK(f.lines->Described() == described + 1);
		}
	}
}

TEST_CASE("a bind pinned before DETACH and attach again, with no connection yet, starts again on the new handle",
          "[reattach]") {
	// The pin is granted and the open waits at the gate; the handle is detached and attached again
	// meanwhile. The open finishes after the DETACH, so its connection is closed unused, and the bind
	// starts again from the name: the bystander had no session before, so it proceeds on the new handle.
	Fixture f;
	Connection bystander(f.db);
	Connection detaching(f.db);
	auto gate = make_shared_ptr<MockGate>();
	HoldNextMockOpen(gate);

	auto use = Start(bystander, Use("rs"));
	const bool held = gate->WaitUntilHeldFor(BOUND);
	auto reattach = Start(detaching, "DETACH rs; " + redshift_test::Attach("rs"));
	const bool reattached_in_bound = FinishesInBound(reattach);
	gate->Release();
	const auto reattach_error = reattach.get();
	REQUIRE(FinishesInBound(use));
	const auto use_error = use.get();

	CHECK(held);
	CHECK(reattached_in_bound);
	CHECK(reattach_error.empty());
	CHECK(use_error.empty());
	// The held open's connection closed unused; the ATTACH opened the detaching DuckDB connection's
	// session on the new handle, and the bystander's opened, described and executed.
	CHECK(f.lines->Opened() == 3);
	CHECK(f.lines->Closed() == 1);
	CHECK(f.lines->Described() == 1);
	CHECK(f.lines->Executed() == 1);
}

TEST_CASE("a bind whose Describe lost a stateless connection while the handle was replaced starts again on the "
          "new handle",
          "[reattach]") {
	// The lost session held no state, so Describe would try once more on a replacement;
	// the replacement waits at the gate while the handle is detached and attached again. It is
	// closed unused, and the bind starts again from the name: nothing was lost, so it proceeds on the
	// new handle, silently.
	Fixture f;
	Connection bystander(f.db);
	Connection detaching(f.db);
	Run(bystander, "DESCRIBE " + Use("rs"));
	auto gate = make_shared_ptr<MockGate>();
	HoldNextMockOpen(gate);

	auto use = Start(bystander, "SELECT * FROM redshift_query('rs', 'lose=describe rows=1 cols=1')");
	const bool held = gate->WaitUntilHeldFor(BOUND);
	auto reattach = Start(detaching, "DETACH rs; " + redshift_test::Attach("rs"));
	const bool reattached_in_bound = FinishesInBound(reattach);
	gate->Release();
	const auto reattach_error = reattach.get();
	REQUIRE(FinishesInBound(use));
	const auto use_error = use.get();

	CHECK(held);
	CHECK(reattached_in_bound);
	CHECK(reattach_error.empty());
	CHECK(use_error.empty());
	// The lost connection and the unused replacement closed; the ATTACH opened its own session on the
	// new handle, and the bystander's opened and ran.
	CHECK(f.lines->Opened() == 4);
	CHECK(f.lines->Closed() == 2);
	CHECK(f.lines->Executed() == 1);
}

TEST_CASE("a bind whose Describe lost its connection after DETACH opens no replacement and logs none", "[reattach]") {
	// The lost connection's close waits at the gate, so DETACH lands before Describe decides whether
	// to open a replacement. It opens none, and says nothing about opening one; the bind starts again
	// from the name, which nothing has now.
	Fixture f;
	Connection bystander(f.db);
	Connection detaching(f.db);
	Run(bystander, "DESCRIBE " + Use("rs"));
	auto gate = make_shared_ptr<MockGate>();
	HoldNextMockClose(gate);

	auto use = Start(bystander, "SELECT * FROM redshift_query('rs', 'lose=describe rows=1 cols=1')");
	const bool held = gate->WaitUntilHeldFor(BOUND);
	auto detach = Start(detaching, "DETACH rs");
	const bool detached_in_bound = FinishesInBound(detach);
	gate->Release();
	const auto detach_error = detach.get();
	REQUIRE(FinishesInBound(use));
	const auto use_error = use.get();

	CHECK(held);
	CHECK(detached_in_bound);
	CHECK(detach_error.empty());
	CHECK(StringUtil::Contains(use_error, "no attached database is named \"rs\""));
	CHECK(f.lines->Opened() == 1);
	CHECK(f.lines->Closed() == 1);
	CHECK(f.lines->CountStartingWith("redshift_query: the Redshift connection of attach handle \"rs\" was lost "
	                                 "during Describe") == 0);
}

TEST_CASE("a bind gives up when the handle it starts again on is detached too", "[reattach]") {
	// Each time the bind finds the handle, another DuckDB connection detaches it and attaches a new one
	// before the bind's pin, so each pin is refused.
	Fixture f;
	Connection bystander(f.db);
	Connection admin(f.db);
	std::atomic<idx_t> pauses {0};
	SetRedshiftPinPauseForTesting([&]() {
		// At most five, so a bind that does not give up still ends, and the count says how many it made.
		// ErrorOf, not Run: Catch2's checks are not for other threads.
		if (pauses++ < 5) {
			ErrorOf(admin, "DETACH rs");
			ErrorOf(admin, redshift_test::Attach("rs"));
		}
	});
	auto use = Start(bystander, Use("rs"));
	const bool used_in_bound = FinishesInBound(use);
	SetRedshiftPinPauseForTesting(nullptr);
	REQUIRE(used_in_bound);
	const auto use_error = use.get();

	CHECK(StringUtil::Contains(use_error, "redshift_query: attach handle \"rs\" was detached or replaced, or an "
	                                      "ATTACH under its name was rolled back, twice while this statement was "
	                                      "being bound, so it was not described on Redshift."));
	CHECK(pauses.load() == 2);
	// Only the two ATTACHes' own sessions, each closed by the DETACH after it or left on the last handle.
	CHECK(f.lines->Opened() == 2);
}

TEST_CASE("a bind finds its session on a handle DuckDB took out of the list before anything retired it", "[reattach]") {
	// Driven on the database's own map, with the list of handles TakeLostSession is given standing in
	// for DuckDB's list in the moment between its removing the name and its OnDetach.
	Fixture f;
	Connection bystander(f.db);
	Run(bystander, Use("rs"));
	auto &state = RedshiftClientState::Get(*bystander.context);
	auto &map = *state.ConnectionMap();
	const auto id = state.ConnectionId();
	auto db = DatabaseManager::Get(*bystander.context).GetDatabase(Identifier("rs"));
	REQUIRE(db);
	const auto handle_id = db->GetCatalog().Cast<RedshiftCatalog>().HandleId();

	// While the list still has the handle, nothing is taken.
	unordered_set<idx_t> listed;
	listed.insert(handle_id);
	CHECK_FALSE(map.TakeLostSession(id, Identifier("rs"), listed));
	CHECK(f.lines->Closed() == 0);
	// Out of the list: the session, which ran a statement, is taken once, and closed.
	CHECK(map.TakeLostSession(id, Identifier("RS"), {}));
	CHECK(f.lines->Closed() == 1);
	CHECK_FALSE(map.TakeLostSession(id, Identifier("rs"), {}));
	// Its retirement by another DuckDB connection after this finds no session left to record.
	Connection detaching(f.db);
	Run(detaching, "DETACH rs");
	CHECK_FALSE(map.TakeLostSession(id, Identifier("rs"), {}));
	CHECK(f.lines->Closed() == 1);
}

TEST_CASE("a bind in that moment takes only its own DuckDB connection's session, and one that held no state silently",
          "[reattach]") {
	// As above, on the database's own map. Two DuckDB connections have idle sessions on the handle: one
	// ran a statement, the other only bound.
	Fixture f;
	Connection ran(f.db);
	Connection described(f.db);
	Run(ran, Use("rs"));
	Run(described, "DESCRIBE " + Use("rs"));
	REQUIRE(f.lines->Opened() == 2);
	auto &ran_state = RedshiftClientState::Get(*ran.context);
	auto &map = *ran_state.ConnectionMap();
	const auto described_id = RedshiftClientState::Get(*described.context).ConnectionId();

	// The session that only bound closes, and is not reported; the other
	// DuckDB connection's session stays for that connection's own next bind.
	CHECK_FALSE(map.TakeLostSession(described_id, Identifier("rs"), {}));
	CHECK(f.lines->Closed() == 1);
	CHECK(map.TakeLostSession(ran_state.ConnectionId(), Identifier("rs"), {}));
	CHECK(f.lines->Closed() == 2);
}

TEST_CASE("a bind whose lookup comes before DETACH, and whose check for a lost session comes after, looks again",
          "[reattach]") {
	// The bystander's first lookup finds the handle, and the bind waits before the map's lock while
	// another DuckDB connection's DETACH completes and records the bystander's session, which ran a
	// statement. The bind takes that record, and resolves the name again rather than use the handle
	// its first lookup found, which the DETACH retired.
	Fixture f;
	Connection bystander(f.db);
	Connection detaching(f.db);
	Run(bystander, Use("rs"));
	PauseHold hold(SetRedshiftLostSessionPauseForTesting);
	hold.Arm();

	auto use = Start(bystander, Use("rs"));
	const bool held = hold.WaitUntilHeldFor(BOUND);
	auto detach = Start(detaching, "DETACH rs");
	const bool detached_in_bound = FinishesInBound(detach);
	hold.Release();
	const auto detach_error = detach.get();
	REQUIRE(FinishesInBound(use));
	const auto use_error = use.get();

	CHECK(held);
	CHECK(detached_in_bound);
	CHECK(detach_error.empty());
	INFO(use_error);
	CHECK(StringUtil::Contains(use_error, "no attached database is named \"rs\""));
	CHECK(StringUtil::Contains(use_error, "that session may have held state, which is now lost."));
	CHECK(f.lines->Opened() == 1);
	CHECK(f.lines->Closed() == 1);
}

TEST_CASE("a bystander's prepared redshift_query does not keep the detached handle", "[detach]") {
	// The bind data does not hold the handle, so DETACH destroys it while a statement
	// bound on it is still prepared. The next case covers the transaction half.
	Fixture f;
	Connection bystander(f.db);
	Connection detaching(f.db);
	weak_ptr<AttachedDatabase> handle;
	{
		auto db = DatabaseManager::Get(*bystander.context).GetDatabase(Identifier("rs"));
		REQUIRE(db);
		handle = db;
	}
	auto prepared = bystander.Prepare(Use("rs"));
	REQUIRE_FALSE(prepared->HasError());
	Run(detaching, "DETACH rs");
	CHECK(handle.expired());
}

TEST_CASE("a DuckDB transaction that ran redshift_query does not keep the detached handle", "[detach]") {
	// The transaction half, which only the option reaches: ResolveHandle never adds the
	// handle to the transaction, so DETACH on another DuckDB connection destroys it and closes the
	// session while the transaction is still open.
	Fixture f;
	Connection bystander(f.db);
	Connection detaching(f.db);
	weak_ptr<AttachedDatabase> handle;
	{
		auto db = DatabaseManager::Get(*bystander.context).GetDatabase(Identifier("rs"));
		REQUIRE(db);
		handle = db;
	}
	Run(bystander, "SET redshift_allow_query_in_duckdb_transaction = true");
	Run(bystander, "BEGIN");
	Run(bystander, Use("rs"));
	REQUIRE(f.lines->Opened() == 1);
	Run(detaching, "DETACH rs");
	CHECK(handle.expired());
	CHECK(f.lines->Closed() == 1);
	Run(bystander, "ROLLBACK");
}

TEST_CASE("a name whose detached Redshift handle this transaction still holds is not a handle for redshift_query",
          "[detach]") {
	// A transaction that used the handle as a database keeps it under its name after another DuckDB
	// connection detaches it. redshift_query looks the name up in the database list, so it does not
	// find it, and the error is the unresolved-handle one, not a dereference of nothing.
	// The gate refuses inside an explicit transaction by default, so the statement runs in it the one
	// way that passes the default gate: the client turns autocommit back on without ending the
	// transaction. DuckDB then commits it if the statement succeeds and rolls it back if it fails, as
	// this one does.
	Fixture f;
	Connection bystander(f.db);
	Connection detaching(f.db);
	bystander.SetAutoCommit(false);
	{
		auto db = DatabaseManager::Get(*bystander.context).GetDatabase(Identifier("rs"));
		REQUIRE(db);
		bystander.context->RunFunctionInTransaction(
		    [&]() { MetaTransaction::Get(*bystander.context).UseDatabase(db); });
	}
	Run(detaching, "DETACH rs");
	bystander.SetAutoCommit(true);
	REQUIRE(bystander.context->transaction.HasActiveTransaction());
	CHECK(StringUtil::Contains(ErrorOf(bystander, Use("rs")), "no attached database is named \"rs\""));
}

TEST_CASE("a DuckDB connection that closes with its ATTACH not committed takes the handle away, and its bystander is "
          "told",
          "[reattach]") {
	Fixture f;
	Connection bystander(f.db);
	{
		Connection attaching(f.db);
		Run(attaching, "BEGIN");
		Run(attaching, redshift_test::Attach("rs_tx"));
		Run(bystander, Use("rs_tx"));
	}
	CHECK(
	    StringUtil::Contains(ErrorOf(bystander, Use("rs_tx")), "that session may have held state, which is now lost."));
	CHECK(StringUtil::Contains(ErrorOf(bystander, Use("rs_tx")), "no attached database is named \"rs_tx\""));
}

TEST_CASE("a bind compares the name a handle has now, after ALTER DATABASE renamed it", "[reattach]") {
	Fixture f;
	Connection bystander(f.db);
	Run(bystander, Use("rs"));
	Run(bystander, "ALTER DATABASE rs SET ALIAS TO rs_renamed");
	auto &state = RedshiftClientState::Get(*bystander.context);
	auto &map = *state.ConnectionMap();
	const auto id = state.ConnectionId();

	// Taken out of the list under its new name, the session is found by that name, not the old one.
	CHECK_FALSE(map.TakeLostSession(id, Identifier("rs"), {}));
	CHECK(map.TakeLostSession(id, Identifier("rs_renamed"), {}));
	CHECK(f.lines->Closed() == 1);
}

TEST_CASE("the map records a bystander's lost session once, never the detaching connection's, and forgets it on "
          "close",
          "[reattach]") {
	// Driven on the map directly, with two DuckDB connections' IDs. Only a session that may have held
	// state is recorded: one that held none needs no report.
	Fixture f;
	Connection one(f.db);
	Connection two(f.db);
	auto &context = *one.context;
	RedshiftConnectionMap map;
	const auto config = redshift_test::MockConfig();
	const auto bystander = one.context->GetConnectionId();
	const auto detaching = two.context->GetConnectionId();
	const Identifier name("rs");
	// Executing a statement sets the mock's latch, as the client does.
	auto run_statement = [&](connection_t connection_id, idx_t handle_id) {
		auto connection = map.GetConnection(connection_id, handle_id, config, context);
		REQUIRE(connection);
		connection->Execute("rows=1 cols=1", vector<string>(), MockDescribedShape("rows=1 cols=1"));
	};

	auto handle_id = map.NewHandleId();
	run_statement(bystander, handle_id);
	run_statement(detaching, handle_id);
	// A third pair that only opened, so its session holds no state.
	const auto idle = detaching + 1000;
	REQUIRE(map.GetConnection(idle, handle_id, config, context));
	map.RetireHandle(handle_id, name, detaching);
	CHECK_FALSE(map.TakeLostSession(detaching, name, {}));
	CHECK_FALSE(map.TakeLostSession(idle, name, {}));
	CHECK(map.TakeLostSession(bystander, Identifier("RS"), {}));
	// Taken once.
	CHECK_FALSE(map.TakeLostSession(bystander, name, {}));
	map.CloseHandle(handle_id);

	// A record the bystander never takes goes when its DuckDB connection closes.
	handle_id = map.NewHandleId();
	run_statement(bystander, handle_id);
	map.RetireHandle(handle_id, name, detaching);
	map.CloseConnection(bystander);
	CHECK_FALSE(map.TakeLostSession(bystander, name, {}));
	map.CloseHandle(handle_id);
	CHECK(f.lines->Opened() == f.lines->Closed());
}
