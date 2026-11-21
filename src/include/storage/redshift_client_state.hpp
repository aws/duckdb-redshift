//===----------------------------------------------------------------------===//
//                         DuckDB - Redshift
//
// storage/redshift_client_state.hpp
//
// The extension's state on each DuckDB connection, and its unseen possible-state-loss reports.
//===----------------------------------------------------------------------===//

#pragma once

#include "duckdb/common/atomic.hpp"
#include "duckdb/common/common.hpp"
#include "duckdb/common/error_data.hpp"
#include "duckdb/common/identifier.hpp"
#include "duckdb/common/mutex.hpp"
#include "duckdb/common/optional_ptr.hpp"
#include "duckdb/main/client_context_state.hpp"

namespace duckdb {

class ClientContext;
class ExtensionLoader;
class RedshiftConnectionMap;

//! The extension's state on one DuckDB connection: its ID; the database's connection map, in which
//! it closes this DuckDB connection's Redshift connections when the DuckDB connection closes; and
//! the possible-state-loss reports no user has seen yet (HoldLossReport).
//!
//! Installed when the DuckDB connection opens, and at LOAD on connections that are already open, so
//! every DuckDB connection has one before it binds a redshift_query, and Get only looks it up.
class RedshiftClientState : public ClientContextState {
public:
	RedshiftClientState(shared_ptr<RedshiftConnectionMap> connection_map, connection_t connection_id);

	//! Installs the state on `context`, if it has none, with the DuckDB connection's ID. DuckDB
	//! assigns the ID before it calls OnConnectionOpened, and a connection open at LOAD has one.
	//! Only creates the state: DuckDB calls this from OnConnectionOpened under its connection-list
	//! lock.
	static void Install(ClientContext &context, const shared_ptr<RedshiftConnectionMap> &connection_map);
	//! `context`'s state. Throws an InternalException if it was never installed.
	static RedshiftClientState &Get(ClientContext &context);

	//! The database's connection map, which each handle's catalog shares.
	const shared_ptr<RedshiftConnectionMap> &ConnectionMap() const {
		return connection_map;
	}

	//! This DuckDB connection's ID, the first half of its keys in the connection map.
	connection_t ConnectionId() const {
		return connection_id;
	}

	//! Keeps `report`, the possible-state-loss error a call on the attach handle named `handle` raised,
	//! until a query ends with it. Any thread of the query may
	//! call this.
	void HoldLossReport(ClientContext &context, const string &handle, ErrorData report);
	//! Throws a kept report no user has seen: one `context`'s current query raised, or one about the
	//! handle named `handle`, by the rules for a lost connection and its retry.
	void ThrowUnseenLossReport(ClientContext &context, const string &handle);

	using ClientContextState::QueryEnd;
	//! Forgets each kept report that `error`, the error the query ended with, is: the user has seen it.
	void QueryEnd(ClientContext &context, optional_ptr<ErrorData> error) override;

	//! Notes that this DuckDB connection's current transaction attached the handle `handle_id`, so
	//! that its rollback retires the handle if DuckDB undoes the ATTACH. Any thread of the query may
	//! call this.
	void NoteAttach(idx_t handle_id);
	//! Whether this DuckDB connection's current transaction attached the handle `handle_id`.
	bool AttachedInTransaction(idx_t handle_id);
	//! True the first time it is called on this DuckDB connection, and false after: the extension
	//! warns once per DuckDB connection that redshift_query runs inside a DuckDB transaction.
	//! Any thread of the query may call this.
	bool TakeFirstUseInDuckDBTransaction();
	//! Forgets the handles the transaction attached: they stay, and are no longer this DuckDB
	//! connection's to retire.
	void TransactionCommit(MetaTransaction &transaction, ClientContext &context) override;
	using ClientContextState::TransactionRollback;
	//! Retires each handle the transaction attached that DuckDB no longer lists
	//! (RedshiftConnectionMap::RetireRolledBackHandle), with this DuckDB connection as the detaching
	//! one: DuckDB's rollback has taken it out of the list without calling OnDetach. DuckDB also runs
	//! this when a COMMIT fails, which can leave an ATTACH committed, and so listed.
	void TransactionRollback(MetaTransaction &transaction, ClientContext &context,
	                         optional_ptr<ErrorData> error) override;

	//! Closes this DuckDB connection's Redshift connections, silently. DuckDB destroys the state with
	//! its ClientContext, before the DuckDB connection lets the database go, so the client can still
	//! log each close.
	~RedshiftClientState() override;

private:
	//! Points to the database's one RedshiftConnectionMap, created at LOAD. Ownership is shared with
	//! the LOAD callback, every other DuckDB connection's state and every handle's catalog; the map
	//! lives until the last of them lets go, so it outlives this state.
	shared_ptr<RedshiftConnectionMap> connection_map;
	const connection_t connection_id;

	//! A report HoldLossReport kept: the query that raised it, or MAXIMUM_QUERY_ID when none was active
	//! or once ThrowUnseenLossReport has raised it outside a query, and the name of the handle it is
	//! about, compared as DuckDB compares attached databases' names.
	struct HeldLossReport {
		transaction_t query;
		Identifier handle;
		ErrorData report;
	};
	//! Guards `held_loss_reports`: a query's executor threads keep reports while it runs.
	mutex held_loss_reports_lock;
	//! The reports no user has seen yet, at most one per name.
	vector<HeldLossReport> held_loss_reports;
	//! Guards `attached_in_transaction`: the executor thread that runs an ATTACH notes it.
	mutex attached_lock;
	//! The handles the current transaction attached, by ID.
	vector<idx_t> attached_in_transaction;
	//! Set once redshift_query has run inside a DuckDB transaction on this DuckDB connection.
	atomic<bool> used_in_duckdb_transaction {false};
};

//! Creates the database's RedshiftConnectionMap and installs a RedshiftClientState holding it on
//! every DuckDB connection: each that opens from now on, and each that is already open, the loading
//! one included.
void CreateConnectionMapAndInstallClientStates(ExtensionLoader &loader);

} // namespace duckdb
