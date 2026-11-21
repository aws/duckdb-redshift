// The same-handle statement fence on the paths SQL tests cannot reach: DuckDB connections opened
// before LOAD, a statement planned while another connection loads the extension, DuckDB's C++ and C
// prepare APIs, Relation API and logical plans, statements another registered state makes DuckDB
// plan again, the CTEs the fence keeps from inlining, and the backstop that refuses a scan the fence
// never checked. Executions are read from the lines the mock logs, through a log storage the test owns.

#include "catch.hpp"

#include <chrono>
#include <condition_variable>
#include <future>
#include <mutex>

#include "duckdb.h"
#include "duckdb.hpp"
#include "duckdb/main/client_context_state.hpp"
#include "duckdb/main/extension/extension_loader.hpp"
#include "duckdb/optimizer/optimizer.hpp"
#include "duckdb/optimizer/optimizer_extension.hpp"
#include "duckdb/parser/parser.hpp"
#include "duckdb/parser/statement/logical_plan_statement.hpp"
#include "duckdb/parser/tableref/table_function_ref.hpp"
#include "duckdb/planner/operator/logical_get.hpp"
#include "duckdb/planner/operator/logical_materialized_cte.hpp"
#include "duckdb/planner/planner_extension.hpp"
#include "duckdb/planner/planner.hpp"
#include "redshift_extension.hpp"
#include "test_log_storage.hpp"
#include "test_util.hpp"

using namespace duckdb;
using redshift_test::Attach;
using redshift_test::BOUND;
using redshift_test::ErrorIn;
using redshift_test::ErrorOf;
using redshift_test::Run;

namespace {

const char *const TWO_SCANS = "SELECT * FROM redshift_query('rs', 'rows=1') a, redshift_query('rs', 'rows=1') b";
const char *const REFUSED = "more than once in the same DuckDB statement";
const char *const UNCHECKED = "was not checked for a second use of the handle";

//! A database whose mock lines are counted. DuckDB loads every linked extension while it starts,
//! unless told not to, so the extension is not loaded until Load(), and a test can open DuckDB
//! connections before it.
struct Fixture {
	//! Counts the mock's lines. Shared with the database's log manager, which writes to it, so the
	//! counts can be read on any thread and after the database closes.
	shared_ptr<redshift_test::RedshiftLineStorage> lines = make_shared_ptr<redshift_test::RedshiftLineStorage>();
	//! The database, owned by the fixture. A pointer so the constructor can configure it first.
	unique_ptr<DuckDB> db;

	Fixture() {
		auto config = redshift_test::NoPersistentSecrets();
		config->options.load_extensions = false;
		db = make_uniq<DuckDB>(nullptr, config.get());
		redshift_test::UseLogStorage(*db->instance, lines);
	}

	void Load() {
		db->LoadStaticExtension<RedshiftExtension>();
	}

	idx_t Described() {
		return lines->Described();
	}

	idx_t Executed() {
		return lines->Executed();
	}
};

//! A table function whose bind waits, up to BOUND, until the test releases it, to hold a statement
//! inside the binder. It returns no rows.
class Gate {
public:
	static Gate &Get() {
		static Gate gate;
		return gate;
	}

	void Reset() {
		std::lock_guard<std::mutex> guard(lock);
		entered = false;
		released = false;
	}

	bool WaitUntilEntered() {
		std::unique_lock<std::mutex> guard(lock);
		return changed.wait_until(guard, std::chrono::system_clock::now() + BOUND, [this]() { return entered; });
	}

	void Release() {
		std::lock_guard<std::mutex> guard(lock);
		released = true;
		changed.notify_all();
	}

	static void Register(DatabaseInstance &db) {
		ExtensionLoader loader(db, "redshift_test_gate");
		loader.RegisterFunction(TableFunction("test_gate", {}, Scan, Bind));
	}

private:
	static unique_ptr<FunctionData> Bind(ClientContext &, TableFunctionBindInput &, vector<LogicalType> &types,
	                                     vector<Identifier> &names) {
		auto &gate = Get();
		{
			std::unique_lock<std::mutex> guard(gate.lock);
			gate.entered = true;
			gate.changed.notify_all();
			// Against the system clock, as MockGate waits, for GCC 11's ThreadSanitizer.
			gate.changed.wait_until(guard, std::chrono::system_clock::now() + BOUND,
			                        [&gate]() { return gate.released; });
		}
		types.push_back(LogicalType::BIGINT);
		names.emplace_back("gate");
		return make_uniq<TableFunctionData>();
	}

	static void Scan(ClientContext &, TableFunctionInput &, DataChunk &output) {
		output.SetChildCardinality(0);
	}

