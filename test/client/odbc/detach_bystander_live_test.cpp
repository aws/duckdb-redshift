// What another DuckDB connection's DETACH does to a bystander's live Redshift session, on the
// embedded driver. Each DuckDB connection under test runs on a thread of its own, so a DETACH tears
// a session down on a thread that neither opened nor last used it. The case's own thread orders the
// steps; the cases that race (a statement in flight, a result held open, the stress loop) overlap
// them on purpose, and each checks that the race it needs happened. A second handle, `obs`, reads
// `sys_session_history` to see sessions end.
//
// The sessions a case reads are found by their pid, or, for a bystander that only described a
// statement, as the one session of this database user that its DESCRIBE opened: running
// pg_backend_pid() would make that session stateful. Other sessions of the same user can make that
// search retry, and never make a case pass. That session's end is checked whenever the search
// singles it out; when all five tries are ambiguous, the case fails with
// REDSHIFT_TEST_LIVE_EXCLUSIVE=1, which says nothing else uses the user meanwhile, and otherwise
// skips that check and says so. The stress case's check that no session is left open counts every
// session of the user, so it runs only with that variable. REDSHIFT_TEST_STRESS_ROUNDS and
// REDSHIFT_TEST_STRESS_SEED set the stress loop's rounds (10) and seed (random, and reported). The
// cases tagged [writes] write a temporary table, which ends with its session; the others write
// nothing.

// catch.hpp comes first, as in this directory's other tests.
#include "catch.hpp"

#include "duckdb/common/string_util.hpp"
#include "duckdb/main/query_result_stream.hpp"
#include "extension_database.hpp"
#include "live_workgroup.hpp"
#include "thrown.hpp"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <cstdlib>
#include <deque>
#include <functional>
#include <future>
#include <iostream>
#include <map>
#include <mutex>
#include <random>
#include <set>
#include <string>
#include <thread>
#include <vector>

using namespace duckdb;
using redshift_test::Thrown;

namespace {

using Clock = std::chrono::steady_clock;
using Rows = vector<vector<string>>;

// --- The texts the extension refuses with (src/redshift_query.cpp) ------------------------------------

//! ResolveHandle's refusal of a name no database has, with the possible-state-loss note when `lost`.
string NotAttached(const string &handle, bool lost = false) {
	auto text = "redshift_query: no attached database is named \"" + handle +
	            "\". Attach a Redshift handle first with ATTACH '' AS " + handle + " (TYPE redshift, ...)";
	if (lost) {
		text += ". Another DuckDB connection detached or replaced the Redshift handle that had this name, or an "
		        "ATTACH under this name was rolled back, while this DuckDB connection had a Redshift session on it, "
		        "and that session may have held state, which is now lost.";
	}
	return text;
}

//! The refusal, once, of a bystander whose session may have held state, after a new handle took the name.
string StateLost(const string &handle) {
	return "redshift_query: attach handle \"" + handle +
	       "\" was detached or replaced by another DuckDB connection, or an ATTACH under its name was rolled back, "
	       "and a new handle is attached under the name now. This DuckDB connection's Redshift session on the "
	       "earlier handle may have held state, which is now lost, so this statement was not run on Redshift. Run "
	       "it again to start a new, empty Redshift session on the handle attached now.";
}

//! The starts of the refusals a statement racing DETACH and ATTACH may also meet: its handle retired between
//! its bind and its execution, or twice while it was bound.
const vector<std::pair<string, string>> RACE_REFUSALS {
    {"detached after bind", "redshift_query: attach handle \"ex\" was detached, or an ATTACH under its name was "
                            "rolled back, after this statement was bound, so this call was not run on Redshift"},
    {"replaced after bind", "redshift_query: attach handle \"ex\" was detached or replaced, or an ATTACH under its "
                            "name was rolled back, after this statement was bound, and another handle has the name "
                            "now, so this call was not run on Redshift"},
    {"retired twice in bind", "redshift_query: attach handle \"ex\" was detached or replaced, or an ATTACH under its "
                              "name was rolled back, twice while this statement was being bound, so it was not "
                              "described on Redshift."},
};

// --- Steps and the threads that run them ------------------------------------------------------------------

//! What a step returned: its rows, or what DuckDB refused it with.
struct Outcome {
	bool ok = false;
	Thrown thrown;
	Rows rows;
};

std::ostream &operator<<(std::ostream &out, const Outcome &outcome) {
	if (!outcome.ok) {
		return out << "refused: " << outcome.thrown;
	}
	return out << "ok, " << outcome.rows.size() << " rows";
}

//! The client's message in `message`, without the query location DuckDB adds after a blank line.
string WithoutQueryLocation(const string &message) {
	return message.substr(0, message.find("\n\n"));
}

//! `result`'s outcome, reading whole chunks until it holds at least `max_rows` rows or the result ends: a
//! QueryResult, or a QueryResultStream, whose query stays open until a Fetch reads its end.
template <class RESULT>
Outcome OutcomeOf(RESULT &result, idx_t max_rows = DConstants::INVALID_INDEX) {
	Outcome outcome;
	if (result.HasError()) {
		const auto &error = result.GetErrorObject();
		outcome.thrown = Thrown {error.Type(), WithoutQueryLocation(error.RawMessage())};
		return outcome;
	}
	outcome.ok = true;
	while (outcome.rows.size() < max_rows) {
		auto chunk = result.Fetch();
		if (!chunk || chunk->size() == 0) {
			break;
		}
		for (idx_t row = 0; row < chunk->size(); row++) {
			vector<string> cells;
			for (idx_t column = 0; column < chunk->ColumnCount(); column++) {
				cells.push_back(chunk->GetValue(column, row).ToString());
			}
			outcome.rows.push_back(std::move(cells));
		}
	}
	if (result.HasError()) {
		const auto &error = result.GetErrorObject();
		outcome.ok = false;
		outcome.thrown = Thrown {error.Type(), WithoutQueryLocation(error.RawMessage())};
	}
	return outcome;
}

string RedshiftQuery(const string &handle, const string &sql) {
	return "SELECT * FROM redshift_query('" + handle + "', '" + StringUtil::Replace(sql, "'", "''") + "')";
}

//! A thread with a DuckDB connection of its own, running steps in turn. Steps make no Catch2 assertion:
//! they hand what happened back to the case's thread, which checks it.
class Worker {
public:
	using Step = std::function<Outcome(Connection &)>;

