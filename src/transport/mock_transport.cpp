//===----------------------------------------------------------------------===//
// The mock client, compiled in by REDSHIFT_CLIENT=mock.
//
// The shipping client is the bundled Amazon Redshift ODBC driver; this mock lets the SQL
// surface be tested without a driver, credentials or a network.
//
// An ODBC build compiles src/transport/odbc/ instead, whose OpenRedshiftConnection
// takes this one's place. Nothing above the interface changes between the two -- that
// is the test of whether the interface is right.
//
// Apart from the gaps below, no directive or control answers other than the real client can through
// the driver, so a test never builds the SQL surface on an answer it will not get. It differs from the
// real client in four ways: some of the real client's refusals are missing, such as the
// parameter-count refusal; the multi-statement refusal, and a refusal
// without sqlstates=, carry none of the sqlstate and sqlstates the real client attaches; the mock
// executes a statement with an UNKNOWN column, which the real client refuses at Execute with a
// BinderException naming the column, and a C++ test that does so reads TEXT cells; and an error= refusal shows its text
// as Redshift's for any SQLSTATE class, where the real client shows Redshift's text only for a list of
// classes (42, 22, 23, 0A, XX, 3D, 3F, 2B, 25, 40, 55, 57, P0) and its own message for the rest. Some answers are not
// confirmed against Redshift; the directives that give them say so. C++ tests also reach the controls declared in
// mock_transport.hpp, defined below.
//===----------------------------------------------------------------------===//

#include "mock_transport.hpp"
#include "redshift_transport.hpp"
#include "statement_shape.hpp"

#include "duckdb/common/exception.hpp"
#include "duckdb/common/exception/binder_exception.hpp"
#include "duckdb/common/string_util.hpp"

#include <algorithm>
#include <cstring>
#include <string>

namespace duckdb {

//===--------------------------------------------------------------------===//
// Test controls
//===--------------------------------------------------------------------===//

//! The gates HoldNextMockOpen and HoldNextMockClose armed, if any, for the next open and the next
//! close to take.
class MockGates {
public:
	//! Where a gate holds a mock connection.
	enum class Point { OPEN, CLOSE };

	static MockGates &Get() {
		static MockGates gates;
		return gates;
	}

	void Arm(Point point, shared_ptr<MockGate> gate) {
		std::lock_guard<std::mutex> guard(lock);
		Slot(point) = std::move(gate);
	}

	//! Takes the gate armed at `point`, if there is one, and waits at it. The gate is taken under the
	//! lock, so exactly one open or close gets it, and waited at outside it, so others run meanwhile.
	void HoldIfArmed(Point point) {
		shared_ptr<MockGate> gate;
		{
			std::lock_guard<std::mutex> guard(lock);
			gate = std::move(Slot(point));
		}
		if (gate) {
			gate->Hold();
		}
	}

private:
	shared_ptr<MockGate> &Slot(Point point) {
		return point == Point::OPEN ? next_open : next_close;
	}

	//! Guards next_open and next_close.
	std::mutex lock;
	//! Point to the gates HoldNextMockOpen and HoldNextMockClose armed, if any. Each is shared with the
	//! test that armed it, and moved to the open or close that takes it.
	shared_ptr<MockGate> next_open;
	shared_ptr<MockGate> next_close;
};

bool MockGate::WaitUntilHeldFor(std::chrono::milliseconds timeout) {
	std::unique_lock<std::mutex> guard(lock);
	// Against the system clock, as FakeGate waits: libstdc++ 11 waits on a steady clock with
	// pthread_cond_clockwait, which GCC 11's ThreadSanitizer does not intercept.
	return changed.wait_until(guard, std::chrono::system_clock::now() + timeout, [this]() { return held; });
}

void MockGate::Release() {
	std::lock_guard<std::mutex> guard(lock);
	released = true;
	changed.notify_all();
}

void MockGate::Hold() {
	std::unique_lock<std::mutex> guard(lock);
	held = true;
	changed.notify_all();
	changed.wait(guard, [this]() { return released; });
}

void HoldNextMockOpen(shared_ptr<MockGate> gate) {
	MockGates::Get().Arm(MockGates::Point::OPEN, std::move(gate));
}

void HoldNextMockClose(shared_ptr<MockGate> gate) {
	MockGates::Get().Arm(MockGates::Point::CLOSE, std::move(gate));
}

namespace {

//! The message, exception type and extra_info FailNextMockOpen armed for the next open to fail with, if any.
class MockOpenFailure {
public:
	static MockOpenFailure &Get() {
		static MockOpenFailure failure;
		return failure;
	}

	void Arm(const string &message, ExceptionType type, const unordered_map<string, string> &extra_info) {
		std::lock_guard<std::mutex> guard(lock);
		armed = true;
		next_message = message;
		next_type = type;
		next_extra_info = extra_info;
	}

	void Disarm() {
		std::lock_guard<std::mutex> guard(lock);
		armed = false;
		next_message.clear();
		next_extra_info.clear();
	}