	//! Guards entered and released; changed signals each change.
	std::mutex lock;
	std::condition_variable changed;
	bool entered = false;
	bool released = false;
};

//! Starts `sql` on `planning`, holds it in the binder at the gate while `loading` loads the extension
//! and attaches `rs`, then releases it. Returns the statement's error. Requires that the statement
//! reached the gate and that LOAD and ATTACH succeeded, after releasing the gate.
string RunWhileLoading(Fixture &f, Connection &planning, Connection &loading, const string &sql) {
	Gate::Get().Reset();
	auto query = std::async(std::launch::async, [&]() { return ErrorOf(planning, sql); });
	const bool entered = Gate::Get().WaitUntilEntered();
	string load_error;
	try {
		f.Load();
		load_error = ErrorOf(loading, Attach("rs"));
	} catch (std::exception &ex) {
		load_error = ex.what();
	}
	Gate::Get().Release();
	auto error = query.get();
	REQUIRE(entered);
	REQUIRE(load_error.empty());
	return error;
}

//! A statement RunWhileLoading holds across LOAD, and whether it uses the handle twice.
struct LoadRaceCase {
	//! A string literal.
	const char *sql;
	bool twice;
};

} // namespace

TEST_CASE("a DuckDB connection opened before LOAD is fenced", "[fence]") {
	Fixture f;
	Connection before(*f.db);
	// Also opened before LOAD, and its first statement after it is a prepare.
	Connection prepares_first(*f.db);
	f.Load();
	Run(before, Attach("rs"));

	CHECK_THAT(ErrorOf(before, TWO_SCANS), Catch::Contains(REFUSED));
	auto prepared = prepares_first.Prepare(TWO_SCANS);
	REQUIRE(prepared->HasError());
	CHECK_THAT(prepared->GetError(), Catch::Contains(REFUSED));
	CHECK(f.Executed() == 0);
}

TEST_CASE("a statement planned while the extension loads is fenced when its binding ends", "[fence]") {
	// A statement held in the binder across LOAD is fenced when its binding ends. The gate opens only
	// after LOAD and ATTACH return, so this does not test the order Load registers the fence in.
	const LoadRaceCase cases[] = {
	    {"SELECT * FROM test_gate(), redshift_query('rs', 'rows=1')", false},
	    {"SELECT * FROM test_gate(), redshift_query('rs', 'rows=1') a, redshift_query('rs', 'rows=1') b", true},
	};
	for (auto &c : cases) {
		INFO(c.sql);
		Fixture f;
		Gate::Register(*f.db->instance);
		Connection planning(*f.db);
		Connection loading(*f.db);
		auto error = RunWhileLoading(f, planning, loading, c.sql);
		if (c.twice) {
			CHECK_THAT(error, Catch::Contains(REFUSED));
			CHECK(f.Executed() == 0);
		} else {
			CHECK(error.empty());
			CHECK(f.Executed() == 1);
		}
	}
}

TEST_CASE("a statement prepared while the extension loads is fenced at PREPARE", "[fence]") {
	// The PREPARE is in the binder while another connection loads the extension. Its statement is
	// checked when its binding ends: a single scan is prepared, and runs whenever it is executed,
	// through EXECUTE or EXPLAIN ANALYZE EXECUTE; a double one is refused at PREPARE.
	const LoadRaceCase cases[] = {
	    {"PREPARE p AS SELECT * FROM test_gate(), redshift_query('rs', 'rows=1')", false},
	    {"PREPARE p AS SELECT * FROM test_gate(), redshift_query('rs', 'rows=1') a, redshift_query('rs', 'rows=1') b",
	     true},
	};
	for (auto &c : cases) {
		INFO(c.sql);
		Fixture f;
		Gate::Register(*f.db->instance);
		Connection planning(*f.db);
		Connection loading(*f.db);
		auto error = RunWhileLoading(f, planning, loading, c.sql);
		if (c.twice) {
			CHECK_THAT(error, Catch::Contains(REFUSED));
			CHECK(f.Executed() == 0);
			continue;
		}
		REQUIRE(error.empty());
		for (auto execute : {"EXECUTE p", "EXPLAIN ANALYZE EXECUTE p", "EXECUTE p"}) {
			INFO(execute);
			CHECK(ErrorOf(planning, execute).empty());
		}
		CHECK(f.Executed() == 3);
	}
}