	Worker(DuckDB &db, string name_p) : name(std::move(name_p)), thread([this, &db] { Run(db); }) {
	}

	~Worker() {
		{
			std::lock_guard<std::mutex> guard(lock);
			stopping = true;
		}
		wake.notify_all();
		thread.join();
	}

	std::future<Outcome> Submit(Step step) {
		std::packaged_task<Outcome(Connection &)> task(std::move(step));
		auto future = task.get_future();
		{
			std::lock_guard<std::mutex> guard(lock);
			steps.push_back(std::move(task));
		}
		wake.notify_all();
		return future;
	}

	Outcome Do(Step step) {
		return Wait(Submit(std::move(step)), name);
	}

	Outcome Sql(const string &sql) {
		return Do([sql](Connection &con) { return OutcomeOf(*con.Query(sql)); });
	}

	Outcome Query(const string &handle, const string &sql) {
		return Sql(RedshiftQuery(handle, sql));
	}

	//! A step's outcome. A step that never finishes would leave its thread unjoinable, so the run ends here.
	static Outcome Wait(std::future<Outcome> future, const string &who, int seconds = 300) {
		if (future.wait_for(std::chrono::seconds(seconds)) != std::future_status::ready) {
			std::cerr << "detach_bystander_live_test: " << who << "'s step did not finish in " << seconds
			          << " s; ending the run" << std::endl;
			std::_Exit(3);
		}
		return future.get();
	}

	const string name;
	//! A result a step left open, for a later step on this thread to read.
	unique_ptr<QueryResultStream<>> held;

private:
	void Run(DuckDB &db) {
		Connection con(db);
		while (true) {
			std::packaged_task<Outcome(Connection &)> task;
			{
				std::unique_lock<std::mutex> guard(lock);
				wake.wait(guard, [this] { return stopping || !steps.empty(); });
				if (steps.empty()) {
					break;
				}
				task = std::move(steps.front());
				steps.pop_front();
			}
			task(con);
		}
		held.reset();
	}

	std::mutex lock;
	std::condition_variable wake;
	std::deque<std::packaged_task<Outcome(Connection &)>> steps;
	bool stopping = false;
	std::thread thread;
};

// --- The case's database and its observer ------------------------------------------------------------------

string Variable(const char *name) {
	const char *value = std::getenv(name);
	return value ? value : "";
}

//! The ATTACH of `name` to the live workgroup.
string Attach(const string &name, bool replace = false) {
	auto sql = string("ATTACH ") + (replace ? "OR REPLACE " : "") + "'' AS " + name + " (TYPE redshift, REGION '" +
	           Variable(redshift_test::LIVE_REGION_VARIABLE) + "', CREDENTIAL_PROVIDER aws_default_chain, WORKGROUP '" +
	           Variable(redshift_test::LIVE_WORKGROUP_VARIABLE) + "'";
	return sql + ", ACCOUNT_ID '" + Variable(redshift_test::LIVE_ACCOUNT_ID_VARIABLE) + "')";
}

int64_t AsInteger(const Outcome &outcome) {
	REQUIRE(outcome.ok);
	REQUIRE(outcome.rows.size() == 1);
	return std::stoll(outcome.rows[0][0]);
}

//! One case's database: `ex` and `obs` attached, the observer's session open, and the workers it starts.
//! Members are destroyed in reverse: the workers before the database, and the process state reset last.
struct Scene {
	Scene() {
		SetDriverSourceForTesting(&redshift_driver::LoadEmbeddedDriver);
		database.Run(Attach("ex"));
		database.Run(Attach("obs"));
		obs = &Start("obs");
		obs_pid = Pid(*obs, "obs");
	}

	Worker &Start(const string &name) {
		workers.push_back(make_uniq<Worker>(database.db, name));
		return *workers.back();
	}

	//! Ends `worker`'s thread, which closes its DuckDB connection there, as a client closing it does.
	void Stop(Worker &worker) {
		for (auto it = workers.begin(); it != workers.end(); ++it) {
			if (it->get() == &worker) {
				workers.erase(it);
				return;
			}
		}
	}

	//! The session `worker` has on `handle`. Running it makes the session stateful.
	int64_t Pid(Worker &worker, const string &handle = "ex") {
		const auto outcome = worker.Query(handle, "SELECT pg_backend_pid()");
		INFO(worker.name << "'s pg_backend_pid(): " << outcome);
		return AsInteger(outcome);
	}

