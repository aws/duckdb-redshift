//===----------------------------------------------------------------------===//
//                         DuckDB - Redshift
//
// test/client/fake_driver/fake_odbc_driver.hpp
//
// An ODBC driver inside the test process, behind odbc_driver.hpp, so the ODBC client's real code runs in
// offline tests with no driver file, no platform loader and no network.
//===----------------------------------------------------------------------===//

#pragma once

#include "odbc_driver.hpp"

#include <chrono>
#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <utility>
#include <vector>

namespace duckdb {
namespace redshift_driver {
namespace testing {

//! One diagnostic record.
struct FakeDiag {
	std::string sqlstate;  //!< 5 characters
	SQLINTEGER native = 0; //!< the driver's native error code, as SQLGetDiagRec reports it
	std::string message;   //!< the message the driver formed; reads report its first SetMessageLimit bytes
	//! Further fields by SQLGetDiagField identifier, written as text, for a test that models one a
	//! recorded driver trace shows. Reading a field the record lacks answers SQL_NO_DATA.
	std::map<SQLSMALLINT, std::string> fields;
};

//! One result column, as SQLDescribeCol and SQLColAttribute report it, in the driver's own spelling
//! ("CHARACTER VARYING", not "varchar"). SQL_DESC_PRECISION is derived from the type, and from the
//! size for NUMERIC and DECIMAL, as the driver's is.
struct FakeColumn {
	std::string name;                    //!< the column name SQLDescribeCol reports
	std::string type_name;               //!< SQL_DESC_TYPE_NAME, e.g. "NUMERIC"
	SQLSMALLINT sql_type = 0;            //!< SQL_DESC_CONCISE_TYPE, and SQLDescribeCol's data type
	SQLULEN column_size = 0;             //!< SQL_DESC_LENGTH, and SQLDescribeCol's column size
	SQLSMALLINT decimal_digits = 0;      //!< SQL_DESC_SCALE, and SQLDescribeCol's decimal digits
	SQLSMALLINT nullable = SQL_NULLABLE; //!< SQL_DESC_NULLABLE, and SQLDescribeCol's nullable flag
	//! SQLGetData reports SQL_NO_TOTAL for each part it cuts, and the last part its own length, and
	//! SQLFetch so reports a bound cell it cuts. The driver never reports SQL_NO_TOTAL, so this is for
	//! testing a caller's handling of it.
	bool report_no_total = false;
};

//! One result set of a statement.
struct FakeResultSet {
	std::vector<FakeColumn> columns; //!< empty for a statement with no result set
	//! One cell per column: the server's text, converted when read as SQL_C_BIT, SQL_C_SBIGINT or
	//! SQL_C_DOUBLE, else the bytes the driver returns for the C type read, such as a VARBYTE's hex
	//! for SQL_C_CHAR. A std::string, so an embedded NUL keeps its length.
	std::vector<std::vector<std::string>> rows;
	std::vector<std::vector<bool>> nulls; //!< true where the cell is SQL NULL; empty when none is
	std::vector<SQLUSMALLINT> row_status; //!< per row; empty means SQL_ROW_SUCCESS throughout
	//! By row index, for a row in error. As in the driver, only the last bound cell's records outlive
	//! a fetch, as each cell's read clears them, so a later row's bound read drops these.
	std::map<size_t, std::vector<FakeDiag>> row_diags;
	//! What SQLRowCount reports: for a set with no columns the rows the command affected, which is 0,
	//! not -1, for a command such as SET, and -1 only for an empty query; for a set with columns -1
	//! until the driver holds every row.
	SQLLEN row_count = -1;
};

//! Holds one call that BlockCall names until the test releases it. The test and the fake share it.
struct FakeGate {
	//! Returns whether the held call arrived within timeout, and never throws, so a test whose call
	//! never comes still releases the gate and joins its threads before it asserts.
	bool WaitUntilHeldFor(std::chrono::milliseconds timeout);
	//! Lets the held call continue; a call that arrives later does not wait.
	void Release();

private:
	friend class FakeOdbcDriver;
	//! Run by the held call, outside the fake's mutex. It waits without a bound, because a release
	//! on a timer would let the call through while the test still believed it held, and the test is
	//! what decides when the call proceeds. A test that never releases the gate therefore hangs, so
	//! each one releases it on every path, before it asserts.
	void Hold();