TEST_CASE("a call prepared through the C++ API runs once per execution, its plan kept or bound again", "[fence]") {
	Fixture f;
	f.Load();
	Connection con(*f.db);
	Run(con, Attach("rs"));

	// Prepared once, executed twice.
	auto prepared = con.Prepare("SELECT count(*) FROM redshift_query('rs', 'rows=2')");
	REQUIRE_FALSE(prepared->HasError());
	for (int i = 0; i < 2; i++) {
		auto result = prepared->Execute();
		INFO(ErrorIn(*result));
		REQUIRE_FALSE(result->HasError());
	}
	CHECK(f.Executed() == 2);

	// A parameter inside a redshift_query argument: DuckDB plans the statement at each execution.
	auto deferred = con.Prepare("SELECT count(*) FROM redshift_query('rs', ?)");
	REQUIRE_FALSE(deferred->HasError());
	for (auto rows : {"rows=1", "rows=3"}) {
		auto result = deferred->Execute(rows);
		INFO(ErrorIn(*result));
		REQUIRE_FALSE(result->HasError());
	}
	CHECK(f.Executed() == 4);

	// With the optimizer enabled, DuckDB keeps no plan for a prepared statement that reads a DuckDB
	// table, and binds it again at every execution: each describes the call again, and the fence
	// checks each new plan.
	Run(con, "CREATE TABLE local_rows AS SELECT 1 AS i");
	auto joined = con.Prepare("SELECT count(*) FROM redshift_query('rs', 'rows=2'), local_rows");
	REQUIRE_FALSE(joined->HasError());
	auto described = f.Described();
	for (idx_t i = 1; i <= 2; i++) {
		auto result = joined->Execute();
		INFO(ErrorIn(*result));
		REQUIRE_FALSE(result->HasError());
		CHECK(f.Described() == described + i);
	}
	CHECK(f.Executed() == 6);

	// With it disabled, DuckDB keeps the plan, and binds the statement again only once a table it reads
	// changes.
	Run(con, "PRAGMA disable_optimizer");
	auto kept = con.Prepare("SELECT count(*) FROM redshift_query('rs', 'rows=2'), local_rows");
	REQUIRE_FALSE(kept->HasError());
	described = f.Described();
	REQUIRE_FALSE(kept->Execute()->HasError());
	CHECK(f.Described() == described);
	Run(con, "ALTER TABLE local_rows ADD COLUMN j INTEGER");
	auto rebound = kept->Execute();
	INFO(ErrorIn(*rebound));
	REQUIRE_FALSE(rebound->HasError());
	CHECK(f.Described() == described + 1);
	CHECK(f.Executed() == 8);
}

TEST_CASE("the fence checks Relation API queries like SQL ones", "[fence]") {
	Fixture f;
	f.Load();
	Connection con(*f.db);
	Run(con, Attach("rs"));
	Run(con, Attach("rs2"));

	// The Relation API binds its relations again as it builds the query. Two handles in one relation
	// run; one handle twice is refused.
	auto rs = con.TableFunction("redshift_query", {Value("rs"), Value("rows=2")})->Alias("a");
	auto rs2 = con.TableFunction("redshift_query", {Value("rs2"), Value("rows=3")})->Alias("b");
	auto result = rs->Join(rs2, "true")->Aggregate("count(*)")->Execute();
	INFO(ErrorIn(*result));
	REQUIRE_FALSE(result->HasError());
	CHECK(result->Fetch()->GetValue(0, 0).GetValue<int64_t>() == 6);
	CHECK(f.Executed() == 2);

	auto rs_again = con.TableFunction("redshift_query", {Value("rs"), Value("rows=1")})->Alias("c");
	auto twice = rs->Join(rs_again, "true")->Execute();
	REQUIRE(twice->HasError());
	CHECK_THAT(twice->GetError(), Catch::Contains(REFUSED));
	CHECK(f.Executed() == 2);
}

TEST_CASE("the fence checks statements submitted the way DuckDB's shell submits them", "[fence]") {
	// The shell runs each statement through Connection::Submit.
	Fixture f;
	f.Load();
	Connection con(*f.db);
	Run(con, Attach("rs"));
	auto one = con.Submit("SELECT * FROM redshift_query('rs', 'rows=1')");
	REQUIRE_FALSE(one->HasError());
	auto chunk = one->Fetch();
	REQUIRE(chunk);
	CHECK(chunk->GetValue(0, 0).ToString() == "r0c0");
	one.reset();
	CHECK(f.Executed() == 1);

	auto twice = con.Submit(TWO_SCANS);
	REQUIRE(twice->HasError());
	CHECK_THAT(twice->GetError(), Catch::Contains(REFUSED));
	CHECK(f.Executed() == 1);
}

