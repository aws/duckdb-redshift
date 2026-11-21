// A translation unit that defines UNICODE before the interface header, as a Windows host file may.
// sqlucode.h would then map SQLDriverConnect to SQLDriverConnectW, so the table would hold the
// wide entry point while the ODBC client passes narrow text; the header defines SQL_NOUNICODEMAP against it.
#ifndef UNICODE
#define UNICODE
#endif
#include "odbc_driver.hpp"

#include "catch.hpp"

#include <type_traits>

namespace {

using NarrowDriverConnect = SQLRETURN(SQL_API *)(SQLHDBC, SQLHWND, SQLCHAR *, SQLSMALLINT, SQLCHAR *, SQLSMALLINT,
                                                 SQLSMALLINT *, SQLUSMALLINT);

} // namespace

TEST_CASE("With UNICODE defined, the table still holds the narrow SQLDriverConnect", "[interface]") {
	CHECK(std::is_same<decltype(duckdb::redshift_driver::OdbcApi::DriverConnect), NarrowDriverConnect>::value);
}
