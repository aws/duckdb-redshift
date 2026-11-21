//===----------------------------------------------------------------------===//
// The Redshift connections redshift_query keeps open, and when each one goes away. See
// storage/redshift_connection_ownership.hpp.
//
// One RedshiftConnectionMap per DuckDB database holds them all, by (DuckDB connection ID, attach
// handle ID). Each attach handle's catalog and each DuckDB connection's RedshiftClientState
// share it, and neither refers to the other. The RedshiftConnection object leaves the map, and is
// destroyed, exactly once, in one of five ways:
//
//   * Its DuckDB connection closes: the connection's RedshiftClientState, destroyed with it,
//     calls CloseConnection.
//   * Its handle is detached, replaced by ATTACH OR REPLACE, or removed by the rollback of the
//     transaction that attached it: the handle's RedshiftCatalog (OnDetach) calls RetireHandle, or
//     the rolling-back DuckDB connection's state calls RetireRolledBackHandle. Either closes the
//     connection at once if no pin holds it, or else leaves it for the last pin to close in Unpin.
//   * Its handle has left DuckDB's list, but nothing has retired it yet, and its own DuckDB
//     connection's next bind on the name closes it (TakeLostSession).
//   * Its handle goes away without a retirement, as the database closes: DuckDB destroys the
//     handle's RedshiftCatalog once nothing holds the handle (see Lifetime in
//     storage/redshift_catalog.hpp), and the catalog calls CloseHandle.
//   * The server or the network drops it: the client then throws IOException from Describe,
//     Execute or NextBatch, and keeps throwing it without network access. RedshiftConnectionCalls
//     discards the dead connection, so the pair's next use opens a new one.
//
// Only the last can take a connection out while the pair's own query holds a pin on it, and only
// that query does it, which is why GetConnection can hand out a reference to it. A pin keeps the
// catalog, so CloseHandle cannot run under one, and retirement and TakeLostSession skip a pinned
// pair.
//===----------------------------------------------------------------------===//

#include "storage/redshift_connection_ownership.hpp"

#include "redshift_aws_secret.hpp"

#include "duckdb/main/attached_database.hpp"
#include "duckdb/main/client_context.hpp"
#include "duckdb/main/database.hpp"
#include "redshift_log_sink.hpp"
#include "storage/redshift_catalog.hpp"
#include "storage/redshift_client_state.hpp"

#include <algorithm>