TEST_CASE("the fence checks statements through the C API's query and prepare", "[fence]") {
	// The C API opens a database of its own, which loads the linked extension as it starts, and
	// executions are read back from duckdb_logs.
	duckdb_database db;
	duckdb_connection con;
	REQUIRE(duckdb_open(nullptr, &db) == DuckDBSuccess);
	REQUIRE(duckdb_connect(db, &con) == DuckDBSuccess);
	//! Runs `sql` and returns its error, or "" when it succeeded.
	auto run = [&](const string &sql) {
		duckdb_result result;
		auto state = duckdb_query(con, sql.c_str(), &result);
		string error = state == DuckDBError ? duckdb_result_error(&result) : "";
		duckdb_destroy_result(&result);
		return error;
	};
	//! The BIGINT in the first row and column of `result`, which it destroys.
	auto first_bigint = [](duckdb_result &result) {
		int64_t value = -1;
		auto chunk = duckdb_fetch_chunk(result);
		if (chunk && duckdb_data_chunk_get_size(chunk) > 0) {
			value = static_cast<int64_t *>(duckdb_vector_get_data(duckdb_data_chunk_get_vector(chunk, 0)))[0];
		}
		duckdb_destroy_data_chunk(&chunk);
		duckdb_destroy_result(&result);
		return value;
	};
	auto executed = [&]() {
		duckdb_result result;
		REQUIRE(duckdb_query(
		            con,
		            "SELECT count(*) FROM duckdb_logs WHERE message LIKE 'redshift mock: executed a statement on %'",
		            &result) == DuckDBSuccess);
		return first_bigint(result);
	};
	CHECK(run("SET allow_persistent_secrets = false").empty());
	CHECK(run("CALL enable_logging(level = 'debug')").empty());
	CHECK(run("LOAD redshift").empty());
	CHECK(run(Attach("rs")).empty());

	duckdb_prepared_statement statement;
	REQUIRE(duckdb_prepare(con, "SELECT count(*) FROM redshift_query('rs', 'rows=2')", &statement) == DuckDBSuccess);
	for (int i = 0; i < 2; i++) {
		duckdb_result result;
		auto state = duckdb_execute_prepared(statement, &result);
		INFO((state == DuckDBError ? duckdb_result_error(&result) : ""));
		CHECK(state == DuckDBSuccess);
		if (state == DuckDBSuccess) {
			CHECK(first_bigint(result) == 2);
		} else {
			duckdb_destroy_result(&result);
		}
	}
	duckdb_destroy_prepare(&statement);
	CHECK(executed() == 2);

	REQUIRE(duckdb_prepare(con, TWO_SCANS, &statement) == DuckDBError);
	CHECK_THAT(string(duckdb_prepare_error(statement)), Catch::Contains(REFUSED));
	duckdb_destroy_prepare(&statement);
	CHECK(executed() == 2);

	// duckdb_query: one scan runs, two on one handle are refused.
	CHECK(run("SELECT * FROM redshift_query('rs', 'rows=1')").empty());
	CHECK(executed() == 3);
	CHECK_THAT(run(TWO_SCANS), Catch::Contains(REFUSED));
	CHECK(executed() == 3);

	duckdb_disconnect(&con);
	duckdb_close(&db);
}

