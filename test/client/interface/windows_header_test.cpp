// Windows only. A host file may include <windows.h> without NOMINMAX before the interface header,
// so min and max are macros; the header removes them and the other Win32 macros DuckDB removes,
// and DuckDB's Windows include and a DuckDB header after it still compile.
#include <windows.h>

#include "odbc_driver.hpp"

#if defined(min) || defined(max)
#error "odbc_driver.hpp leaves windows.h's min or max defined, which breaks DuckDB headers after it"
#endif

#include "duckdb/common/windows.hpp"
#include "duckdb/common/types.hpp"

#include "catch.hpp"

#include <algorithm>
#include <string>

TEST_CASE("After windows.h without NOMINMAX and the interface header, DuckDB headers compile", "[interface]") {
	CHECK(std::max(1, 2) == 2);
	CHECK(duckdb::TypeIdToString(duckdb::PhysicalType::BOOL) == "BOOL");
}
