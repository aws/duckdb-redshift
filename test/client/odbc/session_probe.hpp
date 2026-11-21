//===----------------------------------------------------------------------===//
//                         DuckDB - Redshift
//
// test/client/odbc/session_probe.hpp
//
// A driver source that keeps the connection handle the client connected, so a [live] case can read
// the driver's own answers on the client's session: the descriptor fields the client classifies
// and each cell's text before the client checks or parses it, and run a statement of its own on that
// session, which the client's calls never see. Test-only.
//===----------------------------------------------------------------------===//

#pragma once

#include "../fake_driver/fake_odbc_driver.hpp"
#include "duckdb/common/numeric_utils.hpp"
#include "odbc_driver.hpp"

#include <cstdint>
#include <mutex>
#include <stdexcept>
#include <string>
#include <vector>

namespace redshift_test {

//! One result row as the driver sends it: each cell's bytes, and whether it is SQL NULL.
struct ProbedRow {
	std::vector<std::string> cells;
	std::vector<bool> nulls;
};

//! One kept connection for the process, because a DriverSource takes no arguments.
class SessionProbe {
public:
	//! Forgets the last connection and makes Source wrap the driver `inner` gives. Call it after
	//! SetDriverSourceForTesting, whose reset can free an environment through the last wrapped driver.
	static void Start(duckdb::redshift_driver::DriverSource inner) {
		std::lock_guard<std::mutex> guard(lock);
		source = inner;
		connection = SQL_NULL_HANDLE;
	}

	//! A DriverSource: inner's result, its driver copied with SQLDriverConnect keeping the handle and
	//! SQLFreeHandle forgetting it.
	static duckdb::redshift_driver::DriverLoadResult Source() {
		auto result = source();
		if (result.ok()) {
			std::lock_guard<std::mutex> guard(lock);
			api = result.driver->api;
			wrapped = *result.driver;
			wrapped.api.DriverConnect = &KeepConnection;
			wrapped.api.FreeHandle = &ForgetConnection;
			result.driver = &wrapped;
		}
		return result;
	}

	//! The columns `sql` describes once prepared, unexecuted, as the fake's describe script for it.
	//! Read while the client's connection is idle, on the thread that uses it.
	static std::vector<duckdb::redshift_driver::testing::FakeColumn> Columns(const std::string &sql) {
		Statement statement;
		statement.Prepare(sql);
		std::vector<duckdb::redshift_driver::testing::FakeColumn> columns;
		const auto count = statement.ColumnCount();
		for (SQLUSMALLINT number = 1; number <= count; number++) {
			columns.push_back(statement.Describe(number));
		}
		return columns;
	}

	//! How Rows runs its statement once prepared: SQLExecute, or SQLExecDirect of the same text, the
	//! simple protocol, as the client runs a statement with no parameters whose shape it keeps.
	enum class Run : uint8_t { PREPARED, DIRECT };

	//! Every row `sql` returns, run as `run` says, each cell read whole: VARBYTE as SQL_C_BINARY, as the
	//! client reads a BLOB, and every other type as SQL_C_CHAR, the server's text.
	static std::vector<ProbedRow> Rows(const std::string &sql, Run run = Run::PREPARED) {
		Statement statement;
		statement.Prepare(sql);
		std::vector<SQLSMALLINT> c_types;
		const auto count = statement.ColumnCount();
		for (SQLUSMALLINT number = 1; number <= count; number++) {
			c_types.push_back(statement.Describe(number).type_name == "VARBYTE" ? SQL_C_BINARY : SQL_C_CHAR);
		}
		if (run == Run::DIRECT) {
			auto text = sql;
			statement.Check(api.ExecDirect(statement.handle, reinterpret_cast<SQLCHAR *>(&text[0]), SQL_NTS),
			                "SQLExecDirect");
		} else {
			statement.Check(api.Execute(statement.handle), "SQLExecute");
		}
		std::vector<ProbedRow> rows;
		while (statement.Check(api.Fetch(statement.handle), "SQLFetch") != SQL_NO_DATA) {
			ProbedRow row;
			for (SQLUSMALLINT number = 1; number <= c_types.size(); number++) {
				bool null = false;
				row.cells.push_back(statement.Cell(number, c_types[number - 1], null));
				row.nulls.push_back(null);
			}
			rows.push_back(row);
		}
		return rows;
	}

	//! Runs `sql`, a statement that returns no rows, on a statement handle of its own, so the client's
	//! connection sees none of its calls. Run it while the client's connection is idle, on its thread.
	static void RunStatement(const std::string &sql) {
		Statement statement;
		auto text = sql;
		statement.Check(api.ExecDirect(statement.handle, reinterpret_cast<SQLCHAR *>(&text[0]), SQL_NTS),
		                "SQLExecDirect of " + sql);
	}

private:
	//! A statement handle on the kept connection, freed when it goes.
	struct Statement {
		Statement() {
			std::lock_guard<std::mutex> guard(lock);
			if (connection == SQL_NULL_HANDLE) {
				throw std::runtime_error("no connection was kept: open one through SessionProbe::Source first");
			}
			if (!SQL_SUCCEEDED(api.AllocHandle(SQL_HANDLE_STMT, connection, &handle))) {
				throw std::runtime_error("SQLAllocHandle failed on the kept connection");
			}
		}
		~Statement() {
			api.FreeHandle(SQL_HANDLE_STMT, handle);
		}
		Statement(const Statement &) = delete;
		Statement &operator=(const Statement &) = delete;