	//! Guards held and released; changed signals each change. The gate's own, so a held call waits
	//! on it with the fake's mutex released.
	std::mutex mutex;
	std::condition_variable changed;
	bool held = false;
	bool released = false;
};

//! One call the fake received.
struct FakeCall {
	//! The ODBC function called, e.g. "SQLDriverConnect".
	std::string entry_point;
	//! The connection string, the SQL text, "attribute=value" for an attribute set, the attribute,
	//! info type, completion type or SQLFreeStmt option, the handle type SQLFreeHandle was given
	//! ("statement"), "column c_type" for SQLGetData ("1 -2"), "column c_type buffer_length" for
	//! SQLBindCol ("1 1 301"), the operation for SQLSetPos, or "number=value" per marker SQLExecute
	//! read, one per line; empty for every other call. A statement attribute other than the row-array
	//! size logs the attribute alone.
	std::string detail;
};

//! The output buffer one SQLDriverConnect was given for the completed connection string.
struct FakeConnectOutput {
	//! Whether the buffer pointer was not null.
	bool has_buffer = false;
	//! The buffer's capacity in bytes, as passed, or -1 before any SQLDriverConnect.
	SQLSMALLINT capacity = -1;
	//! The completion mode, such as SQL_DRIVER_NOPROMPT, or -1 before any SQLDriverConnect.
	int completion = -1;
};

//! The fake driver: static trampolines into the current fake, behind one mutex, so tests may call it
//! from several threads, and fetch on one a statement executed on another. Only one fake is current,
//! and with none every entry point returns SQL_INVALID_HANDLE. A test that runs statement work
//! connects first: a statement call on a connection that is not open is not modelled.
class FakeOdbcDriver {
public:
	//! Becomes the current fake. Throws InternalException while another fake is current.
	FakeOdbcDriver();
	//! Stops being the current fake.
	~FakeOdbcDriver();
	FakeOdbcDriver(const FakeOdbcDriver &) = delete;
	FakeOdbcDriver &operator=(const FakeOdbcDriver &) = delete;

