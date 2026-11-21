//===----------------------------------------------------------------------===//
//                         DuckDB - Redshift
//
// storage/redshift_connection_ownership.hpp
//
// The map of the Redshift connections redshift_query keeps open; the pin a bind or an execution
// holds on a pair, whose connection a retirement leaves open until the pin ends; and when each
// connection goes away.
//===----------------------------------------------------------------------===//

#pragma once

#include "duckdb/common/common.hpp"
#include "duckdb/common/identifier.hpp"
#include "duckdb/common/map.hpp"
#include "duckdb/common/mutex.hpp"
#include "duckdb/common/optional_ptr.hpp"
#include "duckdb/common/unordered_map.hpp"
#include "duckdb/common/unordered_set.hpp"
#include "redshift_transport.hpp"

#include <atomic>
#include <condition_variable>
#include <functional>
#include <utility>

namespace duckdb {

class AttachedDatabase;
class Catalog;
class ClientContext;

//! What a pair's Redshift connection is being used for, by the bind or execution whose pin holds
//! it (RedshiftConnectionPin::BeginUse). The client takes one call at a time and keeps one result
//! open per connection. A bind runs on the thread that called DuckDB, but an execution can run on a
//! DuckDB worker thread while that thread is free, as later parts of a streamed statement do, so the map
//! orders the two.
enum class RedshiftConnectionUse : uint8_t {
	//! Nothing uses the connection.
	NONE,
	//! A bind, for its Describe.
	DESCRIBE,
	//! An execution, from just before its Execute until its result ends: drained, failed or
	//! destroyed.
	RESULT
};

//! Maps (DuckDB connection ID, attach handle ID) -> Redshift connection, for one DuckDB database.
//!
//! The ownership model in short. Each pair owns at most one Redshift connection, opened at the ATTACH
//! for the attaching DuckDB connection's pair and on first use for any other, and kept idle between
//! queries, so temporary tables, SET values and open transactions stay
//! visible. It closes when its DuckDB connection closes, or is discarded
//! after a transport failure, so the pair's next use opens a new one. A bind or an execution pins
//! its pair while it uses it (RedshiftConnectionPin), and the pin also keeps the handle alive.
//! DETACH, ATTACH OR REPLACE, or the rollback of the transaction that attached the handle, retires
//! it, a tombstone kept in `retired_handles`: each idle pair closes at once, each busy one when its
//! last pin ends, and no pair opens a new connection. A bystander whose closed session may have held
//! state gets one record, which its next bind on the name reports. The tombstone goes when the last
//! reference to the handle does, which destroys its catalog. Keys are IDs, never addresses.
//!
//! A Redshift connection leaves the map exactly once, through CloseConnection, CloseHandle,
//! RetireHandle, RetireRolledBackHandle, Unpin, TakeLostSession or Discard, as
//! redshift_connection_ownership.cpp describes. Each takes it out under connections_lock, and the
//! connection is destroyed after the lock is released, because the client's destructor can block on
//! the network. A connection is opened outside the lock too, so the lock is held
//! only for a map operation. The one client call made under it is SessionHoldsState(), whose answer
//! is a value in memory.
//!
//! Handle lifetime. An attach handle goes through these stages; the map makes each change it makes
//! in them under connections_lock:
//!
//! 1. Attached. The catalog's constructor registers it (RegisterHandle, `handles`), the attaching
//!    DuckDB connection's pair opens its connection (RedshiftCatalog::OpenAttachingConnection, under a
//!    pin), and that DuckDB connection's RedshiftClientState notes it, in case its transaction rolls
//!    back. An open that fails fails the ATTACH, and the catalog's destructor forgets the handle.
//! 2. In use. A bind or an execution finds the handle in DuckDB's database list and pins its pair
//!    (Pin): the pin counts on the pair's entry, and a RedshiftConnectionPin holds the catalog alive
//!    meanwhile. A pair with no connection yet opens it on first use; each stays, idle, between pins.
//! 3. Removed. DETACH, ATTACH OR REPLACE, or the rollback of the transaction that attached it takes
//!    the handle out of DuckDB's list. DuckDB does that first and tells this extension after
//!    (Catalog::OnDetach, or the rollback's TransactionRollback hook), so for a moment the handle is
//!    out of the list but not retired: a pin is still granted then, and its call runs as one already
//!    in flight; a bind on the name in that moment finds its own idle session on the handle through
//!    TakeLostSession, by the name the handle has now, which it reads from `handles`. DuckDB undoes a
//!    rolled-back ATTACH only while its name still refers to that handle, so another DuckDB
//!    connection's rollback leaves a newer handle under the name alone; a handle DuckDB removes and
//!    tells this extension nothing of is never retired, and goes straight to stage 5.
//! 4. Retired (RetireHandle or RetireRolledBackHandle, `retired_handles`). From here on no pin is
//!    granted, so a call that found the handle before the removal is refused; a pinned pair opens
//!    no new connection; and each pair no pin holds closes now, each pinned one when its last pin
//!    ends. Each session closed for a DuckDB connection other than the detaching one, and that may
//!    have held state, is recorded (`lost_sessions`) under the handle's name. Pin and retirement
//!    take the same lock, so either the pin comes first and the close waits for it, or the
//!    retirement comes first and the pin is refused. The catalog can outlive this stage, held by a
//!    pin or by something DuckDB keeps, such as a transaction that looked the name up as a catalog.
//! 5. Destroyed. Once nothing holds the catalog, its destructor calls CloseHandle, which forgets the
//!    handle in `handles` and `retired_handles`, and closes any connection still left: as the database
//!    closes, or on a handle DuckDB removed and told this extension nothing of (stage 3), whose
//!    sessions it records as stage 4 does, for every DuckDB connection, since none is known to have
//!    removed it. Nothing can pin a
//!    handle whose catalog is gone, and IDs are never reused.
//! 6. Reported. A record in `lost_sessions` outlives the handle: the DuckDB connection's next bind on
//!    the name takes it, once, or it goes when that DuckDB connection closes.
class RedshiftConnectionMap {
public:
	//! An ID for a new attach handle. IDs are never reused, so a handle attached again under an old
	//! name never reaches the old handle's Redshift connections.
	idx_t NewHandleId();
	//! A new handle ID for `catalog`. The map knows the catalog by it until CloseHandle, so it can tell
	//! which handles DuckDB lists (ListedHandleIds) and read the name a handle has after DuckDB took it
	//! out of the database list (TakeLostSession, RetireRolledBackHandle, CloseHandle).
	idx_t RegisterHandle(const Catalog &catalog);

