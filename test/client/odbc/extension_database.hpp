//===----------------------------------------------------------------------===//
//                         DuckDB - Redshift
//
// test/client/odbc/extension_database.hpp
//
// An in-memory DuckDB database with the extension loaded, for the Catch2 tests in this directory
// that run the extension's SQL against the fake driver, or the embedded driver in a [live] case.
//===----------------------------------------------------------------------===//

#pragma once

// catch.hpp comes first, as in this directory's tests.
#include "catch.hpp"

#include "duckdb.hpp"
#include "redshift_extension.hpp"

#include <string>
#include <vector>

namespace redshift_test {

//! redshift_query reaches Redshift through an attach handle; attaching opens the attaching DuckDB
//! connection's session. This attaches `rs` to the account-backed Serverless workgroup `workgroup` of
//! `account_id` in `region`.
inline std::string AttachRs(const std::string &region, const std::string &workgroup, const std::string &account_id) {
	return "ATTACH '' AS rs (TYPE redshift, REGION '" + region +
	       "', CREDENTIAL_PROVIDER aws_default_chain, WORKGROUP '" + workgroup + "', ACCOUNT_ID '" + account_id + "')";
}

//! The same for the provisioned cluster `cluster` in `region`.
inline std::string AttachRsCluster(const std::string &region, const std::string &cluster) {
	return "ATTACH '' AS rs (TYPE redshift, REGION '" + region + "', CREDENTIAL_PROVIDER aws_default_chain, CLUSTER '" +
	       cluster + "')";
}

//! The attach the offline tests use.
inline const std::string ATTACH_RS = AttachRs("us-east-1", "analytics", "012345678901");

//! The first column of `result`'s rows, through strings: the tests name none of DuckDB's value types.
inline std::vector<std::string> FirstColumn(duckdb::QueryResult &result) {
	std::vector<std::string> cells;
	while (auto chunk = result.Fetch()) {
		for (duckdb::idx_t row = 0; row < chunk->size(); row++) {
			cells.push_back(chunk->GetValue(0, row).ToString());
		}
	}
	return cells;
}

//! An in-memory database with the extension loaded. Persistent secrets are off, so a secret stored
//! in the home directory never clashes with a test's; they are turned off in SQL, before any
//! secret is used, because client code names none of DuckDB's value types.
struct ExtensionDatabase {
	ExtensionDatabase() : db(nullptr), con(db) {
		Run("SET allow_persistent_secrets = false");
		db.LoadStaticExtension<duckdb::RedshiftExtension>();
	}

	//! Runs `sql`, failing the test with DuckDB's error when it fails.
	duckdb::unique_ptr<duckdb::QueryResult> Run(const std::string &sql) {
		auto result = con.Query(sql);
		INFO(sql << ": " << (result->HasError() ? result->GetError() : ""));
		REQUIRE_FALSE(result->HasError());
		return result;
	}

	duckdb::DuckDB db;
	duckdb::Connection con;
};

} // namespace redshift_test