namespace {

//! Asks DuckDB to plan each statement a second time, as any extension may through DuckDB's rebind
//! hook. DuckDB then plans it again without calling any state's hooks.
class RebindEveryStatement : public ClientContextState {
public:
	bool CanRequestRebind() override {
		return true;
	}
	RebindQueryInfo OnFinalizePrepare(ClientContext &, PreparedStatementData &, PreparedStatementMode) override {
		rebinds++;
		return RebindQueryInfo::ATTEMPT_TO_REBIND;
	}
	idx_t rebinds = 0;
};

const char *const RECURSIVE = "recursive part of a recursive CTE";

//! Statements and what each must do: refuse with `error`, or run, and execute `executions` times.
struct Expectation {
	//! String literals; `error` is empty for a statement that runs.
	const char *sql;
	const char *error;
	idx_t executions;
};

const Expectation EXPECTATIONS[] = {
    {"SELECT * FROM redshift_query('rs', 'rows=1')", "", 1},
    {TWO_SCANS, REFUSED, 0},
    {"SELECT * FROM redshift_query('rs', 'rows=1') a, redshift_query('rs2', 'rows=1') b", "", 2},
    {"WITH c AS NOT MATERIALIZED (SELECT * FROM redshift_query('rs', 'rows=1')) SELECT * FROM c a, c b", "", 1},
    {"SELECT c0, count(*) OVER (PARTITION BY c0) FROM redshift_query('rs', 'rows=2')", "", 1},
    {"SELECT c0, count(*) FROM redshift_query('rs', 'rows=2') GROUP BY ROLLUP (c0)", "", 1},
    {"WITH RECURSIVE r(n) AS (SELECT 1 UNION ALL SELECT n + 1 FROM r, redshift_query('rs', 'rows=1') WHERE n < 3) "
     "SELECT * FROM r",
     RECURSIVE, 0},
    {"PREPARE p AS SELECT * FROM redshift_query('rs', 'rows=1')", "", 0},
    {"EXECUTE p", "", 1},
    {"PREPARE q AS SELECT * FROM redshift_query('rs', 'rows=1') a, redshift_query('rs', ?) b", "", 0},
    {"EXECUTE q('rows=2')", REFUSED, 0},
    {"EXPLAIN ANALYZE SELECT * FROM redshift_query('rs', 'rows=1') a, redshift_query('rs', 'rows=1') b", REFUSED, 0},
    {"CREATE TEMP TABLE staged AS SELECT * FROM redshift_query('rs', 'rows=2')", "", 1},
    {"SELECT * FROM staged, redshift_query('rs', 'rows=1')", "", 1},
    {"CREATE TEMP TABLE outer_left AS SELECT 'r0c0' AS a", "", 0},
    {"SELECT * FROM outer_left l LEFT JOIN outer_left r ON l.a = r.a AND l.a IN (SELECT c0 FROM "
     "redshift_query('rs', 'rows=1'))",
     "", 1},
    {"SELECT * FROM redshift_query('rs', 'rows=1') x, outer_left l LEFT JOIN outer_left r ON l.a = r.a AND l.a IN "
     "(SELECT c0 FROM redshift_query('rs', 'rows=1'))",
     REFUSED, 0},
    {"WITH RECURSIVE src AS (SELECT * FROM redshift_query('rs', 'rows=1')), r(i) AS (SELECT 1 UNION ALL SELECT "
     "r.i + 1 FROM r, src WHERE r.i < 5) SELECT count(*) FROM r",
     RECURSIVE, 0},
    {"WITH RECURSIVE src AS MATERIALIZED (SELECT * FROM redshift_query('rs', 'rows=1')), r(i) AS (SELECT 1 UNION "
     "ALL SELECT r.i + 1 FROM r, src WHERE r.i < 5) SELECT count(*) FROM r",
     "", 1},
    {"PIVOT redshift_query('rs', 'rows=2') ON c0 USING count(*)", "ahead of a PIVOT", 0},
    {"WITH RECURSIVE r(n) AS (SELECT 1 UNION ALL SELECT r.n + 1 FROM r, (WITH s AS MATERIALIZED (SELECT * FROM "
     "redshift_query('rs', 'rows=1')) SELECT * FROM s) x WHERE r.n < 3) SELECT * FROM r",
     RECURSIVE, 0},
};

} // namespace

TEST_CASE("statements another state makes DuckDB plan again are fenced like any other", "[fence]") {
	// DuckDB's second planning pass calls no state's hooks, but it binds the statement again, and the
	// fence runs when binding ends. The optimizer setting does not matter: the fence checks the plan
	// before the optimizer runs.
	for (int disable_optimizer = 0; disable_optimizer < 2; disable_optimizer++) {
		INFO("optimizer disabled: " << disable_optimizer);
		Fixture f;
		f.Load();
		Connection con(*f.db);
		Run(con, Attach("rs"));
		Run(con, Attach("rs2"));
		if (disable_optimizer) {
			Run(con, "PRAGMA disable_optimizer");
		}
		auto state = con.context->registered_state->GetOrCreate<RebindEveryStatement>("redshift_test_rebind");
		for (auto &e : EXPECTATIONS) {
			INFO(e.sql);
			auto before = f.Executed();
			auto error = ErrorOf(con, e.sql);
			INFO(error);
			if (string(e.error).empty()) {
				CHECK(error.empty());
			} else {
				CHECK_THAT(error, Catch::Contains(e.error));
			}
			CHECK(f.Executed() - before == e.executions);
		}
		CHECK(state->rebinds > 0);
	}
}

namespace {

//! Plans and optimizes `sql` the way DuckDB's own LogicalPlanStatement tests do, in a transaction
//! DuckDB starts and ends itself, as ClientContext::ExtractPlan does, so the connection stays in
//! autocommit. Unoptimized, even SELECT 42 fails a DuckDB assertion on its result types, extension or
//! not; ExtractPlan's own plan has its column references resolved, which binding it again refuses.
unique_ptr<LogicalOperator> PlanOf(Connection &con, const string &sql) {
	unique_ptr<LogicalOperator> plan;
	con.context->RunFunctionInTransaction([&]() {
		Parser parser(*con.context);
		parser.ParseQuery(sql);
		Planner planner(*con.context);
		planner.CreatePlan(std::move(parser.statements[0]));
		Optimizer optimizer(*planner.binder, *con.context);
		plan = optimizer.Optimize(std::move(planner.plan));
	});
	return plan;
}

} // namespace

