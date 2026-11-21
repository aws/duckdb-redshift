// The closed-port smoke, shared by the driver suite (embedded_test_helpers.hpp) and the
// host probe (probe/driver_probe.cpp), which has no Catch2: one definition of the driver's own answer.
#pragma once

#include "odbc_driver.hpp"

#include <string>

#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>

namespace redshift_driver_test {

// A local TCP port that nothing listens on, or -1.
inline int ClosedLocalPort() {
	const int fd = ::socket(AF_INET, SOCK_STREAM, 0);
	sockaddr_in address {};
	address.sin_family = AF_INET;
	address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
	socklen_t length = sizeof(address);
	const bool bound = fd >= 0 && ::bind(fd, reinterpret_cast<sockaddr *>(&address), sizeof(address)) == 0 &&
	                   ::getsockname(fd, reinterpret_cast<sockaddr *>(&address), &length) == 0;
	if (fd >= 0) {
		::close(fd);
	}
	return bound ? ntohs(address.sin_port) : -1;
}

// The smoke on the environment handle `env` of `api`, which may be the embedded driver's or a driver
// manager's: connects to 127.0.0.1:`port`, where nothing listens, and is true when the driver answered
// with its own `HY000 [Redshift][ODBC Driver]` diagnostic and `Connection refused`. Frees both handles.
// `answer` gets the SQLSTATE and the message; `attributes` starts the connection string, for a driver
// manager's `Driver={...};`.
template <class API>
bool SmokeOn(const API &api, SQLHENV env, int port, std::string &answer, const std::string &attributes = "") {
	SQLHDBC dbc = nullptr;
	if (api.SetEnvAttr(env, SQL_ATTR_ODBC_VERSION, reinterpret_cast<SQLPOINTER>(SQL_OV_ODBC3), 0) != SQL_SUCCESS ||
	    api.AllocHandle(SQL_HANDLE_DBC, env, &dbc) != SQL_SUCCESS) {
		answer = "handle allocation failed";
		api.FreeHandle(SQL_HANDLE_ENV, env);
		return false;
	}
	std::string connect = attributes + "Host=127.0.0.1;Port=" + std::to_string(port) +
	                      ";Database=dev;UID=nobody;PWD=not-a-password;SSLMode=require;LoginTimeout=5";
	SQLCHAR out[1024];
	SQLSMALLINT out_length = 0;
	const auto rc =
	    api.DriverConnect(dbc, nullptr, reinterpret_cast<SQLCHAR *>(&connect[0]),
	                      static_cast<SQLSMALLINT>(connect.size()), out, sizeof(out), &out_length, SQL_DRIVER_NOPROMPT);
	SQLCHAR state[6] = {0};
	SQLCHAR message[1024] = {0};
	SQLINTEGER native = 0;
	SQLSMALLINT message_length = 0;
	const bool diagnosed = api.GetDiagRec(SQL_HANDLE_DBC, dbc, 1, state, &native, message, sizeof(message),
	                                      &message_length) == SQL_SUCCESS;
	const std::string sqlstate(reinterpret_cast<char *>(state));
	const std::string text(reinterpret_cast<char *>(message));
	answer = sqlstate + " " + text;
	api.FreeHandle(SQL_HANDLE_DBC, dbc);
	api.FreeHandle(SQL_HANDLE_ENV, env);
	return rc == SQL_ERROR && diagnosed && sqlstate == "HY000" &&
	       text.find("[Redshift][ODBC Driver]") != std::string::npos &&
	       text.find("Connection refused") != std::string::npos;
}

// The smoke through the embedded driver, its environment handle allocated as the ODBC client allocates it.
// `attributes`, if any, go before the smoke's own, as SmokeOn takes them.
inline bool ClosedPortSmoke(const duckdb::redshift_driver::LoadedDriver &driver, int port, std::string &answer,
                            const std::string &attributes = "") {
	SQLHENV env = nullptr;
	if (duckdb::redshift_driver::AllocateEnvironmentHandle(driver, &env) != SQL_SUCCESS) {
		answer = "environment allocation failed";
		return false;
	}
	return SmokeOn(driver.api, env, port, answer, attributes);
}

} // namespace redshift_driver_test