	//! The status of the newest session with id `pid`: Redshift reuses session ids, so older rows can share it.
	string Status(int64_t pid) {
		const auto outcome =
		    obs->Query("obs", "SELECT status FROM sys_session_history WHERE session_id = " + std::to_string(pid) +
		                          " ORDER BY start_time DESC LIMIT 1");
		INFO("sys_session_history: " << outcome);
		REQUIRE(outcome.ok);
		return outcome.rows.empty() ? string("(none)") : outcome.rows[0][0];
	}

	//! That the newest session with id `pid` is active, as Status reads it. A failure shows every row with that id.
	void CheckActive(int64_t pid, const string &who) {
		const auto outcome = obs->Query("obs", "SELECT status, start_time, end_time, user_id, database_name FROM "
		                                       "sys_session_history WHERE session_id = " +
		                                           std::to_string(pid) + " ORDER BY start_time");
		INFO(who << "'s session " << pid << ": " << outcome);
		REQUIRE(outcome.ok);
		string rows;
		for (const auto &row : outcome.rows) {
			rows += "\n  " + StringUtil::Join(row, " | ");
		}
		INFO("sys_session_history (status | start_time | end_time | user_id | database_name), oldest first:" << rows);
		const auto newest = outcome.rows.empty() ? string("(none)") : outcome.rows.back()[0];
		CHECK(newest == "active");
	}

	//! This database user's open sessions but the observer's. A superuser sees every user's, the system's
	//! own included, which come and go every few seconds, so the user is named.
	std::set<int64_t> ActiveSessions() {
		const auto outcome = obs->Query(
		    "obs", "SELECT session_id FROM sys_session_history WHERE status = 'active' AND user_id = current_user_id");
		INFO("sys_session_history: " << outcome);
		REQUIRE(outcome.ok);
		std::set<int64_t> active;
		for (const auto &row : outcome.rows) {
			active.insert(std::stoll(row[0]));
		}
		active.erase(obs_pid);
		return active;
	}

	//! Whether `pid`'s session is closed within `seconds`; the status last read in `last`.
	bool ClosedWithin(int64_t pid, string &last, int seconds = 90) {
		const auto deadline = Clock::now() + std::chrono::seconds(seconds);
		while (true) {
			last = Status(pid);
			if (last == "closed") {
				return true;
			}
			if (Clock::now() > deadline) {
				return false;
			}
			std::this_thread::sleep_for(std::chrono::seconds(1));
		}
	}

	//! That `pid`'s session closes; nothing for 0, a session DescribingBystander could not single out.
	void RequireClosed(int64_t pid, const string &who) {
		if (pid == 0) {
			return;
		}
		string last;
		INFO(who << "'s session " << pid);
		const bool closed = ClosedWithin(pid, last);
		INFO("status after 90 s: " << last);
		REQUIRE(closed);
	}

	//! A new bystander that only described a statement on `ex`, and its session: the one session of this
	//! database user its DESCRIBE opened. A DESCRIBE always opens one, so when exactly one new session
	//! shows, and still does two seconds later, it is the bystander's. Another session opening meanwhile
	//! makes the attempt ambiguous, and it is made again with a new bystander. After five ambiguous
	//! attempts the session is 0, not checked, unless REDSHIFT_TEST_LIVE_EXCLUSIVE=1 says the user is
	//! the case's alone, when the case fails.
	std::pair<Worker *, int64_t> DescribingBystander(const string &name, const string &handle = "ex") {
		Worker *last = nullptr;
		for (int attempt = 1; attempt <= 5; attempt++) {
			const auto before = ActiveSessions();
			auto &worker = Start(name + (attempt > 1 ? std::to_string(attempt) : ""));
			last = &worker;
			const auto described = worker.Sql("DESCRIBE " + RedshiftQuery(handle, "SELECT 1 AS m"));
			INFO("DESCRIBE: " << described);
			REQUIRE(described.ok);
			std::set<int64_t> fresh;
			const auto deadline = Clock::now() + std::chrono::seconds(20);
			while (fresh.empty() && Clock::now() < deadline) {
				for (auto pid : ActiveSessions()) {
					if (!before.count(pid)) {
						fresh.insert(pid);
					}
				}
				if (fresh.empty()) {
					std::this_thread::sleep_for(std::chrono::seconds(1));
				}
			}
			{
				INFO("no session of this user appeared after the DESCRIBE");
				REQUIRE_FALSE(fresh.empty());
			}
			std::this_thread::sleep_for(std::chrono::seconds(2));
			for (auto pid : ActiveSessions()) {
				if (!before.count(pid)) {
					fresh.insert(pid);
				}
			}
			if (fresh.size() == 1) {
				return {&worker, *fresh.begin()};
			}
			WARN("attempt " << attempt << ": " << fresh.size() << " sessions appeared during the DESCRIBE; again");
		}
		if (Variable("REDSHIFT_TEST_LIVE_EXCLUSIVE") == "1") {
			FAIL("inconclusive: another session of this database user opened during each of five DESCRIBEs");
		}
		WARN(name << "'s session is not checked: another session of this database user opened during each of five "
		             "DESCRIBEs (REDSHIFT_TEST_LIVE_EXCLUSIVE=1 checks it where nothing else uses the user)");
		return {last, 0};
	}