	//! Pins (`connection_id`, `handle_id`) for one bind or one execution, so its connection stays
	//! open until Unpin, even if the handle is retired meanwhile. Returns false, pinning
	//! nothing, once the handle has been retired.
	bool Pin(connection_t connection_id, idx_t handle_id);
	//! Ends a pin that Pin granted. The pair's last pin to end closes its connection when the handle
	//! has been retired meanwhile.
	void Unpin(connection_t connection_id, idx_t handle_id);

	//! Starts `use` of the pinned pair's connection: DESCRIBE only when nothing uses it, RESULT also
	//! once a bind's Describe ends. Returns false, starting nothing, otherwise.
	bool BeginUse(connection_t connection_id, idx_t handle_id, RedshiftConnectionUse use);
	//! Ends the use BeginUse started, and wakes an execution waiting for it.
	void EndUse(connection_t connection_id, idx_t handle_id);

	//! The Redshift connection of (`connection_id`, `handle_id`), opened through
	//! OpenRedshiftConnection with `config` when the pair has none: at the ATTACH, for the attaching
	//! DuckDB connection's pair, on another pair's first use, and on a pair's next use after Discard.
	//! Only the pair's DuckDB connection calls this, from the ATTACH or the query that uses the
	//! handle, holding a pin on the pair. `opened`, if given, is set to whether this call opened it.
	//!
	//! Null when the pair has none and the handle has been retired: a pin taken before the retirement
	//! lets work on an open connection finish, but opens no new one, and a connection still opening
	//! when the handle was retired is destroyed unused.
	optional_ptr<RedshiftConnection> GetConnection(connection_t connection_id, idx_t handle_id,
	                                               const RedshiftConfig &config, ClientContext &context,
	                                               optional_ptr<bool> opened = nullptr);

