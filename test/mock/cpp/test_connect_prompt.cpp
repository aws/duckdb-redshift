// The DuckDB CLI's default prompt while CONNECT-ed. The shell's prompt code is in DuckDB's
// duckdb_shell library, which this suite links, so the first case runs it as the CLI does and pins
// the query it sends: a DuckDB version that sends another text fails here, and
// DUCKDB_SHELL_PROMPT_QUERY (redshift_query.hpp) is then changed to match. The mock binds no
// buffers, so the short length Redshift declares for the uncast query is not checked here.

#include "catch.hpp"

#include <cstring>
#include <mutex>

#include "duckdb.hpp"
#include "duckdb/common/string_util.hpp"
#include "duckdb/logging/log_manager.hpp"
#include "duckdb/logging/log_sink.hpp"
#include "duckdb/logging/log_type.hpp"
#include "duckdb/main/database.hpp"
#include "redshift_extension.hpp"
#include "redshift_query.hpp"
#include "shell_extension.hpp"
#include "shell_prompt.hpp"
#include "shell_state.hpp"
#include "test_util.hpp"

using namespace duckdb;
using redshift_test::Run;

namespace {

//! Keeps the text of each query DuckDB logs as QueryLog, in order.
class QueryLines : public LogSink {
public:
	const string GetSinkName() override {
		return "redshift_test_queries";
	}

	void WriteLogEntry(timestamp_t, LogLevel, const string &log_type, const string &log_message,
	                   const RegisteredLoggingContext &) override {
		if (log_type != QueryLogType::NAME) {
			return;
		}
		std::lock_guard<std::mutex> guard(lock);
		queries.push_back(log_message);
	}

	void WriteLogEntries(DataChunk &, const RegisteredLoggingContext &) override {
	}

	void FlushAll() override {
	}

	void Flush(LoggingTargetTable) override {
	}

	bool IsEnabled(LoggingTargetTable) override {
		return true;
	}

	//! The queries logged since the last call, and forgets them.
	vector<string> Take() {
		std::lock_guard<std::mutex> guard(lock);
		auto taken = std::move(queries);
		queries.clear();
		return taken;
	}

private:
	std::mutex lock;
	vector<string> queries;
};

//! A database with the extension and the mock handle `rs`, logging each query's text to `queries`.
unique_ptr<DuckDB> PromptDatabase(shared_ptr<QueryLines> queries) {
	auto db = make_uniq<DuckDB>(nullptr, redshift_test::NoPersistentSecrets().get());
	db->LoadStaticExtension<RedshiftExtension>();
	auto &log_manager = db->instance->GetLogManager();
	shared_ptr<LogSink> registered = std::move(queries); // RegisterLogSink moves from it
	REQUIRE(log_manager.RegisterLogSink("redshift_test_queries", registered));
	log_manager.SetLogSink(*db->instance, "redshift_test_queries");
	log_manager.SetLogLevel(LogLevel::LOG_INFO);
	log_manager.SetEnableLogging(true);
	Connection con(*db);
	Run(con, redshift_test::Attach("rs"));
	return db;
}

//! The one value `con` returns for `sql`, as text.
string OnlyValue(Connection &con, const string &sql) {
	auto result = con.Query(sql);
	REQUIRE_FALSE(result->HasError());
	REQUIRE(result->ColumnCount() == 1);
	auto chunk = result->Fetch();
	REQUIRE(chunk);
	REQUIRE(chunk->size() == 1);
	return chunk->GetValue(0, 0).ToString();
}

} // namespace

namespace {

//! Drops the database and connection a case gave the shell's process-wide state, connection first,
//! so the next case starts without them.
struct ShellDatabaseReset {
	explicit ShellDatabaseReset(duckdb_shell::ShellState &state_p) : state(state_p) {
	}
	~ShellDatabaseReset() {
		state.conn.reset();
		state.db.reset();
	}
	duckdb_shell::ShellState &state;
};

//! The CLI's own state, with its default prompt (ShellState::Initialize), its shell extension and
//! grammar as the CLI loads them when it opens a database, CONNECT-ed to the mock handle, and the
//! queries its prompt sends. The state is the shell's process-wide one, as in the CLI, because
//! Initialize gives linenoise pointers into it. `reset` is built before the body runs, so the
//! database and connection go even when a REQUIRE in the body throws.
struct CliPrompt {
	CliPrompt() : queries(make_shared_ptr<QueryLines>()), state(duckdb_shell::ShellState::Get()), reset(state) {
		state.db = PromptDatabase(queries);
		state.conn = make_uniq<Connection>(*state.db);
		state.db->LoadStaticExtension<ShellExtension>();
		Run(*state.conn, "SET active_grammar_extensions=['shell']");
		Run(*state.conn, "CONNECT rs");
		state.Initialize();
		queries->Take();
	}