TEST_CASE("a C++ caller can run a logical plan once the extension is loaded", "[fence]") {
	// Running the LogicalPlanStatement passes its plan through Planner::CreatePlan, which runs the
	// post-bind fence on it again without binding its expressions again.
	Fixture f;
	f.Load();
	Connection con(*f.db);
	Run(con, Attach("rs"));
	struct Plan {
		//! A string literal.
		const char *sql;
		idx_t executions;
	};
	const Plan plans[] = {
	    {"SELECT 42", 0},
	    {"SELECT * FROM redshift_query('rs', 'rows=2')", 1},
	};
	for (auto &p : plans) {
		INFO(p.sql);
		auto before = f.Executed();
		auto result = con.Query(make_uniq<LogicalPlanStatement>(PlanOf(con, p.sql)));
		INFO(ErrorIn(*result));
		CHECK_FALSE(result->HasError());
		CHECK(f.Executed() - before == p.executions);
	}
	// Two scans on one handle are refused while the plan is made.
	auto thrown = redshift_test::Capture([&]() { PlanOf(con, TWO_SCANS); });
	CHECK(thrown.type == ExceptionType::BINDER);
	CHECK_THAT(thrown.message, Catch::Contains(REFUSED));
	CHECK(f.Executed() == 1);
}

namespace {

//! What CTE inlining left of a plan: each materialized CTE's setting, outermost first, and the number
//! of redshift_query scans.
struct PlanShape {
	vector<CTEMaterialize> ctes;
	idx_t scans = 0;
};

void CollectShape(LogicalOperator &op, PlanShape &shape) {
	if (op.type == LogicalOperatorType::LOGICAL_MATERIALIZED_CTE) {
		shape.ctes.push_back(op.Cast<LogicalMaterializedCTE>().materialize);
	}
	if (op.type == LogicalOperatorType::LOGICAL_GET && op.Cast<LogicalGet>().function.GetName() == "redshift_query") {
		shape.scans++;
	}
	for (auto &child : op.children) {
		CollectShape(*child, shape);
	}
}

} // namespace