		void Prepare(const std::string &sql) {
			auto text = sql;
			Check(api.Prepare(handle, reinterpret_cast<SQLCHAR *>(&text[0]), SQL_NTS), "SQLPrepare of " + sql);
		}
		//! `rc`, unless the call failed: then throws with the statement's first record.
		SQLRETURN Check(SQLRETURN rc, const std::string &call) const {
			if (!SQL_SUCCEEDED(rc) && rc != SQL_NO_DATA) {
				SQLCHAR state[6] = {};
				SQLCHAR message[512] = {};
				SQLINTEGER native = 0;
				SQLSMALLINT length = 0;
				api.GetDiagRec(SQL_HANDLE_STMT, handle, 1, state, &native, message, sizeof(message), &length);
				throw std::runtime_error(call + " failed: " + reinterpret_cast<char *>(state) + " " +
				                         reinterpret_cast<char *>(message));
			}
			return rc;
		}
		SQLUSMALLINT ColumnCount() const {
			SQLSMALLINT count = 0;
			Check(api.NumResultCols(handle, &count), "SQLNumResultCols");
			return duckdb::NumericCast<SQLUSMALLINT>(count);
		}
		duckdb::redshift_driver::testing::FakeColumn Describe(SQLUSMALLINT number) const {
			duckdb::redshift_driver::testing::FakeColumn column;
			SQLCHAR text[256] = {};
			SQLSMALLINT length = 0;
			Check(api.DescribeCol(handle, number, text, sizeof(text), &length, &column.sql_type, &column.column_size,
			                      &column.decimal_digits, &column.nullable),
			      "SQLDescribeCol");
			column.name = reinterpret_cast<char *>(text);
			Check(api.ColAttribute(handle, number, SQL_DESC_TYPE_NAME, text, sizeof(text), &length, nullptr),
			      "SQLColAttribute");
			column.type_name = reinterpret_cast<char *>(text);
			return column;
		}
		//! Column `number`'s cell in the fetched row, read in parts until SQLGetData has no more. The
		//! driver returns a long binary cell read as text whole again on every call (rsutil.c
		//! copyBinaryDataBigLen), so a part that leaves as much to come fails rather than loops.
		std::string Cell(SQLUSMALLINT number, SQLSMALLINT c_type, bool &null) const {
			std::string cell;
			char part[4096];
			const SQLLEN room = c_type == SQL_C_CHAR ? sizeof(part) - 1 : sizeof(part);
			SQLLEN previous = 0;
			for (;;) {
				SQLLEN indicator = 0;
				Check(api.GetData(handle, number, c_type, part, sizeof(part), &indicator),
				      "SQLGetData of column " + std::to_string(number));
				null = indicator == SQL_NULL_DATA;
				if (null) {
					return cell;
				}
				const bool whole = indicator != SQL_NO_TOTAL && indicator <= room;
				if (!whole && indicator != SQL_NO_TOTAL && indicator == previous) {
					throw std::runtime_error("SQLGetData of column " + std::to_string(number) +
					                         " returned the same part again; read the column as SQL_C_BINARY");
				}
				previous = indicator;
				cell.append(part, duckdb::NumericCast<size_t>(whole ? indicator : room));
				if (whole) {
					return cell;
				}
			}
		}

		SQLHANDLE handle = SQL_NULL_HANDLE;
	};

	// Its parameters are deduced from the member it is assigned to, as CallTrace's are. A failed
	// connect's handle is freed before any read, which forgets it.
	template <class... Args>
	static SQLRETURN KeepConnection(SQLHDBC handle, Args... args) {
		{
			std::lock_guard<std::mutex> guard(lock);
			connection = handle;
		}
		return api.DriverConnect(handle, args...);
	}

	static SQLRETURN ForgetConnection(SQLSMALLINT type, SQLHANDLE handle) {
		{
			std::lock_guard<std::mutex> guard(lock);
			if (type == SQL_HANDLE_DBC && handle == connection) {
				connection = SQL_NULL_HANDLE;
			}
		}
		return api.FreeHandle(type, handle);
	}

	static inline std::mutex lock;
	static inline duckdb::redshift_driver::DriverSource source = nullptr;
	static inline duckdb::redshift_driver::OdbcApi api;
	static inline duckdb::redshift_driver::LoadedDriver wrapped;
	static inline SQLHANDLE connection = SQL_NULL_HANDLE;
};

} // namespace redshift_test