	//! The prompt, evaluated as the CLI does before each statement.
	string Generate() {
		return state.main_prompt->GeneratePrompt(state);
	}

	shared_ptr<QueryLines> queries;
	duckdb_shell::ShellState &state;
	ShellDatabaseReset reset;
};

//! A rewording a later DuckDB could give the prompt's query: the same value, built with concat.
constexpr const char *REWORDED_PROMPT_QUERY = "SELECT CASE WHEN current_schema() = 'main' THEN current_database() "
                                              "ELSE concat(current_database(), '.', current_schema()) END";

} // namespace

TEST_CASE("DuckDB v2.0.1's CLI prompt still sends DUCKDB_SHELL_PROMPT_QUERY while CONNECT-ed", "[connect]") {
	// If this fails after the DuckDB pin moves, set DUCKDB_SHELL_PROMPT_QUERY to the query the
	// failure shows; the last case then says what DUCKDB_SHELL_PROMPT_QUERY_SENT must become.
	CliPrompt cli;

	const auto prompt = cli.Generate();

	CHECK(cli.queries->Take() == vector<string> {DUCKDB_SHELL_PROMPT_QUERY});
	// The mock answers with the text it was sent, so the prompt shows the cast form, cut to the
	// prompt's 50 characters, and no #ERROR.
	CHECK(StringUtil::Contains(prompt, "rs "));
	CHECK(StringUtil::Contains(prompt, "SELECT (CASE WHEN current_schema()"));
	CHECK_FALSE(StringUtil::Contains(prompt, "#ERROR"));
}

TEST_CASE("A CLI prompt that sends another query fails the pin's comparison and is sent uncast", "[connect]") {
	// Stands in for a DuckDB whose shell rewords the query: the default prompt's components, with the
	// setting replaced by the reworded query, which the shell sends through the same
	// Prompt::ExecuteSQL. The pin's comparison above then fails, and the query reaches Redshift as
	// it is, and the client would refuse its value.
	CliPrompt cli;
	cli.state.main_prompt = make_uniq<duckdb_shell::Prompt>();
	cli.state.main_prompt->ParsePrompt(string("{max_length:50}{setting:connect_name_prefix}{sql:") +
	                                   REWORDED_PROMPT_QUERY + "} D ");

	const auto prompt = cli.Generate();

	const auto sent = cli.queries->Take();
	CHECK(sent == vector<string> {REWORDED_PROMPT_QUERY});
	CHECK_FALSE(sent == vector<string> {DUCKDB_SHELL_PROMPT_QUERY});
	CHECK(ConnectedStatementText(REWORDED_PROMPT_QUERY) == REWORDED_PROMPT_QUERY);
	// The mock's echo shows the uncast text.
	CHECK(StringUtil::Contains(prompt, "rs SELECT CASE WHEN current_schema()"));
}

TEST_CASE("CONNECT sends the CLI prompt's query cast, and every other text as it is", "[connect]") {
	auto queries = make_shared_ptr<QueryLines>();
	auto db = PromptDatabase(queries);
	Connection con(*db);
	const string query = DUCKDB_SHELL_PROMPT_QUERY;
	// The cast form is the pinned query's CASE, cast to the longest VARCHAR, under the column name
	// the uncast query has, so a pin update that changes only one of the two fails here.
	CHECK(string(DUCKDB_SHELL_PROMPT_QUERY_SENT) ==
	      "SELECT (" + query.substr(strlen("SELECT ")) + ")::varchar(65535) AS \"case\"");
	// The query a user writes in redshift_query is theirs: only CONNECT's hand-off changes it.
	CHECK(OnlyValue(con, "SELECT * FROM redshift_query('rs', '" + StringUtil::Replace(query, "'", "''") + "')") ==
	      query);

	Run(con, "CONNECT rs");
	// The mock answers with the text it was sent.
	CHECK(OnlyValue(con, query) == DUCKDB_SHELL_PROMPT_QUERY_SENT);
	// Compared whole: with a terminator, as a user types it, it is another text and passes as it is.
	CHECK(OnlyValue(con, query + ";") == query + ";");
	CHECK(OnlyValue(con, query + " ") == query + " ");
	// DuckDB hands over the text with its leading whitespace, so that is another text too.
	CHECK(OnlyValue(con, " " + query) == " " + query);
	CHECK(OnlyValue(con, "SELECT current_schema()") == "SELECT current_schema()");
	Run(con, "DISCONNECT");
	CHECK(ConnectedStatementText(query) == DUCKDB_SHELL_PROMPT_QUERY_SENT);
	CHECK(ConnectedStatementText(query + " ") == query + " ");
	CHECK(ConnectedStatementText(" " + query) == " " + query);
	CHECK(ConnectedStatementText("SELECT 1") == "SELECT 1");
}