	//! Takes the armed failure, if there is one, so exactly one open fails.
	bool Take(string &message, ExceptionType &type, unordered_map<string, string> &extra_info) {
		std::lock_guard<std::mutex> guard(lock);
		if (!armed) {
			return false;
		}
		armed = false;
		message = std::move(next_message);
		type = next_type;
		extra_info = std::move(next_extra_info);
		return true;
	}

private:
	//! Guards armed, next_message, next_type and next_extra_info.
	std::mutex lock;
	bool armed = false;
	string next_message;
	ExceptionType next_type = ExceptionType::IO;
	unordered_map<string, string> next_extra_info;
};

} // namespace

void FailNextMockOpen(const string &message, ExceptionType type, const unordered_map<string, string> &extra_info) {
	MockOpenFailure::Get().Arm(message, type, extra_info);
}

void DisarmMockOpenFailure() {
	MockOpenFailure::Get().Disarm();
}

namespace {

//! The mock's control language, in the SQL string. Keeps tests deterministic and
//! readable, and keeps the mock from needing a configuration surface of its own.
//!
//!   rows=N cols=M   M VARCHAR columns named c0..c(M-1), N rows, value "r<row>c<col>"
//!   types=T,...     one column per type, named c0.., instead of cols=. Each is the mock's shorthand
//!                   for a Redshift type, mapped to the RedshiftTypeId the ODBC client gives that type, not the
//!                   driver's type name, which the ODBC client classifies: bool, int2, int4, int8, float4, float8,
//!                   numeric:<precision>:<scale>, varchar, char, date, time, timetz, timestamp, timestamptz and varbyte
//!                   map to their type IDs; geometry, geography and hllsketch, and NUMERIC metadata outside 1 <= p <=
//!                   38 and 0 <= s <= p, are UNKNOWN. Values arrive in the ODBC client's carriers and text forms; see
//!                   MockValue. An empty `types=` is ignored, leaving cols=.
//!   names=N,...     the columns' names, one per column, instead of c0..; a name may repeat, as a
//!                   Redshift join's SELECT * reports it
//!   batch=N,...     the rows of each batch, in turn, instead of 2 each, repeating the list, the last
//!                   batch holding what is left. The ODBC client sizes a result's buffer sets from its row width,
//!                   from 1 row up, and a set
//!                   whose long values fill its arena ends early,
//!                   so sizes can change within a result that has a long column. The mock does not
//!                   tie the sizes to the columns, as the ODBC client does, and above 2048 rows is past the
//!                   batch the ODBC client starts from; neither is confirmed. A size of 0 is a true,
//!                   empty batch, which the transport header allows ("A zero-row batch with a true
//!                   return is legal") though the ODBC client's buffer sets each hold a row at least, so the
//!                   scan's fetch after an empty batch has a test. A list
//!                   needs one size above 0, or the result would never end
//!   append          NextBatch adds its rows to those already in the batch it is given instead of
//!                   replacing them, which the header leaves open ("Fill `batch`"), so the scan's
//!                   clearing of a used-up batch has a test; a drained result still leaves the batch
//!                   empty, as the header says
//!   norows          no result set, with a row count
//!   rowcount=N      with norows, the row count RowCount() reports instead of 0; rowcount=none, a
//!                   count the driver could not report, which RowCount() gives as -1
//!   changed=T,...   the columns the statement has when it executes, typed as in types=, where
//!                   Describe reports types= or cols=: a table or view replaced between bind and
//!                   execution. The transport contract has Execute compare the shape the statement
//!                   has with the one the caller bound (redshift_transport.hpp, Execute), so the mock refuses the
//!                   statement with `shape_changed` before it runs, as the ODBC client does, and as for any bound shape
//!                   that differs from the one the statement has. With norows, the statement now returns rows.
//!   unchecked       with changed=, Execute does not compare the bound shape, standing in for a
//!                   client without the check, so the statement runs and cells of the new types
//!                   reach the scan, whose own checks must refuse
//!                   them
//!   error=<text>    the statement is refused, as Redshift would refuse it, at Describe and at Execute.
//!                   <text> stands for Redshift's message, which the real client shows for
//!                   a list of classes (ShowsServerMessage in
//!                   odbc_errors.cpp); for the rest it sends its
//!                   own message, which names the SQLSTATE class and says to act on the SQLSTATE.
//!                   The mock shows <text> for any class
//!   error_at=execute  with error=, only Execute refuses it: Describe accepted it, and the real
//!                   client's prepare at Execute is refused, because another session dropped the table meanwhile, or,
//!                   between a PREPARE and its EXECUTE, because the session's transaction aborted
//!   fail=execute    the statement fails while it runs, once Execute has sent it, with an
//!                   ExceptionType::EXECUTOR failure on a connection that stays usable
//!   fail=fetch:N    the same, once NextBatch has returned N rows, at most rows=. A later NextBatch
//!                   fails again rather than return rows after a failure; the ODBC client specifies that only
//!                   for its producer thread, and the extension never calls
//!                   NextBatch after a failure. Neither fail= takes a sqlstates= whose `sqlstate` is
//!                   25P02: the ODBC client prepares the statement again at Execute, where an aborted
//!                   transaction refuses it, and nothing can abort the
//!                   transaction after that prepare, so the real client never fails a statement
//!                   that way
//!   sqlstates=S,... with error=, fail= or lose=, the SQLSTATE of each diagnostic record, in order,
//!                   each five digits or capital letters, carried as the client's error format carries
//!                   them (redshift_transport.hpp): `sqlstates` and `sqlstate` in the
//!                   exception's extra_info, and the message ending in "SQLSTATE: " and `sqlstate`.
//!                   `sqlstate` is the first state that is neither HY000 nor a success or warning
//!                   state (class 00 or 01), and HY000 when every state is one of those, as the ODBC client's
//!                   classifier picks it in every phase. Without it, the
//!                   failure carries none. With error= or fail=, no state may be in class 08: the
//!                   real client takes a class 08 record for a lost connection, which lose= gives
//!   unusable        with error= or fail=, the refusal or failure is one the real client reports as a session
//!                   Redshift's answer left unusable (redshift_transport.hpp), such as XX000 "Internal Service
//!                   Error": the same type, with `redshift_error` = `session_unusable` beside any sqlstates=, and
//!                   the connection lost from then on, as the real client closes it. A sqlstates= with it must
//!                   hold XX000, the state of every record the real client reports that way
//!
//! Connection loss, as the real client reports it: the call
//! throws IOException, and every later call on the connection, or on a result of it, throws
//! IOException too, without reaching the network, while SessionHoldsState() keeps its answer from
//! before the loss.
//!
//!   lose=describe   the connection was lost while idle, before the statement reached it: Describe
//!                   loses it on a connection that has served a call before. One opened for this
//!                   call has just connected, so it describes the statement as usual.
//!   lose=describe:any  Describe loses the connection on every connection, one opened for the call
//!                   too
//!   lose=prepare    Execute loses the connection while it prepares the statement, before sending
//!                   it, so SessionHoldsState() does not change
//!   lose=execute    Execute loses the connection after sending the statement, which may have run
//!   lose=fetch:N    NextBatch loses the connection once it has returned N rows, at most rows=
//!
//! Directives are read only outside quoted strings and identifiers, dollar-quoted strings and
//! comments, which Redshift reads as text, so `SELECT 'rows=5'` is an ordinary statement.
//!
//! Like Redshift, which the real client asks to prepare every statement, the mock refuses a string
//! holding more than one statement, at Describe and at Execute, before it
//! reads any directive: a ';' outside quotes and comments followed by anything but whitespace,
//! comments and ';'. Like the real client, it refuses Describe and Execute with an
//! InvalidInputException while a result on the connection is active, and never buffers that result
//! to make room: from Execute until NextBatch first returns
//! false or the result is destroyed. A no-result statement's result is never fetched, so it is
//! active until destroyed.
//!
//! Anything else is treated as a single-column, single-row statement echoing the SQL,
//! so an arbitrary statement still exercises the full path.
//!
//! Besides its open and close, each connection logs, through the sink, each statement it describes,
//! once Describe has passed its refusals, and each statement it executes, once Execute has passed
//! its refusals and the statement runs. Tests count Describe and Execute calls from these lines. Two
//! more lines let tests check the caller's side of the contract, and change no answer: a result
//! asked for another batch after NextBatch returned false, and a
//! connection destroyed while a result of it still exists. A result that
//! does outlive its connection fails from then on, as the real client's
//! does; no redshift_query test relies on that, since the line already shows the mistake.
struct MockDirective {
	//! Where lose= loses the connection.
	enum class Loss { NONE, DESCRIBE, DESCRIBE_ANY, PREPARE, EXECUTE, FETCH };
	//! Where fail= fails the statement.
	enum class Failure { NONE, EXECUTE, FETCH };

