//===----------------------------------------------------------------------===//
//                         DuckDB - Redshift
//
// test/client/odbc/call_trace.hpp
//
// A driver source that records every call the driver it wraps receives, with what each returned,
// so a trace recorded on the real driver can be compared with the fake's for the same SQL.
// Test-only, like the source hook it is installed through.
//===----------------------------------------------------------------------===//

#pragma once

#include "duckdb/common/numeric_utils.hpp"
#include "odbc_driver.hpp"

#include <algorithm>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

namespace redshift_test {

//! One trace for the process, because a DriverSource takes no arguments. Each line is the entry
//! point and its SQLRETURN, as "SQLFetch 100"; GetConnectAttr's and GetDiagRec's add what they read,
//! and a column's description what the driver wrote out, which the fake's scripts must match
//! (TracedDescribeColValues, TracedColAttributeValues).
class CallTrace {
public:
	//! Clears the trace and makes Source wrap the driver `inner` gives. Call it after
	//! SetDriverSourceForTesting, whose reset can free an environment through the last traced driver.
	static void Start(duckdb::redshift_driver::DriverSource inner) {
		std::lock_guard<std::mutex> guard(lock);
		source = inner;
		calls.clear();
		threads.clear();
	}

	//! A DriverSource: inner's result, its driver copied with every entry point recording.
	static duckdb::redshift_driver::DriverLoadResult Source() {
		auto result = source();
		if (result.ok()) {
			std::lock_guard<std::mutex> guard(lock);
			target = result.driver->api;
			traced = *result.driver;
#define REDSHIFT_TRACE_MEMBER(name) traced.api.name = &Traced##name;
			REDSHIFT_ODBC_ENTRY_POINTS(REDSHIFT_TRACE_MEMBER)
#undef REDSHIFT_TRACE_MEMBER
			traced.api.GetConnectAttr = &TracedConnectionAttribute;
			traced.api.GetDiagRec = &TracedDiagnosticRecord;
			traced.api.DescribeCol = &TracedDescribeColValues;
			traced.api.ColAttribute = &TracedColAttributeValues;
			result.driver = &traced;
		}
		return result;
	}

	//! The calls so far, oldest first.
	static std::vector<std::string> Calls() {
		std::lock_guard<std::mutex> guard(lock);
		return calls;
	}
	//! The thread that made each of those calls, in the same order.
	static std::vector<std::thread::id> Threads() {
		std::lock_guard<std::mutex> guard(lock);
		return threads;
	}

private:
	static void Record(const char *entry_point, SQLRETURN rc, const std::string &answer = std::string()) {
		std::lock_guard<std::mutex> guard(lock);
		calls.push_back(std::string(entry_point) + " " + std::to_string(rc) + answer);
		threads.push_back(std::this_thread::get_id());
	}

	//! GetConnectAttr's line also carries the connection-dead answer, as "SQLGetConnectAttr 0 dead=1",
	//! since the client's lost-connection rule turns on it.
	static SQLRETURN TracedConnectionAttribute(SQLHDBC connection, SQLINTEGER attribute, SQLPOINTER value,
	                                           SQLINTEGER length, SQLINTEGER *written) {
		const SQLRETURN rc = target.GetConnectAttr(connection, attribute, value, length, written);
		const bool answered =
		    attribute == SQL_ATTR_CONNECTION_DEAD && (rc == SQL_SUCCESS || rc == SQL_SUCCESS_WITH_INFO);
		Record("SQLGetConnectAttr", rc,
		       answered ? " dead=" + std::to_string(*static_cast<SQLUINTEGER *>(value)) : std::string());
		return rc;
	}

	//! GetDiagRec's line also carries the record read, as "SQLGetDiagRec 0 22012 <message>", so a
	//! live case can check the fake's recorded records against the driver's.
	static SQLRETURN TracedDiagnosticRecord(SQLSMALLINT type, SQLHANDLE handle, SQLSMALLINT number, SQLCHAR *state,
	                                        SQLINTEGER *native, SQLCHAR *message, SQLSMALLINT length,
	                                        SQLSMALLINT *written) {
		const SQLRETURN rc = target.GetDiagRec(type, handle, number, state, native, message, length, written);
		const bool read = (rc == SQL_SUCCESS || rc == SQL_SUCCESS_WITH_INFO) && state && message;
		Record("SQLGetDiagRec", rc,
		       read ? " " + std::string(reinterpret_cast<const char *>(state)) + " " +
		                  std::string(reinterpret_cast<const char *>(message))
		            : std::string());
		return rc;
	}