namespace duckdb {

//===--------------------------------------------------------------------===//
// RedshiftConnectionMap
//===--------------------------------------------------------------------===//

namespace {

//! The detaching DuckDB connection of a handle that no known DuckDB connection removed. No DuckDB
//! connection has this ID.
constexpr connection_t NO_DETACHING_CONNECTION = DConstants::INVALID_INDEX;

//! What `connection`'s client says about its session's state, where its answer
//! decides a report. An answer the client cannot give counts as possible state, which reports a loss
//! rather than hides one.
bool MayHoldState(const RedshiftConnection &connection) noexcept {
	try {
		return connection.SessionHoldsState();
	} catch (...) { // NOLINT: the conservative answer stands
		// Neither client throws here, and the mock has no control to make it, so no test reaches this.
		return true;
	}
}

} // namespace

#ifdef REDSHIFT_TEST_HOOKS
namespace {

//! The pause a test set at one test-only pause point, if any.
class TestPause {
public:
	void Set(std::function<void()> pause_p) {
		lock_guard<mutex> guard(lock);
		pause = std::move(pause_p);
	}
	//! Runs the pause outside the lock, so the test can set another meanwhile.
	void Run() {
		std::function<void()> current;
		{
			lock_guard<mutex> guard(lock);
			current = pause;
		}
		if (current) {
			current();
		}
	}

private:
	mutex lock;
	std::function<void()> pause;
};

TestPause &PinPause() {
	static TestPause pause;
	return pause;
}

TestPause &LostSessionPause() {
	static TestPause pause;
	return pause;
}

} // namespace

void SetRedshiftPinPauseForTesting(std::function<void()> pause) {
	PinPause().Set(std::move(pause));
}

void SetRedshiftLostSessionPauseForTesting(std::function<void()> pause) {
	LostSessionPause().Set(std::move(pause));
}
#endif

idx_t RedshiftConnectionMap::NewHandleId() {
	return next_handle_id.fetch_add(1);
}

idx_t RedshiftConnectionMap::RegisterHandle(const Catalog &catalog) {
	const auto handle_id = NewHandleId();
	lock_guard<mutex> guard(connections_lock);
	handles[handle_id] = &catalog;
	return handle_id;
}

bool RedshiftConnectionMap::Pin(connection_t connection_id, idx_t handle_id) {
	lock_guard<mutex> guard(connections_lock);
	if (retired_handles.count(handle_id) != 0) {
		return false;
	}
	connections[Key(connection_id, handle_id)].pins++;
	return true;
}

void RedshiftConnectionMap::Unpin(connection_t connection_id, idx_t handle_id) {
	unique_ptr<RedshiftConnection> closing;
	{
		lock_guard<mutex> guard(connections_lock);
		auto entry = connections.find(Key(connection_id, handle_id));
		if (entry == connections.end()) {
			// Only CloseConnection or CloseHandle takes a pinned entry out, and neither runs while the
			// pin's query does; defensive.
			return;
		}
		D_ASSERT(entry->second.pins > 0);
		if (--entry->second.pins > 0) {
			return;
		}
		auto &connection = entry->second.connection;
		auto retired = retired_handles.find(handle_id);
		if (retired == retired_handles.end()) {
			if (connection) {
				// The pair keeps its idle connection for its next call.
				return;
			}
		} else if (connection) {
			// The handle was retired during the pin.
			RecordLostSession(entry->first, *connection, retired->second);
		}
		closing = std::move(connection);
		connections.erase(entry);
	}
	// Destroyed outside the lock: the client's teardown can block on the network.
	closing.reset();
}

bool RedshiftConnectionMap::BeginUse(connection_t connection_id, idx_t handle_id, RedshiftConnectionUse use) {
	D_ASSERT(use != RedshiftConnectionUse::NONE);
	const Key key(connection_id, handle_id);
	unique_lock<mutex> guard(connections_lock);
	while (true) {
		// Found again after each wait: the entry stays while the caller's pin holds it, but nothing
		// else here relies on that.
		auto entry = connections.find(key);
		if (entry == connections.end()) {
			// Only CloseConnection or CloseHandle takes a pinned entry out, and neither runs while the
			// pin's query does; defensive. Nothing else can use a connection the map no longer has.
			return true;
		}
		auto &current = entry->second.use;
		if (current == RedshiftConnectionUse::NONE) {
			current = use;
			return true;
		}
		// A bind is refused, never made to wait.
		if (use == RedshiftConnectionUse::DESCRIBE) {
			return false;
		}
		// A second result is a defect, and waiting for it could deadlock.
		if (current == RedshiftConnectionUse::RESULT) {
			return false;
		}
		// A scan meets a bind's Describe, which waits on nothing the scan holds; refusing the scan would
		// fail its statement partway, after other calls may have run. wait() releases the lock while it
		// sleeps, and waking is no promise the pair is free, so the loop decides again.
		use_ended.wait(guard);
	}
}

void RedshiftConnectionMap::EndUse(connection_t connection_id, idx_t handle_id) {
	{
		lock_guard<mutex> guard(connections_lock);
		auto entry = connections.find(Key(connection_id, handle_id));
		if (entry != connections.end()) {
			entry->second.use = RedshiftConnectionUse::NONE;
		}
	}
	use_ended.notify_all();
}

optional_ptr<RedshiftConnection> RedshiftConnectionMap::GetConnection(connection_t connection_id, idx_t handle_id,
                                                                      const RedshiftConfig &config,
                                                                      ClientContext &context,
                                                                      optional_ptr<bool> opened) {
	const Key key(connection_id, handle_id);
	if (opened) {
		*opened = false;
	}
	{
		lock_guard<mutex> guard(connections_lock);
		auto entry = connections.find(key);
		if (entry != connections.end() && entry->second.connection) {
			// Kept through a retirement by the caller's pin: work on it finishes.
			return entry->second.connection.get();
		}
		if (retired_handles.count(handle_id) != 0) {
			// A pin granted before the retirement lets work on an open connection finish, never start on
			// a new one.
			return nullptr;
		}
	}

	// The pair has no connection: this is the ATTACH's open, the pair's first use, or its next after
	// its last connection was discarded after a failure. The Redshift connection is opened outside the lock, because
	// opening can take as long as several connection attempts. With SECRET, the AWS secret is read now, for this open
	// alone, and this extension keeps no copy of its credentials past the open, so a replaced secret takes effect at
	// the next open. The driver's own copies are described at RedshiftAwsCredentials.
	unique_ptr<RedshiftAwsCredentials> credentials;
	if (!config.aws_secret.empty()) {
		credentials = make_uniq<RedshiftAwsCredentials>(ReadAwsSecret(context, config.aws_secret).credentials);
	}
	auto new_connection = OpenRedshiftConnection(config, credentials.get(),
	                                             CreateRedshiftLogSink(DatabaseInstance::GetDatabase(context)));
	credentials.reset();
	// Declared before the lock, so a connection this does not keep is destroyed after the lock is
	// released.
	unique_ptr<RedshiftConnection> unused;
	lock_guard<mutex> guard(connections_lock);
	auto &slot = connections[key].connection;
	if (slot) {
		// Another first use of the pair inserted first. The same-handle fence keeps two scans of one
		// statement off one pair, so nothing in SQL reaches this; it is defensive.
		// Keep that connection.
		unused = std::move(new_connection);
	} else if (retired_handles.count(handle_id) != 0) {
		// The handle was retired while this opened: the connection is destroyed unused.
		unused = std::move(new_connection);
		return nullptr;
	} else {
		slot = std::move(new_connection);
		if (opened) {
			*opened = true;
		}
	}
	return slot.get();
}

void RedshiftConnectionMap::RetireHandle(idx_t handle_id, const Identifier &name, connection_t detaching) {
	vector<unique_ptr<RedshiftConnection>> removed;
	{
		lock_guard<mutex> guard(connections_lock);
		RetireLocked(handle_id, RetiredHandle {name, detaching}, removed);
	}
	// Closed after the lock is released, so the client's slow teardown blocks no other caller.
	removed.clear();
}

void RedshiftConnectionMap::RetireRolledBackHandle(idx_t handle_id, connection_t rolling_back) {
	vector<unique_ptr<RedshiftConnection>> removed;
	{
		lock_guard<mutex> guard(connections_lock);
		auto handle = handles.find(handle_id);
		if (handle == handles.end() || retired_handles.count(handle_id) != 0) {
			return;
		}
		// DuckDB has taken the handle out of the list already, so nothing renames it now.
		RetireLocked(handle_id, RetiredHandle {handle->second->GetName(), rolling_back}, removed);
	}
	removed.clear();
}

void RedshiftConnectionMap::RetireLocked(idx_t handle_id, const RetiredHandle &retired,
                                         vector<unique_ptr<RedshiftConnection>> &removed) {
	retired_handles[handle_id] = retired;
	for (auto entry = connections.begin(); entry != connections.end();) {
		if (entry->first.second != handle_id || entry->second.pins > 0) {
			// A pinned connection closes when its last pin ends (Unpin).
			++entry;
			continue;
		}
		// Recorded under the lock that retires the handle, so a statement that resolves the name after
		// this cannot miss it; TakeLostSession covers one that resolves it before. The client answers
		// from memory, so this makes no network call.
		if (entry->second.connection) {
			RecordLostSession(entry->first, *entry->second.connection, retired);
		}
		removed.push_back(std::move(entry->second.connection));
		entry = connections.erase(entry);
	}
}

bool RedshiftConnectionMap::IsRetired(idx_t handle_id) {
	lock_guard<mutex> guard(connections_lock);
	return retired_handles.count(handle_id) != 0;
}

unordered_set<idx_t> RedshiftConnectionMap::ListedHandleIds(const vector<shared_ptr<AttachedDatabase>> &databases) {
	unordered_set<const AttachedDatabase *> listed_databases;
	for (auto &db : databases) {
		listed_databases.insert(db.get());
	}
	unordered_set<idx_t> listed;
	lock_guard<mutex> guard(connections_lock);
	for (auto &handle : handles) {
		if (listed_databases.count(&handle.second->GetAttached()) != 0) {
			listed.insert(handle.first);
		}
	}
	return listed;
}

bool RedshiftConnectionMap::TakeLostSession(connection_t connection_id, const Identifier &name,
                                            const unordered_set<idx_t> &listed) {
#ifdef REDSHIFT_TEST_HOOKS
	LostSessionPause().Run();
#endif
	bool lost = false;
	vector<unique_ptr<RedshiftConnection>> removed;
	{
		lock_guard<mutex> guard(connections_lock);
		for (auto record = lost_sessions.begin(); record != lost_sessions.end(); ++record) {
			if (record->connection_id == connection_id && record->name == name) {
				lost = true;
				lost_sessions.erase(record);
				break;
			}
		}
		for (auto entry = connections.begin(); entry != connections.end();) {
			const auto handle_id = entry->first.second;
			// Only this DuckDB connection's idle sessions: a pinned one is still in use.
			if (entry->first.first != connection_id || entry->second.pins > 0 || listed.count(handle_id) != 0 ||
			    retired_handles.count(handle_id) != 0) {
				++entry;
				continue;
			}
			// A handle out of the list stays out of it, so its name, read only now, is the one it left
			// the list under.
			auto handle = handles.find(handle_id);
			if (handle == handles.end() || handle->second->GetName() != name) {
				++entry;
				continue;
			}
			if (entry->second.connection) {
				lost = lost || MayHoldState(*entry->second.connection);
				removed.push_back(std::move(entry->second.connection));
			}
			entry = connections.erase(entry);
		}
	}
	removed.clear();
	return lost;
}

void RedshiftConnectionMap::CloseHandle(idx_t handle_id) {
	vector<unique_ptr<RedshiftConnection>> removed;
	{
		lock_guard<mutex> guard(connections_lock);
		// A handle DuckDB removed without OnDetach or the rollback hook, which DuckDB v2.0.1 does nowhere:
		// its rollback undoes an ATTACH only while the name still refers to that handle
		// (DatabaseManager::DetachInternal). Kept as a defence: its sessions are recorded as a retirement's
		// are, for every DuckDB connection, since none is known to have removed it. As the database
		// closes, no DuckDB connection is left with a session to record.
		auto handle = handles.find(handle_id);
		const bool was_retired = retired_handles.erase(handle_id) != 0;
		const bool unretired = !was_retired && handle != handles.end();
		const RetiredHandle removed_unretired {unretired ? handle->second->GetName() : Identifier(),
		                                       NO_DETACHING_CONNECTION};
		if (handle != handles.end()) {
			handles.erase(handle);
		}
		for (auto entry = connections.begin(); entry != connections.end();) {
			if (entry->first.second != handle_id) {
				++entry;
				continue;
			}
			if (unretired && entry->second.connection) {
				RecordLostSession(entry->first, *entry->second.connection, removed_unretired);
			}
			removed.push_back(std::move(entry->second.connection));
			entry = connections.erase(entry);
		}
	}
	// Destroyed after the lock is released, so the client's slow teardown blocks no other caller.
	removed.clear();
}

void RedshiftConnectionMap::RecordLostSession(const Key &key, const RedshiftConnection &connection,
                                              const RetiredHandle &retired) noexcept {
	if (key.first == retired.detaching) {
		return;
	}
	// Asked before the connection is destroyed: only the client knows.
	if (!MayHoldState(connection)) {
		return;
	}
	try {
		for (auto &record : lost_sessions) {
			if (record.connection_id == key.first && record.name == retired.name) {
				// An earlier handle under the name, whose loss the DuckDB connection has not been told yet.
				return;
			}
		}
		lost_sessions.push_back(LostSession {key.first, retired.name});
	} catch (...) { // NOLINT: a failed allocation, which no test causes, drops the record
	}
}

void RedshiftConnectionMap::CloseConnection(connection_t connection_id) {
	vector<unique_ptr<RedshiftConnection>> removed;
	{
		// One critical section, so no retirement, Unpin or CloseHandle between forgetting the records and
		// taking the connections out can record a session for a DuckDB connection that has closed.
		lock_guard<mutex> guard(connections_lock);
		lost_sessions.erase(
		    std::remove_if(lost_sessions.begin(), lost_sessions.end(),
		                   [&](const LostSession &record) { return record.connection_id == connection_id; }),
		    lost_sessions.end());
		for (auto entry = connections.begin(); entry != connections.end();) {
			if (entry->first.first == connection_id) {
				removed.push_back(std::move(entry->second.connection));
				entry = connections.erase(entry);
			} else {
				++entry;
			}
		}
	}
	// As in CloseHandle: destroyed outside the lock.
	removed.clear();
}

unique_ptr<RedshiftConnection> RedshiftConnectionMap::Discard(connection_t connection_id, idx_t handle_id,
                                                              const RedshiftConnection &connection) {
	lock_guard<mutex> guard(connections_lock);
	auto entry = connections.find(Key(connection_id, handle_id));
	if (entry == connections.end() || entry->second.connection.get() != &connection) {
		// Only the pair's own query discards its connection, and nothing else removes it meanwhile, so
		// this is defensive.
		return nullptr;
	}
	auto discarded = std::move(entry->second.connection);
	if (entry->second.pins == 0) {
		connections.erase(entry);
	}
	return discarded;
}

//===--------------------------------------------------------------------===//
// RedshiftConnectionPin
//===--------------------------------------------------------------------===//

unique_ptr<RedshiftConnectionPin> RedshiftConnectionPin::TryPin(ClientContext &context,
                                                                shared_ptr<AttachedDatabase> db) {
#ifdef REDSHIFT_TEST_HOOKS
	PinPause().Run();
#endif
	const auto connection_id = RedshiftClientState::Get(context).ConnectionId();
	// Allocated before the map counts the pin, so nothing that can throw comes between the count
	// and the object whose destructor undoes it.
	auto pin = make_uniq<RedshiftConnectionPin>(std::move(db), connection_id);
	if (!pin->Catalog().PinConnection(connection_id)) {
		// Destroyed unpinned, so it releases nothing.
		return nullptr;
	}
	pin->pinned = true;
	return pin;
}

RedshiftConnectionPin::RedshiftConnectionPin(shared_ptr<AttachedDatabase> db_p, connection_t connection_id_p)
    : db(std::move(db_p)), connection_id(connection_id_p) {
}

RedshiftConnectionPin::~RedshiftConnectionPin() {
	// Before `db` lets the handle go: the pin must end while the catalog, and its connection map,
	// still exist. A use ends before its pin, while the pin still keeps the pair's entry.
	EndUse();
	if (pinned) {
		Catalog().UnpinConnection(connection_id);
	}
}

bool RedshiftConnectionPin::BeginUse(RedshiftConnectionUse use) {
	D_ASSERT(pinned && !in_use);
	in_use = Catalog().BeginConnectionUse(connection_id, use);
	return in_use;
}

void RedshiftConnectionPin::EndUse() {
	if (!in_use) {
		return;
	}
	in_use = false;
	Catalog().EndConnectionUse(connection_id);
}

RedshiftCatalog &RedshiftConnectionPin::Catalog() const {
	return db->GetCatalog().Cast<RedshiftCatalog>();
}

} // namespace duckdb
