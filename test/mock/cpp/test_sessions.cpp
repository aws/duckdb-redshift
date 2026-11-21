// The Redshift connections redshift_query keeps, one per (attach handle, DuckDB connection): whose
// close closes which, that each closes exactly once whichever path closes it, and that opening and
// closing one happen outside the map's lock. Opens and closes are read from the lines the mock
// logs, through a log storage the test owns, so the counts outlive the database.

#include "catch.hpp"

#include <atomic>
#include <future>
#include <thread>

#include "duckdb.hpp"
#include "duckdb/common/string_util.hpp"
#include "duckdb/main/attached_database.hpp"
#include "duckdb/main/database_manager.hpp"
#include "duckdb/main/query_result_stream.hpp"
#include "mock_transport.hpp"
#include "redshift_attach.hpp"
#include "redshift_extension.hpp"
#include "storage/redshift_catalog.hpp"
#include "storage/redshift_client_state.hpp"
#include "storage/redshift_connection_ownership.hpp"
#include "test_log_storage.hpp"
#include "test_util.hpp"

using namespace duckdb;
using redshift_test::Attach;
using redshift_test::BOUND;
using redshift_test::ErrorOf;
using redshift_test::Run;

namespace {

string Use(const string &handle) {
	return "SELECT * FROM redshift_query('" + handle + "', 'rows=1 cols=1')";
}

using Fixture = redshift_test::LoggedDatabase;

} // namespace

TEST_CASE("closing a DuckDB connection closes its sessions on every handle, and no other's", "[session]") {
	Fixture f;
	Connection setup(f.db);
	Run(setup, Attach("rs_other", "eu-west-1"));
	auto closing = make_uniq<Connection>(f.db);
	Connection staying(f.db);
	Run(*closing, Use("rs"));
	Run(*closing, Use("rs_other"));
	Run(staying, Use("rs"));
	// The ATTACH opened setup's session on rs_other, which stays open with setup.
	REQUIRE(f.lines->Opened() == 4);

	closing.reset();
	CHECK(f.lines->Closed() == 2);

	// The other connection's session is still open, and still the one it uses.
	Run(staying, Use("rs"));
	CHECK(f.lines->Opened() == 4);
	CHECK(f.lines->Closed() == 2);
}

TEST_CASE("a DuckDB connection closing while its handle is detached closes each session once", "[session]") {
	Fixture f;
	Connection detaching(f.db);
	Run(detaching, "DETACH rs");
	// Either path can take the closing connection's session first: its own close, or DETACH, whose
	// retirement of the handle (OnDetach) takes it. Many rounds cover both orders.
	for (idx_t round = 0; round < 50; round++) {
		Run(detaching, Attach("rs"));
		auto closing = make_uniq<Connection>(f.db);
		Run(*closing, Use("rs"));
		Run(detaching, Use("rs"));
		const auto closed_before = f.lines->Closed();

		std::atomic<bool> go {false};
		std::thread closer([&]() {
			while (!go) {
				std::this_thread::yield();
			}
			closing.reset();
		});
		go = true;
		const auto detach_error = ErrorOf(detaching, "DETACH rs");
		closer.join();

		INFO("round " << round);
		CHECK(detach_error.empty());
		CHECK(f.lines->Closed() == closed_before + 2);
	}
	CHECK(f.lines->Opened() == f.lines->Closed());
}

TEST_CASE("a DuckDB connection's slow close blocks no other DuckDB connection's use of the handle", "[session]") {
	Fixture f;
	auto closing = make_uniq<Connection>(f.db);
	Connection reusing(f.db);
	Connection arriving(f.db);
	Run(*closing, Use("rs"));
	Run(reusing, Use("rs"));
	auto gate = make_shared_ptr<MockGate>();
	HoldNextMockClose(gate);

	std::thread closer([&]() { closing.reset(); });
	const bool held = gate->WaitUntilHeldFor(BOUND);
	// Reusing a Redshift connection and adding one both take the map's lock, so each would wait for
	// a close made under it. Bounded, so such a wait fails the case instead of hanging it; the release below
	// ends it.
	auto reused = std::async(std::launch::async, [&]() { return ErrorOf(reusing, Use("rs")); });
	const auto reused_status = reused.wait_for(BOUND);
	auto added = std::async(std::launch::async, [&]() { return ErrorOf(arriving, Use("rs")); });
	const auto added_status = added.wait_for(BOUND);
	const auto closes_while_held = f.lines->Closed();
	gate->Release();
	closer.join();

	CHECK(held);
	CHECK(reused_status == std::future_status::ready);
	CHECK(reused.get().empty());
	CHECK(added_status == std::future_status::ready);
	CHECK(added.get().empty());
	CHECK(closes_while_held == 0);
	CHECK(f.lines->Opened() == 3);
	CHECK(f.lines->Closed() == 1);
}