	idx_t rows = 1;
	//! From types=, or cols= VARCHAR columns; named c0.. either way, unless names= names them.
	vector<RedshiftColumn> columns;
	//! From changed=: the columns the statement has when it executes. Empty means `columns`.
	vector<RedshiftColumn> changed_columns;
	//! From unchecked: Execute does not compare the bound shape.
	bool unchecked = false;
	//! From batch=: the rows of each batch, in turn.
	vector<idx_t> batch_rows {2};
	//! From append: NextBatch adds its rows to those in the batch it is given.
	bool append = false;
	bool returns_rows = true;
	//! From rowcount=, for a statement without a result set.
	bool has_row_count = false;
	int64_t row_count = 0;
	string error;
	bool error_at_execute_only = false;
	Loss loss = Loss::NONE;
	Failure failure = Failure::NONE;
	//! For lose=fetch: and fail=fetch:, the rows NextBatch returns first.
	idx_t after_rows = 0;
	//! From sqlstates=: each diagnostic record's SQLSTATE, in order.
	vector<string> sqlstates;
	//! From unusable: error= and fail= report a session left unusable.
	bool unusable = false;
	bool echo = true;

	//! Whether lose= or fail= fires at NextBatch, once after_rows rows have come.
	bool StopsAtFetch() const {
		return loss == Loss::FETCH || failure == Failure::FETCH;
	}
};

//! What a lose= or fail= value that fires at NextBatch starts with; the rows to return first follow.
constexpr const char *FETCH_PREFIX = "fetch:";

//! Whether `c` can continue an unquoted identifier.
bool IsIdentifierCharacter(char c) {
	return StringUtil::CharacterIsAlphaNumeric(c) || c == '_' || c == '$';
}

//! Where the quoted string or quoted identifier that opens at `start` ends, past its closing quote.
//! In a string, as Redshift reads string literals, a backslash escapes the next character. A doubled
//! quote closes and reopens the quote. An unterminated one runs past the end of `sql`.
idx_t SkipQuoted(const string &sql, idx_t start) {
	const char quote = sql[start];
	idx_t i = start + 1;
	while (i < sql.size() && sql[i] != quote) {
		i += quote == '\'' && sql[i] == '\\' ? 2 : 1;
	}
	return i + 1;
}

//! Where the line comment that opens at `start` ends, past its newline or at the end of `sql`.
idx_t SkipLineComment(const string &sql, idx_t start) {
	auto newline = sql.find('\n', start);
	return newline == string::npos ? sql.size() : newline + 1;
}

//! Where the block comment that opens at `start` ends, past the close that matches it: block
//! comments nest, as in PostgreSQL. An unterminated one runs to the end of `sql`.
idx_t SkipBlockComment(const string &sql, idx_t start) {
	idx_t depth = 1;
	idx_t i = start + 2;
	while (i < sql.size() && depth > 0) {
		if (sql.compare(i, 2, "/*") == 0) {
			depth++;
			i += 2;
		} else if (sql.compare(i, 2, "*/") == 0) {
			depth--;
			i += 2;
		} else {
			i++;
		}
	}
	return i;
}

//! The tag of the dollar quote that opens at `start`, $tag$ with an empty or identifier-like tag, or
//! empty when none does. A '$' inside an identifier, as in a$b$c, which Redshift allows, starts none.
string DollarQuoteTag(const string &sql, idx_t start) {
	if (start > 0 && IsIdentifierCharacter(sql[start - 1])) {
		return string();
	}
	auto tag_end = sql.find('$', start + 1);
	if (tag_end == string::npos) {
		return string();
	}
	for (idx_t t = start + 1; t < tag_end; t++) {
		if (!StringUtil::CharacterIsAlphaNumeric(sql[t]) && sql[t] != '_') {
			return string();
		}
	}
	return sql.substr(start, tag_end - start + 1);
}

//! `sql` as Redshift reads it as SQL: each comment replaced by a space, and the text inside each
//! quoted string, quoted identifier and dollar-quoted string removed, leaving its quotes. An
//! unterminated quote or comment runs to the end of the string.
string OutsideQuotes(const string &sql) {
	string code;
	idx_t i = 0;
	while (i < sql.size()) {
		const char c = sql[i];
		const auto tag = c == '$' ? DollarQuoteTag(sql, i) : string();
		if (c == '\'' || c == '"') {
			code += string(2, c);
			i = SkipQuoted(sql, i);
		} else if (sql.compare(i, 2, "--") == 0) {
			code += ' ';
			i = SkipLineComment(sql, i);
		} else if (sql.compare(i, 2, "/*") == 0) {
			code += ' ';
			i = SkipBlockComment(sql, i);
		} else if (!tag.empty()) {
			code += tag + tag;
			auto close = sql.find(tag, i + tag.size());
			i = close == string::npos ? sql.size() : close + tag.size();
		} else {
			code += c;
			i++;
		}
	}
	return code;
}

//! Whether `sql` holds more than one statement: a ';' outside quotes and comments followed by
//! anything but whitespace, comments and ';'.
bool HoldsSeveralStatements(const string &sql) {
	auto code = OutsideQuotes(sql);
	auto end = code.find(';');
	return end != string::npos && code.find_first_not_of("; \t\n\v\f\r", end) != string::npos;
}

//! The state of `sqlstates`, which is not empty, that the client's classifier acts on: the first that is
//! neither HY000 nor a success or warning state (class 00 or 01), which the ODBC client counts
//! as HY000, and HY000 when every state is one of those.
string ClassifiedState(const vector<string> &sqlstates) {
	for (auto &state : sqlstates) {
		if (state != "HY000" && !StringUtil::StartsWith(state, "00") && !StringUtil::StartsWith(state, "01")) {
			return state;
		}
	}
	return "HY000";
}

//! A non-negative integer from a directive. Hand-parsed rather than std::stoll: on bad
//! input stoll throws std::invalid_argument, whose what() is the bare word "stoll", and
//! on a negative value it succeeds and the result later underflows idx_t. Neither is a
//! usable error for someone who mistyped a test fixture.
idx_t DirectiveInt(const string &key, const string &value) {
	constexpr idx_t MAX = 1000000;
	if (value.empty()) {
		throw InvalidInputException("redshift mock: %s needs a value", key);
	}
	idx_t parsed = 0;
	for (auto c : value) {
		if (c < '0' || c > '9') {
			throw InvalidInputException("redshift mock: %s must be a non-negative integer, got \"%s\"", key, value);
		}
		parsed = parsed * 10 + static_cast<idx_t>(c - '0');
		if (parsed > MAX) {
			throw InvalidInputException("redshift mock: %s must be at most %llu", key, (uint64_t)MAX);
		}
	}
	return parsed;
}

//! One column of a types= list: the mock's shorthand for a Redshift type, mapped to the RedshiftTypeId
//! the ODBC client gives that type.
RedshiftColumn ParseType(const string &name) {
	static const std::pair<const char *, RedshiftTypeId> TYPES[] = {{"bool", RedshiftTypeId::BOOLEAN},
	                                                                {"int2", RedshiftTypeId::SMALLINT},
	                                                                {"int4", RedshiftTypeId::INTEGER},
	                                                                {"int8", RedshiftTypeId::BIGINT},
	                                                                {"float4", RedshiftTypeId::FLOAT},
	                                                                {"float8", RedshiftTypeId::DOUBLE},
	                                                                {"varchar", RedshiftTypeId::VARCHAR},
	                                                                {"char", RedshiftTypeId::CHAR},
	                                                                {"date", RedshiftTypeId::DATE},
	                                                                {"time", RedshiftTypeId::TIME},
	                                                                {"timetz", RedshiftTypeId::TIMETZ},
	                                                                {"timestamp", RedshiftTypeId::TIMESTAMP},
	                                                                {"timestamptz", RedshiftTypeId::TIMESTAMPTZ},
	                                                                {"varbyte", RedshiftTypeId::BLOB},
	                                                                {"geometry", RedshiftTypeId::UNKNOWN},
	                                                                {"geography", RedshiftTypeId::UNKNOWN},
	                                                                {"hllsketch", RedshiftTypeId::UNKNOWN}};
	RedshiftColumn column;
	auto parts = StringUtil::Split(name, ':');
	if (parts.size() == 3 && parts[0] == "numeric") {
		// A negative precision or scale is outside the ODBC client's range as well.
		auto magnitude = [](const string &part) {
			return StringUtil::StartsWith(part, "-") ? part.substr(1) : part;
		};
		bool negative = magnitude(parts[1]) != parts[1] || magnitude(parts[2]) != parts[2];
		auto precision = DirectiveInt("numeric precision", magnitude(parts[1]));
		auto scale = DirectiveInt("numeric scale", magnitude(parts[2]));
		// Metadata DuckDB cannot represent never reaches the extension as DECIMAL: the ODBC client reports it UNKNOWN.
		if (negative || precision < 1 || precision > 38 || scale > precision) {
			return column;
		}
		column.type = RedshiftTypeId::DECIMAL;
		column.precision = static_cast<int32_t>(precision);
		column.scale = static_cast<int32_t>(scale);
		return column;
	}
	for (auto &type : TYPES) {
		if (name == type.first) {
			column.type = type.second;
			return column;
		}
	}
	throw InvalidInputException("redshift mock: unknown type \"%s\" in types=", name);
}

//! batch=: the rows of each batch, in turn. A size of 0 is a true, empty batch, which the transport
//! header allows, so the list needs one size above 0 for the result to end.
void ParseBatches(MockDirective &directive, const string &value) {
	directive.batch_rows.clear();
	bool progresses = false;
	for (auto &size : StringUtil::Split(value, ',')) {
		directive.batch_rows.push_back(DirectiveInt("batch", size));
		progresses = progresses || directive.batch_rows.back() > 0;
	}
	if (directive.batch_rows.empty()) {
		throw InvalidInputException("redshift mock: batch needs a value");
	}
	if (!progresses) {
		throw InvalidInputException("redshift mock: batch= needs a size above 0, or the result never ends");
	}
}

//! The rows after fetch:, for lose= or fail= `key`.
idx_t FetchRows(const string &key, const string &value) {
	return DirectiveInt(key + "=" + FETCH_PREFIX, value.substr(strlen(FETCH_PREFIX)));
}

//! fail=: where the statement fails.
void ParseFailure(MockDirective &directive, const string &value) {
	if (value == "execute") {
		directive.failure = MockDirective::Failure::EXECUTE;
	} else if (StringUtil::StartsWith(value, FETCH_PREFIX)) {
		directive.failure = MockDirective::Failure::FETCH;
		directive.after_rows = FetchRows("fail", value);
	} else {
		throw InvalidInputException("redshift mock: fail must be execute or fetch:<rows>, got \"%s\"", value);
	}
}

//! lose=: where the connection is lost.
void ParseLoss(MockDirective &directive, const string &value) {
	if (value == "describe") {
		directive.loss = MockDirective::Loss::DESCRIBE;
	} else if (value == "describe:any") {
		directive.loss = MockDirective::Loss::DESCRIBE_ANY;
	} else if (value == "prepare") {
		directive.loss = MockDirective::Loss::PREPARE;
	} else if (value == "execute") {
		directive.loss = MockDirective::Loss::EXECUTE;
	} else if (StringUtil::StartsWith(value, FETCH_PREFIX)) {
		directive.loss = MockDirective::Loss::FETCH;
		directive.after_rows = FetchRows("lose", value);
	} else {
		throw InvalidInputException(
		    "redshift mock: lose must be describe, describe:any, prepare, execute or fetch:<rows>, got \"%s\"", value);
	}
}

//! sqlstates=: each diagnostic record's SQLSTATE, which is five digits or capital letters, as ODBC
//! defines it.
void ParseSqlstates(MockDirective &directive, const string &value) {
	directive.sqlstates = StringUtil::Split(value, ',');
	if (directive.sqlstates.empty()) {
		throw InvalidInputException("redshift mock: sqlstates needs a value");
	}
	for (auto &state : directive.sqlstates) {
		bool valid = state.size() == 5;
		for (auto c : state) {
			valid = valid && (StringUtil::CharacterIsDigit(c) || (c >= 'A' && c <= 'Z'));
		}
		if (!valid) {
			throw InvalidInputException(
			    "redshift mock: sqlstates= needs SQLSTATEs of five digits or capital letters, got \"%s\"", state);
		}
	}
}

//! Names the columns, and refuses a names= that cannot apply to them.
void NameColumns(MockDirective &directive, const vector<string> &names) {
	if (!names.empty() && !directive.returns_rows) {
		throw InvalidInputException("redshift mock: names= names result columns, and norows has none");
	}
	if (!names.empty() && names.size() != directive.columns.size()) {
		throw InvalidInputException("redshift mock: names= gives %llu names for %llu columns", (uint64_t)names.size(),
		                            (uint64_t)directive.columns.size());
	}
	for (auto &name : names) {
		if (name.empty()) {
			// Redshift reports no empty column name.
			throw InvalidInputException("redshift mock: names= has an empty name");
		}
	}
	for (idx_t c = 0; c < directive.columns.size(); c++) {
		directive.columns[c].name = names.empty() ? "c" + std::to_string(c) : names[c];
	}
	for (idx_t c = 0; c < directive.changed_columns.size(); c++) {
		directive.changed_columns[c].name = "c" + std::to_string(c);
	}
}

//! Gives the columns their types and names once every token is read: `cols` VARCHAR columns when
//! types= gave none, then the names names= gave.
void FinishColumns(MockDirective &directive, idx_t cols, const vector<string> &names) {
	if (directive.columns.empty()) {
		directive.columns.resize(cols);
		for (auto &column : directive.columns) {
			column.type = RedshiftTypeId::VARCHAR;
		}
	}
	NameColumns(directive, names);
}

//! Refuses a combination of directives the real client cannot give, or that never fires through
//! redshift_query, so a test written with one would test nothing.
void RefuseImpossibleCombinations(const MockDirective &directive) {
	if (directive.has_row_count && directive.returns_rows) {
		// The real client reports a negative count for a statement that returns rows.
		throw InvalidInputException("redshift mock: rowcount= needs norows");
	}
	if (directive.error_at_execute_only && directive.error.empty()) {
		throw InvalidInputException("redshift mock: error_at= needs error=");
	}
	if (directive.unusable && directive.error.empty() && directive.failure == MockDirective::Failure::NONE) {
		throw InvalidInputException("redshift mock: unusable needs error= or fail=");
	}
	if (directive.unusable && !directive.sqlstates.empty() &&
	    std::find(directive.sqlstates.begin(), directive.sqlstates.end(), "XX000") == directive.sqlstates.end()) {
		// The real client reports a session left unusable only for a record whose SQLSTATE is XX000.
		throw InvalidInputException("redshift mock: unusable needs XX000 among sqlstates=");
	}
	if (directive.loss != MockDirective::Loss::NONE && directive.failure != MockDirective::Failure::NONE) {
		throw InvalidInputException("redshift mock: lose= and fail= cannot be combined");
	}
	if (directive.StopsAtFetch() && directive.after_rows > directive.rows) {
		// It would never fire: the result ends first.
		throw InvalidInputException("redshift mock: fetch:%llu is past the %llu rows the result has",
		                            (uint64_t)directive.after_rows, (uint64_t)directive.rows);
	}
	if (!directive.sqlstates.empty() && directive.error.empty() && directive.loss == MockDirective::Loss::NONE &&
	    directive.failure == MockDirective::Failure::NONE) {
		throw InvalidInputException("redshift mock: sqlstates= needs error=, fail= or lose=");
	}
	if ((!directive.error.empty() || directive.failure != MockDirective::Failure::NONE) &&
	    std::any_of(directive.sqlstates.begin(), directive.sqlstates.end(),
	                [](const string &state) { return StringUtil::StartsWith(state, "08"); })) {
		throw InvalidInputException("redshift mock: a class 08 record is a lost connection; use lose=");
	}
	if (directive.failure != MockDirective::Failure::NONE && !directive.sqlstates.empty() &&
	    ClassifiedState(directive.sqlstates) == "25P02") {
		throw InvalidInputException("redshift mock: fail= cannot carry 25P02, which Redshift gives when it refuses to "
		                            "prepare a statement; use error=");
	}
	if (directive.StopsAtFetch() && !directive.returns_rows) {
		// Bound as a statement without a result set, which redshift_query never fetches, whatever
		// changed= gives it when it runs.
		throw InvalidInputException("redshift mock: fetch: needs a statement with a result set");
	}
	if (directive.unchecked && directive.changed_columns.empty()) {
		// Without changed=, the bound shape is the statement's, so skipping the comparison changes nothing.
		throw InvalidInputException("redshift mock: unchecked needs changed=");
	}
	if (directive.has_row_count && !directive.changed_columns.empty()) {
		throw InvalidInputException("redshift mock: rowcount= and changed= cannot be combined");
	}
	// No result without a result set is fetched, a refusal at Describe comes before anything at
	// Execute, and one at Execute's prepare before the statement is sent or fetched.
	const bool after_prepare = directive.StopsAtFetch() || directive.loss == MockDirective::Loss::EXECUTE ||
	                           directive.failure == MockDirective::Failure::EXECUTE;
	if (!directive.error.empty() && (after_prepare || directive.loss == MockDirective::Loss::PREPARE ||
	                                 directive.loss == MockDirective::Loss::DESCRIBE_ANY)) {
		// Execute's prepare loses the connection before its refusal comes, and lose=describe:any loses
		// every Describe before its refusal.
		throw InvalidInputException("redshift mock: error= and that lose= or fail= cannot both fire");
	}
}

MockDirective ParseDirective(const string &sql) {
	MockDirective directive;
	idx_t cols = 1;
	vector<string> names;
	// A ';' left here ends the statement, as Redshift reads it; any other was refused already.
	for (auto &token : StringUtil::Split(StringUtil::Replace(OutsideQuotes(sql), ";", " "), ' ')) {
		auto part = token;
		StringUtil::Trim(part);
		if (part.empty()) {
			continue;
		}
		auto eq = part.find('=');
		auto key = StringUtil::Lower(eq == string::npos ? part : part.substr(0, eq));
		auto value = eq == string::npos ? string() : part.substr(eq + 1);
		if (key == "rows") {
			directive.rows = DirectiveInt("rows", value);
		} else if (key == "cols") {
			cols = DirectiveInt("cols", value);
			if (cols == 0) {
				// The real client describes a statement with no columns as having no result set,
				// so a result set without columns is not an answer it gives.
				throw InvalidInputException("redshift mock: cols must be at least 1; use norows for a statement "
				                            "with no result set");
			}
		} else if (key == "types") {
			for (auto &type : StringUtil::Split(value, ',')) {
				directive.columns.push_back(ParseType(StringUtil::Lower(type)));
			}
		} else if (key == "names") {
			names = StringUtil::Split(value, ',');
		} else if (key == "batch") {
			ParseBatches(directive, value);
		} else if (key == "append") {
			directive.append = true;
		} else if (key == "unchecked") {
			directive.unchecked = true;
		} else if (key == "unusable") {
			directive.unusable = true;
		} else if (key == "norows") {
			directive.returns_rows = false;
		} else if (key == "rowcount") {
			directive.has_row_count = true;
			directive.row_count = value == "none" ? -1 : static_cast<int64_t>(DirectiveInt("rowcount", value));
		} else if (key == "changed") {
			for (auto &type : StringUtil::Split(value, ',')) {
				directive.changed_columns.push_back(ParseType(StringUtil::Lower(type)));
			}
		} else if (key == "error") {
			directive.error = value;
		} else if (key == "error_at") {
			if (value != "execute") {
				throw InvalidInputException("redshift mock: error_at must be execute, got \"%s\"", value);
			}
			directive.error_at_execute_only = true;
		} else if (key == "fail") {
			ParseFailure(directive, value);
		} else if (key == "lose") {
			ParseLoss(directive, value);
		} else if (key == "sqlstates") {
			ParseSqlstates(directive, value);
		} else {
			// Unrecognised tokens are ordinary SQL words, left alone.
			continue;
		}
		directive.echo = false;
	}
	FinishColumns(directive, cols, names);
	RefuseImpossibleCombinations(directive);
	return directive;
}

//! Row `row` of column `col`, of `column`'s type, carried and written as the ODBC client carries and
//! writes it: "r<row>c<col>" for VARCHAR, the same followed by three
//! blanks for CHAR, as a CHAR(n) wider than the value pads it, and for the other types a value
//! derived from the row.
RedshiftValue MockValue(const RedshiftColumn &column, idx_t row, idx_t col) {
	RedshiftValue value;
	auto two_digits = [](idx_t n) {
		return (n < 10 ? "0" : "") + std::to_string(n);
	};
	switch (column.type) {
	case RedshiftTypeId::BOOLEAN:
		value.kind = RedshiftValueKind::BOOLEAN;
		value.boolean = row % 2 == 0;
		break;
	case RedshiftTypeId::SMALLINT:
		// Within SMALLINT's range at any row count, as an int2 always is.
		value.kind = RedshiftValueKind::INTEGER;
		value.integer = static_cast<int64_t>(row % 32768);
		break;
	case RedshiftTypeId::INTEGER:
	case RedshiftTypeId::BIGINT:
		value.kind = RedshiftValueKind::INTEGER;
		value.integer = static_cast<int64_t>(row);
		break;
	case RedshiftTypeId::FLOAT:
	case RedshiftTypeId::DOUBLE:
		value.kind = RedshiftValueKind::REAL;
		value.real = static_cast<double>(row) + 0.5;
		break;
	case RedshiftTypeId::DECIMAL: {
		// The row, cut to the integer digits the precision leaves, then exactly `scale` digits.
		idx_t limit = 1;
		for (int32_t d = 0; d < MinValue<int32_t>(column.precision - column.scale, 9); d++) {
			limit *= 10;
		}
		value.kind = RedshiftValueKind::TEXT;
		value.text = std::to_string(row % limit);
		if (column.scale > 0) {
			value.text += "." + string(static_cast<idx_t>(column.scale), '0');
		}
		break;
	}
	case RedshiftTypeId::DATE:
		value.kind = RedshiftValueKind::TEXT;
		value.text = "2024-01-" + two_digits(1 + row % 28);
		break;
	case RedshiftTypeId::TIME:
		value.kind = RedshiftValueKind::TEXT;
		value.text = "12:00:" + two_digits(row % 60);
		break;
	case RedshiftTypeId::TIMETZ:
		value.kind = RedshiftValueKind::TEXT;
		value.text = "12:00:" + two_digits(row % 60) + "+05";
		break;
	case RedshiftTypeId::TIMESTAMP:
		value.kind = RedshiftValueKind::TEXT;
		value.text = "2024-01-01 12:00:" + two_digits(row % 60);
		break;
	case RedshiftTypeId::TIMESTAMPTZ:
		value.kind = RedshiftValueKind::TEXT;
		value.text = "2024-01-01 12:00:" + two_digits(row % 60) + "+00";
		break;
	case RedshiftTypeId::BLOB:
		// Raw bytes, a NUL among them, never the driver's hex text.
		value.kind = RedshiftValueKind::BLOB;
		value.text = "r" + std::to_string(row) + string(1, '\0') + "c" + std::to_string(col);
		break;
	case RedshiftTypeId::CHAR:
		value.kind = RedshiftValueKind::TEXT;
		value.text = "r" + std::to_string(row) + "c" + std::to_string(col) + "   ";
		break;
	case RedshiftTypeId::VARCHAR:
	case RedshiftTypeId::UNKNOWN:
		// redshift_query refuses an UNKNOWN column at bind, and the real client refuses one at Execute.
		// A test reads one only from the mock: by calling its Execute directly, or through
		// redshift_query with changed= and unchecked.
		value.kind = RedshiftValueKind::TEXT;
		value.text = "r" + std::to_string(row) + "c" + std::to_string(col);
		break;
	}
	return value;
}

//! What a mock connection shares with its results: whether one of them is active, how many exist,
//! and whether the connection has been lost or destroyed. Shared, so a result that outlives its
//! connection never reaches into it.
struct MockConnectionState {
	bool result_active = false;
	idx_t live_results = 0;
	bool lost = false;
	bool closed = false;
};

//! Throws the refusal, failure or loss a directive injects, with `text`, as the client throws
//! one. Given `sqlstates`, it carries them as the client carries a driver failure's (the error format in
//! redshift_transport.hpp): each record's state in order, and the state the client's classifier acts on
//! (ClassifiedState), in extra_info, with the message ending in that state. Without them it carries
//! neither. A refusal reads as Redshift's would; failures and losses say "redshift mock" rather than
//! imitate the real client's messages.
[[noreturn]] void ThrowInjected(ExceptionType type, const string &text, const vector<string> &sqlstates,
                                bool session_unusable = false) {
	unordered_map<string, string> extra_info;
	if (session_unusable) {
		extra_info["redshift_error"] = "session_unusable";
	}
	auto message = text;
	if (!sqlstates.empty()) {
		const auto sqlstate = ClassifiedState(sqlstates);
		extra_info["sqlstate"] = sqlstate;
		extra_info["sqlstates"] = StringUtil::Join(sqlstates, ",");
		message += "\nSQLSTATE: " + sqlstate;
	}
	// The classes the transport header names, as the real client throws them. A statement failure has
	// no class of its own that takes extra_info, so it is the base class with the type the transport
	// header names for it.
	switch (type) {
	case ExceptionType::BINDER:
		throw BinderException(extra_info, message);
	case ExceptionType::IO:
		throw IOException(extra_info, message);
	default:
		throw Exception(extra_info, type, message);
	}
}

//! Throws the refusal or failure that error= or fail= injects, as ThrowInjected does with `type`; with unusable,
//! as the real client reports a session Redshift's answer left unusable: the same type with `redshift_error` =
//! `session_unusable`, after which `state`'s connection is lost, as the client closes it.
[[noreturn]] void ThrowStatementInjected(ExceptionType type, const string &text, const MockDirective &directive,
                                         MockConnectionState &state) {
	if (directive.unusable) {
		state.lost = true;
	}
	ThrowInjected(type, text, directive.sqlstates, directive.unusable);
}

//! The shape Describe reports for the statement.
RedshiftStatementShape DescribedShape(const MockDirective &directive) {
	RedshiftStatementShape shape;
	shape.returns_rows = directive.returns_rows;
	if (shape.returns_rows) {
		shape.columns = directive.columns;
	}
	return shape;
}

//! The shape the statement has when it executes, which changed= says differs from the described one.
RedshiftStatementShape ExecutedShape(const MockDirective &directive) {
	if (directive.changed_columns.empty()) {
		return DescribedShape(directive);
	}
	RedshiftStatementShape shape;
	shape.returns_rows = true;
	shape.columns = directive.changed_columns;
	return shape;
}

class MockResult : public RedshiftResult {
public:
	MockResult(const MockDirective &directive_p, vector<RedshiftColumn> columns_p, string echo_p, int64_t row_count_p,
	           shared_ptr<MockConnectionState> state_p, shared_ptr<RedshiftLogSink> log_sink_p, string target_p)
	    : directive(directive_p), columns(std::move(columns_p)), echo(std::move(echo_p)), row_count(row_count_p),
	      state(std::move(state_p)), log_sink(std::move(log_sink_p)), target(std::move(target_p)) {
		// A result without columns is a no-result statement's, which has no rows to fetch.
		remaining = columns.empty() ? 0 : directive.rows;
		state->result_active = true;
		state->live_results++;
	}