	//! A DriverSource: the current fake's driver (origin TEST_FAKE, version "fake", DuckDB's platform), the same
	//! object on every call while this fake lives, or the failure FailLoad set. Unlike a loaded driver it dies with
	//! the fake, so code that caches it must drop it before the fake is destroyed. Throws InternalException when no
	//! fake is current.
	static DriverLoadResult Source();
	//! How many times Source has been called since this fake was constructed, so a test can assert
	//! that the code under test loaded the driver once, or loaded it again after a failure, without
	//! a counting wrapper of its own. A failed call counts, and so does one a test's own fixture
	//! makes.
	size_t SourceCalls() const;
	//! Makes every later Source call fail with this kind and message, as a load failure whose cause
	//! persists does. To load again, replace this fake with a new one.
	void FailLoad(DriverErrorKind kind, std::string message);
	//! Sets the driver's refused_environment; empty until set. Call it before the code under test connects: that
	//! code reads the list through Source's driver, without the fake's mutex.
	void SetRefusedEnvironment(std::vector<std::string> names);
	//! Sets the driver's overridden_environment; empty until set. Call it before the code under test connects: that
	//! code reads the list through Source's driver, without the fake's mutex.
	void SetOverriddenEnvironment(std::vector<OverriddenVariable> variables);
	//! Sets the driver's trust_directory; empty until set, as on Linux and macOS. Call it before the code
	//! under test connects, as the lists above.
	void SetTrustDirectory(std::string directory);
	//! What SQLDriverConnect returns, with diags on the connection; SQL_SUCCESS with none until set.
	void SetConnectOutcome(SQLRETURN rc, std::vector<FakeDiag> diags);
	//! What SQLGetConnectAttr(SQL_ATTR_CONNECTION_DEAD) reports for every connection; false until
	//! set. Any other attribute fails.
	void SetConnectionDead(bool dead);
	//! Driver 2.3.0.0's limit on a message, which every fake starts with: its addError keeps at most
	//! 326 bytes of one, its prefix included, in a char[327] (public source v2.3.0, rsodbc.h
	//! MAX_ERR_MSG_LEN, rsutil.c addError).
	static constexpr size_t DRIVER_MESSAGE_LIMIT = 326;
	//! The most bytes of a record's message that SQLGetDiagRec and SQLGetDiagField report, from
	//! the start, on every read after this call; DRIVER_MESSAGE_LIMIT until set. A test of a
	//! caller's handling of a longer message sets a larger limit, which departs from that driver.
	void SetMessageLimit(size_t bytes);
	//! What SQLGetDiagField(SQL_DIAG_NUMBER) reports on every handle after this call, in place of the
	//! number of records the handle holds, which the driver counts (source v2.3.0, rserror.cpp
	//! RS_SQLGetDiagField). A test of a count no driver gives, such as a negative one, sets it.
	void SetRecordCount(SQLINTEGER count);
	//! SQLGetInfo's answer for info_type, written as an SQLUINTEGER. SQL_GETDATA_EXTENSIONS starts as
	//! driver 2.3.0.0's measured answer, 0xb: SQL_GD_ANY_COLUMN | SQL_GD_ANY_ORDER | SQL_GD_BOUND; any
	//! other info type never set fails.
	void SetInfo(SQLUSMALLINT info_type, SQLUINTEGER value);
	//! Makes the nth_call-th call to entry_point since this fake was constructed, counting from 1,
	//! return rc with diags on its handle instead of doing its work.
	//! Throws InternalException for a name not in
	//! REDSHIFT_ODBC_ENTRY_POINTS, a call already made, or a success code, whose effects would be missing.
	void FailCall(const char *entry_point, int nth_call, SQLRETURN rc, std::vector<FakeDiag> diags);
	//! Makes the nth_call-th call to entry_point, counted and checked as FailCall's, run and, if it
	//! succeeded, report SQL_SUCCESS_WITH_INFO with diags ahead of its own records, as a cut value
	//! does. Throws InternalException as FailCall does, and for SQLFreeHandle, SQLGetDiagRec,
	//! SQLGetDiagField or no records.
	void WarnCall(const char *entry_point, int nth_call, std::vector<FakeDiag> diags);
	//! Holds the nth_call-th call to entry_point, counted and checked as FailCall's, until the test
	//! releases gate, then runs it. It waits outside the fake's mutex, so other threads can still call
	//! the fake, for example to free the handle it is on.
	void BlockCall(const char *entry_point, int nth_call, std::shared_ptr<FakeGate> gate);
	//! Scripts what SQLPrepare of exactly this text gives a statement, replacing an earlier script:
	//! param_count, and the result sets SQLExecute and SQLMoreResults step through. "SELECT 1" starts
	//! scripted, and text never scripted fails to prepare. Throws InternalException for no result set,
	//! or a set whose rows, nulls, row_status, row_diags or row_count do not fit its columns and rows,
	//! or that use a row status the fake cannot answer.
	void AddResult(std::string sql, std::vector<FakeResultSet> results, SQLSMALLINT param_count = 0);
	//! The columns SQLPrepare of exactly this text describes on the current fake: its script's first
	//! result set's, empty for a set with none or text never scripted. Records no call, so a test can
	//! work out the shape a Describe would report without the fake seeing one. Throws
	//! InternalException when no fake is current.
	static std::vector<FakeColumn> ScriptedColumns(const std::string &sql);
	//! Every call so far, oldest first. Read it only once the calls under test have returned.
	const std::vector<FakeCall> &Calls() const;
	//! The connection string the latest SQLDriverConnect on a valid connection handle received, unless
	//! FailCall failed that call; empty until then. Calls() records every one.
	std::string LastConnectionString() const;
	//! The output buffer and completion mode that same SQLDriverConnect was given; {false, -1, -1}
	//! until then.
	FakeConnectOutput LastConnectOutput() const;
	//! The handles still allocated, oldest first, e.g. "environment, connection"; empty when none.
	//! An abandoned handle is left out, so this is what the leak check reads: the handles the test
	//! still has to free.
	std::string AllocatedHandles() const;
	//! Every call that reached an abandoned handle, oldest first, e.g.
	//! "SQLDisconnect on a connection"; empty when none. The destructor reports these, because such
	//! a call means the test, or the code under test, used a handle whose free it had scripted to
	//! fail.
	std::string MisusedAbandonedHandles() const;
	//! Keeps the destructor quiet about those calls, for the self-tests that make one on purpose.
	//! The ODBC client's own tests never need it: a call on an abandoned handle is a mistake there.
	void AllowAbandonedHandleMisuse();

private:
	//! Where a statement stands, after the driver's own status (rsodbc.h, RS_ALLOCATE_STMT to
	//! RS_CLOSE_STMT): SQLFetch, for one, posts 24000 before execution and HY010 once closed.
	enum class StatementStep : uint8_t { ALLOCATED, PREPARED, EXECUTED, CLOSED };
	//! How much of one cell on the current row SQLGetData has returned.
	struct ColumnRead {
		size_t returned = 0;   //!< bytes returned so far
		bool complete = false; //!< whether all of the cell has been returned
		int repeats = 0;       //!< first parts of a binary cell returned again as text
	};
	//! Where an executed statement stands in its results.
	struct Cursor {
		size_t result = 0; //!< the current result set
		//! Rows fetched from the current result set, through the last rowset; the statement is on
		//! row fetched - 1, the rowset's last, while that is one of the set's rows.
		size_t fetched = 0;
		//! One entry per column of the current result set, emptied by each SQLFetch.
		std::vector<ColumnRead> reads;
		//! No current result: SQLMoreResults passed the last set, or SQLExecute released the
		//! results. The statement stays executed, as the driver's SQLMoreResults leaves its status.
		bool drained = false;
	};
	//! A statement as SQLPrepare leaves it, from its text's AddResult script. Its results are empty
	//! only while step is ALLOCATED, so every answer that reads them checks the step first.
	struct Statement {
		StatementStep step = StatementStep::ALLOCATED;
		SQLSMALLINT param_count = 0;
		//! Whether the SQL text's leading word is UPDATE, INSERT or DELETE, which is what decides
		//! whether a statement that affected no rows reports SQL_NO_DATA.
		bool is_dml = false;
		std::vector<FakeResultSet> results;
		Cursor cursor;
	};
	//! What SQLBindParameter named: the buffers SQLExecute reads, as the driver reads them then, and
	//! the BufferLength the driver takes as the text's length unless the indicator decides it.
	struct BoundParameter {
		SQLPOINTER value = nullptr;
		SQLLEN buffer_length = 0;
		SQLLEN *indicator = nullptr;
	};
	//! What SQLBindCol named for one column: the C type, and the first array element's buffer, its
	//! BufferLength and its indicator, which a row-array SQLFetch writes from.
	struct BoundColumn {
		SQLSMALLINT c_type = 0;
		SQLPOINTER target = nullptr;
		SQLLEN capacity = 0;
		SQLLEN *indicator = nullptr;
	};
	//! A bound column and its number.
	using BoundEntry = std::pair<SQLUSMALLINT, BoundColumn>;
	//! The statement attributes a row-array SQLFetch reads, as SQLSetStmtAttr left them.
	struct RowArray {
		SQLULEN size = 1;
		SQLULEN *rows_fetched = nullptr;
		SQLUSMALLINT *row_status = nullptr;
		//! Bytes added to every bound buffer and indicator address, when set.
		SQLULEN *bind_offset = nullptr;
	};
	struct Handle {
		SQLSMALLINT type = 0;
		//! The environment or connection it was allocated on; SQL_NULL_HANDLE for an environment.
		SQLHANDLE parent = SQL_NULL_HANDLE;
		Statement statement; //!< used by a statement handle only
		//! A statement's bound parameters by number. ODBC binds them to the handle, so SQLPrepare
		//! keeps them and only SQLFreeStmt(SQL_RESET_PARAMS) drops them.
		std::map<SQLUSMALLINT, BoundParameter> parameters;
		//! A statement's bound columns by number, in the order they were first bound, which is the order
		//! SQLFetch reads them in, and its row array, also kept across SQLPrepare and SQLExecute.
		//! SQLFreeStmt(SQL_UNBIND) drops every column, and SQLBindCol with no buffer and no indicator
		//! drops one.
		std::vector<BoundEntry> columns;
		RowArray row_array;
		std::vector<FakeDiag> diags;
		//! For a connection: SQLDriverConnect opened it and SQLDisconnect has not closed it since.
		bool connected = false;
		//! A scripted SQLFreeHandle failure left this handle, or the connection it is on, allocated.
		//! The client's teardown then drops it rather than calling again, so the leak check leaves it
		//! out and any later call but a diagnostic read is a mistake.
		bool abandoned = false;
	};
	//! A scripted return code and the diagnostics it leaves on the handle.
	struct Outcome {
		SQLRETURN rc = SQL_SUCCESS;
		std::vector<FakeDiag> diags;
	};
	//! What a test scripted for the call being answered: a failure that replaces its work, a
	//! warning to add to whatever it answers, or neither. Each is null when nothing was scripted.
	struct Scripted {
		const Outcome *failure = nullptr;
		const std::vector<FakeDiag> *warning = nullptr;
	};
	//! One entry point's calls so far, and the failures, warnings and gates scripted for later
	//! calls by number.
	struct EntryPointScript {
		int calls_made = 0;
		std::map<int, Outcome> failures;
		std::map<int, std::vector<FakeDiag>> warnings;
		std::map<int, std::shared_ptr<FakeGate>> gates;
	};
	//! The trampolines, which hold the fake's behaviour.
	struct EntryPoints;