	//! Retires the handle, which DETACH or ATTACH OR REPLACE by DuckDB connection `detaching` has
	//! removed from under the name `name`: grants no pin on it from now on, destroys each of its
	//! Redshift connections that no pin holds, and leaves each other for its last pin to close.
	//! Closes nothing twice: a pair it closes leaves the map.
	//!
	//! For each other DuckDB connection whose connection it closes, now or at its last pin, records
	//! under `name` that the session may have held state, when the client's SessionHoldsState()
	//! answers so just before the close, for that DuckDB connection's next statement on the name
	//! (TakeLostSession). The detaching connection gets no record: it ended its session itself.
	//! A session that held no state needs no record.
	void RetireHandle(idx_t handle_id, const Identifier &name, connection_t detaching);
	//! Retires the handle, as RetireHandle does, because the transaction of DuckDB connection
	//! `rolling_back`, which attached it, has rolled back, and DuckDB has taken it out of the list
	//! without OnDetach. That transaction still holds the catalog. Does nothing when the handle is
	//! retired already, or its catalog is gone.
	void RetireRolledBackHandle(idx_t handle_id, connection_t rolling_back);
	//! Whether the handle has been retired.
	bool IsRetired(idx_t handle_id);
	//! The IDs of the handles whose attached databases are among `databases`, a copy of DuckDB's
	//! database list. Those are compared by address, never read: a thread's reads of a listed
	//! database's catalog through a copy it then drops are not ordered before
	//! AttachedDatabase::Cleanup, which InvokeCloseIfLastReference runs on another thread after a
	//! relaxed use_count() load (at a DETACH, a transaction's end or a CONNECT-routed statement).
	//! Only the registered handles' catalogs are read (`handles`).
	unordered_set<idx_t> ListedHandleIds(const vector<shared_ptr<AttachedDatabase>> &databases);
	//! Takes out, for DuckDB connection `connection_id`'s statement on the name `name`, whether a
	//! session it had on an earlier handle under the name may have held state that is now lost, so
	//! that it is reported once. Two kinds count:
	//!
	//! - a record RetireHandle, RetireRolledBackHandle, Unpin or CloseHandle made;
	//! - an idle session on a handle DuckDB has already taken out of its database list, which
	//!   `listed`, the IDs of the Redshift handles in the list now, does not hold, but which nothing
	//!   has retired: DuckDB changes the list before it calls OnDetach, and before a rollback's hook
	//!   (stage 3). The name compared is the one that handle has now. That session closes here, so a
	//!   later retirement, if any, records nothing more.
	bool TakeLostSession(connection_t connection_id, const Identifier &name, const unordered_set<idx_t> &listed);
	//! Destroys every Redshift connection of the handle, which has gone: its catalog is being destroyed,
	//! after DETACH, ATTACH OR REPLACE or a rolled-back ATTACH, or as the database closes, and the map
	//! forgets the handle. A handle nothing retired, which the rollback of another DuckDB connection's
	//! ATTACH under its name can remove, has its sessions recorded as RetireHandle records them, for
	//! every DuckDB connection.
	void CloseHandle(idx_t handle_id);
	//! Destroys every Redshift connection of the DuckDB connection, which has closed,
	//! and forgets what was recorded for it.
	void CloseConnection(connection_t connection_id);
	//! Takes `connection`, the Redshift connection of (`connection_id`, `handle_id`), out of the map,
	//! and returns it for the caller to destroy now that the lock is released. For a connection the
	//! client has closed after a transport failure, so the pair's next use opens a
	//! new one. Returns null if the pair's connection is no longer `connection`. The pair's pin, if
	//! any, stays.
	unique_ptr<RedshiftConnection> Discard(connection_t connection_id, idx_t handle_id,
	                                       const RedshiftConnection &connection);

private:
	//! (DuckDB connection ID, attach handle ID).
	using Key = std::pair<connection_t, idx_t>;
	//! A pair's entry: its connection, null until one is opened or after a discard; how many pins
	//! hold it; and what one of them is using the connection for. An entry with neither a connection
	//! nor a pin leaves the map. Only a pinned call starts a use, so a pair in use always has an entry.
	struct Entry {
		unique_ptr<RedshiftConnection> connection;
		idx_t pins = 0;
		RedshiftConnectionUse use = RedshiftConnectionUse::NONE;
	};