TEST_CASE("a CTE read more than once keeps its one scan without CTE inlining trying to copy it", "[fence]") {
	// Inlining such a CTE would copy its definition into each reference. DuckDB keeps the CTE when the
	// copy fails, but it learns that by catching the scan's serialization refusal by type, which a
	// loadable on macOS throws as a type the catch does not match. The fence pins the CTE instead, so
	// the plan keeps it AS MATERIALIZED on every platform. A CTE read once is still inlined, which moves
	// its scan without copying it.
	Fixture f;
	f.Load();
	Connection con(*f.db);
	Run(con, Attach("rs"));
	const auto ALWAYS = CTEMaterialize::CTE_MATERIALIZE_ALWAYS;
	struct Case {
		//! A string literal.
		const char *sql;
		vector<CTEMaterialize> ctes;
		idx_t executions;
	};
	const Case cases[] = {
	    // Not materialized, so DuckDB inlines it whatever it costs.
	    {"WITH c AS NOT MATERIALIZED (SELECT * FROM redshift_query('rs', 'rows=1')) "
	     "SELECT a.c0, b.c0 FROM c a, c b",
	     {ALWAYS},
	     1},
	    // The default, inlined under a LIMIT so that the LIMIT can stop the scan.
	    {"WITH c AS (SELECT * FROM redshift_query('rs', 'rows=1')) SELECT a.c0, b.c0 FROM c a, c b LIMIT 1",
	     {ALWAYS},
	     1},
	    // One reference from a scalar subquery, which binding moves into the plan as a cross product.
	    {"WITH c AS NOT MATERIALIZED (SELECT * FROM redshift_query('rs', 'rows=1')) "
	     "SELECT c0, (SELECT count(*) FROM c) FROM c",
	     {ALWAYS},
	     1},
	    // One reference from a subquery in an outer join's condition, which is still a plan inside an
	    // expression when binding ends.
	    {"WITH c AS NOT MATERIALIZED (SELECT * FROM redshift_query('rs', 'rows=1')) "
	     "SELECT * FROM c, (SELECT 'r0c0' AS a) l LEFT JOIN (SELECT 'r0c0' AS b) r "
	     "ON l.a = r.b AND l.a IN (SELECT c0 FROM c)",
	     {ALWAYS},
	     1},
	    // Read through another CTE that is read twice: that one is pinned, and this one, read once, is
	    // inlined into its definition.
	    {"WITH a AS NOT MATERIALIZED (SELECT * FROM redshift_query('rs', 'rows=1')), "
	     "b AS NOT MATERIALIZED (SELECT * FROM a) SELECT x.c0, y.c0 FROM b x, b y",
	     {ALWAYS},
	     1},
	    // Read once in the body and once from another CTE's definition, which inlining then moves into
	    // the body.
	    {"WITH a AS NOT MATERIALIZED (SELECT * FROM redshift_query('rs', 'rows=1')), "
	     "b AS NOT MATERIALIZED (SELECT * FROM a) SELECT x.c0, y.c0 FROM a x, b y",
	     {ALWAYS},
	     1},
	    // Read once, from the outer side of a correlated subquery, which decorrelation can copy.
	    {"WITH c AS NOT MATERIALIZED (SELECT * FROM redshift_query('rs', 'rows=2')) "
	     "SELECT c0 FROM c WHERE EXISTS (SELECT 1 FROM range(3) t WHERE t.range::VARCHAR <> c.c0)",
	     {ALWAYS},
	     1},
	    // Read once, by an outer join whose condition holds a correlated subquery, which decorrelation can
	    // copy too, once the subquery is planned after binding.
	    {"WITH c AS NOT MATERIALIZED (SELECT * FROM redshift_query('rs', 'rows=2')) "
	     "SELECT c.c0, t.range FROM c LEFT JOIN range(2) t "
	     "ON t.range::VARCHAR <> c.c0 AND EXISTS (SELECT 1 FROM range(1) u WHERE u.range::VARCHAR <> c.c0)",
	     {ALWAYS},
	     1},
	    // A plain EXPLAIN, which runs nothing, but whose plan DuckDB optimizes all the same.
	    {"EXPLAIN WITH c AS NOT MATERIALIZED (SELECT * FROM redshift_query('rs', 'rows=1')) "
	     "SELECT a.c0, b.c0 FROM c a, c b",
	     {ALWAYS},
	     0},
	    // A plain EXPLAIN of a CTE read from a recursive part and from the body: the recursive part's
	    // reference is not refused there, but still counts toward the pin.
	    {"EXPLAIN WITH RECURSIVE src AS NOT MATERIALIZED (SELECT * FROM redshift_query('rs', 'rows=1')), "
	     "r(i) AS (SELECT 1 UNION ALL SELECT r.i + 1 FROM r, src WHERE r.i < 5) SELECT * FROM r, src",
	     {ALWAYS},
	     0},
	    // Read once: inlined.
	    {"WITH c AS NOT MATERIALIZED (SELECT * FROM redshift_query('rs', 'rows=1')) SELECT * FROM c", {}, 1},
	};
	for (auto &c : cases) {
		INFO(c.sql);
		PlanShape shape;
		CollectShape(*PlanOf(con, c.sql), shape);
		CHECK(shape.ctes == c.ctes);
		CHECK(shape.scans == 1);
		auto before = f.Executed();
		Run(con, c.sql);
		CHECK(f.Executed() - before == c.executions);
	}
}

namespace {

//! Rebuilds every redshift_query scan's bind data after the optimizer, as DuckDB itself rebuilds a
//! table function's bind data when it deserializes a copied scan (LogicalGet::Deserialize). The new
//! bind data never passed the fence, which stands in for any path that adds a scan after binding.
void RebuildRedshiftScans(ClientContext &context, LogicalOperator &op) {
	if (op.type == LogicalOperatorType::LOGICAL_GET) {
		auto &get = op.Cast<LogicalGet>();
		if (get.function.GetName() == "redshift_query") {
			TableFunctionRef empty_ref;
			TableFunctionBindInput input(get.parameters, get.named_parameters, get.input_table_types,
			                             get.input_table_names, get.function.function_info.get(), nullptr, get.function,
			                             empty_ref, nullptr);
			vector<LogicalType> types;
			vector<Identifier> names;
			get.bind_data = get.function.bind(context, input, types, names);
		}
	}
	for (auto &child : op.children) {
		RebuildRedshiftScans(context, *child);
	}
}

} // namespace

