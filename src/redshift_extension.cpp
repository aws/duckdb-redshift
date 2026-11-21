#define DUCKDB_EXTENSION_MAIN

#include "redshift_extension.hpp"
#include "redshift_attach.hpp"
#include "redshift_query.hpp"
#include "storage/redshift_client_state.hpp"
#include "duckdb.hpp"
#include "duckdb/main/extension/extension_loader.hpp"

namespace duckdb {

//! Registration is the only thing this file does. The SQL surface is registered here;
//! the client below the interface never appears, by design.
static void LoadInternal(ExtensionLoader &loader) {
	CreateConnectionMapAndInstallClientStates(loader);
	RegisterRedshiftAttach(loader);
	RegisterRedshiftQuery(loader);
}

void RedshiftExtension::Load(ExtensionLoader &loader) {
	LoadInternal(loader);
}

std::string RedshiftExtension::Name() {
	return "redshift";
}

std::string RedshiftExtension::Version() const {
#ifdef EXT_VERSION_REDSHIFT
	return EXT_VERSION_REDSHIFT;
#else
	return "";
#endif
}

} // namespace duckdb

extern "C" {

DUCKDB_CPP_EXTENSION_ENTRY(redshift, loader) {
	duckdb::LoadInternal(loader);
}
}
