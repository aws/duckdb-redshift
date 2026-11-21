//===----------------------------------------------------------------------===//
//                         DuckDB - Redshift
//
// test/client/odbc/transfer_probe.hpp
//
// A driver source for measuring the read path: it replaces the
// value of transfer settings the client sends, such as Compression=off, caps the row array the
// client asks for, times the driver's fetch and prepare calls, and records what the driver
// describes and how it answers SQLSetPos. Test-only, like the source hook it is installed through,
// so no shipped build can send another transfer setting.
//===----------------------------------------------------------------------===//

#pragma once

#include "odbc_driver.hpp"

#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstring>
#include <mutex>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

namespace redshift_test {

//! What a probed driver changes. No settings and no cap leave every choice the client's.
struct TransferChanges {
	//! Keyword and value pairs. Each keyword is one the client sends, whose value is replaced.
	std::vector<std::pair<std::string, std::string>> settings;
	//! The most rows one fetch takes, or 0 for the client's own row array.
	SQLULEN row_array_cap = 0;
	//! After the first fetch that returns SQL_SUCCESS, ask SQLSetPos(SQL_POSITION) for row 1 and keep
	//! the answer (a row array above 1 reads a long column only through it). A
	//! fetch with records is left alone, as SQLSetPos clears them before the client reads them.
	bool probe_position = false;
};

//! One column as SQLDescribeCol reported it.
struct DescribedColumn {
	std::string name;
	SQLSMALLINT data_type = 0;
	SQLULEN column_size = 0;
};

//! The fetch calls a probed driver received since Start, and the time spent inside them, and how
//! long each SQLPrepare took, oldest first.
struct FetchTiming {
	uint64_t fetch_calls = 0;
	uint64_t get_data_calls = 0;
	std::chrono::nanoseconds driver_time {0};
	std::vector<std::chrono::nanoseconds> prepares;
};

//! One probe for the process, because a DriverSource takes no arguments.
class TransferProbe {
public:
	//! Makes Source wrap the driver `inner` gives, applying `changes`, and clears the timing. Call it
	//! after SetDriverSourceForTesting, whose reset can free an environment through the last driver.
	static void Start(duckdb::redshift_driver::DriverSource inner, TransferChanges changes) {
		std::lock_guard<std::mutex> guard(lock);
		source = inner;
		applied = std::move(changes);
		fetch_calls = 0;
		get_data_calls = 0;
		driver_nanoseconds = 0;
		described.clear();
		position_answer.clear();
		prepares.clear();
	}

	//! A DriverSource: inner's result, its driver copied with the six probed entry points.
	static duckdb::redshift_driver::DriverLoadResult Source() {
		auto result = source();
		if (result.ok()) {
			std::lock_guard<std::mutex> guard(lock);
			target = result.driver->api;
			probed = *result.driver;
			probed.api.DriverConnect = &ProbedDriverConnect;
			probed.api.DescribeCol = &ProbedDescribeCol;
			probed.api.Prepare = &ProbedPrepare;
			probed.api.SetStmtAttr = &ProbedSetStmtAttr;
			probed.api.Fetch = &ProbedFetch;
			probed.api.GetData = &ProbedGetData;
			result.driver = &probed;
		}
		return result;
	}

	//! The counts and time since Start.
	static FetchTiming Timing() {
		FetchTiming timing;
		timing.fetch_calls = fetch_calls;
		timing.get_data_calls = get_data_calls;
		timing.driver_time = std::chrono::nanoseconds(driver_nanoseconds.load());
		std::lock_guard<std::mutex> guard(lock);
		timing.prepares = prepares;
		return timing;
	}

	//! Every column SQLDescribeCol described since Start, oldest first.
	static std::vector<DescribedColumn> Described() {
		std::lock_guard<std::mutex> guard(lock);
		return described;
	}

	//! SQLSetPos's return code and first SQLSTATE, as "-1 HY106", once probe_position has asked;
	//! empty before.
	static std::string PositionAnswer() {
		std::lock_guard<std::mutex> guard(lock);
		return position_answer;
	}

	//! `connection_string` with each of `changes`' settings given its value. Throws
	//! std::invalid_argument for a keyword the string does not set, so a misspelt one fails the run
	//! instead of measuring the client's own setting.
	static std::string Apply(const std::string &connection_string, const TransferChanges &changes) {
		auto text = connection_string;
		for (const auto &setting : changes.settings) {
			// The encoder writes `keyword={value};` and no value holds `}`, so a setting starts the
			// string or follows `};`, and its value ends at the next `}`.
			const auto key = setting.first + "={";
			auto start = text.rfind(key, 0) == 0 ? 0 : text.find("};" + key);
			if (start == std::string::npos) {
				throw std::invalid_argument("the client sends no " + setting.first + " setting to replace");
			}
			start = start == 0 ? key.size() : start + 2 + key.size();
			text.replace(start, text.find('}', start) - start, setting.second);
		}
		return text;
	}

private:
	//! Time spent inside the driver, added to the total when the call returns.
	class DriverTimer {
	public:
		DriverTimer() : started(std::chrono::steady_clock::now()) {
		}
		~DriverTimer() {
			const auto spent = std::chrono::steady_clock::now() - started;
			driver_nanoseconds += uint64_t(std::chrono::duration_cast<std::chrono::nanoseconds>(spent).count());
		}