	//! Leaves `worker` with a Redshift transaction open on `handle` and a temporary table written.
	int64_t MakeStateful(Worker &worker, const string &handle = "ex") {
		for (const string sql :
		     {"BEGIN", "CREATE TEMP TABLE v9_bystander (a int)", "INSERT INTO v9_bystander VALUES (1)"}) {
			const auto outcome = worker.Query(handle, sql);
			INFO(worker.name << ": " << sql << ": " << outcome);
			REQUIRE(outcome.ok);
		}
		return Pid(worker, handle);
	}

	redshift_test::ResetOnExit reset;
	redshift_test::LiveEndpoints endpoints;
	redshift_test::ExtensionDatabase database;
	vector<unique_ptr<Worker>> workers;
	Worker *obs = nullptr;
	int64_t obs_pid = 0;
};

void RequireRefused(Worker &worker, const string &message, const string &what, const string &handle = "ex") {
	const auto outcome = worker.Query(handle, "SELECT 1");
	INFO(what << ": " << outcome);
	REQUIRE(outcome.thrown == Thrown {ExceptionType::BINDER, message});
}

//! As RequireRefused, for a refusal whose message starts with `start`.
void RequireRefusedStarting(Worker &worker, const string &start, const string &what, const string &handle) {
	const auto outcome = worker.Query(handle, "SELECT 1");
	INFO(what << ": " << outcome);
	REQUIRE(outcome.thrown.type == ExceptionType::BINDER);
	REQUIRE(StringUtil::StartsWith(outcome.thrown.message, start));
}

void RequireRuns(Worker &worker, const string &what, const string &handle = "ex") {
	const auto outcome = worker.Query(handle, "SELECT 1");
	INFO(what << ": " << outcome);
	REQUIRE(outcome.ok);
	REQUIRE(outcome.rows == Rows {{"1"}});
}

void RequireSql(Worker &worker, const string &sql) {
	const auto outcome = worker.Sql(sql);
	INFO(worker.name << ": " << sql << ": " << outcome);
	REQUIRE(outcome.ok);
}

int EnvInt(const char *name, int fallback) {
	const auto value = Variable(name);
	return value.empty() ? fallback : std::stoi(value);
}

} // namespace

TEST_CASE("Live: a DETACH ends both bystanders' sessions; after ATTACH again, only the stateful one is refused, once",
          "[odbc][live][writes][detach]") {
	SKIP_UNLESS_LIVE(redshift_driver::LoadEmbeddedDriver());
	Scene scene;
	auto &a = scene.Start("A");
	auto &d = scene.Start("D");
	const auto pid_a = scene.MakeStateful(a);
	auto b = scene.DescribingBystander("B");

	RequireSql(d, "DETACH ex");
	scene.RequireClosed(pid_a, "A");
	scene.RequireClosed(b.second, "B");
	RequireSql(d, Attach("ex"));

	RequireRefused(a, StateLost("ex"), "A's next statement");
	const auto new_a = scene.Pid(a);
	CHECK(new_a != pid_a);
	CHECK(new_a != b.second);
	{
		// pg_class lists every session's temporary tables; only this session's is visible to it.
		const auto temp =
		    a.Query("ex", "SELECT count(*) FROM pg_class WHERE relname = 'v9_bystander' AND pg_table_is_visible(oid)");
		INFO("A's temporary table on its new session: " << temp);
		CHECK(temp.rows == Rows {{"0"}});
	}
	RequireRuns(*b.first, "B's next statement");
	const auto new_b = scene.Pid(*b.first);
	CHECK(new_b != pid_a);
	CHECK(new_b != b.second);
	RequireRuns(d, "D's next statement");
}

TEST_CASE("Live: with nothing attached under the name again, only the stateful bystander hears its state may be lost",
          "[odbc][live][writes][detach]") {
	SKIP_UNLESS_LIVE(redshift_driver::LoadEmbeddedDriver());
	Scene scene;
	auto &a = scene.Start("A");
	auto &d = scene.Start("D");
	const auto pid_a = scene.MakeStateful(a);
	auto b = scene.DescribingBystander("B");
	const auto pid_d = scene.Pid(d);

	RequireSql(d, "DETACH ex");
	scene.RequireClosed(pid_a, "A");
	scene.RequireClosed(b.second, "B");
	scene.RequireClosed(pid_d, "D");

	RequireRefused(a, NotAttached("ex", true), "A's next statement");
	RequireRefused(a, NotAttached("ex"), "A's second statement");
	RequireRefused(*b.first, NotAttached("ex"), "B's next statement");
	// The detaching connection ended its own session deliberately, so it hears of no lost state.
	RequireRefused(d, NotAttached("ex"), "D's next statement");
}

TEST_CASE("Live: ATTACH OR REPLACE retires the handle as DETACH does", "[odbc][live][writes][detach]") {
	SKIP_UNLESS_LIVE(redshift_driver::LoadEmbeddedDriver());
	Scene scene;
	auto &a = scene.Start("A");
	auto &d = scene.Start("D");
	const auto pid_a = scene.MakeStateful(a);
	auto b = scene.DescribingBystander("B");
	const auto pid_d = scene.Pid(d);

	RequireSql(d, Attach("ex", true));
	scene.RequireClosed(pid_a, "A");
	scene.RequireClosed(b.second, "B");
	scene.RequireClosed(pid_d, "D");

	RequireRefused(a, StateLost("ex"), "A's next statement");
	RequireRuns(a, "A's statement after the refusal");
	RequireRuns(*b.first, "B's next statement");
	RequireRuns(d, "D's next statement");
}