TEST_CASE("a DuckDB connection's slow open blocks no other DuckDB connection's use of the handle", "[session]") {
	Fixture f;
	Connection opening(f.db);
	Connection reusing(f.db);
	Connection arriving(f.db);
	Run(reusing, Use("rs"));
	auto gate = make_shared_ptr<MockGate>();
	HoldNextMockOpen(gate);

	// The opening connection's first use of the handle waits at the gate, inside the open.
	auto opened = std::async(std::launch::async, [&]() { return ErrorOf(opening, Use("rs")); });
	const bool held = gate->WaitUntilHeldFor(BOUND);
	// Reusing a Redshift connection and adding one both take the map's lock, so each would wait for
	// an open made under it. Bounded, so such a wait fails the case instead of hanging it; the release
	// below ends it. The gate holds one open, so the arriving connection's own open goes through.
	auto reused = std::async(std::launch::async, [&]() { return ErrorOf(reusing, Use("rs")); });
	const auto reused_status = reused.wait_for(BOUND);
	auto added = std::async(std::launch::async, [&]() { return ErrorOf(arriving, Use("rs")); });
	const auto added_status = added.wait_for(BOUND);
	const auto opens_while_held = f.lines->Opened();
	gate->Release();
	const auto opened_error = opened.get();

	CHECK(held);
	CHECK(reused_status == std::future_status::ready);
	CHECK(reused.get().empty());
	CHECK(added_status == std::future_status::ready);
	CHECK(added.get().empty());
	// The reusing and arriving connections' opens, not the held one.
	CHECK(opens_while_held == 2);
	CHECK(opened_error.empty());
	CHECK(f.lines->Opened() == 3);
}

TEST_CASE("a connection lost by one DuckDB connection is closed outside the map's lock, and no other's", "[session]") {
	Fixture f;
	Connection losing(f.db);
	Connection other(f.db);
	Run(losing, Use("rs"));
	Run(other, Use("rs"));
	auto gate = make_shared_ptr<MockGate>();
	HoldNextMockClose(gate);

	// The lost connection is taken out of the map, then closed, and the close waits at the gate.
	auto lost = std::async(std::launch::async, [&]() {
		return ErrorOf(losing, "SELECT * FROM redshift_query('rs', 'lose=execute rows=1')");
	});
	const bool held = gate->WaitUntilHeldFor(BOUND);
	// The other DuckDB connection's session on the handle is untouched, and using it takes the map's
	// lock, which a close made under the lock would still hold. Bounded, so that fails the case
	// instead of hanging it; the release below ends it.
	auto reused = std::async(std::launch::async, [&]() { return ErrorOf(other, Use("rs")); });
	const auto reused_status = reused.wait_for(BOUND);
	gate->Release();
	const auto lost_error = lost.get();

	CHECK(held);
	CHECK(reused_status == std::future_status::ready);
	CHECK(reused.get().empty());
	CHECK(StringUtil::Contains(lost_error, "was lost while executing the statement"));
	CHECK(f.lines->Opened() == 2);
	CHECK(f.lines->Closed() == 1);
	// The losing DuckDB connection's next call opens a new session; the other's is still its first.
	Run(losing, Use("rs"));
	Run(other, Use("rs"));
	CHECK(f.lines->Opened() == 3);
	CHECK(f.lines->Closed() == 1);
}

TEST_CASE("two first uses of one pair that open at once keep one session and close the other", "[session]") {
	// Driven on the map directly: through SQL, the same-handle fence keeps two scans of one statement
	// off one pair, so nothing reaches this.
	Fixture f;
	Connection con(f.db);
	auto &context = *con.context;
	RedshiftConnectionMap map;
	const auto config = redshift_test::MockConfig();
	const auto connection_id = context.GetConnectionId();
	const auto handle_id = map.NewHandleId();
	auto gate = make_shared_ptr<MockGate>();
	HoldNextMockOpen(gate);
	// Nothing has opened yet.
	CHECK_FALSE(gate->WaitUntilHeldFor(std::chrono::milliseconds(0)));

	// The first use opens outside the map's lock and waits at the gate, before its connection exists.
	bool held_opened = true;
	auto held_use = std::async(std::launch::async, [&]() {
		return map.GetConnection(connection_id, handle_id, config, context, &held_opened).get();
	});
	const bool held = gate->WaitUntilHeldFor(BOUND);
	if (!held) {
		// Released at once, so nothing below waits for an open that never came.
		gate->Release();
	}
	// The second finds no entry, opens past the gate, which holds one open only, and inserts its own.
	// Bounded, while the gate holds the first, so an open made under the map's lock fails the case
	// instead of hanging it.
	bool inserted_opened = false;
	auto inserted_use = std::async(std::launch::async, [&]() {
		return map.GetConnection(connection_id, handle_id, config, context, &inserted_opened).get();
	});
	const auto inserted_status = inserted_use.wait_for(BOUND);
	const auto closes_while_held = f.lines->Closed();
	// The held open then finds the slot filled: it returns the connection already there and destroys
	// its own, whose close is held here. Another map call meanwhile shows the close is made outside
	// the map's lock; bounded, so one made under it fails the case.
	auto close_gate = make_shared_ptr<MockGate>();
	HoldNextMockClose(close_gate);
	gate->Release();
	const bool close_held = close_gate->WaitUntilHeldFor(BOUND);
	auto other_call = std::async(std::launch::async, [&]() { return map.IsRetired(handle_id); });
	const auto other_status = other_call.wait_for(BOUND);
	close_gate->Release();
	auto found = held_use.get();
	auto inserted = inserted_use.get();
	other_call.wait();

	CHECK(held);
	CHECK(inserted_status == std::future_status::ready);
	CHECK(closes_while_held == 0);
	CHECK(close_held);
	CHECK(other_status == std::future_status::ready);
	CHECK(found == inserted);
	// Only the call whose connection the pair keeps reports that it opened it.
	CHECK(inserted_opened);
	CHECK_FALSE(held_opened);
	CHECK(f.lines->Opened() == 2);
	CHECK(f.lines->Closed() == 1);
	// One session is left in the map for the pair.
	map.CloseConnection(connection_id);
	CHECK(f.lines->Closed() == 2);
}