	private:
		std::chrono::steady_clock::time_point started;
	};

	static SQLRETURN SQL_API ProbedDriverConnect(SQLHDBC connection, SQLHWND window, SQLCHAR *in, SQLSMALLINT in_length,
	                                             SQLCHAR *out, SQLSMALLINT out_capacity, SQLSMALLINT *out_length,
	                                             SQLUSMALLINT completion) {
		const auto given = in_length == SQL_NTS ? std::string(reinterpret_cast<const char *>(in))
		                                        : std::string(reinterpret_cast<const char *>(in), size_t(in_length));
		auto changed = Apply(given, applied);
		return target.DriverConnect(connection, window, reinterpret_cast<SQLCHAR *>(&changed[0]), SQL_NTS, out,
		                            out_capacity, out_length, completion);
	}

	static SQLRETURN SQL_API ProbedSetStmtAttr(SQLHSTMT statement, SQLINTEGER attribute, SQLPOINTER value,
	                                           SQLINTEGER length) {
		// ODBC passes the row-array size in the pointer itself.
		const auto rows = SQLULEN(reinterpret_cast<uintptr_t>(value));
		if (attribute == SQL_ATTR_ROW_ARRAY_SIZE && applied.row_array_cap != 0 && rows > applied.row_array_cap) {
			value = reinterpret_cast<SQLPOINTER>(uintptr_t(applied.row_array_cap));
		}
		return target.SetStmtAttr(statement, attribute, value, length);
	}

	static SQLRETURN SQL_API ProbedDescribeCol(SQLHSTMT statement, SQLUSMALLINT column, SQLCHAR *name,
	                                           SQLSMALLINT name_capacity, SQLSMALLINT *name_length,
	                                           SQLSMALLINT *data_type, SQLULEN *column_size, SQLSMALLINT *digits,
	                                           SQLSMALLINT *nullable) {
		const auto rc = target.DescribeCol(statement, column, name, name_capacity, name_length, data_type, column_size,
		                                   digits, nullable);
		if ((rc == SQL_SUCCESS || rc == SQL_SUCCESS_WITH_INFO) && name && data_type && column_size) {
			std::lock_guard<std::mutex> guard(lock);
			described.push_back({reinterpret_cast<const char *>(name), *data_type, *column_size});
		}
		return rc;
	}

	static SQLRETURN SQL_API ProbedPrepare(SQLHSTMT statement, SQLCHAR *text, SQLINTEGER length) {
		const auto started = std::chrono::steady_clock::now();
		const auto rc = target.Prepare(statement, text, length);
		std::lock_guard<std::mutex> guard(lock);
		prepares.push_back(std::chrono::steady_clock::now() - started);
		return rc;
	}

	static SQLRETURN SQL_API ProbedFetch(SQLHSTMT statement) {
		fetch_calls++;
		SQLRETURN rc;
		{
			DriverTimer timer;
			rc = target.Fetch(statement);
		}
		if (applied.probe_position && rc == SQL_SUCCESS) {
			AskPosition(statement);
		}
		return rc;
	}

	//! Asks SQLSetPos(SQL_POSITION) for row 1 once, keeping its return code and first SQLSTATE.
	static void AskPosition(SQLHSTMT statement) {
		std::lock_guard<std::mutex> guard(lock);
		if (!position_answer.empty()) {
			return;
		}
		const auto rc = target.SetPos(statement, 1, SQL_POSITION, SQL_LOCK_NO_CHANGE);
		SQLCHAR state[6] = {};
		SQLINTEGER native = 0;
		SQLSMALLINT length = 0;
		target.GetDiagRec(SQL_HANDLE_STMT, statement, 1, state, &native, nullptr, 0, &length);
		position_answer = std::to_string(rc) + " " + reinterpret_cast<const char *>(state);
	}

	static SQLRETURN SQL_API ProbedGetData(SQLHSTMT statement, SQLUSMALLINT column, SQLSMALLINT c_type,
	                                       SQLPOINTER value, SQLLEN capacity, SQLLEN *indicator) {
		get_data_calls++;
		DriverTimer timer;
		return target.GetData(statement, column, c_type, value, capacity, indicator);
	}

	static inline std::mutex lock;
	static inline duckdb::redshift_driver::DriverSource source = nullptr;
	static inline duckdb::redshift_driver::OdbcApi target;
	static inline duckdb::redshift_driver::LoadedDriver probed;
	static inline TransferChanges applied;
	static inline std::vector<DescribedColumn> described;
	static inline std::string position_answer;
	static inline std::vector<std::chrono::nanoseconds> prepares;
	static inline std::atomic<uint64_t> fetch_calls {0};
	static inline std::atomic<uint64_t> get_data_calls {0};
	static inline std::atomic<uint64_t> driver_nanoseconds {0};
};

} // namespace redshift_test