TEST_CASE("Live: a DETACH while a bystander's statement runs neither waits for it nor ends it",
          "[odbc][live][detach]") {
	SKIP_UNLESS_LIVE(redshift_driver::LoadEmbeddedDriver());
	Scene scene;
	auto &a = scene.Start("A");
	auto &d = scene.Start("D");
	const auto pid_a = scene.Pid(a);
	// The statement must run long enough that Redshift shows it running before it ends: no result cache, a
	// text of its own, and a size from this target's pg_class, about fifteen million rows.
	RequireSql(a, RedshiftQuery("ex", "SET enable_result_cache_for_session TO off"));
	const auto n = AsInteger(a.Query("ex", "SELECT count(*) FROM pg_class"));
	REQUIRE(n > 1);
	// pg_class crossed with itself until the product reaches the target; a LIMIT can give no more rows
	// than pg_class has, so each factor is at most n.
	const int64_t target = 15000000;
	const auto nonce = "v9_" + std::to_string(std::random_device {}());
	auto sql = string("SELECT count(*) FROM pg_class f0");
	int factor = 0;
	for (int64_t rows = n; rows < target; factor++) {
		const auto limit = std::min<int64_t>(n, (target + rows - 1) / rows);
		sql += ", (SELECT 1 FROM pg_class LIMIT " + std::to_string(limit) + ") f" + std::to_string(factor + 1);
		rows *= limit;
	}
	sql += " /* " + nonce + " */";
	// A reads the count and keeps its result open. A retired handle's session closes when its statement's query
	// ends, and an open result keeps the query from ending until A reads to the end below, so the status read
	// after the DETACH cannot race the statement's end, however soon that comes.
	auto &held = a.held;
	auto running = a.Submit([sql, &held](Connection &con) {
		held = make_uniq<QueryResultStream<>>(con.Submit(RedshiftQuery("ex", sql)));
		return OutcomeOf(*held, 1);
	});

	// DETACH once Redshift shows the statement running, so A is waiting in Execute under its pin.
	const auto seen_running = [&] {
		const auto deadline = Clock::now() + std::chrono::seconds(60);
		while (Clock::now() < deadline) {
			const auto outcome = scene.obs->Query(
			    "obs", "SELECT count(*) FROM sys_query_history WHERE session_id = " + std::to_string(pid_a) +
			               " AND status = 'running' AND query_text LIKE '%" + nonce + "%'");
			if (outcome.ok && outcome.rows == Rows {{"1"}}) {
				return true;
			}
			std::this_thread::sleep_for(std::chrono::milliseconds(200));
		}
		return false;
	}();
	REQUIRE(seen_running);
	RequireSql(d, "DETACH ex");
	{
		INFO("the statement finished before DETACH returned, so DETACH may have waited for it");
		REQUIRE(running.wait_for(std::chrono::seconds(0)) != std::future_status::ready);
	}
	scene.CheckActive(pid_a, "A");

	const auto counted = Worker::Wait(std::move(running), "A");
	{
		INFO("A's statement: " << counted);
		REQUIRE(counted.ok);
	}
	// pg_class can gain or lose a row meanwhile, so the count is only known to be positive.
	CHECK(std::stoll(counted.rows.at(0).at(0)) > 0);
	// Reading to the end ends the statement's query, and with it the session the DETACH retired.
	const auto rest = a.Do([&held](Connection &) {
		auto outcome = OutcomeOf(*held);
		held.reset();
		return outcome;
	});
	{
		INFO("A's statement after its count: " << rest);
		CHECK((rest.ok && rest.rows.empty()));
	}
	scene.RequireClosed(pid_a, "A, after its statement");
	RequireRefused(a, NotAttached("ex", true), "A's next statement");
}

TEST_CASE("Live: a DETACH while a bystander holds a result open lets it read every row, then ends the session",
          "[odbc][live][detach]") {
	SKIP_UNLESS_LIVE(redshift_driver::LoadEmbeddedDriver());
	Scene scene;
	auto &a = scene.Start("A");
	auto &d = scene.Start("D");
	const auto pid_a = scene.Pid(a);
	{
		// The statement numbers 1000 rows of pg_class, and the expected rows assume it has them.
		const auto n = AsInteger(a.Query("ex", "SELECT count(*) FROM pg_class"));
		INFO("pg_class has " << n << " rows");
		REQUIRE(n >= 1000);
	}
	const int64_t width = 600;
	const auto sql = "SELECT x.n * 1000 + y.n AS v FROM (SELECT row_number() OVER () AS n FROM pg_class LIMIT 1000) x, "
	                 "(SELECT row_number() OVER () AS n FROM pg_class LIMIT " +
	                 std::to_string(width) + ") y";
	auto &held = a.held;
	// A small streaming buffer, so the scan cannot run ahead to its end while A holds the result.
	RequireSql(a, "SET max_streaming_buffer_size = '16KB'");
	vector<int64_t> values;
	const auto read = [&values](QueryResultStream<> &stream, bool one_chunk) {
		while (auto chunk = stream.Fetch()) {
			for (idx_t row = 0; row < chunk->size(); row++) {
				values.push_back(chunk->GetValue(0, row).GetValue<int64_t>());
			}
			if (one_chunk) {
				break;
			}
		}
		Outcome outcome;
		outcome.ok = !stream.HasError();
		if (!outcome.ok) {
			outcome.thrown = Thrown {stream.GetErrorType(), WithoutQueryLocation(stream.GetErrorObject().RawMessage())};
		}
		return outcome;
	};
	const auto first = a.Do([&](Connection &con) {
		held = make_uniq<QueryResultStream<>>(con.Submit(RedshiftQuery("ex", sql)));
		return read(*held, true);
	});
	{
		INFO("A's first fetch: " << first);
		REQUIRE(first.ok);
		REQUIRE_FALSE(values.empty());
	}
	const auto first_rows = values.size();

	RequireSql(d, "DETACH ex");
	scene.CheckActive(pid_a, "A");
	const auto rest = a.Do([&](Connection &) {
		auto outcome = read(*held, false);
		held.reset();
		return outcome;
	});
	{
		INFO("A's other fetches: " << rest << ", after " << first_rows << " rows first");
		REQUIRE(rest.ok);
	}
	std::sort(values.begin(), values.end());
	vector<int64_t> expected;
	for (int64_t x = 1; x <= 1000; x++) {
		for (int64_t y = 1; y <= width; y++) {
			expected.push_back(x * 1000 + y);
		}
	}
	CHECK(values.size() == expected.size());
	CHECK(values == expected);
	scene.RequireClosed(pid_a, "A, after its result");
}

