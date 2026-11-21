//===----------------------------------------------------------------------===//
//                         DuckDB - Redshift
//
// src/transport/odbc/odbc_statement.hpp
//
// RedshiftResult over one executed statement handle, each value fetched as its column's binding says
// and returned in its carrier: bound over a ring of row arrays when every column
// is bounded, and otherwise one row a fetch, the columns before the first long one bound and the
// rest read with SQLGetData, into a ring of arenas and the values' own strings. A producer thread
// fills the sets and NextBatch builds a batch from each.
//===----------------------------------------------------------------------===//

#pragma once

#include "duckdb/common/common.hpp"
#include "duckdb/common/optional.hpp"
#include "odbc/bulk_fetch.hpp"
#include "odbc/fetch_producer.hpp"
#include "odbc/odbc_diagnostics.hpp"
#include "odbc/odbc_return_code.hpp"
#include "odbc_driver.hpp"
#include "redshift_transport.hpp"

#include <exception>
#include <limits>

namespace duckdb {

//! What a connection shares with every result it returns, so that
//! neither passes the driver a statement handle the other has freed. Only the producer has a lock:
//! the transport interface says a connection needs none and has one in-flight result. The producer
//! writes `statement` and `bound_values` only before it hands over the set that ends the rows, and
//! other threads read them after that hand-over or a join.
struct OdbcResultState {
	//! The open result's statement handle, null once the result or the connection has freed it.
	SQLHANDLE statement = SQL_NULL_HANDLE;
	//! The one-open-result fence: set by Execute, cleared when NextBatch first returns false or the
	//! result is destroyed. While it is set, Describe and Execute are refused.
	bool logical_result_open = false;
	//! Cleared when the connection closes, having freed the statement first.
	bool connection_open = true;
	//! The connection's handle and Target(), for the open result's connection-dead question, and its
	//! parameter values in bind order, for redacting its records. The handle is used only while
	//! connection_open is set.
	SQLHANDLE connection = SQL_NULL_HANDLE;
	string target;
	vector<string> bound_values;
	//! Set by a result whose failure lost the connection, or left its session unusable
	//! (RecordsShowUnusableSession). The connection then closes before its next driver call.
	bool connection_lost = false;
	//! The connection's sink, where the records of each fetch that succeeded with them are logged, on
	//! the caller's thread.
	shared_ptr<RedshiftLogSink> log_sink;
	//! The open result's producer thread, stopped and joined before any handle it may be using is
	//! freed; whichever caller comes first joins it, and later calls do nothing.
	FetchProducer producer;
};

//! What reading past a statement's current result found.
enum class FurtherResults : uint8_t {
	//! Nothing, or only results with no columns, the driver's cleanup state, now drained. In streaming
	//! mode a further UPDATE, INSERT or DELETE that changed no row also ends the drain here, leaving
	//! any result after it unread.
	NONE,
	//! A result set with columns, left unread.
	RESULT_SET,
	//! SQLMoreResults or SQLNumResultCols failed on the statement.
	DRIVER_FAILED
};

//! Reads past the executed `statement`'s current result, draining each further result with no
//! columns and stopping at the first that has columns, or at the driver's SQL_NO_DATA, which a
//! further statement that changed no row also gives.
FurtherResults DrainFurtherResults(const redshift_driver::OdbcApi &api, SQLHANDLE statement);

//! Whether the failure that left `records` lost `connection`: the driver reports it dead, its
//! answer cannot be read, or RecordsShowConnectionLoss. Read the records first:
//! the question clears the connection handle's.
bool ConnectionLost(const redshift_driver::OdbcApi &api, SQLHANDLE connection, const DiagnosticSet &records);

//! The rows of one executed statement, in batches.
class OdbcStatement : public RedshiftResult {
public:
	//! `columns` describes what is read, the prepared statement before SQLExecute or the executed
	//! result after SQLExecDirect, with none of them UNKNOWN; `column_sizes` is the size the driver
	//! reported for each and `numbers` how the numbers are fetched; `row_count` is what RowCount
	//! reports; `fetch_thread` which thread fills the sets. `api` lives for the process.
	OdbcStatement(const redshift_driver::OdbcApi &api, shared_ptr<OdbcResultState> state,
	              vector<RedshiftColumn> columns, const vector<SQLULEN> &column_sizes, NumberBinding numbers,
	              int64_t row_count, FetchThread fetch_thread);
	//! Unless NextBatch has returned false, stops and joins the producer, logs its unread sets'
	//! warnings, frees the statement handle if it is still held, ends the open result and marks the
	//! connection lost when a failed drain, or a set filled ahead and never read, lost it. Never throws.
	~OdbcStatement() override;
	OdbcStatement(const OdbcStatement &) = delete;
	OdbcStatement &operator=(const OdbcStatement &) = delete;

