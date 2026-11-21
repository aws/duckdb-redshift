//===----------------------------------------------------------------------===//
// The extension's state on each DuckDB connection, and how each DuckDB connection gets one. See
// storage/redshift_client_state.hpp.
//===----------------------------------------------------------------------===//

#include "storage/redshift_client_state.hpp"

#include "duckdb/common/exception.hpp"
#include "duckdb/common/string_util.hpp"
#include "duckdb/main/client_context.hpp"
#include "duckdb/main/config.hpp"
#include "duckdb/main/connection_manager.hpp"
#include "duckdb/main/extension/extension_loader.hpp"
#include "duckdb/planner/extension_callback.hpp"
#include "storage/redshift_catalog.hpp"
#include "storage/redshift_connection_ownership.hpp"

#include <algorithm>

namespace duckdb {

namespace {

//! The key the state is stored under in each DuckDB connection's registered state.
constexpr const char *CLIENT_STATE_NAME = "redshift_client_state";

//! Installs the state on each DuckDB connection opened after LOAD. DuckDB calls OnConnectionOpened
//! from ConnectionManager::AddConnection, under its connection-list lock, so this only creates the
//! state.
class RedshiftClientCallback : public ExtensionCallback {
public:
	explicit RedshiftClientCallback(shared_ptr<RedshiftConnectionMap> connection_map_p)
	    : connection_map(std::move(connection_map_p)) {
	}

