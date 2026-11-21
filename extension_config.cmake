# This file is included by DuckDB's build system. It specifies which extension to load

# Extension from this repo
duckdb_extension_load(redshift
    SOURCE_DIR ${CMAKE_CURRENT_LIST_DIR}
)
# Since DuckDB v2.0 a loaded extension is only built; this links it into DuckDB's targets as well,
# as v1.5 did, so the shell, the SQL test runner and the C++ test suites have it built in.
duckdb_extension_statically_link(redshift)

# Any extra extensions that should be built
# e.g.: duckdb_extension_load(json)