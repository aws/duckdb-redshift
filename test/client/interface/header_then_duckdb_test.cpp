// The ODBC client's files include the interface header and DuckDB headers in one translation unit. sqltypes.h
// defines BOOL, which would rewrite PhysicalType::BOOL in any DuckDB header read after it, so the
// header must leave it undefined. A DuckDB header follows here to show that order compiles. On
// Windows, DuckDB's own Windows include comes first, as in a DuckDB source file, so the header's
// stand-ins meet the windows.h DuckDB includes; it is empty elsewhere.
#include "duckdb/common/windows.hpp"
#include "odbc_driver.hpp"

#ifdef BOOL
#error "odbc_driver.hpp leaves BOOL defined, which breaks DuckDB headers included after it"
#endif

#include "duckdb/common/types.hpp"

#include "catch.hpp"

TEST_CASE("A DuckDB header included after the interface header keeps PhysicalType::BOOL", "[interface]") {
	CHECK(duckdb::TypeIdToString(duckdb::PhysicalType::BOOL) == "BOOL");
}