TEST_CASE("Live: bystanders running statements while the name is detached and attached again, round after round",
          "[odbc][live][detach]") {
	SKIP_UNLESS_LIVE(redshift_driver::LoadEmbeddedDriver());
	const auto rounds = EnvInt("REDSHIFT_TEST_STRESS_ROUNDS", 10);
	const auto seed =
	    static_cast<unsigned>(EnvInt("REDSHIFT_TEST_STRESS_SEED", static_cast<int>(std::random_device {}() >> 1)));
	WARN("stress: " << rounds << " rounds, REDSHIFT_TEST_STRESS_SEED=" << seed);
	// What the loops share, declared before the Scene, so it outlives the threads that use it.
	const std::map<string, string> exact {{NotAttached("ex"), "not attached"},
	                                      {NotAttached("ex", true), "not attached, state lost"},
	                                      {StateLost("ex"), "attached again, state lost"}};
	std::atomic<bool> done {false};
	std::mutex lock;
	std::map<string, std::map<string, int>> counts;
	std::set<int64_t> pids;
	vector<string> unexpected;
	Scene scene;
	const auto before = scene.ActiveSessions();
	auto &a = scene.Start("A");
	auto &b = scene.Start("B");
	auto &d = scene.Start("D");

	const auto loop = [&](const string &who, unsigned loop_seed) {
		return [&, who, loop_seed](Connection &con) {
			std::mt19937 random(loop_seed);
			while (!done) {
				const auto outcome = OutcomeOf(*con.Query(RedshiftQuery("ex", "SELECT pg_backend_pid()")));
				std::lock_guard<std::mutex> guard(lock);
				if (outcome.ok) {
					pids.insert(std::stoll(outcome.rows.at(0).at(0)));
					counts[who]["ran"]++;
				} else {
					string label;
					const auto found = exact.find(outcome.thrown.message);
					if (found != exact.end()) {
						label = found->second;
					}
					for (const auto &race : RACE_REFUSALS) {
						if (label.empty() && StringUtil::StartsWith(outcome.thrown.message, race.second)) {
							label = race.first;
						}
					}
					if (label.empty()) {
						unexpected.push_back(who + ": " + Exception::ExceptionTypeToString(outcome.thrown.type) + ": " +
						                     outcome.thrown.message);
					} else {
						counts[who][label]++;
					}
				}
				std::this_thread::sleep_for(std::chrono::milliseconds(random() % 50));
			}
			return Outcome {true, {}, {}};
		};
	};
	// A failed REQUIRE below unwinds past the loops: this stops them first, so ~Scene can join their threads.
	struct StopLoops {
		std::atomic<bool> &done;
		~StopLoops() {
			done = true;
		}
	} stop_loops {done};
	auto loop_a = a.Submit(loop("A", seed + 1));
	auto loop_b = b.Submit(loop("B", seed + 2));
	std::mt19937 random(seed);
	const auto pause = [&](int low_ms, int high_ms) {
		std::this_thread::sleep_for(
		    std::chrono::milliseconds(low_ms + static_cast<int>(random() % (high_ms - low_ms))));
	};
	// The statements `who` has had answered so far.
	const auto ran = [&](const string &who) {
		std::lock_guard<std::mutex> guard(lock);
		return counts[who]["ran"];
	};
	for (int round = 0; round < rounds; round++) {
		if (round % 2) {
			// A long round: both bystanders have a statement answered on the handle attached now, so each holds a
			// session on it at the next DETACH, however long a connect takes. The first answer after the ATTACH
			// can be a statement that started before it, on the retired handle; the second started after the
			// first ended, so on the handle attached now. The short rounds race connects and binds instead.
			const auto a_before = ran("A");
			const auto b_before = ran("B");
			const auto answered = [&] {
				return ran("A") >= a_before + 2 && ran("B") >= b_before + 2;
			};
			const auto deadline = Clock::now() + std::chrono::seconds(60);
			while (!answered() && Clock::now() < deadline) {
				std::this_thread::sleep_for(std::chrono::milliseconds(100));
			}
			if (!answered()) {
				// Waited for here, not left to ~Scene: a bystander stuck in a statement ends the run after
				// Worker::Wait's bound, and one whose loop threw names its cause.
				done = true;
				Worker::Wait(std::move(loop_a), "A");
				Worker::Wait(std::move(loop_b), "B");
				FAIL("round " << round << ": A had " << ran("A") - a_before << " and B " << ran("B") - b_before
				              << " statements answered in the 60 s after the ATTACH, not two each");
			}
			pause(0, 1000);
		} else {
			pause(100, 1000);
		}
		RequireSql(d, "DETACH ex");
		pause(0, 500);
		RequireSql(d, Attach("ex"));
	}
	std::this_thread::sleep_for(std::chrono::seconds(4));
	done = true;
	Worker::Wait(std::move(loop_a), "A");
	Worker::Wait(std::move(loop_b), "B");

	for (const auto &who : counts) {
		std::string line;
		for (const auto &count : who.second) {
			line += " " + count.first + "=" + std::to_string(count.second);
		}
		WARN(who.first << ":" << line);
	}
	{
		INFO("refusals the race does not allow: " << StringUtil::Join(unexpected, "; "));
		CHECK(unexpected.empty());
	}
	CHECK(counts["A"]["ran"] >= rounds / 2);
	CHECK(counts["B"]["ran"] >= rounds / 2);
	RequireSql(d, "DETACH ex");
	for (auto pid : pids) {
		scene.RequireClosed(pid, "a loop session");
	}
	if (Variable("REDSHIFT_TEST_LIVE_EXCLUSIVE") != "1") {
		WARN("REDSHIFT_TEST_LIVE_EXCLUSIVE is not 1, so sessions left open are not checked");
		return;
	}
	// Sessions no statement answered from, such as a connect that finished after a DETACH, end too.
	std::set<int64_t> left;
	const auto deadline = Clock::now() + std::chrono::seconds(90);
	do {
		left.clear();
		for (auto pid : scene.ActiveSessions()) {
			if (!before.count(pid)) {
				left.insert(pid);
			}
		}
		if (!left.empty()) {
			std::this_thread::sleep_for(std::chrono::seconds(2));
		}
	} while (!left.empty() && Clock::now() < deadline);
	if (!left.empty()) {
		string ids;
		for (auto pid : left) {
			ids += (ids.empty() ? "" : ", ") + std::to_string(pid);
		}
		const auto rows = scene.obs->Query(
		    "obs", "SELECT session_id, start_time, status FROM sys_session_history WHERE session_id IN (" + ids + ")");
		string shown;
		for (const auto &row : rows.rows) {
			shown += "\n  " + StringUtil::Join(row, " ") +
			         (pids.count(std::stoll(row[0])) ? " (answered a loop statement)" : "");
		}
		WARN("left open 90 s after the last DETACH:" << shown);
		// Whether closing the bystanders' DuckDB connections ends them, which tells a session the extension
		// still holds from one nothing holds.
		scene.Stop(a);
		scene.Stop(b);
		std::set<int64_t> still;
		for (auto pid : left) {
			string last;
			if (!scene.ClosedWithin(pid, last, 30)) {
				still.insert(pid);
			}
		}
		WARN(still.size() << " of them still open 30 s after A's and B's DuckDB connections closed");
	}
	INFO(left.size() << " sessions opened during the case are still active 90 s after the last DETACH");
	CHECK(left.empty());
}