	~MockResult() override {
		EndActive();
		state->live_results--;
	}

	bool NextBatch(RedshiftBatch &batch) override {
		if (!directive.append) {
			batch.rows.clear();
		}
		if (state->closed) {
			// The caller destroyed the connection before this result, which it must not do; the real
			// client then fails the result rather than reach into the connection.
			throw IOException("redshift mock: NextBatch on a result whose connection to %s was closed", target);
		}
		if (state->lost) {
			throw IOException("redshift mock: the connection to %s was lost", target);
		}
		if (ended) {
			// The answer stays false; the line lets a test see the caller ask again.
			log_sink->Debug("redshift mock: NextBatch was called after the result ended, on a connection to " + target);
			batch.rows.clear();
			return false;
		}
		if (failed) {
			ThrowStatementInjected(ExceptionType::EXECUTOR, "redshift mock: the statement failed while it ran",
			                       directive, *state);
		}
		if (emitted == directive.after_rows && directive.loss == MockDirective::Loss::FETCH) {
			state->lost = true;
			ThrowInjected(ExceptionType::IO,
			              "redshift mock: lost the connection to " + target + " while reading the result",
			              directive.sqlstates);
		}
		if (emitted == directive.after_rows && directive.failure == MockDirective::Failure::FETCH) {
			failed = true;
			ThrowStatementInjected(ExceptionType::EXECUTOR, "redshift mock: the statement failed while it ran",
			                       directive, *state);
		}
		if (remaining == 0) {
			// The transport header has a drained result leave the batch empty, appending or not.
			batch.rows.clear();
			EndActive();
			ended = true;
			return false;
		}
		// A deliberately small batch by default, so a test with a handful of rows still crosses the
		// batch boundary and exercises the streaming path rather than a single gulp. A batch stops at
		// a lose=fetch: or fail=fetch: threshold, so exactly that many rows come first, as a buffer set
		// that ends early would hold them.
		idx_t count = MinValue<idx_t>(directive.batch_rows[batches++ % directive.batch_rows.size()], remaining);
		if (directive.StopsAtFetch() && emitted < directive.after_rows) {
			count = MinValue<idx_t>(count, directive.after_rows - emitted);
		}
		for (idx_t i = 0; i < count; i++) {
			vector<RedshiftValue> row;
			for (idx_t c = 0; c < columns.size(); c++) {
				if (echo.empty()) {
					row.push_back(MockValue(columns[c], emitted, c));
					continue;
				}
				RedshiftValue value;
				value.kind = RedshiftValueKind::TEXT;
				value.text = echo;
				row.push_back(std::move(value));
			}
			batch.rows.push_back(std::move(row));
			emitted++;
		}
		remaining -= count;
		return true;
	}