TEST_CASE("a discard of another connection, or of a pair with none, takes nothing out of the map", "[session]") {
	// Driven on the map directly: redshift_query discards only the connection it just used.
	Fixture f;
	Connection con(f.db);
	auto &context = *con.context;
	RedshiftConnectionMap map;
	const auto config = redshift_test::MockConfig();
	const auto connection_id = context.GetConnectionId();
	const auto handle_id = map.NewHandleId();
	auto &kept = *map.GetConnection(connection_id, handle_id, config, context);
	auto stray = OpenRedshiftConnection(config, make_shared_ptr<redshift_test::DroppingLogSink>());

	CHECK_FALSE(map.Discard(connection_id, handle_id, *stray));
	CHECK_FALSE(map.Discard(connection_id, map.NewHandleId(), kept));
	CHECK(f.lines->Closed() == 0);
	bool opened = true;
	CHECK(map.GetConnection(connection_id, handle_id, config, context, &opened).get() == &kept);
	CHECK_FALSE(opened);
	// The pair's own connection is discarded, and its next use opens a new one.
	auto discarded = map.Discard(connection_id, handle_id, kept);
	REQUIRE(discarded);
	discarded.reset();
	CHECK(f.lines->Closed() == 1);
	map.GetConnection(connection_id, handle_id, config, context, &opened);
	CHECK(opened);
	map.CloseConnection(connection_id);
}

TEST_CASE("an open failure an earlier case armed and never used does not reach the next fixture", "[session]") {
	FailNextMockOpen("redshift mock test: armed by a case that stopped early");
	Fixture f;
	Connection con(f.db);
	Run(con, Use("rs"));
	CHECK(f.lines->Opened() == 1);
}

TEST_CASE("an open that throws opens nothing, and the pair's next use opens again", "[session]") {
	// Driven on the map directly, with a configuration the mock's open refuses, for its missing
	// region: ATTACH refuses it first.
	Fixture f;
	Connection con(f.db);
	auto &context = *con.context;
	RedshiftConnectionMap map;
	const auto connection_id = context.GetConnectionId();
	const auto handle_id = map.NewHandleId();

	auto thrown = redshift_test::Capture(
	    [&]() { map.GetConnection(connection_id, handle_id, redshift_test::MockConfig(""), context); });
	CHECK(thrown.type == ExceptionType::INVALID_INPUT);
	CHECK(thrown.message == "Redshift connection requires REGION");
	CHECK(f.lines->Opened() == 0);

	const auto config = redshift_test::MockConfig();
	auto &connection = *map.GetConnection(connection_id, handle_id, config, context);
	CHECK(StringUtil::Contains(connection.Target(), config.Describe()));
	CHECK(f.lines->Opened() == 1);
	map.CloseConnection(connection_id);
	CHECK(f.lines->Closed() == 1);
}

TEST_CASE("DETACH closes each session once, outside the map's lock", "[session]") {
	Fixture f;
	auto closing = make_uniq<Connection>(f.db);
	Connection detaching(f.db);
	Run(*closing, Use("rs"));
	Run(detaching, Use("rs"));
	auto gate = make_shared_ptr<MockGate>();
	HoldNextMockClose(gate);

	// DETACH retires the handle (OnDetach), which takes both sessions out of the map and then closes
	// them; the first close waits at the gate.
	auto detach = std::async(std::launch::async, [&]() { return ErrorOf(detaching, "DETACH rs"); });
	const bool held = gate->WaitUntilHeldFor(BOUND);
	// The closing DuckDB connection takes the map's lock to close its own session, finds it
	// already taken, and returns. Bounded, so a close made under the lock fails the case.
	auto closed = std::async(std::launch::async, [&]() { closing.reset(); });
	const auto closed_status = closed.wait_for(BOUND);
	const auto closes_while_held = f.lines->Closed();
	gate->Release();
	const auto detach_error = detach.get();
	closed.wait();

	CHECK(held);
	CHECK(closed_status == std::future_status::ready);
	CHECK(closes_while_held == 0);
	CHECK(detach_error.empty());
	CHECK(f.lines->Closed() == 2);
}

TEST_CASE("a handle that goes away while a DuckDB connection streams from it keeps that session until the stream "
          "ends",
          "[session]") {
	Fixture f;
	Connection streaming(f.db);
	Connection detaching(f.db);
	// More rows than one DuckDB chunk, so the first fetch leaves the query open. Only the streaming
	// DuckDB connection has a session on the handle.
	auto stream =
	    make_uniq<QueryResultStream<>>(streaming.Submit("SELECT * FROM redshift_query('rs', 'rows=5000 cols=1')"));
	REQUIRE_FALSE(stream->HasError());
	auto chunk = stream->Fetch();
	REQUIRE(chunk);
	idx_t rows = chunk->size();
	REQUIRE(rows < 5000);
	REQUIRE(f.lines->Opened() == 1);

	bool replaced = false;
	SECTION("DETACH") {
		Run(detaching, "DETACH rs");
	}
	SECTION("ATTACH OR REPLACE") {
		Run(detaching, redshift_test::AttachOrReplace("rs"));
		replaced = true;
		// The new handle has an ID of its own, so the detaching connection's first use opens a session
		// on it, which the old handle's close below must leave alone.
		Run(detaching, Use("rs"));
	}
	const idx_t opened = replaced ? 2 : 1;
	// The stream's pin holds the old handle and its session, which stays open, and the stream
	// finishes on it.
	CHECK(f.lines->Opened() == opened);
	CHECK(f.lines->Closed() == 0);
	while ((chunk = stream->Fetch())) {
		rows += chunk->size();
	}
	CHECK(rows == 5000);
	CHECK(f.lines->Opened() == opened);

	// The query's end releases the pin, which closes the session, and no other.
	stream.reset();
	CHECK(f.lines->Closed() == 1);
	if (replaced) {
		// An ID taken from the alias would have closed this session with the old handle's.
		Run(detaching, Use("rs"));
		CHECK(f.lines->Opened() == 2);
		CHECK(f.lines->Closed() == 1);
	}
}