	//! A retired handle: the name it had, and the DuckDB connection that removed it, which gets no
	//! record. For a handle CloseHandle finds that nothing retired, no DuckDB connection is known to
	//! have removed it, and `detaching` is NO_DETACHING_CONNECTION, an ID no DuckDB connection has.
	struct RetiredHandle {
		Identifier name;
		connection_t detaching;
	};
	//! A session that may have held state, closed by a retirement, Unpin or CloseHandle for a DuckDB
	//! connection other than the one that removed the handle, under the name its handle had.
	struct LostSession {
		connection_t connection_id;
		Identifier name;
	};

	//! Retires the handle as `retired` says, under connections_lock: RetireHandle's and
	//! RetireRolledBackHandle's work, with the connections it takes out added to `removed`, for the
	//! caller to destroy after the lock.
	void RetireLocked(idx_t handle_id, const RetiredHandle &retired, vector<unique_ptr<RedshiftConnection>> &removed);
	//! Records, for a connection that is about to close, of the handle `retired` describes (retired,
	//! or for CloseHandle gone without a retirement), that its session may have held state, when its
	//! client says so, unless `key`'s DuckDB connection is the one that removed the handle
	//! (`retired.detaching`). Called under connections_lock, before the connection is destroyed.
	//! Never throws, since Unpin and CloseHandle call it from destructors, a pin's and a catalog's:
	//! an answer the client cannot give counts as possible state, and a record that cannot be stored
	//! is dropped.
	void RecordLostSession(const Key &key, const RedshiftConnection &connection, const RetiredHandle &retired) noexcept;