	int64_t RowCount() const override {
		return row_count;
	}

private:
	//! This result stops being the connection's active one, once: a drained result destroyed after a
	//! later result started must not end that one's turn.
	void EndActive() {
		if (active) {
			state->result_active = false;
			active = false;
		}
	}

	//! The directive the statement ran with, which decides how its result is fetched and when it fails.
	const MockDirective directive;
	vector<RedshiftColumn> columns;
	string echo;
	int64_t row_count;
	//! Points to the state the connection that executed this result created and shares with it.
	//! Ownership is shared with that connection and its other results, so the state lives until the
	//! last of them is destroyed, and this result finds the connection closed there if it outlives it.
	shared_ptr<MockConnectionState> state;
	//! Points to the sink the connection that executed this result was opened with, which the caller
	//! of OpenRedshiftConnection created; ownership is shared with that connection, its other results
	//! and the caller, as the real client shares its sink, so it outlives this result.
	shared_ptr<RedshiftLogSink> log_sink;
	//! The connection's configuration, for messages.
	string target;
	idx_t remaining = 0;
	idx_t emitted = 0;
	idx_t batches = 0;
	bool active = true;
	//! Set once NextBatch has returned false.
	bool ended = false;
	//! Set once fail=fetch: has failed the statement, so every later NextBatch fails too.
	bool failed = false;
};

class MockConnection : public RedshiftConnection {
public:
	MockConnection(RedshiftConfig config_p, bool with_aws_credentials, shared_ptr<RedshiftLogSink> log_sink_p)
	    : config(std::move(config_p)), log_sink(std::move(log_sink_p)) {
		// Records only that AWS credentials were given, never them, so tests can check they arrived
		// without the mock holding them.
		log_sink->Debug("redshift mock: opened a connection to " + config.Describe() +
		                (with_aws_credentials ? " (with AWS credentials)" : ""));
	}