TEST_CASE("a prepared statement's stream keeps its session when another DuckDB connection detaches the handle",
          "[session]") {
	Fixture f;
	Connection streaming(f.db);
	Connection detaching(f.db);
	// Bound in a transaction of its own, so while it streams only execution's hold on the handle keeps
	// the handle, and with it the session, alive.
	Run(streaming, "PREPARE later AS SELECT * FROM redshift_query('rs', 'rows=5000 cols=1')");
	auto stream = make_uniq<QueryResultStream<>>(streaming.Submit("EXECUTE later"));
	REQUIRE_FALSE(stream->HasError());
	auto chunk = stream->Fetch();
	REQUIRE(chunk);
	idx_t rows = chunk->size();
	REQUIRE(rows < 5000);

	Run(detaching, "DETACH rs");
	CHECK(f.lines->Closed() == 0);
	while ((chunk = stream->Fetch())) {
		rows += chunk->size();
	}
	CHECK_FALSE(stream->HasError());
	CHECK(rows == 5000);
	// The query's end lets the handle go, and with it the session, after the result.
	stream.reset();
	CHECK(f.lines->Closed() == 1);
	CHECK(f.lines->CountStartingWith("redshift mock: a result outlived its connection") == 0);
}

TEST_CASE("a DuckDB connection closed while it streams keeps its session until the stream ends", "[session]") {
	// The stream holds the DuckDB connection's ClientContext, so the session outlives the Connection
	// object, and the result is destroyed with the query, before the session.
	// When another connection detaches the handle meanwhile, the stream's pin holds the handle's last
	// reference, so the stream's end is what closes the session.
	for (const bool detached : {false, true}) {
		INFO("with the handle detached elsewhere: " << detached);
		Fixture f;
		auto streaming = make_uniq<Connection>(f.db);
		auto stream =
		    make_uniq<QueryResultStream<>>(streaming->Submit("SELECT * FROM redshift_query('rs', 'rows=5000 cols=1')"));
		REQUIRE_FALSE(stream->HasError());
		auto chunk = stream->Fetch();
		REQUIRE(chunk);
		idx_t rows = chunk->size();
		REQUIRE(rows < 5000);
		if (detached) {
			Connection detaching(f.db);
			Run(detaching, "DETACH rs");
		}

		streaming.reset();
		CHECK(f.lines->Closed() == 0);
		while ((chunk = stream->Fetch())) {
			rows += chunk->size();
		}
		CHECK(rows == 5000);
		stream.reset();
		CHECK(f.lines->Opened() == 1);
		CHECK(f.lines->Closed() == 1);
	}
}

TEST_CASE("a session's close is logged even when its DuckDB connection holds the database's last reference",
          "[session]") {
	auto lines = make_shared_ptr<redshift_test::RedshiftLineStorage>();
	auto db = make_uniq<DuckDB>(nullptr, redshift_test::NoPersistentSecrets().get());
	db->LoadStaticExtension<RedshiftExtension>();
	redshift_test::UseLogStorage(*db->instance, lines);
	weak_ptr<DatabaseInstance> instance = db->instance;
	auto con = make_uniq<Connection>(*db);
	Run(*con, Attach("rs"));
	Run(*con, Use("rs"));

	db.reset();
	REQUIRE_FALSE(instance.expired());
	// The connection closes its session before it lets the database go, so the line still reaches
	// the logger. Had the session been left for the handle to close as the database closed, the line
	// would be dropped: the client's sink holds the database only while it writes.
	con.reset();

	CHECK(instance.expired());
	CHECK(lines->Opened() == 1);
	CHECK(lines->Closed() == 1);
}

TEST_CASE("sessions are keyed on each DuckDB connection's ID, whether it opened before or after LOAD", "[session]") {
	auto lines = make_shared_ptr<redshift_test::RedshiftLineStorage>();
	// DuckDB loads every linked extension while it starts, unless told not to, which would install
	// the state on both connections through OnConnectionOpened.
	auto config = redshift_test::NoPersistentSecrets();
	config->options.load_extensions = false;
	DuckDB db(nullptr, config.get());
	// LOAD's sweep installs the client state on this one; OnConnectionOpened, on the one after. Each
	// takes the DuckDB connection's ID as it is installed.
	Connection before_load(db);
	REQUIRE_FALSE(db.ExtensionIsLoaded("redshift"));
	db.LoadStaticExtension<RedshiftExtension>();
	REQUIRE(db.ExtensionIsLoaded("redshift"));
	// CMakeLists.txt passes the version DuckDB gives the extension, from DuckDB's own variable, so a macro
	// name in redshift_extension.cpp that DuckDB does not define fails here. A build from a tree outside
	// git has no version, and both are empty.
	if (std::string(REDSHIFT_EXPECTED_VERSION).empty()) {
		WARN("this build sets no extension version, so both sides are empty");
	}
	CHECK(RedshiftExtension().Version() == REDSHIFT_EXPECTED_VERSION);
	redshift_test::UseLogStorage(*db.instance, lines);
	auto after_load = make_uniq<Connection>(db);
	Run(before_load, Attach("rs"));

	for (idx_t call = 0; call < 2; call++) {
		Run(before_load, Use("rs"));
		Run(*after_load, Use("rs"));
	}
	// Keyed alike, the two would share one session.
	CHECK(lines->Opened() == 2);

	after_load.reset();
	CHECK(lines->Closed() == 1);
	Run(before_load, Use("rs"));
	CHECK(lines->Opened() == 2);
}

