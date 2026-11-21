#include "odbc/odbc_statement.hpp"

#include "duckdb/common/exception.hpp"
#include "duckdb/common/numeric_utils.hpp"
#include "duckdb/common/operator/double_cast_operator.hpp"
#include "duckdb/common/string_util.hpp"
#include "odbc/odbc_errors.hpp"
#include "odbc/text_forms.hpp"
#include "utf8proc_wrapper.hpp"

#include <charconv>
#include <cmath>
#include <cstring>
#include <limits>
#include <string>
#include <utility>

namespace duckdb {

namespace {

//! The SQLSTATE of a value the driver cut to fit its buffer: string data, right truncated.
constexpr const char *CUT_VALUE_SQLSTATE = "01004";
//! The buffer one SQLGetData call reads into. A longer value arrives in several parts.
constexpr idx_t PART_BYTES = 4096;
//! More than two strings' own rounding of a reservation can add (15 bytes each in libc++ and MSVC). A
//! set's arena and short values together take at most its share less this, so the rounding cannot take
//! them past the share; an arena's spare below it is not worth giving back.
constexpr idx_t ARENA_SLACK_BYTES = 64;
//! The most a set's short values grow to. A larger buffer, freed and grown again for each batch, took
//! a page fault for each of its pages each time, so 2 to 4 KB values read on the caller's thread took
//! about a third more CPU than in strings of their own; within this cap, about the same.
constexpr idx_t SHORT_VALUES_MOST_BYTES = idx_t(256) * 1024;

//! The carrier the client gives a column of `type`, or NULL_VALUE for UNKNOWN, which has none.
//! Must match RedshiftCellWriter::RequiredKind, which refuses any other carrier.
RedshiftValueKind CarrierOf(RedshiftTypeId type) {
	switch (type) {
	case RedshiftTypeId::BOOLEAN:
		return RedshiftValueKind::BOOLEAN;
	case RedshiftTypeId::SMALLINT:
	case RedshiftTypeId::INTEGER:
	case RedshiftTypeId::BIGINT:
		return RedshiftValueKind::INTEGER;
	case RedshiftTypeId::FLOAT:
	case RedshiftTypeId::DOUBLE:
		return RedshiftValueKind::REAL;
	case RedshiftTypeId::DECIMAL:
	case RedshiftTypeId::VARCHAR:
	case RedshiftTypeId::CHAR:
	case RedshiftTypeId::DATE:
	case RedshiftTypeId::TIME:
	case RedshiftTypeId::TIMETZ:
	case RedshiftTypeId::TIMESTAMP:
	case RedshiftTypeId::TIMESTAMPTZ:
		return RedshiftValueKind::TEXT;
	case RedshiftTypeId::BLOB:
		return RedshiftValueKind::BLOB;
	case RedshiftTypeId::UNKNOWN:
		break;
	}
	return RedshiftValueKind::NULL_VALUE;
}

//! Redshift's name for a type whose text is checked against a form, as a refusal shows it.
const char *TypeName(RedshiftTypeId type) {
	switch (type) {
	case RedshiftTypeId::BOOLEAN:
		return "BOOLEAN";
	case RedshiftTypeId::SMALLINT:
		return "SMALLINT";
	case RedshiftTypeId::INTEGER:
		return "INTEGER";
	case RedshiftTypeId::BIGINT:
		return "BIGINT";
	case RedshiftTypeId::FLOAT:
		return "REAL";
	case RedshiftTypeId::DOUBLE:
		return "DOUBLE PRECISION";
	case RedshiftTypeId::DECIMAL:
		return "DECIMAL";
	case RedshiftTypeId::DATE:
		return "DATE";
	case RedshiftTypeId::TIME:
		return "TIME";
	case RedshiftTypeId::TIMETZ:
		return "TIMETZ";
	case RedshiftTypeId::TIMESTAMP:
		return "TIMESTAMP";
	case RedshiftTypeId::TIMESTAMPTZ:
		return "TIMESTAMPTZ";
	default:
		// Any VARCHAR or CHAR text is in its form, and a BLOB is never read as text.
		break;
	}
	throw InternalException("a Redshift column whose type has no text form failed a text-form check");
}

//! The refusal of a value whose text is not its type's form, as the server would never send it.
[[noreturn]] void ThrowNotInTextForm(RedshiftTypeId type, SQLUSMALLINT column) {
	throw Exception(ExceptionType::EXECUTOR,
	                "Redshift returned a value in column " + std::to_string(column) + " that is not in the " +
	                    TypeName(type) +
	                    " text form, so the batch was not returned; if the session's datestyle was changed, run "
	                    "the statement on a new connection, otherwise report it");
}

//! `text` as the driver writes a BOOLEAN: `1` or `0`, not `true` or `false` (measured on 2.3.0.0).
bool ParseBoolean(const string &text, bool &boolean) {
	if (text != "1" && text != "0") {
		return false;
	}
	boolean = text == "1";
	return true;
}

//! `text`, already in the integer text form, as an int64. std::from_chars follows no locale and fails
//! a value past BIGINT's range. Every width takes any int64, so an `oid` of 2^31 or more, described as
//! INTEGER, reaches the INTEGER column's range check instead of wrapping as the driver's parse does.
bool ParseInteger(const string &text, int64_t &integer) {
	const auto end = text.data() + text.size();
	const auto parsed = std::from_chars(text.data(), end, integer);
	return parsed.ec == std::errc() && parsed.ptr == end;
}

//! `text`, already in the float text form, parsed as a `type` (FLOAT or DOUBLE) with DuckDB's
//! fast_float parse, which follows no locale and reads the server's spellings of NaN and the
//! infinities. A REAL parses as a float, so a value the server rounded to its 6 digits stays within
//! the float range the caller narrows it to.
bool ParseReal(RedshiftTypeId type, const string &text, double &real) {
	if (type == RedshiftTypeId::FLOAT) {
		float narrow = 0;
		if (!TryDoubleCast<float>(text.data(), text.size(), narrow, false)) {
			return false;
		}
		real = narrow;
		return true;
	}
	return TryDoubleCast<double>(text.data(), text.size(), real, false);
}

//! Whether `text`, already in its type's text form, is written in digits rather than spelled, as
//! `inf` is. Decided by shape, so the infinity spellings are listed only in the text forms.
bool WrittenInDigits(const string &text) {
	const idx_t start = StringUtil::StartsWith(text, "-") ? 1 : 0;
	return start < text.size() && StringUtil::CharacterIsDigit(text[start]);
}

RedshiftValue AsBoolean(bool boolean) {
	RedshiftValue value;
	value.kind = RedshiftValueKind::BOOLEAN;
	value.boolean = boolean;
	return value;
}

RedshiftValue AsInteger(int64_t integer) {
	RedshiftValue value;
	value.kind = RedshiftValueKind::INTEGER;
	value.integer = integer;
	return value;
}

RedshiftValue AsReal(double real) {
	RedshiftValue value;
	value.kind = RedshiftValueKind::REAL;
	value.real = real;
	return value;
}

//! A number the driver parsed into the native C type of `kind`'s carrier, whose bytes start at `cell`.
RedshiftValue NativeNumber(RedshiftValueKind kind, const char *cell) {
	switch (kind) {
	case RedshiftValueKind::BOOLEAN:
		return AsBoolean(*cell != 0);
	case RedshiftValueKind::INTEGER: {
		SQLBIGINT integer = 0;
		memcpy(&integer, cell, sizeof(integer));
		return AsInteger(integer);
	}
	case RedshiftValueKind::REAL: {
		SQLDOUBLE real = 0;
		memcpy(&real, cell, sizeof(real));
		// The driver's C runtime parsed this cell, and its NaN bits differ by runtime (MSVC sets every
		// payload bit), so a NaN is made the quiet NaN, signed as the value is.
		return AsReal(std::isnan(real) ? std::copysign(std::numeric_limits<double>::quiet_NaN(), real) : real);
	}
	default:
		break;
	}
	throw InternalException("a Redshift column whose carrier is not a number was fetched as one");
}

//! A TEXT or BLOB value holding `bytes`.
RedshiftValue AsBytes(RedshiftValueKind kind, string bytes) {
	RedshiftValue value;
	value.kind = kind;
	value.text = std::move(bytes);
	return value;
}

//! How each of `columns` is fetched, given the size the driver reported for each.
vector<ColumnBinding> BindingsOf(const vector<RedshiftColumn> &columns, const vector<SQLULEN> &sizes,
                                 NumberBinding numbers) {
	vector<ColumnBinding> bindings;
	for (idx_t index = 0; index < columns.size(); index++) {
		bindings.push_back(BindingOf(columns[index], sizes[index], numbers));
	}
	return bindings;
}

//! How many of these columns come before the first long one, and so are bound: all of them when
//! none is long.
idx_t BoundColumnCount(const vector<ColumnBinding> &bindings) {
	for (idx_t index = 0; index < bindings.size(); index++) {
		if (bindings[index].IsLong()) {
			return index;
		}
	}
	return bindings.size();
}

//! The length the arena records for a cell of `binding` whose indicator the driver set to `length`.
//! A native number records its binding's whole width, because NativeNumber reads that many bytes.
SQLLEN StoredLength(const ColumnBinding &binding, SQLLEN length) {
	if (length == SQL_NULL_DATA) {
		return SQL_NULL_DATA;
	}
	return binding.IsNative() ? binding.width : length;
}

//! The records of a failure the client found itself, which has no driver record behind it.
DiagnosticSet NoRecords() {
	return DiagnosticSet {DiagnosticPhase::STATEMENT, {}, false};
}

//! Whether the driver reported that it cut a value, which it does as a warning on the fetch (public
//! source v2.3.0, rsutil.c copyStrDataBigLen).
bool ReportsCutValue(const DiagnosticSet &records) {
	for (const auto &record : records.records) {
		if (record.SqlState() == CUT_VALUE_SQLSTATE) {
			return true;
		}
	}
	return false;
}

//! The driver's connection-dead answer for `connection`: true unless it reports the connection alive.
bool ConnectionIsDead(const redshift_driver::OdbcApi &api, SQLHANDLE connection) {
	// The driver writes the answer through an int pointer (public source v2.3.0, rsoptions.cpp
	// RsOptions::RS_SQLGetConnectAttr, its SQL_ATTR_CONNECTION_DEAD case), so SQLUINTEGER must be as
	// wide. Checked here, as the platform headers odbc_driver.hpp includes choose its typedef.
	static_assert(sizeof(SQLUINTEGER) == sizeof(int), "the driver writes the connection-dead answer as an int");
	SQLUINTEGER dead = SQL_CD_FALSE;
	if (!Succeeded(api.GetConnectAttr(connection, SQL_ATTR_CONNECTION_DEAD, &dead, SQL_IS_UINTEGER, nullptr))) {
		// An answer that cannot be read counts as dead.
		return true;
	}
	return dead != SQL_CD_FALSE;
}

} // namespace

FurtherResults DrainFurtherResults(const redshift_driver::OdbcApi &api, SQLHANDLE statement) {
	for (;;) {
		const auto rc = api.MoreResults(statement);
		if (rc == SQL_NO_DATA) {
			return FurtherResults::NONE;
		}
		SQLSMALLINT count = 0;
		if (!Succeeded(rc) || !Succeeded(api.NumResultCols(statement, &count))) {
			return FurtherResults::DRIVER_FAILED;
		}
		if (count > 0) {
			return FurtherResults::RESULT_SET;
		}
	}
}

bool ConnectionLost(const redshift_driver::OdbcApi &api, SQLHANDLE connection, const DiagnosticSet &records) {
	return ConnectionIsDead(api, connection) || RecordsShowConnectionLoss(records);
}

OdbcStatement::OdbcStatement(const redshift_driver::OdbcApi &api_p, shared_ptr<OdbcResultState> state_p,
                             vector<RedshiftColumn> columns_p, const vector<SQLULEN> &column_sizes,
                             NumberBinding numbers, int64_t row_count_p, FetchThread fetch_thread_p)
    : api(api_p), state(std::move(state_p)), columns(std::move(columns_p)),
      bindings(BindingsOf(columns, column_sizes, numbers)), bound_columns(BoundColumnCount(bindings)), fills(RING_SETS),
      fetch_thread(fetch_thread_p), row_count(row_count_p), last_set_taken(columns.empty()) {
}

OdbcStatement::~OdbcStatement() {
	if (!ended) {
		// Joined first: the producer may be in a driver call on the handle freed next.
		StopProducer();
		LogUnreadWarnings();
		FreeStatement();
		state->logical_result_open = false;
		// A caller can stop after the last rows, before NextBatch raises the drain's failure, or before
		// it reaches a set the producer failed to fill. Both failures' records are read already, so only
		// the driver is asked here; a destructor must not let what that throws escape, and the
		// connection then stays as it was. A failure whose records show a session left unusable marks
		// the connection too, as RaiseFailure does for one it raises.
		try {
			if (state->connection_open &&
			    ((further == FurtherResults::DRIVER_FAILED &&
			      (LostConnection(drain_failure) || RecordsShowUnusableSession(drain_failure.records))) ||
			     UnreadFailureLostConnection())) {
				state->connection_lost = true;
			}
		} catch (...) {
		}
	}
	ReleaseArenas();
}

bool OdbcStatement::NextBatch(RedshiftBatch &batch) {
	batch.rows.clear();
	if (!state->connection_open || state->connection_lost) {
		throw IOException("this Redshift result's connection has closed; run the statement again on a new connection");
	}
	if (ended) {
		return false;
	}
	if (failed) {
		throw Exception(ExceptionType::EXECUTOR,
		                "an earlier batch of this Redshift result failed, and the rest of its rows are not "
		                "returned without it; run the statement again");
	}
	if (!last_set_taken) {
		try {
			batch.rows = ReadSet();
		} catch (FailedDriverCall &call) {
			// The connection-dead question is a driver call, so it waits for the join.
			FailBatch();
			RaiseFailure(call.failure);
		} catch (...) {
			FailBatch();
			throw;
		}
	}
	// Rows already read are returned first, so a further result set or a failed drain is raised by
	// the call that would report the end, and by every call after it.
	if (!batch.rows.empty()) {
		return true;
	}
	if (further == FurtherResults::RESULT_SET) {
		ThrowMultipleResultSets();
	}
	if (further == FurtherResults::DRIVER_FAILED) {
		RaiseFailure(drain_failure);
	}
	ended = true;
	state->logical_result_open = false;
	return false;
}

int64_t OdbcStatement::RowCount() const {
	return row_count;
}

bool OdbcStatement::FillSet(idx_t set) noexcept {
	auto &fill = fills[set];
	fill = FilledSet();
	try {
		if (!bound_rows) {
			BindBuffers();
		}
		if (HasLongColumn()) {
			FillLongSet(set);
		} else {
			FillBoundSet(set);
		}
		fill.last = driver_done;
	} catch (...) {
		fill.error = std::current_exception();
		fill.last = true;
	}
	return !fill.last;
}

void OdbcStatement::BindBuffers() {
	// Without SQL_GD_BLOCK, which the pinned driver lacks, SQLGetData reads only a row array of 1, so a
	// long column takes one row a fetch whatever SQL_GETDATA_EXTENSIONS says. The columns before it
	// are still bound, so only it and those after it take a driver call a cell; its sets copy the
	// cells into an arena, the values of one part beside it and any other into a string of its own.
	const vector<ColumnBinding> bound(bindings.begin(), bindings.begin() + NumericCast<int64_t>(bound_columns));
	bound_rows = HasLongColumn() ? make_uniq<BoundRowArray>(bound, 1)
	                             : make_uniq<BoundRowArray>(bound, RowArraySize(bound), RING_SETS);
	if (HasLongColumn()) {
		SizeLongSets();
	}
	if (!bound_rows->Bind(api, state->statement)) {
		ThrowFetchFailure();
	}
}

void OdbcStatement::SizeLongSets() {
	// The bound columns' row comes out of the budget first, so it and the arenas fit it together.
	// Redshift's column limit keeps that row under 2 MiB; a driver that reports more still leaves
	// each set a share.
	long_set_share = (MAX_BATCH_BYTES - MinValue<idx_t>(bound_rows->Bytes(), MAX_BATCH_BYTES / 2)) / RING_SETS;
	// An arena holds each cell's length and the cells not read whole, so the set being filled is
	// reserved at what its rows can need rather than at a whole share, and its short values may take
	// the rest.
	idx_t row_bytes = columns.size() * sizeof(SQLLEN);
	for (idx_t index = 0; index < columns.size(); index++) {
		if (!ReadsWhole(index)) {
			row_bytes += NumericCast<idx_t>(bindings[index].width);
		}
	}
	long_set_reserve = MinValue<idx_t>(long_set_share - ARENA_SLACK_BYTES, MAX_BATCH_ROWS * row_bytes);
	// Values of one part grow only into what the arena's reservation and wholes leave, so growing
	// them never takes the reservation, the short values and the strings past the share.
	short_values_most = long_set_share - ARENA_SLACK_BYTES - long_set_reserve;
}

void OdbcStatement::FillBoundSet(idx_t set) {
	bound_rows->Select(set);
	auto &fill = fills[set];
	if (FetchRowArray(fill)) {
		fill.rows = bound_rows->Fetched();
	}
}

void OdbcStatement::FillLongSet(idx_t set) {
	if (long_sets.empty()) {
		long_sets.resize(RING_SETS);
	}
	auto &long_set = long_sets[set];
	long_set.Clear(long_set_reserve);
	// Without the share a set of long values would hold MAX_BATCH_ROWS of them, whatever their size.
	// It is checked before each row, so a set keeps the row that reaches it: a set
	// can exceed it by its last row, and a row above the whole budget still crosses whole.
	while (long_set.rows < MAX_BATCH_ROWS && long_set.Bytes(columns.size()) < long_set_share &&
	       FetchRowArray(fills[set])) {
		StageRow(long_set);
	}
	// The share counts the arena's bytes, not its reservation, so a set filled ahead gives back what
	// its rows left unused; four sets of wide bound cells would otherwise hold about twice the budget.
	if (long_set.arena.capacity() > long_set.arena.size() + ARENA_SLACK_BYTES) {
		string(long_set.arena).swap(long_set.arena);
	}
	CountArena(long_set);
}

vector<vector<RedshiftValue>> OdbcStatement::ReadSet() {
	const auto set = sets_read++ % RING_SETS;
	WaitForNextSet(set);
	auto &fill = fills[set];
	// Logged here rather than by the producer: a log call can close the database on its thread.
	LogSetWarnings(fill);
	if (fill.last) {
		StopProducer();
		last_set_taken = true;
	}
	if (fill.error) {
		// Taken as it is raised, so a failure left in a set is one never raised.
		std::rethrow_exception(std::exchange(fill.error, nullptr));
	}
	auto rows = HasLongColumn() ? BuildLongSet(set) : BuildBoundSet(set, fill.rows);
	if (fill.last) {
		// The handle is freed, so the driver no longer holds the buffers' addresses.
		bound_rows.reset();
		ReleaseArenas();
	} else if (producer_started) {
		state->producer.Release();
	}
	return rows;
}

void OdbcStatement::WaitForNextSet(idx_t set) {
	if (fetch_thread == FetchThread::CALLER) {
		FillSet(set);
		return;
	}
	if (!producer_started) {
		// Started by the first NextBatch, after Execute's own driver calls, so a result destroyed
		// unread makes none.
		state->producer.Start(RING_SETS, [this](idx_t next) { return FillSet(next); });
		producer_started = true;
	}
	if (!state->producer.WaitForFilledSet()) {
		throw InternalException("a Redshift result's producer thread ended before filling the set it reads next");
	}
}

vector<vector<RedshiftValue>> OdbcStatement::BuildBoundSet(idx_t set, idx_t rows) {
	vector<vector<RedshiftValue>> built;
	built.reserve(rows);
	for (idx_t row = 0; row < rows; row++) {
		built.push_back(ReadBoundRow(set, row));
	}
	return built;
}

vector<vector<RedshiftValue>> OdbcStatement::BuildLongSet(idx_t set) {
	auto &long_set = long_sets[set];
	auto rows = BuildLongRows(long_set);
	// The row that reaches the share is appended whole, so its bound cells can grow the arena past
	// its reservation, and an arena keeps what it grew to.
	if (long_set.arena.capacity() > long_set_share) {
		string().swap(long_set.arena);
	}
	// BuildLongRows gave the whole values' strings to the rows and freed their slots and the short
	// values.
	CountArena(long_set);
	return rows;
}

void OdbcStatement::StopIfRequested() const {
	if (fetch_thread == FetchThread::PRODUCER && state->producer.Stopping()) {
		throw FetchStopped();
	}
}

void OdbcStatement::StopProducer() noexcept {
	if (producer_started) {
		state->producer.Stop();
	}
}

void OdbcStatement::FailBatch() noexcept {
	failed = true;
	StopProducer();
	// No later batch reads the arenas, so they go now rather than with the result.
	ReleaseArenas();
}

void OdbcStatement::LogUnreadWarnings() noexcept {
	// A set's warnings are cleared once logged, so what is left belongs to sets never read.
	for (idx_t offset = 0; offset < RING_SETS; offset++) {
		LogSetWarnings(fills[(sets_read + offset) % RING_SETS]);
	}
}

void OdbcStatement::LogSetWarnings(FilledSet &fill) noexcept {
	try {
		for (const auto &records : fill.warnings) {
			LogInformationalDiagnostics(*state->log_sink, records);
		}
	} catch (...) {
		// Building a line can fail; the lines are dropped rather than fail the batch or a destructor.
	}
	fill.warnings.clear();
}

void OdbcStatement::ReleaseArenas() noexcept {
	for (auto &long_set : long_sets) {
		CountFetchBytes(0, long_set.counted);
	}
	long_sets.clear();
	long_sets.shrink_to_fit();
}

void OdbcStatement::CountArena(LongSet &long_set) {
	// A filled set's strings are counted until its batch takes them over.
	const auto bytes = long_set.arena.capacity() + long_set.short_values.capacity() + long_set.WholesHeld();
	CountFetchBytes(bytes, long_set.counted);
	long_set.counted = bytes;
}

void OdbcStatement::LongSet::Clear(idx_t reserve) {
	// A given-back arena reserved again would grow by its library's factor, past `reserve`; a new
	// string reserved at it holds that, give or take the library's rounding.
	if (arena.capacity() < reserve) {
		string fresh;
		fresh.reserve(reserve);
		arena.swap(fresh);
	}
	arena.clear();
	rows = 0;
}

void OdbcStatement::LongSet::AppendCell(const char *bytes, SQLLEN length) {
	arena.append(reinterpret_cast<const char *>(&length), sizeof(length));
	if (length != SQL_NULL_DATA) {
		arena.append(bytes, NumericCast<idx_t>(length));
	}
}

bool OdbcStatement::LongSet::AppendShort(const char *bytes, SQLLEN length, idx_t most) {
	const auto needed = short_values.size() + NumericCast<idx_t>(length);
	if (needed > short_values.capacity()) {
		// What wholes holds leaves the short values less, so a doubling cannot take the set past its
		// share when values of both kinds fill it.
		const auto held = WholesHeld();
		const auto room = MinValue<idx_t>(SHORT_VALUES_MOST_BYTES, most > held ? most - held : 0);
		if (needed > room) {
			return false;
		}
		// A string's own growth is geometric and can pass the cap; a new one reserved at the size wanted
		// holds that, give or take a library's rounding.
		string grown;
		grown.reserve(MaxValue<idx_t>(needed, MinValue<idx_t>(2 * short_values.capacity(), room)));
		grown.append(short_values);
		short_values.swap(grown);
	}
	arena.append(reinterpret_cast<const char *>(&length), sizeof(length));
	short_values.append(bytes, NumericCast<idx_t>(length));
	return true;
}

void OdbcStatement::LongSet::AppendWhole(string bytes) {
	const SQLLEN marker = IN_WHOLES;
	arena.append(reinterpret_cast<const char *>(&marker), sizeof(marker));
	whole_bytes += sizeof(string) + bytes.size();
	wholes.push_back(std::move(bytes));
}

void OdbcStatement::LongSet::ReleaseValues() {
	// Each string is its value's now, and those left are empty, so their slots go too. The short
	// values go as well, so a set's next fill counts only the capacity its own values need.
	vector<string>().swap(wholes);
	whole_bytes = 0;
	string().swap(short_values);
}

SQLLEN OdbcStatement::LongSet::NextLength(const char *&at) {
	// The arena gives a length no alignment, so it is copied out.
	SQLLEN length;
	memcpy(&length, at, sizeof(length));
	at += sizeof(length);
	return length;
}

bool OdbcStatement::ReadsWhole(idx_t index) const {
	return index >= bound_columns && !bindings[index].IsNative();
}

void OdbcStatement::StageRow(LongSet &set) {
	// SQLGetData reads the columns after the last bound one in column order, which plain ODBC always
	// allows, so none needs SQL_GD_ANY_COLUMN or SQL_GD_ANY_ORDER.
	for (idx_t index = 0; index < columns.size(); index++) {
		if (index >= bound_columns) {
			StageReadCell(set, index);
			continue;
		}
		set.AppendCell(bound_rows->Value(index, 0), StoredLength(bindings[index], bound_rows->Length(index, 0)));
	}
	set.rows++;
}

void OdbcStatement::StageReadCell(LongSet &set, idx_t index) {
	const auto column = NumericCast<SQLUSMALLINT>(index + 1);
	const auto &binding = bindings[index];
	if (ReadsWhole(index)) {
		// A VARBYTE's binding is SQL_C_BINARY: bytes, not the hex text the driver returns for it as
		// characters. A NULL reported after a part drops the part.
		ReadLong(set, column, binding.c_type);
		return;
	}
	// Wide enough for every native number carrier: a bit, a SQLBIGINT or a SQLDOUBLE.
	char cell[sizeof(SQLBIGINT)] = {};
	SQLLEN indicator = 0;
	StopIfRequested();
	if (!Succeeded(api.GetData(state->statement, column, binding.c_type, cell, binding.width, &indicator))) {
		ThrowCellFailure(column);
	}
	set.AppendCell(cell, StoredLength(binding, indicator));
}

vector<vector<RedshiftValue>> OdbcStatement::BuildLongRows(LongSet &set) {
	vector<vector<RedshiftValue>> rows;
	rows.reserve(set.rows);
	const char *cell = set.arena.data();
	const char *short_value = set.short_values.data();
	auto whole = set.wholes.begin();
	for (idx_t row = 0; row < set.rows; row++) {
		vector<RedshiftValue> values;
		values.reserve(columns.size());
		for (idx_t index = 0; index < columns.size(); index++) {
			const auto length = LongSet::NextLength(cell);
			if (ReadsWhole(index) && length == LongSet::IN_WHOLES) {
				values.push_back(WholeValue(index, std::move(*whole++)));
				continue;
			}
			// A ReadLong cell's bytes are in short_values; any other cell's follow its length.
			auto &bytes = ReadsWhole(index) ? short_value : cell;
			values.push_back(ValueOf(index, bytes, length));
			bytes += length == SQL_NULL_DATA ? 0 : NumericCast<idx_t>(length);
		}
		rows.push_back(std::move(values));
	}
	D_ASSERT(cell == set.arena.data() + set.arena.size() && whole == set.wholes.end() &&
	         short_value == set.short_values.data() + set.short_values.size());
	set.ReleaseValues();
	return rows;
}

bool OdbcStatement::FetchRowArray(FilledSet &fill) {
	StopIfRequested();
	const auto rc = api.Fetch(state->statement);
	// A failed fetch is stored as its failure even when a stop came during it, so the destructor
	// can still ask whether it lost the connection.
	if (rc != SQL_NO_DATA && !Succeeded(rc)) {
		ThrowFetchFailure();
	}
	// A stop that came during any other fetch is seen before the next call, so the producer leaves
	// the rest of the statement to the thread that joins it.
	StopIfRequested();
	// ODBC ends with SQL_NO_DATA; a success with no rows, which it rules out, ends the rows too.
	if (rc == SQL_NO_DATA || bound_rows->Fetched() == 0) {
		FinishStatement();
		return false;
	}
	const auto records = rc == SQL_SUCCESS_WITH_INFO ? ReadFetchRecords() : NoRecords();
	RefuseFailedFetch(records);
	// The next fetch clears them, and a passed fetch reports them nowhere else.
	if (!records.records.empty()) {
		fill.warnings.push_back(records);
	}
	return true;
}

void OdbcStatement::RefuseFailedFetch(const DiagnosticSet &records) {
	for (idx_t row = 0; row < bound_rows->Fetched(); row++) {
		const auto status = bound_rows->Status(row);
		if (status != SQL_ROW_SUCCESS && status != SQL_ROW_SUCCESS_WITH_INFO) {
			throw FailedDriverCall(FetchFailure {records, {}, {}});
		}
	}
	// No lossless fallback: over a row array above 1, without SQL_GD_BLOCK, a bound cell cannot be
	// read again with SQLGetData; over a row array of 1 a cut is refused too, as it is for every
	// bound cell. The driver keeps only the last cell's records (public source
	// v2.3.0, rsresult.cpp RS_SQLGetData), so the length indicators find a cut; the records go with
	// the refusal only when they hold the 01004 that reports one.
	const bool driver_cut = ReportsCutValue(records);
	const auto cut_records = driver_cut ? records : NoRecords();
	for (idx_t row = 0; row < bound_rows->Fetched(); row++) {
		for (idx_t index = 0; index < bound_rows->ColumnCount(); index++) {
			RefuseCutCell(cut_records, index, bound_rows->Length(index, row));
		}
	}
	if (driver_cut) {
		ThrowCutValue(records, "the Redshift driver cut a value to fit this extension's buffer for its column");
	}
}

void OdbcStatement::RefuseCutCell(const DiagnosticSet &records, idx_t index, SQLLEN length) {
	if (length == SQL_NULL_DATA) {
		return;
	}
	// SQL_NO_TOTAL, or any other negative length, leaves the value's size unknown.
	if (length < 0) {
		ThrowCutValue(records, "the Redshift driver did not report the length of a value in column " +
		                           std::to_string(index + 1));
	}
	if (length > bound_rows->Binding(index).Holds()) {
		ThrowCutValue(records, "Redshift returned a value in column " + std::to_string(index + 1) +
		                           " longer than this extension's buffer for it");
	}
}

vector<RedshiftValue> OdbcStatement::ReadBoundRow(idx_t set, idx_t row) {
	vector<RedshiftValue> values;
	values.reserve(columns.size());
	for (idx_t index = 0; index < columns.size(); index++) {
		values.push_back(ValueOf(index, bound_rows->ValueIn(set, index, row), bound_rows->LengthIn(set, index, row)));
	}
	return values;
}

RedshiftValue OdbcStatement::ValueOf(idx_t index, const char *bytes, SQLLEN length) {
	if (length == SQL_NULL_DATA) {
		return RedshiftValue();
	}
	if (bindings[index].IsNative()) {
		return NativeNumber(CarrierOf(columns[index].type), bytes);
	}
	return WholeValue(index, string(bytes, NumericCast<idx_t>(length)));
}

RedshiftValue OdbcStatement::WholeValue(idx_t index, string bytes) {
	switch (CarrierOf(columns[index].type)) {
	case RedshiftValueKind::BOOLEAN:
	case RedshiftValueKind::INTEGER:
	case RedshiftValueKind::REAL:
		return NumberValue(index, bytes);
	case RedshiftValueKind::TEXT:
		return TextValue(index, std::move(bytes));
	case RedshiftValueKind::BLOB:
		return AsBytes(RedshiftValueKind::BLOB, std::move(bytes));
	case RedshiftValueKind::NULL_VALUE:
		break;
	}
	throw InternalException("a Redshift result column with no carrier reached the fetch; Execute refuses one");
}

void OdbcStatement::FinishStatement() {
	further = DrainFurtherResults(api, state->statement);
	if (further == FurtherResults::DRIVER_FAILED) {
		// Read now: freeing the statement discards its records.
		drain_failure = FetchFailure {ReadFetchRecords(), {}, {}};
	}
	// A stopped producer leaves the free to the thread that joins it.
	StopIfRequested();
	// Freed before the caller sees the end, as the transport allows; a further result set is
	// discarded unread with it, and a failed drain is not tried again.
	FreeStatement();
	driver_done = true;
}

RedshiftValue OdbcStatement::TextValue(idx_t index, string text) {
	const auto column = NumericCast<SQLUSMALLINT>(index + 1);
	// Checked on the whole value, never a part, because a character's bytes can span two parts.
	// RedshiftCellWriter copies the bytes without validating them.
	if (!Utf8Proc::IsValid(text.c_str(), text.size())) {
		throw Exception(ExceptionType::EXECUTOR,
		                "Redshift returned text in column " + std::to_string(column) +
		                    " that is not valid UTF-8, so the batch was not returned; check the column's data");
	}
	if (!MatchesTextForm(columns[index], text)) {
		ThrowNotInTextForm(columns[index].type, column);
	}
	return AsBytes(RedshiftValueKind::TEXT, std::move(text));
}

RedshiftValue OdbcStatement::NumberValue(idx_t index, const string &text) {
	const auto type = columns[index].type;
	const auto kind = CarrierOf(type);
	if (kind == RedshiftValueKind::REAL) {
		return RealValue(index, text);
	}
	bool boolean = false;
	if (kind == RedshiftValueKind::BOOLEAN && ParseBoolean(text, boolean)) {
		return AsBoolean(boolean);
	}
	int64_t integer = 0;
	if (kind == RedshiftValueKind::INTEGER && MatchesTextForm(columns[index], text) && ParseInteger(text, integer)) {
		return AsInteger(integer);
	}
	ThrowNotInTextForm(type, NumericCast<SQLUSMALLINT>(index + 1));
}

RedshiftValue OdbcStatement::RealValue(idx_t index, const string &text) {
	const auto column = NumericCast<SQLUSMALLINT>(index + 1);
	const auto type = columns[index].type;
	double real = 0;
	if (!MatchesTextForm(columns[index], text) || !ParseReal(type, text, real)) {
		ThrowNotInTextForm(type, column);
	}
	// A value near its type's largest, printed with fewer digits than the type needs (as after a
	// caller's extra_float_digits 0), can arrive as text that rounds past it.
	if (std::isinf(real) && WrittenInDigits(text)) {
		throw Exception(ExceptionType::EXECUTOR,
		                "Redshift returned a " + string(TypeName(type)) + " value in column " + std::to_string(column) +
		                    " whose text rounds past the type's range, so the batch was not returned; select the "
		                    "column as VARCHAR (::VARCHAR) to read it");
	}
	return AsReal(real);
}

void OdbcStatement::ReadLong(LongSet &set, SQLUSMALLINT column, SQLSMALLINT c_type) {
	char part[PART_BYTES];
	// A character part holds the buffer's size less its terminating NUL, and a binary part all of it.
	const idx_t room = c_type == SQL_C_CHAR ? PART_BYTES - 1 : PART_BYTES;
	string whole;
	for (;;) {
		SQLLEN indicator = 0;
		StopIfRequested();
		if (!Succeeded(
		        api.GetData(state->statement, column, c_type, part, NumericCast<SQLLEN>(PART_BYTES), &indicator))) {
			ThrowCellFailure(column);
		}
		if (indicator == SQL_NULL_DATA) {
			set.AppendCell(nullptr, SQL_NULL_DATA);
			return;
		}
		// The indicator is the length still to come, or SQL_NO_TOTAL when the driver cannot say, so
		// the value is complete when it fits; until then the part is full. A complete value is never
		// read again, so SQL_NO_DATA is a failure here.
		const bool complete = indicator >= 0 && NumericCast<idx_t>(indicator) <= room;
		// A value of one part is copied beside the arena, so this thread makes no string for it;
		// NextBatch's thread does, when it builds the batch. One that would take the short values past
		// their cap gets its own string.
		if (complete && whole.empty() && set.AppendShort(part, indicator, short_values_most)) {
			return;
		}
		AppendLongPart(whole, part, complete ? NumericCast<idx_t>(indicator) : room, indicator);
		if (complete) {
			set.AppendWhole(std::move(whole));
			return;
		}
	}
}

DiagnosticSet OdbcStatement::ReadFetchRecords() {
	return ReadStatementDiagnostics(api, {SQL_HANDLE_STMT, state->statement}, state->bound_values);
}

void OdbcStatement::ThrowFetchFailure() {
	throw FailedDriverCall(FetchFailure {ReadFetchRecords(), {}, {}});
}

void OdbcStatement::ThrowCellFailure(SQLUSMALLINT column) {
	throw FailedDriverCall(FetchFailure {ReadFetchRecords(), column, {}});
}

void OdbcStatement::RaiseFailure(FetchFailure &failure) {
	if (LostConnection(failure)) {
		state->connection_lost = true;
		ThrowTransportFailure(failure.records, state->target, StatementPhase::FETCH);
	}
	// Closed as a lost connection is, when the connection ends this result
	// (OdbcConnection::CloseIfResultLostConnection).
	if (RecordsShowUnusableSession(failure.records)) {
		state->connection_lost = true;
	}
	if (failure.column) {
		ThrowCellReadFailure(failure.records, *failure.column);
	}
	ThrowStatementFailure(failure.records, StatementPhase::FETCH);
}

bool OdbcStatement::UnreadFailureLostConnection() {
	// A failure ends the fills, so at most one set holds one, and ReadSet takes one it raises.
	for (const auto &fill : fills) {
		if (!fill.error) {
			continue;
		}
		try {
			std::rethrow_exception(fill.error);
		} catch (FailedDriverCall &call) {
			return LostConnection(call.failure) || RecordsShowUnusableSession(call.failure.records);
		} catch (...) {
			// The stop, or a local failure such as an allocation: neither says anything of the connection.
		}
	}
	return false;
}

bool OdbcStatement::LostConnection(FetchFailure &failure) {
	if (!failure.connection_lost) {
		failure.connection_lost = ConnectionLost(api, state->connection, failure.records);
	}
	return *failure.connection_lost;
}

void OdbcStatement::FreeStatement() noexcept {
	// No record of the statement is read once it is freed, so its parameter values are not kept.
	state->bound_values.clear();
	if (state->statement == SQL_NULL_HANDLE) {
		return;
	}
	// Whatever the driver answers: a handle it refuses to free is dropped, not used again.
	api.FreeHandle(SQL_HANDLE_STMT, state->statement);
	state->statement = SQL_NULL_HANDLE;
}

} // namespace duckdb