	//! The script that FailCall, WarnCall and BlockCall add to; the caller holds the fake's mutex.
	EntryPointScript &ScriptFor(const char *entry_point, int nth_call);

	LoadedDriver driver;
	DriverLoadResult load_result;
	//! Source's calls, which SourceCalls answers.
	size_t source_calls = 0;
	Outcome connect_outcome {SQL_SUCCESS, {}};
	bool connection_dead = false;
	size_t message_limit = DRIVER_MESSAGE_LIMIT;
	//! What SetRecordCount set; none means each handle's own count.
	std::optional<SQLINTEGER> record_count;
	std::map<SQLUSMALLINT, SQLUINTEGER> info;
	std::map<SQLHANDLE, Handle> handles;
	//! What AddResult scripted, by exact SQL text.
	std::map<std::string, Statement> scripted_statements;
	std::map<std::string, EntryPointScript> scripts;
	std::vector<FakeCall> calls;
	//! Each call that reached an abandoned handle, which MisusedAbandonedHandles answers.
	std::vector<std::string> abandoned_misuses;
	//! Set by AllowAbandonedHandleMisuse; the destructor then leaves abandoned_misuses unreported.
	bool allow_abandoned_misuse = false;
	//! The calls entry of the call being answered, so SQLExecute can log the parameters it read. A
	//! held call's entry need not be the last, because other calls arrive while it waits.
	size_t answering_call = 0;
	std::string last_connection_string;
	FakeConnectOutput last_connect_output;
};

} // namespace testing
} // namespace redshift_driver
} // namespace duckdb
