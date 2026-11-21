//===----------------------------------------------------------------------===//
//                         DuckDB - Redshift
//
// storage/redshift_catalog.hpp
//
// The catalog of a Redshift attached database: the attach handle, not browsable.
//===----------------------------------------------------------------------===//

#pragma once

#include "duckdb/catalog/catalog.hpp"
#include "redshift_transport.hpp"

namespace duckdb {

class RedshiftConnectionMap;
enum class RedshiftConnectionUse : uint8_t;

//! The attached database `ATTACH ... (TYPE <REDSHIFT_ATTACH_TYPE>)` creates. It holds the
//! configuration resolved at ATTACH and the handle's ID in the database's RedshiftConnectionMap,
//! where redshift_query keeps one Redshift connection per DuckDB connection using the handle; ATTACH
//! opens the attaching DuckDB connection's. Its identity is the object itself, never its alias: a re-ATTACH under the
//! same alias, `ATTACH OR REPLACE` included, makes a new RedshiftCatalog with a new handle ID, which never reaches the
//! old one's Redshift connections.
//!
//! Lifetime. The ATTACH callback creates it, and its AttachedDatabase owns it. DuckDB shares the
//! AttachedDatabase between the database list, until DETACH, ATTACH OR REPLACE or a rolled-back
//! ATTACH removes it; each redshift_query bind or execution using the handle, through its
//! RedshiftConnectionPin, until it ends; and anything else that holds it for a while, such as a
//! duckdb_databases() scan or a transaction that looked the name up as a catalog. Whichever lets go
//! last closes the AttachedDatabase, on its own thread, which destroys this catalog. When the
//! database closes, DuckDB closes every attached database regardless, but by then no DuckDB
//! connection, and so no query, is left.
//!
//! DETACH and ATTACH OR REPLACE retire the handle's Redshift connections when they remove it
//! (OnDetach), not when the catalog is destroyed, which can be later, and so does the rollback of
//! the transaction that attached it (RedshiftClientState's rollback hook). DuckDB undoes a rolled-back
//! ATTACH only while its name still refers to that handle, so another DuckDB connection's rollback
//! leaves this one alone.
//!
//! A DuckDB connection can CONNECT to it, and DuckDB then runs each statement the connection is sent,
//! until DISCONNECT, as a redshift_query call on it (RemoteExecute).
//!
//! It has no schemas. Every entry lookup, and every create, drop or write against it, fails with a
//! CatalogException that directs the user to redshift_query. It has no default table either: a
//! DEFAULT_TABLE option is accepted, as for any attached database, but a reference DuckDB resolves
//! through it fails, saying how to reach a same-named local table.
class RedshiftCatalog : public Catalog {
public:
	//! A handle for `db`, holding the configuration ATTACH resolved, with a new ID in `connection_map`.
	//! Opens no connection: ATTACH then calls OpenAttachingConnection.
	RedshiftCatalog(AttachedDatabase &db, RedshiftConfig config, shared_ptr<RedshiftConnectionMap> connection_map);
	//! Closes whatever Redshift connections the handle still has: after DETACH, ATTACH OR REPLACE
	//! or a rolled-back ATTACH none (its retirement closed them); after a removal DuckDB tells this
	//! extension nothing of, which retires nothing, each one, recording any session that may have
	//! held state, for its next statement on the name; after an ATTACH whose open failed, or that
	//! DuckDB refused after its callback, that ATTACH's session, if it opened one; and as the
	//! database closes every one left. Runs once the handle has gone and nothing holds it any more;
	//! see Lifetime above.
	~RedshiftCatalog() override;

	//! The configuration resolved at ATTACH, fixed for the handle's lifetime: OR REPLACE makes a
	//! new handle rather than changing this one.
	const RedshiftConfig &Config() const {
		return config;
	}

	//! This handle's ID, from the connection map's counter, which never reuses one. State that must not
	//! keep the handle alive, such as redshift_query's bind data, holds this to tell the handle apart
	//! from one attached later under the same alias.
	idx_t HandleId() const {
		return handle_id;
	}

	//! Whether DETACH, ATTACH OR REPLACE or a rollback has retired this handle.
	bool IsRetired() const;
	//! The IDs of the Redshift handles in `context`'s DuckDB database list now.
	static unordered_set<idx_t> ListedHandleIds(ClientContext &context);

	//! Opens `context`'s Redshift connection on this handle, for the ATTACH that is creating it, before
	//! DuckDB lists the handle, so that a target or credentials the client cannot connect with fail
	//! the ATTACH. The connection stays as the pair's, idle, for the DuckDB connection's first
	//! statement on the handle. Throws what OpenRedshiftConnection and the AWS secret's read throw.
	void OpenAttachingConnection(ClientContext &context);

