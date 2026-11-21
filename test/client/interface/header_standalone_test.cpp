// The interface header is included first and alone, as the loader and the fake driver include it,
// so it must supply every ODBC type size itself. A build that passed SIZEOF_LONG_INT would hide a
// header that stopped defining it, so such a build is refused before the include.
#ifdef SIZEOF_LONG_INT
#error "the build defines SIZEOF_LONG_INT, so this test no longer shows the header compiles without it"
#endif
#include "odbc_driver.hpp"
// The header defines _WINSOCKAPI_ before windows.h, so windows.h brings in no winsock.h for
// winsock2.h to clash with.
#if defined(_WIN32)
#include <winsock2.h>
#endif

#include "catch.hpp"

#include <cstdint>

static_assert(SIZEOF_LONG_INT == sizeof(long), "the header's default must match this platform's long");

TEST_CASE("The interface header compiles alone and gives ODBC types the driver's sizes", "[interface]") {
	// A wrong SIZEOF_LONG_INT makes sqltypes.h pick a 64-bit SQLINTEGER, which no 64-bit driver uses.
	CHECK(sizeof(SQLINTEGER) == sizeof(int32_t));
	CHECK(sizeof(SQLBIGINT) == sizeof(int64_t));
	CHECK(sizeof(SQLLEN) == sizeof(void *));
	CHECK(sizeof(SQLULEN) == sizeof(void *));
	CHECK(sizeof(SQLHANDLE) == sizeof(void *));
}