	~MockConnection() override {
		state->closed = true;
		if (state->live_results > 0) {
			// The caller must destroy every result before its connection.
			log_sink->Debug("redshift mock: a result outlived its connection to " + config.Describe());
		}
		MockGates::Get().HoldIfArmed(MockGates::Point::CLOSE);
		log_sink->Debug("redshift mock: closed a connection to " + config.Describe());
	}

	RedshiftStatementShape Describe(const string &sql) override {
		RefuseIfLost();
		RefuseWhileActive("Describe");
		const bool served_before = served;
		served = true;
		RefuseSeveralStatements(sql);
		auto directive = ParseDirective(sql);
		if (directive.loss == MockDirective::Loss::DESCRIBE_ANY ||
		    (directive.loss == MockDirective::Loss::DESCRIBE && served_before)) {
			Lose("during Describe", directive);
		}
		if (!directive.error.empty() && !directive.error_at_execute_only) {
			// A refused statement is the user's SQL, so this mirrors how a real client
			// reports a server rejection: at prepare time, before anything executes.
			ThrowStatementInjected(ExceptionType::BINDER, "Redshift rejected the statement: " + directive.error,
			                       directive, *state);
		}
		log_sink->Debug("redshift mock: described a statement on a connection to " + config.Describe());
		return DescribedShape(directive);
	}

