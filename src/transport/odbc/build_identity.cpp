#include "odbc/build_identity.hpp"

#include "duckdb/main/database.hpp"

namespace duckdb {

BuildIdentity ThisBuild() {
	// DuckDB's extension_build_tools.cmake defines the version with add_definitions as it adds each
	// extension; read as RedshiftExtension::Version does.
#ifdef EXT_VERSION_REDSHIFT
	const char *extension_version = EXT_VERSION_REDSHIFT;
#else
	const char *extension_version = "";
#endif
	return BuildIdentity {extension_version, DuckDB::LibraryVersion(), DuckDB::Platform()};
}

} // namespace duckdb
