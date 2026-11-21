//===----------------------------------------------------------------------===//
//                         DuckDB - Redshift
//
// storage/redshift_transaction.hpp
//
// The DuckDB transaction manager of a Redshift attached database: local scaffolding only.
//===----------------------------------------------------------------------===//

#pragma once

#include "duckdb/common/mutex.hpp"
#include "duckdb/common/reference_map.hpp"
#include "duckdb/transaction/transaction.hpp"
#include "duckdb/transaction/transaction_manager.hpp"

namespace duckdb {

//! Hands DuckDB the per-connection Transaction objects it requires for an attached database, and
//! owns each one until DuckDB commits or rolls it back. It sends no transaction control to
//! Redshift: DuckDB commit and rollback do no Redshift work, and a Redshift transaction is only
//! ever the one a user sends as SQL through redshift_query. See redshift_transaction.cpp for why
//! DuckDB needs it and what DuckDB does with each Transaction.
class RedshiftTransactionManager : public TransactionManager {
public:
	explicit RedshiftTransactionManager(AttachedDatabase &db);

	Transaction &StartTransaction(ClientContext &context) override;
	ErrorData CommitTransaction(ClientContext &context, Transaction &transaction) override;
	void RollbackTransaction(Transaction &transaction) override;
	void Checkpoint(ClientContext &context, bool force = false) override;

private:
	//! Ends `transaction`: the only work commit and rollback do.
	void RemoveTransaction(Transaction &transaction);

	//! Guards `transactions`; held only for the map operation.
	mutex transaction_lock;
	reference_map_t<Transaction, unique_ptr<Transaction>> transactions;
};

} // namespace duckdb