namespace {

string Stream(const string &handle, const string &directive) {
	return "SELECT * FROM redshift_query('" + handle + "', '" + directive + "')";
}

//! The refusal of a bind on `handle` while a result on its pair is open.
string StillStreaming(const string &handle) {
	return "redshift_query: attach handle \"" + handle +
	       "\" is still streaming the result of an earlier redshift_query call on this DuckDB connection, and each "
	       "handle runs one Redshift statement at a time, so this call was not described or run on Redshift. Read "
	       "that result to its end or close it, then try again.";
}

} // namespace

TEST_CASE("a pair's connection beside an execution's result refuses a bind and a second result", "[session]") {
	// On the map directly: through SQL, only a defect makes a second result meet the first.
	RedshiftConnectionMap map;
	const connection_t connection_id = 1;
	const auto handle_id = map.NewHandleId();
	const auto other_handle_id = map.NewHandleId();
	REQUIRE(map.Pin(connection_id, handle_id));
	REQUIRE(map.Pin(connection_id, other_handle_id));

	// A bind on the pair is refused, and a second result is refused, not waited for. Another pair is
	// free.
	REQUIRE(map.BeginUse(connection_id, handle_id, RedshiftConnectionUse::RESULT));
	CHECK_FALSE(map.BeginUse(connection_id, handle_id, RedshiftConnectionUse::DESCRIBE));
	CHECK_FALSE(map.BeginUse(connection_id, handle_id, RedshiftConnectionUse::RESULT));
	CHECK(map.BeginUse(connection_id, other_handle_id, RedshiftConnectionUse::DESCRIBE));
	map.EndUse(connection_id, other_handle_id);
	map.EndUse(connection_id, handle_id);
	// Free once the result ends.
	CHECK(map.BeginUse(connection_id, handle_id, RedshiftConnectionUse::DESCRIBE));
	map.EndUse(connection_id, handle_id);

	map.Unpin(connection_id, handle_id);
	map.Unpin(connection_id, other_handle_id);
}

TEST_CASE("a pair's connection beside a bind's Describe refuses a second bind and holds an execution until it ends",
          "[session]") {
	// Driven on the map directly, so the execution's wait can be held open and timed: through SQL it
	// needs DuckDB to start a scan on a worker thread while the thread that called DuckDB binds a
	// relation.
	RedshiftConnectionMap map;
	const connection_t connection_id = 1;
	const auto handle_id = map.NewHandleId();
	REQUIRE(map.Pin(connection_id, handle_id));

	REQUIRE(map.BeginUse(connection_id, handle_id, RedshiftConnectionUse::DESCRIBE));
	CHECK_FALSE(map.BeginUse(connection_id, handle_id, RedshiftConnectionUse::DESCRIBE));
	auto execution = std::async(
	    std::launch::async, [&]() { return map.BeginUse(connection_id, handle_id, RedshiftConnectionUse::RESULT); });
	const auto while_describing = execution.wait_for(std::chrono::milliseconds(200));
	map.EndUse(connection_id, handle_id);
	const auto after_describe = execution.wait_for(BOUND);
	CHECK(while_describing == std::future_status::timeout);
	REQUIRE(after_describe == std::future_status::ready);
	CHECK(execution.get());
	// The execution holds it now, until it ends.
	CHECK_FALSE(map.BeginUse(connection_id, handle_id, RedshiftConnectionUse::DESCRIBE));
	map.EndUse(connection_id, handle_id);
	CHECK(map.BeginUse(connection_id, handle_id, RedshiftConnectionUse::DESCRIBE));
	map.EndUse(connection_id, handle_id);

	map.Unpin(connection_id, handle_id);
}

TEST_CASE("a relation bound on a handle while its result streams is refused before Describe, and the stream reads on",
          "[session]") {
	Fixture f;
	Connection con(f.db);
	Run(con, Attach("rs2"));
	// A small streaming buffer, so DuckDB stops producing once a chunk waits, and the result is still
	// open on the pair's connection while the caller holds the stream.
	Run(con, "SET max_streaming_buffer_size = '16KB'");
	auto stream = make_uniq<QueryResultStream<>>(con.Submit(Stream("rs", "rows=100000 batch=2048 types=int8")));
	auto chunk = stream->Fetch();
	REQUIRE(chunk);
	idx_t rows = chunk->size();
	// Another handle's pair is not in use.
	const auto described = f.lines->Described();
	con.TableFunction("redshift_query", {Value("rs2"), Value("rows=1 types=int8")});
	CHECK(f.lines->Described() == described + 1);
	const auto executed = f.lines->Executed();

	// DuckDB binds a relation without its open-result check, so only the extension refuses this one,
	// before the transport sees it. Like any binder error under DuckDB's default policy, it aborts the
	// transaction the relation was bound in, which is the stream's.
	auto refused = redshift_test::Capture([&]() {
		con.TableFunction("redshift_query", {Value("rs"), Value("rows=1 types=int8")});
	});
	CHECK(refused.type == ExceptionType::BINDER);
	CHECK(refused.message == StillStreaming("rs"));
	CHECK(f.lines->Described() == described + 1);
	CHECK(f.lines->Executed() == executed);

	// The stream reads on to its end.
	while ((chunk = stream->Fetch())) {
		rows += chunk->size();
	}
	CHECK_FALSE(stream->HasError());
	CHECK(rows == 100000);
	stream.reset();
	Run(con, Stream("rs", "rows=1 types=int8"));
	CHECK(f.lines->Opened() == 2);
}

