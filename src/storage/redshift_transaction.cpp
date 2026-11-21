//===----------------------------------------------------------------------===//
// The DuckDB transaction manager of a Redshift attached database.
//
// Why it exists. Every DuckDB attached database has a transaction manager: a storage extension
// supplies one through create_transaction_manager, next to its catalog, and DuckDB cannot attach
// without one. The handle has nothing transactional to manage, but DuckDB still drives this one
// for every DuckDB connection that uses the handle:
//   * Each DuckDB connection's MetaTransaction calls StartTransaction the first time a DuckDB
//     transaction touches the handle -- every catalog lookup does, including the ones
//     RedshiftCatalog refuses -- and keeps only a reference to the Transaction it gets back.
//   * DuckDB writes the active query into that Transaction and passes it to whoever asks for the
//     connection's transaction on this database, such as CatalogTransaction.
//   * When the statement ends, or the explicit transaction, DuckDB hands the same reference back
//     to CommitTransaction or RollbackTransaction, exactly once, and never uses it again.
//
// What follows from that:
//   * The manager owns each Transaction from start to commit or rollback, because DuckDB holds
//     only a reference. Several DuckDB connections can do this at once, so ownership lives in a
//     map under a lock.
//   * Nothing here may throw. StartTransaction runs before every lookup, so a refusal here would
//     replace RedshiftCatalog's own message, and a failed commit or rollback would fail every
//     statement and explicit transaction that touched the handle.
//   * Nothing here does Redshift work. A DuckDB COMMIT or ROLLBACK is never sent to Redshift: a
//     Redshift transaction is only ever SQL a user sends through redshift_query, which the
//     extension refuses inside an explicit DuckDB transaction, because DuckDB cannot govern it, unless the
//     user turns on the option and accepts that.
//===----------------------------------------------------------------------===//

#include "storage/redshift_transaction.hpp"

namespace duckdb {

RedshiftTransactionManager::RedshiftTransactionManager(AttachedDatabase &db) : TransactionManager(db) {
}

Transaction &RedshiftTransactionManager::StartTransaction(ClientContext &context) {
	auto transaction = make_uniq<Transaction>(*this, context);
	auto &result = *transaction;
	lock_guard<mutex> guard(transaction_lock);
	transactions[result] = std::move(transaction);
	return result;
}

ErrorData RedshiftTransactionManager::CommitTransaction(ClientContext &context, Transaction &transaction) {
	RemoveTransaction(transaction);
	return ErrorData();
}

void RedshiftTransactionManager::RollbackTransaction(Transaction &transaction) {
	RemoveTransaction(transaction);
}

void RedshiftTransactionManager::Checkpoint(ClientContext &context, bool force) {
	// Nothing is stored locally, so there is nothing to checkpoint.
}

void RedshiftTransactionManager::RemoveTransaction(Transaction &transaction) {
	lock_guard<mutex> guard(transaction_lock);
	transactions.erase(transaction);
}

} // namespace duckdb