// --- ATTACH inside a DuckDB transaction (a rollback retires the handle as DETACH does,
// with the connection that rolled back as the detaching one). Another DuckDB connection sees and can use
// a handle before the ATTACH commits; the attaching connection is refused on it until then. ---------------

namespace {

//! The opening sentence of the transaction gate's refusal (redshift_query.cpp GATE_REASON).
const string GATE_REFUSAL = "redshift_query cannot run while a DuckDB transaction is open because DuckDB COMMIT or "
                            "ROLLBACK does not control Redshift operations.";

//! The opening of the refusal, with the transaction option on, of a handle the open transaction attached.
string AttachedInTransaction(const string &handle) {
	return "redshift_query cannot run on attach handle \"" + handle +
	       "\" because the open DuckDB transaction attached it";
}

} // namespace

TEST_CASE("Live: a rolled-back ATTACH ends the sessions bystanders opened on it, and only the stateful one is told",
          "[odbc][live][writes][detach][attach_in_transaction]") {
	SKIP_UNLESS_LIVE(redshift_driver::LoadEmbeddedDriver());
	Scene scene;
	auto &a = scene.Start("A");
	auto &d = scene.Start("D");
	RequireSql(d, "BEGIN");
	RequireSql(d, Attach("tx"));
	const auto pid_a = scene.MakeStateful(a, "tx");
	auto b = scene.DescribingBystander("B", "tx");
	RequireRefusedStarting(d, GATE_REFUSAL, "D's call inside its transaction", "tx");

	RequireSql(d, "ROLLBACK");
	scene.RequireClosed(pid_a, "A");
	scene.RequireClosed(b.second, "B");
	RequireRefused(a, NotAttached("tx", true), "A's next statement", "tx");
	RequireRefused(a, NotAttached("tx"), "A's second statement", "tx");
	RequireRefused(*b.first, NotAttached("tx"), "B's next statement", "tx");
	RequireRefused(d, NotAttached("tx"), "D's next statement", "tx");
}