TEST_CASE("a result read to its end no longer refuses a relation bound on the handle, while its stream reads on",
          "[session]") {
	Fixture f;
	Connection con(f.db);
	Run(con, "SET max_streaming_buffer_size = '16KB'");
	// The Redshift rows come first, in insertion order, then a local tail long enough that the stream
	// is still open once they are read, and with it the scan's state, which DuckDB destroys only at the
	// query's end.
	auto stream = make_uniq<QueryResultStream<>>(con.Submit(
	    "SELECT c0 FROM redshift_query('rs', 'rows=10 types=int8') UNION ALL SELECT -1 - range FROM range(1000000)"));
	unique_ptr<DataChunk> chunk;
	bool tail_reached = false;
	while (!tail_reached && (chunk = stream->Fetch())) {
		for (idx_t row = 0; row < chunk->size(); row++) {
			tail_reached = tail_reached || chunk->GetValue(0, row).GetValue<int64_t>() < 0;
		}
	}
	REQUIRE(tail_reached);
	REQUIRE_FALSE(stream->HasError());
	// The scan has read its result to the end, and the client counts it closed: a relation bound on
	// the handle describes.
	const auto described = f.lines->Described();
	auto relation = con.TableFunction("redshift_query", {Value("rs"), Value("rows=1 types=int8")});
	CHECK(f.lines->Described() == described + 1);
	stream.reset();
	REQUIRE_FALSE(relation->Execute()->HasError());
	CHECK(f.lines->Opened() == 1);
}

TEST_CASE("a stream that failed partway no longer refuses a relation bound on the handle", "[session]") {
	Fixture f;
	Connection con(f.db);
	Run(con, "SET max_streaming_buffer_size = '16KB'");
	auto stream = make_uniq<QueryResultStream<>>(
	    con.Submit(Stream("rs", "fail=fetch:5000 sqlstates=22012 rows=10000 batch=1000 types=int8")));
	unique_ptr<DataChunk> chunk;
	while ((chunk = stream->Fetch())) {
	}
	REQUIRE(stream->HasError());
	// The failure ended the result, and the query with it, though the caller still holds the stream:
	// the bind describes.
	const auto described = f.lines->Described();
	con.TableFunction("redshift_query", {Value("rs"), Value("rows=1 types=int8")});
	CHECK(f.lines->Described() == described + 1);
	stream.reset();
}

namespace {

//! The Redshift handle `handle` names on `context`'s DuckDB connection, pinned for the test.
unique_ptr<RedshiftConnectionPin> PinHandle(ClientContext &context, const string &handle) {
	auto db = DatabaseManager::Get(context).GetDatabase(Identifier(handle));
	REQUIRE(db);
	auto pin = RedshiftConnectionPin::TryPin(context, std::move(db));
	REQUIRE(pin);
	return pin;
}

} // namespace

TEST_CASE("an execution meeting another execution's result on its pair is refused before Execute", "[session]") {
	// Only a defect reaches this through SQL, so the test holds the other result's use itself. The
	// statement is prepared first, so EXECUTE reuses its bind and goes straight to execution.
	Fixture f;
	Connection con(f.db);
	Run(con, "PREPARE later AS " + Use("rs"));
	const auto executed = f.lines->Executed();
	auto held = PinHandle(*con.context, "rs");
	REQUIRE(held->BeginUse(RedshiftConnectionUse::RESULT));

	auto result = con.Query("EXECUTE later");
	REQUIRE(result->HasError());
	auto &error = result->GetErrorObject();
	CHECK(error.Type() == ExceptionType::EXECUTOR);
	CHECK(error.RawMessage() ==
	      "redshift_query: attach handle \"rs\" already has a Redshift statement running on this DuckDB connection, "
	      "so this call was not run, though other calls in the same DuckDB statement may have run; check their "
	      "effects on Redshift before running the statement again. Report this as a fault in the Redshift "
	      "extension.");
	CHECK(f.lines->Executed() == executed);
	// The refused execution left the held use in place: a bind on the pair is still refused.
	const auto described = f.lines->Described();
	auto refused = con.Query(Use("rs"));
	REQUIRE(refused->HasError());
	CHECK(refused->GetErrorObject().Type() == ExceptionType::BINDER);
	CHECK(StringUtil::StartsWith(refused->GetErrorObject().RawMessage(), StillStreaming("rs")));
	CHECK(f.lines->Described() == described);

	held.reset();
	Run(con, "EXECUTE later");
	CHECK(f.lines->Executed() == executed + 1);
}