	//! Fills `batch` with the next rows, up to a row cap or a byte budget, and returns false at their
	//! end and on every call after. A closed or lost connection, a failed batch, or a failure found at
	//! the end throws instead, on every later call too.
	bool NextBatch(RedshiftBatch &batch) override;
	//! The rows a statement with no result columns affected, read right after it ran; negative, the
	//! interface's "no count available", for a statement that returns rows or a count the driver
	//! did not report.
	int64_t RowCount() const override;
	//! Whether a column is long, so the rows are read one a fetch with SQLGetData.
	bool HasLongColumn() const {
		return bound_columns < columns.size();
	}

private:
	//! A buffer set of a result with a long column: its rows' cells, until NextBatch builds their values.
	struct LongSet {
		//! The length a cell in wholes has in the arena: no length a driver reports.
		static constexpr SQLLEN IN_WHOLES = (std::numeric_limits<SQLLEN>::min)();
		//! Every cell in row order, each its length, SQL_NULL_DATA or IN_WHOLES, then the bytes of a
		//! bound cell or a native number. The allocation counter counts its capacity once the set is
		//! filled, after the reservation its rows left is given back.
		string arena;
		//! The bytes of each value SQLGetData returned in one part, in cell order, while they fit their
		//! cap and what the arena's reservation and wholes leave of the share. The share and the counter
		//! count its capacity.
		string short_values;
		//! The bytes of each other value read with ReadLong that is not NULL, in cell order, each in a
		//! string of its own that its RedshiftValue takes over, so each of its bytes is held once.
		vector<string> wholes;
		//! What wholes holds: each string and its bytes.
		idx_t whole_bytes = 0;
		idx_t rows = 0;
		//! What HeldFetchBytes counts for the arena, the short values, the slots of wholes and, while a
		//! set filled ahead waits for its batch, the bytes of its strings.
		idx_t counted = 0;

		//! What the set counts against its share of the budget, for rows of `column_count` cells: its
		//! bytes, all the short values' capacity, every string slot wholes has allocated, and the
		//! RedshiftValue each cell becomes.
		idx_t Bytes(idx_t column_count) const {
			return arena.size() + short_values.capacity() + WholesHeld() + rows * column_count * sizeof(RedshiftValue);
		}
		//! whole_bytes and every spare slot wholes has allocated: it grows by a factor, so its spare slots
		//! can number about as many as its strings.
		idx_t WholesHeld() const {
			return whole_bytes + (wholes.capacity() - wholes.size()) * sizeof(string);
		}
		//! Empties the set, its arena reserved at `reserve` bytes. BuildLongRows released the values.
		void Clear(idx_t reserve);
		//! Appends a cell of `length` bytes from `bytes`, or none for SQL_NULL_DATA.
		void AppendCell(const char *bytes, SQLLEN length);
		//! Appends a value of one part: its length to the arena, its bytes to short_values, which grows
		//! by doubling but not past its cap, nor past `most` less what wholes holds. Returns false,
		//! appending nothing, if the bytes need more.
		bool AppendShort(const char *bytes, SQLLEN length, idx_t most);
		//! Appends a value in a string of its own: IN_WHOLES to the arena, and `bytes` to wholes.
		void AppendWhole(string bytes);
		//! Frees wholes and short_values, once BuildLongRows has built each value from them.
		void ReleaseValues();
		//! Reads the length of the cell at `at` in an arena, moving `at` to its bytes, if it has any.
		static SQLLEN NextLength(const char *&at);
	};
	//! The records a failed driver call on the statement left, read straight after it.
	struct FetchFailure {
		DiagnosticSet records;
		//! The column whose cell read failed, which a statement failure names.
		optional<SQLUSMALLINT> column;
		//! Whether the failure lost the connection: asked once, on the caller's thread with the
		//! producer joined, since the question is a driver call.
		optional<bool> connection_lost;
	};
	//! A FetchFailure thrown where the call failed, on the thread that fills the set, and raised by
	//! NextBatch.
	struct FailedDriverCall : std::exception {
		explicit FailedDriverCall(FetchFailure failure_p) : failure(std::move(failure_p)) {
		}
		FetchFailure failure;
	};
	//! Thrown by the producer when it sees the stop flag; the set it was filling is never read.
	struct FetchStopped : std::exception {};
	//! What filling a set found, written by the thread that filled it and read by NextBatch once it
	//! takes the set.
	struct FilledSet {
		//! The rows a bound set's fetch returned.
		idx_t rows = 0;
		//! The records of each fetch that succeeded with information, until they are logged.
		vector<DiagnosticSet> warnings;
		//! Whether no set follows this one: it read the end of the rows, or `error` holds a failure or
		//! the stop.
		bool last = false;
		std::exception_ptr error;
	};