	void OnConnectionOpened(ClientContext &context) override {
		RedshiftClientState::Install(context, connection_map);
	}

private:
	//! Points to the database's one RedshiftConnectionMap, which
	//! CreateConnectionMapAndInstallClientStates created and which each state this installs shares.
	//! DuckDB keeps the callback for the database's lifetime, so this keeps the map alive at least
	//! that long.
	shared_ptr<RedshiftConnectionMap> connection_map;
};

} // namespace

//===--------------------------------------------------------------------===//
// RedshiftClientState
//===--------------------------------------------------------------------===//

RedshiftClientState::RedshiftClientState(shared_ptr<RedshiftConnectionMap> connection_map_p,
                                         connection_t connection_id_p)
    : connection_map(std::move(connection_map_p)), connection_id(connection_id_p) {
}

void RedshiftClientState::Install(ClientContext &context, const shared_ptr<RedshiftConnectionMap> &connection_map) {
	// Connection's constructor assigns the ID before it adds the connection, which is what calls
	// OnConnectionOpened.
	D_ASSERT(context.GetConnectionId() != DConstants::INVALID_INDEX);
	// GetOrCreate constructs the state the first time, under the DuckDB connection's own
	// registered-state lock, and finds it after, so installing twice creates one state.
	context.registered_state->GetOrCreate<RedshiftClientState>(CLIENT_STATE_NAME, connection_map,
	                                                           context.GetConnectionId());
}

RedshiftClientState &RedshiftClientState::Get(ClientContext &context) {
	auto state = context.registered_state->Get<RedshiftClientState>(CLIENT_STATE_NAME);
	if (!state) {
		// Every DuckDB connection is opened through the connection manager, which installs the
		// state, and LOAD installs it on the connections opened before.
		throw InternalException("Redshift extension: this DuckDB connection has no Redshift client state");
	}
	// The DuckDB connection's registered state keeps it alive.
	return *state;
}

void RedshiftClientState::HoldLossReport(ClientContext &context, const string &handle, ErrorData report) {
	HeldLossReport entry {context.transaction.GetActiveQuery(), Identifier(handle), std::move(report)};
	lock_guard<mutex> guard(held_loss_reports_lock);
	// Every bind and execution naming the handle first raises a kept report about it, or outside a
	// query may forget it, before it opens anything, so no call on the handle can lose a connection
	// while one is kept.
	D_ASSERT(std::none_of(held_loss_reports.begin(), held_loss_reports.end(),
	                      [&](const HeldLossReport &kept) { return kept.handle == entry.handle; }));
	held_loss_reports.push_back(std::move(entry));
}

void RedshiftClientState::ThrowUnseenLossReport(ClientContext &context, const string &handle) {
	const auto query = context.transaction.GetActiveQuery();
	const bool in_query = query != MAXIMUM_QUERY_ID;
	const Identifier name(handle);
	ErrorData unseen;
	{
		lock_guard<mutex> guard(held_loss_reports_lock);
		for (auto held = held_loss_reports.begin(); held != held_loss_reports.end(); ++held) {
			const bool same_query = in_query && held->query == query;
			if (!same_query && held->handle != name) {
				continue;
			}
			if (in_query) {
				// QueryEnd forgets it once the query ends with it.
				unseen = held->report;
			} else if (held->query != MAXIMUM_QUERY_ID) {
				// Kept in a query, so no user has seen it. This caller may swallow it, as the shell's
				// agent mode does an extracted plan's error, so it is kept as one raised outside a
				// query: the next query naming the handle raises it again.
				unseen = held->report;
				held->query = MAXIMUM_QUERY_ID;
			} else {
				// Raised outside a query before, to a caller that got it then.
				held_loss_reports.erase(held);
			}
			break;
		}
	}
	if (unseen.HasError()) {
		unseen.Throw();
	}
}

void RedshiftClientState::QueryEnd(ClientContext &, optional_ptr<ErrorData> error) {
	lock_guard<mutex> guard(held_loss_reports_lock);
	if (held_loss_reports.empty() || !error || !error->HasError()) {
		return;
	}
	// The message as it was raised. DuckDB adds the query's location after it, or, with
	// errors_as_json set, writes the whole error as JSON instead. A user's message may only look
	// like JSON, so it is read without throwing, and taken as it is when it is not.
	auto message = error->RawMessage();
	if (!message.empty() && message[0] == '{') {
		auto fields = StringUtil::ParseJSONMap(message, true);
		auto raised = fields.find("exception_message");
		if (raised != fields.end()) {
			message = raised->second;
		}
	}
	auto seen = [&](const HeldLossReport &held) {
		return held.report.Type() == error->Type() && StringUtil::StartsWith(message, held.report.RawMessage());
	};
	held_loss_reports.erase(std::remove_if(held_loss_reports.begin(), held_loss_reports.end(), seen),
	                        held_loss_reports.end());
}

void RedshiftClientState::NoteAttach(idx_t handle_id) {
	lock_guard<mutex> guard(attached_lock);
	attached_in_transaction.push_back(handle_id);
}

bool RedshiftClientState::AttachedInTransaction(idx_t handle_id) {
	lock_guard<mutex> guard(attached_lock);
	return std::find(attached_in_transaction.begin(), attached_in_transaction.end(), handle_id) !=
	       attached_in_transaction.end();
}

bool RedshiftClientState::TakeFirstUseInDuckDBTransaction() {
	return !used_in_duckdb_transaction.exchange(true);
}

void RedshiftClientState::TransactionCommit(MetaTransaction &, ClientContext &) {
	lock_guard<mutex> guard(attached_lock);
	attached_in_transaction.clear();
}

void RedshiftClientState::TransactionRollback(MetaTransaction &, ClientContext &context, optional_ptr<ErrorData>) {
	vector<idx_t> rolled_back;
	{
		lock_guard<mutex> guard(attached_lock);
		rolled_back.swap(attached_in_transaction);
	}
	if (rolled_back.empty()) {
		// Every rollback and failed COMMIT on every DuckDB connection runs this hook, so a plain one
		// skips the database-list read below, which takes DuckDB's list lock and copies the list.
		return;
	}
	// DuckDB has taken each ATTACH it undid out of its list by now, and the transaction still holds
	// its catalog. A handle DuckDB still lists stays, and is not retired: a COMMIT that fails runs
	// this hook too, and may have committed the ATTACH first, and DuckDB lists a new handle before it
	// adds it to the transaction, which can throw before the ATTACH is recorded for undo. A DETACH or
	// ATTACH OR REPLACE after this check retires such a handle through OnDetach, as it does any other.
	// One that has taken the name out of the list before this check, but whose OnDetach comes after
	// it, is not told apart from the rollback: this retires the handle with this DuckDB connection as
	// the detaching one, so the DuckDB connection that ran the DETACH or ATTACH OR REPLACE gets a
	// possible-state-loss report it should not, when its session may have held state. That race is
	// rare, and ordering this check with OnDetach is not worth it.
	const auto listed = RedshiftCatalog::ListedHandleIds(context);
	for (auto handle_id : rolled_back) {
		if (listed.count(handle_id) == 0) {
			connection_map->RetireRolledBackHandle(handle_id, connection_id);
		}
	}
}

RedshiftClientState::~RedshiftClientState() {
	connection_map->CloseConnection(connection_id);
}

void CreateConnectionMapAndInstallClientStates(ExtensionLoader &loader) {
	auto &db = loader.GetDatabaseInstance();
	auto connection_map = make_shared_ptr<RedshiftConnectionMap>();
	// DuckDB connections opened from now on: AddConnection calls the callback for each. The callback
	// stays registered for the database's lifetime.
	ExtensionCallback::Register(DBConfig::GetConfig(db), make_shared_ptr<RedshiftClientCallback>(connection_map));
	// DuckDB connections already open, the one running LOAD included, which the callback never sees.
	// Registered first and swept second, so a connection opening meanwhile is found by one or both,
	// and Install is idempotent.
	for (auto &context : ConnectionManager::Get(db).GetConnectionList()) {
		RedshiftClientState::Install(*context, connection_map);
	}
}

} // namespace duckdb