TEST_CASE("an execution that waited for a bind's Describe raises the loss report that bind left, and runs nothing",
          "[session]") {
	// The test holds the bind's use through a pin, as a Relation API bind on the thread that called
	// DuckDB would while the execution starts on a worker thread. The pin pause tells the test the execution
	// has made its first check for a kept report and is about to pin and wait, so the report the test
	// then keeps can only be found by the check after the wait.
	Fixture f;
	Connection con(f.db);
	Run(con, "PREPARE later AS " + Use("rs"));
	const auto executed = f.lines->Executed();
	auto bind = PinHandle(*con.context, "rs");
	REQUIRE(bind->BeginUse(RedshiftConnectionUse::DESCRIBE));

	std::promise<void> pinning;
	std::atomic<bool> signalled {false};
	SetRedshiftPinPauseForTesting([&]() {
		if (!signalled.exchange(true)) {
			pinning.set_value();
		}
	});
	auto execution = std::async(std::launch::async, [&]() { return con.Query("EXECUTE later"); });
	const auto reached = pinning.get_future().wait_for(BOUND);
	// Removed before anything here can throw: the hook refers to this case's locals.
	SetRedshiftPinPauseForTesting(nullptr);
	const string report = "redshift_query: the Redshift connection of attach handle \"rs\" was lost while describing "
	                      "a statement; the session it carried may have held state";
	RedshiftClientState::Get(*con.context).HoldLossReport(*con.context, "rs", ErrorData(IOException(report)));
	bind.reset();
	const auto finished = execution.wait_for(BOUND);

	REQUIRE(reached == std::future_status::ready);
	REQUIRE(finished == std::future_status::ready);
	auto result = execution.get();
	REQUIRE(result->HasError());
	CHECK(result->GetErrorObject().RawMessage() == report);
	CHECK(f.lines->Executed() == executed);
}

TEST_CASE("the listed handles come from the connection map, not from catalogs read through the database list",
          "[session]") {
	// A DETACH closes a database only once the copy of the list has dropped it, but nothing orders the
	// copy's catalog reads before that close, so the map must answer without reading any. The test
	// makes that state stand still, a listed database whose catalog is gone, by closing one the copy
	// still holds, whose catalog a read would find destroyed. It calls the map directly: the wrapper
	// that hands the map DuckDB's list, RedshiftCatalog::ListedHandleIds, is covered against a
	// concurrent DETACH only by test_mock_tsan's concurrent DETACH case in test_detach.cpp.
	Fixture f;
	Connection con(f.db);
	Run(con, redshift_test::Attach("rs2"));
	auto &context = *con.context;
	auto databases = DatabaseManager::Get(context).GetDatabases();
	shared_ptr<AttachedDatabase> detached;
	optional_idx rs_id;
	optional_idx rs2_id;
	for (auto &db : databases) {
		if (db->GetName() == Identifier("rs")) {
			detached = db;
			rs_id = db->GetCatalog().Cast<RedshiftCatalog>().HandleId();
		} else if (db->GetName() == Identifier("rs2")) {
			rs2_id = db->GetCatalog().Cast<RedshiftCatalog>().HandleId();
		}
	}
	REQUIRE(detached);
	REQUIRE(rs2_id.IsValid());
	auto &map = *RedshiftClientState::Get(context).ConnectionMap();
	CHECK(map.ListedHandleIds(databases) == unordered_set<idx_t> {rs_id.GetIndex(), rs2_id.GetIndex()});

	Run(con, "DETACH rs");
	detached->Close(DatabaseCloseAction::SKIP_CHECKPOINT);
	CHECK(map.ListedHandleIds(databases) == unordered_set<idx_t> {rs2_id.GetIndex()});
}

TEST_CASE("ATTACH opens the attaching DuckDB connection's session, which its first call reuses", "[session][attach]") {
	Fixture f;
	Connection attaching(f.db);
	Connection other(f.db);
	Run(attaching, Attach("rs2"));
	CHECK(f.lines->Opened() == 1);
	CHECK(f.lines->Executed() == 0);
	Run(attaching, Use("rs2"));
	CHECK(f.lines->Opened() == 1);
	CHECK(f.lines->Executed() == 1);
	// Another DuckDB connection opens its own on its first use.
	Run(other, Use("rs2"));
	CHECK(f.lines->Opened() == 2);
	// DETACH closes the attaching connection's session with the other's.
	Run(attaching, "DETACH rs2");
	CHECK(f.lines->Closed() == 2);
}

TEST_CASE("an ATTACH whose open fails names the handle, keeps the open's type and extra_info, and attaches nothing",
          "[session][attach]") {
	Fixture f;
	Connection con(f.db);
	FailNextMockOpen("Could not connect to Redshift: the workgroup was not found.\nSQLSTATE: HY000", ExceptionType::IO,
	                 {{"sqlstate", "HY000"}});
	auto failure = redshift_test::FailureOf(*con.Query(Attach("rs2")));
	CHECK(failure.type == ExceptionType::IO);
	CHECK(failure.message == "Cannot attach rs2: Could not connect to Redshift: the workgroup was not found.\n"
	                         "SQLSTATE: HY000");
	CHECK(failure.extra_info["sqlstate"] == "HY000");
	CHECK(f.lines->Opened() == 0);
	CHECK(f.lines->CountStartingWith("redshift mock: could not open a connection to ") == 1);
	auto listed = con.Query("SELECT count(*) FROM duckdb_databases() WHERE database_name = 'rs2'");
	CHECK(listed->Fetch()->GetValue(0, 0).GetValue<int64_t>() == 0);
	// Nothing is left behind: the name attaches again, and its call runs on the session that ATTACH opened.
	Run(con, Attach("rs2"));
	Run(con, Use("rs2"));
	CHECK(f.lines->Opened() == 1);
	CHECK(f.lines->Executed() == 1);
}

TEST_CASE("an ATTACH whose open fails does not repeat a name derived from the path", "[session][attach]") {
	Fixture f;
	Connection con(f.db);
	FailNextMockOpen("Could not connect to Redshift.");
	auto failure = redshift_test::FailureOf(
	    *con.Query("ATTACH 'redshift://wg1.012345678901.us-east-1.redshift-serverless.amazonaws.com' (TYPE redshift, "
	               "CREDENTIAL_PROVIDER aws_default_chain)"));
	CHECK(failure.type == ExceptionType::IO);
	CHECK(failure.message == "Cannot attach the handle named after its path (give it an AS name other than the path's "
	                         "to see it here): Could not connect to Redshift.");
}