	//! Fills set `set` and returns whether another follows: the producer's work for one set, run on
	//! the caller's thread when the test-only switch says so. Stores any failure in the set's record.
	bool FillSet(idx_t set) noexcept;
	//! Makes and binds the buffers, at the first fill.
	void BindBuffers();
	//! Sets each long set's share of the budget, its arena's reservation and its short values' cap,
	//! once the bound row's buffers are made.
	void SizeLongSets();
	//! Fills bound set `set` with one fetch. The end of the rows is read by a fetch of its own.
	void FillBoundSet(idx_t set);
	//! Fetches rows of a result with a long column one at a time into long set `set`, until it holds
	//! MAX_BATCH_ROWS rows or its share of the budget.
	void FillLongSet(idx_t set);
	//! The next filled set's rows, built on the caller's thread: none once the driver reported the end
	//! of the rows. Rethrows the set's stored failure.
	vector<vector<RedshiftValue>> ReadSet();
	//! Waits for the next set in turn to be filled, or fills it here on the switch's same-thread path.
	void WaitForNextSet(idx_t set);
	//! The rows bound set `set` holds.
	vector<vector<RedshiftValue>> BuildBoundSet(idx_t set, idx_t rows);
	//! The rows long set `set` holds, releasing an arena its last row grew past its share.
	vector<vector<RedshiftValue>> BuildLongSet(idx_t set);
	//! Throws FetchStopped when the producer has been asked to stop. Only the producer path asks: a
	//! closed connection leaves the block's flag set.
	void StopIfRequested() const;
	//! Stops and joins the producer, if it was started; a later call does nothing.
	void StopProducer() noexcept;
	//! Marks the result failed and joins the producer before freeing the arenas it fills.
	void FailBatch() noexcept;
	//! Logs the warnings of sets filled but never read, once the producer is joined.
	void LogUnreadWarnings() noexcept;
	//! Logs `fill`'s warnings and clears them. A line that cannot be built is dropped, so logging
	//! never fails a batch or a destructor.
	void LogSetWarnings(FilledSet &fill) noexcept;
	//! Frees the arenas and tells HeldFetchBytes, once nothing fills them.
	void ReleaseArenas() noexcept;
	//! Copies the current row's cells into `set`: the bound ones from their buffers, the rest read
	//! with SQLGetData in column order.
	void StageRow(LongSet &set);
	//! Reads cell `index` of the current row into `set` with SQLGetData.
	void StageReadCell(LongSet &set, idx_t index);
	//! Whether cell `index` of a row is read whole with ReadLong, in as many parts as it takes: a long
	//! column's, or text after the first long column.
	bool ReadsWhole(idx_t index) const;
	//! Tells HeldFetchBytes what `set`'s arena, short values, string slots and strings not yet in a
	//! batch hold now.
	void CountArena(LongSet &set);
	//! Fetches the next row array and checks its rows and bound cells before any is read, keeping a warning's records
	//! in `fill`. Returns false at the driver's end of the rows, which it finishes.
	bool FetchRowArray(FilledSet &fill);
	//! Throws for the batch, with the fetch's `records`, when a fetched row failed,
	//! a cell's length does not show its value whole, or the driver reported a cut value.
	void RefuseFailedFetch(const DiagnosticSet &records);
	//! Throws for the batch unless `length`, column `index`'s length indicator, is SQL_NULL_DATA or
	//! fits the column's buffer.
	void RefuseCutCell(const DiagnosticSet &records, idx_t index, SQLLEN length);
	//! Row `row` of bound set `set`, counting from 0, one value per column.
	vector<RedshiftValue> ReadBoundRow(idx_t set, idx_t row);
	//! The rows `set` holds, one value per column, each in its carrier, each whole cell's string moved
	//! into its value.
	vector<vector<RedshiftValue>> BuildLongRows(LongSet &set);
	//! Column `index`'s cell of `length` bytes at `bytes`, as its binding fetched it, in its carrier;
	//! a length of SQL_NULL_DATA is NULL.
	RedshiftValue ValueOf(idx_t index, const char *bytes, SQLLEN length);
	//! Column `index`'s whole cell, fetched as SQL_C_CHAR or SQL_C_BINARY, in its carrier.
	RedshiftValue WholeValue(idx_t index, string bytes);
	//! At the driver's end of the rows: drains the further results and frees the statement handle,
	//! unless the producer is stopping. The caller's thread frees the buffers once it has read the
	//! last set.
	void FinishStatement();
	//! Column `index`'s whole text as a TEXT value, checked to be valid UTF-8 and in
	//! its type's text form.
	RedshiftValue TextValue(idx_t index, string text);
	//! Column `index`'s whole text as its BOOLEAN, INTEGER or REAL carrier, parsed with no locale.
	//! Text that is no value of the column's type fails the batch.
	RedshiftValue NumberValue(idx_t index, const string &text);
	//! Column `index`'s whole text as a REAL value, parsed in its own precision with no locale,
	//! refusing finite text that rounds past the type's range.
	RedshiftValue RealValue(idx_t index, const string &text);
	//! Reads `column`'s cell as `c_type`, in as many parts as the driver returns it in, into `set`: a
	//! NULL into the arena, a value of one part into short_values while it fits, any other into wholes.
	void ReadLong(LongSet &set, SQLUSMALLINT column, SQLSMALLINT c_type);
	//! Frees the statement handle, if this result still holds it.
	void FreeStatement() noexcept;
	//! Reads the records the latest call on the statement left.
	DiagnosticSet ReadFetchRecords();
	//! Throws FailedDriverCall with the records the failed call on the statement left.
	[[noreturn]] void ThrowFetchFailure();
	//! The same for a failed read of `column`'s cell.
	[[noreturn]] void ThrowCellFailure(SQLUSMALLINT column);
	//! Called with the producer joined: throws the transport failure when `failure` lost the
	//! connection, marking it lost so that it closes; otherwise the fetch-phase statement failure,
	//! naming the column of a failed cell read.
	[[noreturn]] void RaiseFailure(FetchFailure &failure);
	//! Whether `failure` lost the connection, asked once, with the producer joined.
	bool LostConnection(FetchFailure &failure);
	//! Whether a set filled ahead and never read holds a failure that lost the connection, asked with
	//! the producer joined, or whose records show a session left unusable (RecordsShowUnusableSession).
	bool UnreadFailureLostConnection();