	//! A number the driver wrote through `out`, or "-" where the caller passed none.
	template <class T>
	static std::string OutValue(const T *out) {
		return out ? std::to_string(*out) : "-";
	}

	//! The text the driver wrote to `text`, of `capacity` bytes: the length it reported, cut to the
	//! bytes before the NUL it ends a cut text with, or "-" where the caller passed no buffer.
	template <class LENGTH>
	static std::string OutText(const void *text, LENGTH capacity, const LENGTH *length) {
		if (!text) {
			return "-";
		}
		const LENGTH reported = length ? *length : 0;
		const auto kept = std::max<LENGTH>(0, std::min<LENGTH>(reported, capacity - 1));
		return std::string(static_cast<const char *>(text), duckdb::NumericCast<size_t>(kept));
	}

	//! Whether a call that returned `rc` wrote its out-values.
	static bool Wrote(SQLRETURN rc) {
		return rc == SQL_SUCCESS || rc == SQL_SUCCESS_WITH_INFO;
	}

	//! SQLDescribeCol, recorded with the name, type, size, decimal digits and nullable flag it wrote.
	static SQLRETURN TracedDescribeColValues(SQLHSTMT statement, SQLUSMALLINT number, SQLCHAR *name,
	                                         SQLSMALLINT capacity, SQLSMALLINT *name_length, SQLSMALLINT *type,
	                                         SQLULEN *size, SQLSMALLINT *digits, SQLSMALLINT *nullable) {
		const SQLRETURN rc =
		    target.DescribeCol(statement, number, name, capacity, name_length, type, size, digits, nullable);
		if (!Wrote(rc)) {
			Record("SQLDescribeCol", rc);
			return rc;
		}
		Record("SQLDescribeCol", rc,
		       " name " + OutText(name, capacity, name_length) + " type " + OutValue(type) + " size " + OutValue(size) +
		           " digits " + OutValue(digits) + " nullable " + OutValue(nullable));
		return rc;
	}

	//! SQLColAttribute, recorded with the field asked for and the text or number it wrote.
	static SQLRETURN TracedColAttributeValues(SQLHSTMT statement, SQLUSMALLINT number, SQLUSMALLINT field,
	                                          SQLPOINTER text, SQLSMALLINT capacity, SQLSMALLINT *text_length,
	                                          SQLLEN *numeric) {
		const SQLRETURN rc = target.ColAttribute(statement, number, field, text, capacity, text_length, numeric);
		if (!Wrote(rc)) {
			Record("SQLColAttribute", rc);
			return rc;
		}
		const auto value = text ? OutText(text, capacity, text_length) : OutValue(numeric);
		Record("SQLColAttribute", rc, " field " + std::to_string(field) + " " + value);
		return rc;
	}

	// Each forwards to the wrapped driver; its parameters are deduced from the member it is assigned to.
#define REDSHIFT_TRACE_FORWARD(name)                                                                                   \
	template <class... Args>                                                                                           \
	static SQLRETURN Traced##name(Args... args) {                                                                      \
		const SQLRETURN rc = target.name(args...);                                                                     \
		Record("SQL" #name, rc);                                                                                       \
		return rc;                                                                                                     \
	}
	REDSHIFT_ODBC_ENTRY_POINTS(REDSHIFT_TRACE_FORWARD)
#undef REDSHIFT_TRACE_FORWARD

	static inline std::mutex lock;
	static inline duckdb::redshift_driver::DriverSource source = nullptr;
	static inline duckdb::redshift_driver::OdbcApi target;
	static inline duckdb::redshift_driver::LoadedDriver traced;
	static inline std::vector<std::string> calls;
	static inline std::vector<std::thread::id> threads;
};

} // namespace redshift_test