TEST_CASE("a scan the fence never checked is refused before it runs", "[fence]") {
	Fixture f;
	f.Load();
	OptimizerExtension rebuild;
	rebuild.optimize_function = [](OptimizerExtensionInput &input, unique_ptr<LogicalOperator> &plan) {
		RebuildRedshiftScans(input.context, *plan);
	};
	OptimizerExtension::Register(DBConfig::GetConfig(*f.db->instance), std::move(rebuild));
	Connection con(*f.db);
	Run(con, Attach("rs"));

	auto error = ErrorOf(con, "SELECT * FROM redshift_query('rs', 'rows=1')");
	CHECK(error == "Executor Error: redshift_query: the call on attach handle \"rs\" was not checked for a second "
	               "use of the handle, so it was not run, though other calls in the same DuckDB statement may have "
	               "run; check their effects on Redshift before running the statement again. Something, such as "
	               "another extension, changed the statement's plan after redshift_query checked it. Report this as "
	               "a fault in the Redshift extension or in how it works with the other extensions loaded.");
	CHECK(f.Executed() == 0);
}

namespace {

//! Rebuilds the bind data of the redshift_query scans on the probe side, the left, of the first cross
//! product, and returns whether it found one. DuckDB runs the build side first.
bool RebuildProbeSide(ClientContext &context, LogicalOperator &op) {
	if (op.type == LogicalOperatorType::LOGICAL_CROSS_PRODUCT) {
		RebuildRedshiftScans(context, *op.children[0]);
		return true;
	}
	for (auto &child : op.children) {
		if (RebuildProbeSide(context, *child)) {
			return true;
		}
	}
	return false;
}

} // namespace

TEST_CASE("a scan the fence never checked is refused even after a checked one has run", "[fence]") {
	// Only the probe side's scan is rebuilt, so the build side's checked scan has run by the time the
	// probe side's is refused, and the refusal says other calls may have run.
	Fixture f;
	f.Load();
	OptimizerExtension rebuild;
	rebuild.optimize_function = [](OptimizerExtensionInput &input, unique_ptr<LogicalOperator> &plan) {
		RebuildProbeSide(input.context, *plan);
	};
	OptimizerExtension::Register(DBConfig::GetConfig(*f.db->instance), std::move(rebuild));
	Connection con(*f.db);
	Run(con, Attach("rs"));
	Run(con, Attach("rs2"));

	auto error = ErrorOf(con, "SELECT * FROM redshift_query('rs', 'rows=1') a, redshift_query('rs2', 'rows=1') b");
	CHECK_THAT(error,
	           Catch::Contains(UNCHECKED) && Catch::Contains("other calls in the same DuckDB statement may have run"));
	CHECK(f.Executed() == 1);
}

namespace {

//! Turns every AS MATERIALIZED CTE back into an inlinable one after binding, as a planner hook another
//! extension registers after redshift_query's can rewrite a plan the fence has already checked.
void UnmaterializeCTEs(LogicalOperator &op) {
	if (op.type == LogicalOperatorType::LOGICAL_MATERIALIZED_CTE) {
		op.Cast<LogicalMaterializedCTE>().materialize = CTEMaterialize::CTE_MATERIALIZE_DEFAULT;
	}
	for (auto &child : op.children) {
		UnmaterializeCTEs(*child);
	}
}

} // namespace

TEST_CASE("a scan started again in the same statement is refused before it runs again", "[fence]") {
	// The fence accepts a recursive part reading a CTE declared AS MATERIALIZED. The test's hook then
	// makes the CTE inlinable, and DuckDB's optimizer moves the scan into the recursive part, which
	// starts it once per iteration. Execution refuses the second start, so the statement runs once.
	Fixture f;
	f.Load();
	PlannerExtension rewrite;
	rewrite.post_bind_function = [](PlannerExtensionInput &, BoundStatement &statement) {
		if (statement.plan) {
			UnmaterializeCTEs(*statement.plan);
		}
	};
	PlannerExtension::Register(DBConfig::GetConfig(*f.db->instance), std::move(rewrite));
	Connection con(*f.db);
	Run(con, Attach("rs"));

	auto error =
	    ErrorOf(con, "WITH RECURSIVE src AS MATERIALIZED (SELECT * FROM redshift_query('rs', 'rows=1')), r(i) "
	                 "AS (SELECT 1 UNION ALL SELECT r.i + 1 FROM r, src WHERE r.i < 5) SELECT count(*) FROM r");
	CHECK_THAT(error,
	           Catch::StartsWith("Executor Error: redshift_query: DuckDB started the Redshift statement on attach "
	                             "handle \"rs\" a second time in the same DuckDB statement") &&
	               Catch::Contains("It ran once") && Catch::Contains("check them before running the statement again"));
	CHECK(f.Executed() == 1);

	// A cached plan runs once in each statement that executes it.
	Run(con, "PREPARE once AS SELECT * FROM redshift_query('rs', 'rows=1')");
	Run(con, "EXECUTE once");
	Run(con, "EXECUTE once");
	CHECK(f.Executed() == 3);
}