TEST_CASE("Live: a committed ATTACH keeps the sessions bystanders opened on it before the commit",
          "[odbc][live][writes][detach][attach_in_transaction]") {
	SKIP_UNLESS_LIVE(redshift_driver::LoadEmbeddedDriver());
	Scene scene;
	auto &a = scene.Start("A");
	auto &d = scene.Start("D");
	RequireSql(d, "BEGIN");
	RequireSql(d, Attach("tx"));
	const auto pid_a = scene.MakeStateful(a, "tx");
	RequireSql(d, "COMMIT");

	CHECK(scene.Pid(a, "tx") == pid_a);
	{
		const auto temp = a.Query("tx", "SELECT count(*) FROM v9_bystander");
		INFO("A's temporary table after the commit: " << temp);
		CHECK(temp.rows == Rows {{"1"}});
	}
	scene.CheckActive(pid_a, "A");
	RequireRuns(d, "D's call after its commit", "tx");
}

TEST_CASE("Live: with the transaction option on, the attaching connection is still refused on its own handle, and "
          "its rollback retires only that handle",
          "[odbc][live][writes][detach][attach_in_transaction]") {
	SKIP_UNLESS_LIVE(redshift_driver::LoadEmbeddedDriver());
	Scene scene;
	auto &a = scene.Start("A");
	auto &d = scene.Start("D");
	RequireSql(d, "SET redshift_allow_query_in_duckdb_transaction = true");
	RequireSql(d, "BEGIN");
	RequireSql(d, Attach("tx"));
	// A handle attached before the transaction runs in it, with the option on.
	const auto pid_d = scene.Pid(d, "ex");
	const auto pid_a = scene.MakeStateful(a, "tx");
	RequireRefusedStarting(d, AttachedInTransaction("tx"), "D's call on the handle its transaction attached", "tx");

	RequireSql(d, "ROLLBACK");
	scene.RequireClosed(pid_a, "A");
	scene.CheckActive(pid_d, "D");
	RequireRefused(a, NotAttached("tx", true), "A's next statement", "tx");
	CHECK(scene.Pid(d, "ex") == pid_d);
}

TEST_CASE("Live: a rolled-back ATTACH ends a bystander's session even while another transaction holds the handle",
          "[odbc][live][writes][detach][attach_in_transaction]") {
	SKIP_UNLESS_LIVE(redshift_driver::LoadEmbeddedDriver());
	Scene scene;
	auto &a = scene.Start("A");
	auto &c = scene.Start("C");
	auto &d = scene.Start("D");
	RequireSql(d, "BEGIN");
	RequireSql(d, Attach("tx"));
	const auto pid_a = scene.MakeStateful(a, "tx");
	// C's transaction takes the catalog by reading through it; the read is refused, which aborts C's
	// transaction but leaves the catalog in it until C ends it.
	RequireSql(c, "BEGIN");
	{
		// The refusal comes from the handle's catalog, so the read reached it.
		const auto browsed = c.Sql("SELECT * FROM tx.some_table");
		INFO("C's read: " << browsed);
		REQUIRE_FALSE(browsed.ok);
		REQUIRE(StringUtil::Contains(browsed.thrown.message, "cannot be browsed as a catalog"));
	}

	RequireSql(d, "ROLLBACK");
	// The rollback's own hook retires the handle: the session ends while C still holds the catalog.
	scene.RequireClosed(pid_a, "A");
	RequireRefused(a, NotAttached("tx", true), "A's next statement", "tx");
	RequireSql(c, "ROLLBACK");
}

TEST_CASE("Live: a DuckDB connection that closes with its ATTACH not committed ends the bystanders' sessions",
          "[odbc][live][writes][detach][attach_in_transaction]") {
	SKIP_UNLESS_LIVE(redshift_driver::LoadEmbeddedDriver());
	Scene scene;
	auto &a = scene.Start("A");
	auto &d = scene.Start("D");
	RequireSql(d, "BEGIN");
	RequireSql(d, Attach("tx"));
	const auto pid_a = scene.MakeStateful(a, "tx");
	auto b = scene.DescribingBystander("B", "tx");

	scene.Stop(d);
	scene.RequireClosed(pid_a, "A");
	scene.RequireClosed(b.second, "B");
	RequireRefused(a, NotAttached("tx", true), "A's next statement", "tx");
	RequireRefused(*b.first, NotAttached("tx"), "B's next statement", "tx");
}

TEST_CASE("Live: a rolled-back ATTACH leaves another connection's newer handle under the name, and the sessions on it",
          "[odbc][live][writes][detach][attach_in_transaction]") {
	SKIP_UNLESS_LIVE(redshift_driver::LoadEmbeddedDriver());
	Scene scene;
	auto &a = scene.Start("A");
	auto &d = scene.Start("D");
	auto &e = scene.Start("E");
	RequireSql(d, "BEGIN");
	RequireSql(d, Attach("tx"));
	const auto pid_old = scene.MakeStateful(a, "tx");
	// E replaces D's uncommitted handle with one of its own, which retires D's: A's session on it ends.
	RequireSql(e, Attach("tx", true));
	scene.RequireClosed(pid_old, "A, on the handle E replaced");
	RequireRefused(a, StateLost("tx"), "A's next statement", "tx");
	const auto pid_new = scene.MakeStateful(a, "tx");

	// DuckDB undoes D's ATTACH only while the name still refers to D's handle (rollback_state.cpp), so E's
	// handle and A's session on it stay.
	RequireSql(d, "ROLLBACK");
	RequireRuns(a, "A's statement after D's rollback", "tx");
	REQUIRE(scene.Pid(a, "tx") == pid_new);
	RequireSql(e, "DETACH tx");
}