	unique_ptr<RedshiftResult> Execute(const string &sql, const vector<string> &params,
	                                   const RedshiftStatementShape &expected_shape) override {
		RefuseIfLost();
		RefuseWhileActive("Execute");
		served = true;
		RefuseSeveralStatements(sql);
		auto directive = ParseDirective(sql);
		if (directive.loss == MockDirective::Loss::PREPARE) {
			Lose("while preparing the statement", directive);
		}
		if (!directive.error.empty()) {
			ThrowStatementInjected(ExceptionType::BINDER, "Redshift rejected the statement: " + directive.error,
			                       directive, *state);
		}
		// The transport contract has Execute compare the statement's shape with the bound one before it
		// runs (redshift_transport.hpp), with the comparison the ODBC
		// client makes too (statement_shape.hpp). A local comparison, not a driver error, so it carries no
		// SQLSTATE.
		const auto executed_shape = ExecutedShape(directive);
		const auto difference = directive.unchecked ? string() : ShapeDifference(expected_shape, executed_shape);
		if (!difference.empty()) {
			throw BinderException(unordered_map<string, string> {{"redshift_error", "shape_changed"}},
			                      "redshift mock: the statement's result shape is not the one it was bound with, so it "
			                      "was not run: " +
			                          difference);
		}
		// From here the statement runs, so from here the session may hold state, whatever the
		// statement returns and whether it later fails: the real client's latch, which it sets just before SQLExecute.
		// A refusal made before SQLExecute ran nothing and leaves the answer unchanged, so it belongs above this line;
		// a failure of the run itself, such as fail=execute, belongs below it.
		holds_state = true;
		log_sink->Debug("redshift mock: executed a statement on a connection to " + config.Describe());
		if (directive.loss == MockDirective::Loss::EXECUTE) {
			Lose("after sending the statement", directive);
		}
		if (directive.failure == MockDirective::Failure::EXECUTE) {
			ThrowStatementInjected(ExceptionType::EXECUTOR, "redshift mock: the statement failed while it ran",
			                       directive, *state);
		}
		// Bound parameters are echoed back so a test can prove they arrived positionally
		// without a real server. A client must never substitute them into the SQL text.
		string echo;
		if (directive.echo) {
			echo = sql;
			for (idx_t i = 0; i < params.size(); i++) {
				echo += "|$" + std::to_string(i + 1) + "=" + params[i];
			}
		}
		// The result has the shape the statement has when it runs (executed_shape, above).
		if (!executed_shape.returns_rows) {
			return make_uniq<MockResult>(directive, vector<RedshiftColumn>(), string(), directive.row_count, state,
			                             log_sink, config.Describe());
		}
		return make_uniq<MockResult>(directive, executed_shape.columns, echo, -1, state, log_sink, config.Describe());
	}

