//===----------------------------------------------------------------------===//
// The catalog of a Redshift attached database.
//
// The attached database is an attach handle for redshift_query, not a browsable catalog: it lists
// no schemas, and a qualified reference such as `<handle>.<schema>.<table>` fails with a message
// that directs the user to redshift_query.
//===----------------------------------------------------------------------===//

#include "storage/redshift_catalog.hpp"

#include "duckdb/common/error_data.hpp"
#include "duckdb/common/exception.hpp"
#include "duckdb/common/string_util.hpp"
#include "duckdb/common/sql_identifier.hpp"
#include "duckdb/main/attached_database.hpp"
#include "duckdb/main/client_context.hpp"
#include "duckdb/main/database_manager.hpp"
#include "duckdb/parser/tableref.hpp"
#include "duckdb/storage/database_size.hpp"
#include "redshift_attach.hpp"
#include "redshift_query.hpp"
#include "storage/redshift_client_state.hpp"
#include "storage/redshift_connection_ownership.hpp"

namespace duckdb {

RedshiftCatalog::RedshiftCatalog(AttachedDatabase &db, RedshiftConfig config,
                                 shared_ptr<RedshiftConnectionMap> connection_map_p)
    : Catalog(db), config(std::move(config)), connection_map(std::move(connection_map_p)),
      handle_id(connection_map->RegisterHandle(*this)) {
}

RedshiftCatalog::~RedshiftCatalog() {
	connection_map->CloseHandle(handle_id);
}

bool RedshiftCatalog::IsRetired() const {
	return connection_map->IsRetired(handle_id);
}

unordered_set<idx_t> RedshiftCatalog::ListedHandleIds(ClientContext &context) {
	// Not through each database's catalog: RedshiftConnectionMap::ListedHandleIds says why.
	return RedshiftClientState::Get(context).ConnectionMap()->ListedHandleIds(
	    DatabaseManager::Get(context).GetDatabases());
}

bool RedshiftCatalog::PinConnection(connection_t connection_id) {
	return connection_map->Pin(connection_id, handle_id);
}

void RedshiftCatalog::UnpinConnection(connection_t connection_id) {
	connection_map->Unpin(connection_id, handle_id);
}

bool RedshiftCatalog::BeginConnectionUse(connection_t connection_id, RedshiftConnectionUse use) {
	return connection_map->BeginUse(connection_id, handle_id, use);
}

void RedshiftCatalog::EndConnectionUse(connection_t connection_id) {
	connection_map->EndUse(connection_id, handle_id);
}

void RedshiftCatalog::OnDetach(ClientContext &context) {
	// Now, not when DuckDB destroys this catalog: something else, such as another DuckDB connection's
	// transaction that looked the name up, can hold the catalog after DETACH or ATTACH OR REPLACE,
	// and the handle's idle connections must close at the removal all the same. With the name it had,
	// for the next statement each other DuckDB connection runs on it, and the DuckDB connection
	// removing it, which ended its own session.
	connection_map->RetireHandle(handle_id, GetName(), context.GetConnectionId());
}

void RedshiftCatalog::OpenAttachingConnection(ClientContext &context) {
	auto connection_id = RedshiftClientState::Get(context).ConnectionId();
	// Pinned as every other caller of GetConnection is. Nothing can have retired the handle: DuckDB
	// lists it, and the rollback hook knows it, only once ATTACH has returned it.
	if (!connection_map->Pin(connection_id, handle_id)) {
		throw InternalException("Redshift extension: the handle being attached has been retired");
	}
	try {
		connection_map->GetConnection(connection_id, handle_id, config, context);
	} catch (...) {
		connection_map->Unpin(connection_id, handle_id);
		throw;
	}
	// The connection stays, idle, as the pair's: the ATTACH's DuckDB connection's first statement
	// on the handle reuses it.
	connection_map->Unpin(connection_id, handle_id);
}

optional_ptr<RedshiftConnection> RedshiftCatalog::GetRedshiftConnection(ClientContext &context,
                                                                        optional_ptr<bool> opened) {
	// The DuckDB connection's state closes this Redshift connection when the DuckDB connection closes.
	auto connection_id = RedshiftClientState::Get(context).ConnectionId();
	try {
		return connection_map->GetConnection(connection_id, handle_id, config, context, opened);
	} catch (InvalidConfigurationException &ex) {
		// Only the AWS secret's read raises it there, before any connection opens.
		throw InvalidConfigurationException("Redshift handle %s cannot open a connection: %s",
		                                    SQLIdentifier::ToString(GetName()), ErrorData(ex).RawMessage());
	}
}

void RedshiftCatalog::DiscardRedshiftConnection(ClientContext &context, RedshiftConnection &connection) {
	auto connection_id = RedshiftClientState::Get(context).ConnectionId();
	auto discarded = connection_map->Discard(connection_id, handle_id, connection);
	// Discard has released the map's lock, so the client's teardown blocks no other caller.
	discarded.reset();
}

void RedshiftCatalog::Initialize(bool load_builtin) {
}

string RedshiftCatalog::GetCatalogType() {
	return REDSHIFT_ATTACH_TYPE;
}

unique_ptr<TableRef> RedshiftCatalog::RemoteExecute(ClientContext &, const string &sql) {
	return MakeConnectedQuery(*this, sql);
}

CatalogException RedshiftCatalog::BrowsingNotAvailable(QueryErrorContext error_context, const string &hint) const {
	return CatalogException(error_context,
	                        "Redshift handle \"%s\" cannot be browsed as a catalog: catalog access is not available. "
	                        "Query Redshift through the handle with redshift_query(%s, '<Redshift SQL>')%s",
	                        GetName().GetIdentifierName(), SQLString::ToString(GetName().GetIdentifierName()), hint);
}

optional_ptr<CatalogEntry> RedshiftCatalog::CreateSchema(CatalogTransaction transaction, CreateSchemaInfo &info) {
	throw BrowsingNotAvailable();
}

optional_ptr<SchemaCatalogEntry> RedshiftCatalog::LookupSchema(CatalogTransaction transaction,
                                                               const EntryLookupInfo &schema_lookup,
                                                               OnEntryNotFound if_not_found) {
	if (if_not_found == OnEntryNotFound::RETURN_NULL) {
		return nullptr;
	}
	throw BrowsingNotAvailable(schema_lookup.GetErrorContext());
}

CatalogEntryLookup RedshiftCatalog::TryLookupEntryInternal(CatalogTransaction transaction,
                                                           const EntryLookupInfo &lookup_info) {
	// DuckDB 2.0 carries the schema qualification in the lookup instead of a separate argument.
	const auto &schema = lookup_info.GetSchema();
	const auto &name = GetName().GetIdentifierName();
	// ATTACH ... (DEFAULT_TABLE 'x') makes DuckDB look up x here for an unqualified reference to the
	// handle's name: when nothing else matches it, and, when a local table or view of that name
	// matches, to check for ambiguity (catalog.cpp TryLookupEntry and TryLookupDefaultTable). Both
	// lookups are the same, so one message serves both: there is no default table, and a local
	// table needs its schema.
	if (HasDefaultTable() && schema == GetDefaultTableSchema() && lookup_info.GetEntryName() == GetDefaultTable()) {
		throw CatalogException(lookup_info.GetErrorContext(),
		                       "Redshift handle \"%s\" has no default table; query it with redshift_query(%s, ...). "
		                       "For a local table \"%s\", qualify it with its schema",
		                       name, SQLString::ToString(name), name);
	}
	// An unquoted file name such as `h.csv` parses as table `csv` of catalog `h`, exactly like
	// `h.users`, so DuckDB asks this catalog before trying to read it as a file, and the refusal
	// stops it there. Whether DuckDB would have read it as a file is decided elsewhere, by its
	// replacement scans, so every two-part table reference gets a hint that holds either way.
	//
	// The lookup does not say how the reference was written, but a two-part one is recognizable by
	// its schema. The parser reads `h.csv` as schema `h`; Binder::BindSchemaOrCatalog sees that `h`
	// is an attached database and rewrites it to catalog `h` with no schema; and GetCatalogEntries
	// then fills in this catalog's default schema (catalog.cpp). A three-part reference arrives
	// with the schema as written. An explicit `h.main.x` looks the same and gets the hint too, which
	// is harmless. The search path cannot supply another schema of this catalog, because SET
	// search_path and USE require the schema to exist, and this catalog has none.
	string hint;
	auto default_schema = GetDefaultSchema();
	if (lookup_info.GetCatalogType() == CatalogType::TABLE_ENTRY && default_schema &&
	    schema.GetIdentifierName() == default_schema->GetIdentifierName()) {
		auto file = name + "." + lookup_info.GetEntryName();
		hint = StringUtil::Format(". If %s is a file, quote it like %s", file, SQLString::ToString(file));
	}
	throw BrowsingNotAvailable(lookup_info.GetErrorContext(), hint);
}

void RedshiftCatalog::ScanSchemas(ClientContext &context, std::function<void(SchemaCatalogEntry &)> callback) {
}

void RedshiftCatalog::DropSchema(ClientContext &context, DropInfo &info) {
	throw BrowsingNotAvailable();
}

// Every plan needs a table entry from this catalog, which no lookup returns, so these are
// unreachable; they refuse rather than assert in case DuckDB grows another path to them.
PhysicalOperator &RedshiftCatalog::PlanCreateTableAs(ClientContext &context, PhysicalPlanGenerator &planner,
                                                     LogicalCreateTable &op, PhysicalOperator &plan) {
	throw BrowsingNotAvailable();
}

PhysicalOperator &RedshiftCatalog::PlanInsert(ClientContext &context, PhysicalPlanGenerator &planner, LogicalInsert &op,
                                              optional_ptr<PhysicalOperator> plan) {
	throw BrowsingNotAvailable();
}

PhysicalOperator &RedshiftCatalog::PlanDelete(ClientContext &context, PhysicalPlanGenerator &planner, LogicalDelete &op,
                                              PhysicalOperator &plan) {
	throw BrowsingNotAvailable();
}

PhysicalOperator &RedshiftCatalog::PlanUpdate(ClientContext &context, PhysicalPlanGenerator &planner, LogicalUpdate &op,
                                              PhysicalOperator &plan) {
	throw BrowsingNotAvailable();
}

DatabaseSize RedshiftCatalog::GetDatabaseSize(ClientContext &context) {
	// Nothing is stored locally. Not refused: PRAGMA database_size reports every attached database.
	return DatabaseSize();
}

bool RedshiftCatalog::InMemory() {
	return false;
}

string RedshiftCatalog::GetDBPath() {
	// Nothing is stored at a path: a path ATTACH's path only names the target, which the configuration
	// holds, so duckdb_databases() shows the path empty for every handle.
	return string();
}

bool RedshiftCatalog::HasConflictingAttachOptions(const string &path, const AttachOptions &options) {
	// Always a conflict, so ATTACH OR REPLACE always replaces the handle, unless the new handle's open fails,
	// which keeps the existing handle and its sessions. DuckDB's default compares
	// only the path and the type, and keeps the existing database when they match, without running
	// the attach callback: a different SECRET, a missing one or an unknown option would be ignored.
	// Replacing re-reads a secret whose contents changed under the same name. It closes the old
	// handle's sessions, which the new handle never takes over; the new handle's ATTACH opens its own.
	return true;
}

} // namespace duckdb