	//! `context`'s Redshift connection on this handle: opened through OpenRedshiftConnection when the
	//! DuckDB connection has none on it, at ATTACH (OpenAttachingConnection), at another DuckDB
	//! connection's first use of the handle, or after one was discarded, and reused after. The caller
	//! holds a pin on it. `opened`, if given, is set to whether this call opened it. Null when it has
	//! none and the handle has been retired, which opens none (RedshiftConnectionMap::GetConnection).
	optional_ptr<RedshiftConnection> GetRedshiftConnection(ClientContext &context, optional_ptr<bool> opened = nullptr);
	//! Destroys `connection`, `context`'s Redshift connection on this handle, which the client has
	//! closed after a transport failure, so the DuckDB connection's next use of the handle opens a new
	//! one. Nothing else may use it afterwards.
	void DiscardRedshiftConnection(ClientContext &context, RedshiftConnection &connection);

	//! DETACH, or ATTACH OR REPLACE, has removed the handle. Retires its Redshift connections: each
	//! that no bind or execution holds closes now, silently, the detaching DuckDB connection's included,
	//! and each other when its last pin ends.
	void OnDetach(ClientContext &context) override;

	void Initialize(bool load_builtin) override;
	string GetCatalogType() override;

	//! DuckDB's CONNECT, and no other remote capability.
	bool Supports(RemoteCapability capability) const override {
		return capability == RemoteCapability::CONNECT;
	}
	//! What DuckDB runs a statement typed while CONNECT-ed to this handle as: a redshift_query call on
	//! this handle with the statement's text (MakeConnectedQuery).
	unique_ptr<TableRef> RemoteExecute(ClientContext &context, const string &sql) override;
	//! The other overloads stay Catalog's, which this catalog does not support.
	using Catalog::RemoteExecute;

	optional_ptr<CatalogEntry> CreateSchema(CatalogTransaction transaction, CreateSchemaInfo &info) override;
	optional_ptr<SchemaCatalogEntry> LookupSchema(CatalogTransaction transaction, const EntryLookupInfo &schema_lookup,
	                                              OnEntryNotFound if_not_found) override;
	void ScanSchemas(ClientContext &context, std::function<void(SchemaCatalogEntry &)> callback) override;

	PhysicalOperator &PlanCreateTableAs(ClientContext &context, PhysicalPlanGenerator &planner, LogicalCreateTable &op,
	                                    PhysicalOperator &plan) override;
	PhysicalOperator &PlanInsert(ClientContext &context, PhysicalPlanGenerator &planner, LogicalInsert &op,
	                             optional_ptr<PhysicalOperator> plan) override;
	PhysicalOperator &PlanDelete(ClientContext &context, PhysicalPlanGenerator &planner, LogicalDelete &op,
	                             PhysicalOperator &plan) override;
	PhysicalOperator &PlanUpdate(ClientContext &context, PhysicalPlanGenerator &planner, LogicalUpdate &op,
	                             PhysicalOperator &plan) override;

	DatabaseSize GetDatabaseSize(ClientContext &context) override;
	bool InMemory() override;
	string GetDBPath() override;
	bool HasConflictingAttachOptions(const string &path, const AttachOptions &options) override;

private:
	friend class RedshiftConnectionPin;
	//! Pins DuckDB connection `connection_id`'s Redshift connection on this handle for one bind or
	//! one execution (RedshiftConnectionMap::Pin), for RedshiftConnectionPin. Returns false once the
	//! handle has been retired.
	bool PinConnection(connection_t connection_id);
	//! Ends a pin PinConnection granted.
	void UnpinConnection(connection_t connection_id);
	//! Starts `use` of DuckDB connection `connection_id`'s pinned Redshift connection on this handle
	//! (RedshiftConnectionMap::BeginUse), for RedshiftConnectionPin.
	bool BeginConnectionUse(connection_t connection_id, RedshiftConnectionUse use);
	//! Ends a use BeginConnectionUse started.
	void EndConnectionUse(connection_t connection_id);

	CatalogEntryLookup TryLookupEntryInternal(CatalogTransaction transaction,
	                                          const EntryLookupInfo &lookup_info) override;
	void DropSchema(ClientContext &context, DropInfo &info) override;

	//! The refusal for any use of this catalog as a catalog, naming the handle, followed by `hint`.
	CatalogException BrowsingNotAvailable(QueryErrorContext error_context = QueryErrorContext(),
	                                      const string &hint = string()) const;

	RedshiftConfig config;
	//! Points to the database's one RedshiftConnectionMap, created at LOAD. Ownership is shared with
	//! the LOAD callback, every DuckDB connection's RedshiftClientState and every other handle's
	//! catalog; the map lives until the last of them lets go, so it outlives this catalog.
	shared_ptr<RedshiftConnectionMap> connection_map;
	//! This handle's ID in connection_map: the second half of its keys there.
	idx_t handle_id;
};

} // namespace duckdb