	bool SessionHoldsState() const override {
		// False until the first statement runs, then true for good, as the real client answers:
		// the driver cannot say whether a statement left state, so the client assumes it did.
		// Describe never changes it, and neither does losing the connection.
		return holds_state;
	}

	string Target() const override {
		return "mock (" + config.Describe() + ")";
	}

private:
	//! Redshift refuses to prepare a string holding more than one statement, and the real client
	//! prepares at Describe and again at Execute, so both refuse it before anything runs.
	static void RefuseSeveralStatements(const string &sql) {
		if (HoldsSeveralStatements(sql)) {
			throw BinderException(
			    "Redshift rejected the statement: cannot insert multiple commands into a prepared statement");
		}
	}

	//! Once lost, the real client fails every call without reaching the network.
	//! No driver call fails, so the exception carries no SQLSTATE.
	void RefuseIfLost() const {
		if (state->lost) {
			throw IOException("redshift mock: the connection to %s was lost", config.Describe());
		}
	}

	//! The real client's in-flight refusal: one active result per connection, never buffered to make
	//! room. Nothing reaches the server, so the latch is unchanged.
	void RefuseWhileActive(const char *call) const {
		if (state->result_active) {
			throw InvalidInputException("redshift mock: %s while a result on the connection to %s is still open", call,
			                            config.Describe());
		}
	}

	//! Loses the connection `when` says, as lose= asks.
	[[noreturn]] void Lose(const string &when, const MockDirective &directive) {
		state->lost = true;
		ThrowInjected(ExceptionType::IO, "redshift mock: lost the connection to " + config.Describe() + " " + when,
		              directive.sqlstates);
	}

	RedshiftConfig config;
	//! Points to the sink OpenRedshiftConnection was given, which the caller created; ownership is
	//! shared with the caller, whatever else the caller passed it to, and this connection's results,
	//! so it outlives this connection, which logs its close through it.
	shared_ptr<RedshiftLogSink> log_sink;
	//! Points to the state this connection shares with its results, created with the connection.
	//! Ownership is shared with every result it executed, so a result that outlives the connection
	//! finds it closed there.
	shared_ptr<MockConnectionState> state = make_shared_ptr<MockConnectionState>();
	//! Whether a call has reached the connection: lose=describe loses only a connection that has
	//! served a call before, since one opened for the call has not sat idle.
	bool served = false;
	bool holds_state = false;
};

} // namespace

RedshiftStatementShape MockDescribedShape(const string &sql) {
	return DescribedShape(ParseDirective(sql));
}

//===--------------------------------------------------------------------===//
// Opening a connection
//===--------------------------------------------------------------------===//

unique_ptr<RedshiftConnection> OpenRedshiftConnection(const RedshiftConfig &config,
                                                      optional_ptr<const RedshiftAwsCredentials> credentials,
                                                      shared_ptr<RedshiftLogSink> log_sink) {
	D_ASSERT(log_sink);
	if (config.aws_secret.empty() != !credentials) {
		// Refused like the real client refuses it: AWS credentials come
		// exactly with the AWS secret they were read from.
		throw InvalidInputException("Redshift connection: AWS credentials are given exactly when an AWS secret is "
		                            "named");
	}
	if (credentials && !config.profile.empty()) {
		throw InvalidInputException("Redshift connection: a profile cannot be given with an AWS secret");
	}
	// An endpoint port goes with an endpoint: a workgroup with its account or label, or a cluster
	// endpoint (its label), as the real client requires.
	const bool names_endpoint = config.target == RedshiftTarget::PROVISIONED
	                                ? !config.endpoint_label.empty()
	                                : !config.account_id.empty() || !config.endpoint_label.empty();
	if (config.endpoint_port != 0 && !names_endpoint) {
		throw InvalidInputException("Redshift connection: an endpoint port goes only with a workgroup endpoint or a "
		                            "cluster endpoint");
	}
	if (config.region.empty()) {
		// Checked here because every client needs it: region selects which region's
		// Redshift is reached and signs the credential-minting call, and it cannot be
		// inferred from the caller's credentials.
		throw InvalidInputException("Redshift connection requires REGION");
	}
	if (config.target != RedshiftTarget::SERVERLESS && config.target != RedshiftTarget::PROVISIONED) {
		// Refused like the real client refuses it, before the open is held or failed, so a
		// resolver that forgets the target, or sets a value outside the enum, fails here rather
		// than reaching some default deployment.
		throw InvalidInputException("Redshift connection requires a target: a Serverless workgroup or a "
		                            "provisioned cluster");
	}
	// Held before the connection opens, as a slow connect would be.
	MockGates::Get().HoldIfArmed(MockGates::Point::OPEN);
	string failure;
	auto failure_type = ExceptionType::IO;
	unordered_map<string, string> failure_info;
	if (MockOpenFailure::Get().Take(failure, failure_type, failure_info)) {
		log_sink->Debug("redshift mock: could not open a connection to " + config.Describe());
		if (failure_type == ExceptionType::IO) {
			throw IOException(failure_info, failure);
		}
		throw Exception(failure_info, failure_type, failure);
	}
	return make_uniq<MockConnection>(config, credentials != nullptr, std::move(log_sink));
}

} // namespace duckdb