	const redshift_driver::OdbcApi &api;
	shared_ptr<OdbcResultState> state;
	vector<RedshiftColumn> columns;
	//! How each column is fetched, from its type, its reported size and the number binding.
	vector<ColumnBinding> bindings;
	//! How many columns are bound: every column when none is long, and otherwise those before the
	//! first long column.
	idx_t bound_columns;
	//! The bound buffers, made and bound by the result's first fetch and freed at the end of the rows:
	//! RING_SETS sets when every column is bounded, else one row for the columns before the first
	//! long one.
	unique_ptr<BoundRowArray> bound_rows;
	//! The sets of a result with a long column, used in turn.
	vector<LongSet> long_sets;
	//! Each long set's share of the budget, once the bound row's buffers are known.
	idx_t long_set_share = 0;
	//! What each long set's arena is reserved at: the share, or less when its rows' lengths and
	//! cells not read whole take less.
	idx_t long_set_reserve = 0;
	//! The most each long set's short_values can take while wholes is empty: what the arena's
	//! reservation leaves of the share. AppendShort also holds them to their cap.
	idx_t short_values_most = 0;
	//! What filling each set found, and the sets NextBatch has read.
	vector<FilledSet> fills;
	idx_t sets_read = 0;
	FetchThread fetch_thread;
	bool producer_started = false;
	int64_t row_count;
	//! What reading past the driver's end of data found: a further result set with columns, or a
	//! failed drain, fails this result.
	FurtherResults further = FurtherResults::NONE;
	//! A failed drain's records, read before the statement was freed and raised after the rows; a
	//! result destroyed first still marks a connection the drain lost.
	FetchFailure drain_failure {{DiagnosticPhase::STATEMENT, {}, false}, {}, {}};
	//! Set by the thread that fills the sets once the statement has reached the driver's end of data,
	//! read past its further results and freed its handle.
	bool driver_done = false;
	//! Set once NextBatch has taken the set after which none follows, or from the start when there
	//! are no columns. The result stays open until NextBatch has returned false.
	bool last_set_taken;
	//! Set when NextBatch has returned false. From then on the shared block, which a later result
	//! may be using, is left alone.
	bool ended = false;
	//! Set when a batch failed. Its rows are gone, so a later batch would skip them.
	bool failed = false;
};

} // namespace duckdb
