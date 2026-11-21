//===----------------------------------------------------------------------===//
//                         DuckDB - Redshift
//
// test/client/fake_driver/fake_handles.hpp
//
// Handles on the current fake driver, freed on scope exit, for the fake's own tests and for the
// tests that run the ODBC client code against it. Include catch.hpp before this header.
//===----------------------------------------------------------------------===//

#pragma once

#include "fake_odbc_driver.hpp"

namespace duckdb {
namespace redshift_driver {
namespace testing {

//! The current fake's function table. Fails the test unless Source succeeds.
inline const OdbcApi &Api() {
	const auto source = FakeOdbcDriver::Source();
	REQUIRE(source.ok());
	return source.driver->api;
}

//! One handle on the current fake, freed on scope exit.
struct HandleGuard {
	HandleGuard(SQLSMALLINT type_p, SQLHANDLE parent) : type(type_p), api(Api()) {
		REQUIRE(api.AllocHandle(type, parent, &value) == SQL_SUCCESS);
		REQUIRE(value != SQL_NULL_HANDLE);
	}
	~HandleGuard() {
		if (value != SQL_NULL_HANDLE) {
			api.FreeHandle(type, value);
		}
	}
	HandleGuard(const HandleGuard &) = delete;
	HandleGuard &operator=(const HandleGuard &) = delete;

	//! The handle's type, as AllocHandle took it.
	SQLSMALLINT type;
	//! Taken in the constructor: Api() fails the test after FailLoad and throws with no fake current,
	//! and either escaping ~HandleGuard would end the binary.
	const OdbcApi &api;
	//! The handle the fake allocated.
	SQLHANDLE value = SQL_NULL_HANDLE;
};

//! `text` as the SQLCHAR pointer an ODBC input string takes.
inline SQLCHAR *Text(const char *text) {
	return reinterpret_cast<SQLCHAR *>(const_cast<char *>(text));
}

//! Calls SQLDriverConnect on `connection`, which the fake answers as SetConnectOutcome scripted.
inline SQLRETURN Connect(const HandleGuard &connection, const char *connection_string) {
	return connection.api.DriverConnect(connection.value, nullptr, Text(connection_string), SQL_NTS, nullptr, 0,
	                                    nullptr, SQL_DRIVER_NOPROMPT);
}

//! An environment and a connection on the current fake. Reverse destruction frees the child first,
//! and a guard that is never built leaves the earlier one to free itself, so the fake's leak check
//! passes however the test ends.
struct OpenConnection {
	HandleGuard environment {SQL_HANDLE_ENV, SQL_NULL_HANDLE};
	HandleGuard connection {SQL_HANDLE_DBC, environment.value};
	const OdbcApi &api = Api();
};

//! Connects the connection it is given while the fixture is still being built, so that a statement
//! declared after it is allocated on a connection that is open.
struct Connected {
	explicit Connected(const HandleGuard &connection) {
		REQUIRE(Connect(connection, "Host={a}") == SQL_SUCCESS);
	}
};

//! A statement on a connection that connected first, for every case that prepares, executes or
//! fetches: the driver passes such a call to the connection's libpq handle, so the connection has
//! to be open.
struct OpenStatement : OpenConnection {
	Connected connected {connection};
	HandleGuard statement {SQL_HANDLE_STMT, connection.value};
};

} // namespace testing
} // namespace redshift_driver
} // namespace duckdb