	//! Atomic, so handing out an ID needs no lock.
	std::atomic<idx_t> next_handle_id {1};
	//! Guards `connections`, `handles`, `retired_handles` and `lost_sessions`.
	mutex connections_lock;
	//! Signalled, with connections_lock, when a use ends, for an execution waiting in BeginUse.
	std::condition_variable use_ended;
	//! Owns each pair's Redshift connection. A connection leaves only through CloseHandle,
	//! CloseConnection, RetireHandle, RetireRolledBackHandle, Unpin, TakeLostSession or Discard, which
	//! hand its ownership to the caller to destroy outside the lock.
	map<Key, Entry> connections;
	//! Every live handle's catalog, by handle ID, from its construction to its destruction (stages 1 to
	//! 5), so the map can tell which handles DuckDB lists, and read the name a handle has once DuckDB
	//! has taken it out of its list and only its ID is left: ListedHandleIds compares each catalog's
	//! database with the listed ones, TakeLostSession matches a session by its name in stage 3, the
	//! rollback hook retires under it, and CloseHandle records under it for a handle nothing retired.
	//! A stored name would go stale on ALTER DATABASE ... SET ALIAS, which no hook reports. Not owned,
	//! so it keeps no detached catalog alive: the destructor removes its entry under connections_lock,
	//! so a pointer read under the lock is live, and a name is read only from a handle out of the list,
	//! which nothing can rename. A live catalog means a live database, so an address equal to a listed
	//! one is never a reused one.
	unordered_map<idx_t, const Catalog *> handles;
	//! The retired handles whose catalogs still exist (stage 4), by handle ID, with the name each had
	//! and the DuckDB connection that removed it. Pin refuses a handle here, GetConnection opens no
	//! connection on one, and Unpin closes, rather than keeps, a pair's connection when its last pin
	//! ends, recording the session under this name for every DuckDB connection but the remover. It is
	//! the only way a call that already holds the catalog learns of the removal: DuckDB does not tell
	//! a holder that its database left the list.
	unordered_map<idx_t, RetiredHandle> retired_handles;
	//! The possible-state-loss reports not yet given: a DuckDB connection whose session on a handle
	//! under this name may have held state when a retirement, Unpin or CloseHandle closed it. Kept
	//! here, not in that DuckDB connection's RedshiftClientState, because another DuckDB connection's
	//! DETACH, ATTACH OR REPLACE, rollback or catalog destruction can write it, and it holds neither the
	//! catalog nor the connection, so it outlives both. At most one per DuckDB connection and name.
	vector<LostSession> lost_sessions;
};

class RedshiftCatalog;

//! A bind's or an execution's hold on an attach handle: it keeps the handle's catalog alive, and
//! the DuckDB connection's Redshift connection on the handle open, until it is destroyed, even if
//! the handle is retired meanwhile. Destroy any result the connection gave first. Nothing else
//! holds the handle for redshift_query: neither the query's transaction nor the bind data.
class RedshiftConnectionPin {
public:
	//! A pin on `context`'s pair on the handle `db`, which must be a Redshift handle, or null when
	//! the handle has been retired since `db` was found.
	//!
	//! DuckDB takes the name out of its database list before it calls OnDetach or the rollback
	//! hook, which retires the handle, so a pin taken between the two is granted, and its call runs
	//! as one already in flight. Checking the list again after the pin, and ending the pin when the
	//! handle has left it, would move this cut-off back to the name's removal. That is not done:
	//! Only the order of the pin and the retirement matters.
	static unique_ptr<RedshiftConnectionPin> TryPin(ClientContext &context, shared_ptr<AttachedDatabase> db);
	//! Holds `db` for DuckDB connection `connection_id`, not yet pinned: only TryPin pins it.
	RedshiftConnectionPin(shared_ptr<AttachedDatabase> db, connection_t connection_id);
	//! Ends the pin, if TryPin took one, and any use it started, which closes the pair's connection
	//! when the handle was retired during it, and then lets the handle go.
	~RedshiftConnectionPin();

	//! Starts `use` of the pinned pair's connection (RedshiftConnectionMap::BeginUse), which EndUse,
	//! or this pin's destruction, ends. Returns false, starting nothing, when another call is using it.
	bool BeginUse(RedshiftConnectionUse use);
	//! Ends the use BeginUse started, if it is still on; does nothing otherwise.
	void EndUse();

	//! The handle's catalog, which this pin keeps alive.
	RedshiftCatalog &Catalog() const;

private:
	//! Keeps the catalog, and with it the connection map, alive until the pin has ended.
	shared_ptr<AttachedDatabase> db;
	connection_t connection_id;
	//! Whether the connection map counted this pin, which the destructor then releases.
	bool pinned = false;
	//! Whether this pin started a use of the pair's connection that has not ended.
	bool in_use = false;
};

#ifdef REDSHIFT_TEST_HOOKS
//! Test-only, compiled into mock builds alone and reachable only from C++: runs `pause` on every
//! RedshiftConnectionPin::TryPin, after the caller found the handle and before it asks for the pin,
//! so a race test can hold a bind or an execution there while another DuckDB connection detaches or
//! replaces the handle. An empty function removes it.
void SetRedshiftPinPauseForTesting(std::function<void()> pause);
//! Test-only, as above: runs `pause` on every RedshiftConnectionMap::TakeLostSession, before it takes
//! the map's lock, so a race test can hold a bind between its first lookup of the name and that check
//! while another DuckDB connection detaches the handle. An empty function removes it.
void SetRedshiftLostSessionPauseForTesting(std::function<void()> pause);
#endif

} // namespace duckdb