TEST_CASE("a plain ATTACH of a path whose derived name is in use opens nothing", "[session][attach]") {
	// With no AS the handle in messages is empty, so the check of the name in use reads the name DuckDB
	// derived, not that.
	Fixture f;
	Connection con(f.db);
	const string path = "ATTACH 'redshift://wg1.012345678901.us-east-1.redshift-serverless.amazonaws.com' (TYPE "
	                    "redshift, CREDENTIAL_PROVIDER aws_default_chain)";
	Run(con, path);
	const auto opened = f.lines->Opened();
	FailNextMockOpen("Could not connect to Redshift.");
	auto failure = redshift_test::FailureOf(*con.Query(path));
	DisarmMockOpenFailure();
	CHECK_THAT(failure.message, Catch::Contains("already exists"));
	CHECK(f.lines->Opened() == opened);
	CHECK(f.lines->CountStartingWith("redshift mock: could not open a connection to ") == 0);
}

TEST_CASE("an interrupt or a statement-ending error during the ATTACH's open passes through as it was raised",
          "[session][attach]") {
	for (const auto type :
	     {ExceptionType::INTERRUPT, ExceptionType::FATAL, ExceptionType::INTERNAL, ExceptionType::OUT_OF_MEMORY}) {
		INFO(Exception::ExceptionTypeToString(type));
		Fixture f;
		Connection con(f.db);
		const string failed = "redshift mock test: the open failed inside the client";
		FailNextMockOpen(failed, type);
		auto failure = redshift_test::FailureOf(*con.Query(Attach("rs2")));
		CHECK(failure.type == type);
		CHECK(failure.message.find("Cannot attach") == string::npos);
		CHECK(failure.message.find(failed) != string::npos);
	}
}

TEST_CASE("an ATTACH OR REPLACE whose open fails keeps the handle it would replace, and its sessions",
          "[session][attach]") {
	Fixture f;
	Connection con(f.db);
	Run(con, Use("rs"));
	const auto opened = f.lines->Opened();
	FailNextMockOpen("Could not connect to Redshift.");
	auto failure = redshift_test::FailureOf(*con.Query(redshift_test::AttachOrReplace("rs")));
	CHECK(failure.type == ExceptionType::IO);
	CHECK(failure.message == "Cannot attach rs: Could not connect to Redshift.");
	// The old handle is still attached under the name, and its session is the one the next call uses.
	Run(con, Use("rs"));
	CHECK(f.lines->Opened() == opened);
	CHECK(f.lines->Closed() == 0);
}

TEST_CASE("a plain ATTACH of a name in use opens nothing, and DuckDB's refusal is the one reported",
          "[session][attach]") {
	Fixture f;
	Connection con(f.db);
	// Armed, so an open the ATTACH made would fail with this instead of DuckDB's refusal.
	FailNextMockOpen("Could not connect to Redshift.");
	auto failure = redshift_test::FailureOf(*con.Query(Attach("rs")));
	DisarmMockOpenFailure();
	CHECK_THAT(failure.message, Catch::Contains("already exists"));
	CHECK(failure.message.find("Could not connect") == string::npos);
	CHECK(f.lines->Opened() == 0);
	CHECK(f.lines->CountStartingWith("redshift mock: could not open a connection to ") == 0);
}

TEST_CASE("a rolled-back ATTACH closes the session it opened", "[session][attach]") {
	Fixture f;
	Connection con(f.db);
	Run(con, "BEGIN");
	Run(con, Attach("rs2"));
	CHECK(f.lines->Opened() == 1);
	CHECK(f.lines->Closed() == 0);
	Run(con, "ROLLBACK");
	CHECK(f.lines->Closed() == 1);
	auto listed = con.Query("SELECT count(*) FROM duckdb_databases() WHERE database_name = 'rs2'");
	CHECK(listed->Fetch()->GetValue(0, 0).GetValue<int64_t>() == 0);
}

TEST_CASE("the ATTACH's open refuses a handle already retired, and opens nothing", "[session][attach]") {
#ifdef DUCKDB_CRASH_ON_ASSERT
	return; // InternalException aborts in this build
#endif
	// Unreachable through SQL: ATTACH opens before DuckDB lists the handle, so nothing can retire it
	// first. Driven on the map. GetConnection itself opens nothing on a retired handle, so a missing pin
	// check fails here only through the InternalException it no longer throws.
	Fixture f;
	Connection con(f.db);
	Run(con, Attach("rs2"));
	auto &context = *con.context;
	auto attached = DatabaseManager::Get(context).GetDatabase(Identifier("rs2"));
	REQUIRE(attached);
	auto &catalog = attached->GetCatalog().Cast<RedshiftCatalog>();
	auto &map = *RedshiftClientState::Get(context).ConnectionMap();
	map.RetireHandle(catalog.HandleId(), Identifier("rs2"), context.GetConnectionId());
	const auto opened = f.lines->Opened();
	auto thrown = redshift_test::Capture([&]() { catalog.OpenAttachingConnection(context); });
	CHECK(thrown.type == ExceptionType::INTERNAL);
	CHECK_THAT(thrown.message, Catch::Contains("the handle being attached has been retired"));
	CHECK(f.lines->Opened() == opened);
}
