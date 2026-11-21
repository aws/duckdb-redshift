//===----------------------------------------------------------------------===//
//                         DuckDB - Redshift
//
// redshift_query.hpp
//
// redshift_query('<handle>', '<Redshift SQL>' [, params := [...]]).
//===----------------------------------------------------------------------===//

#pragma once

#include "duckdb/common/common.hpp"

namespace duckdb {

class ExtensionLoader;
class RedshiftCatalog;
class TableRef;

//! Registers redshift_query and its same-handle statement fence, which DuckDB runs on every statement
//! it binds.
void RegisterRedshiftQuery(ExtensionLoader &loader);

//! The query the DuckDB CLI's default prompt sends after every statement while CONNECT-ed, for its
//! current_database_and_schema setting (DuckDB v2.0.1, tools/shell/shell_prompt.cpp,
//! Prompt::HandleSetting), exactly as the shell joins its two string literals. Redshift declares
//! its column shorter than its value (varchar(4) for `dev.public`), so the client refuses it and
//! the prompt shows #ERROR. test_connect_prompt.cpp runs the shell's own prompt code and fails when
//! the shell sends any other text.
constexpr const char *DUCKDB_SHELL_PROMPT_QUERY = "SELECT CASE WHEN current_schema() = 'main' THEN current_database() "
                                                  "ELSE current_database() || '.' || current_schema() END";

//! What CONNECT sends Redshift for DUCKDB_SHELL_PROMPT_QUERY: the same value, cast to the longest
//! VARCHAR, so Redshift declares a length no value exceeds and the client reads it whole, and named
//! as the uncast column is, so a user who types the query sees the same column.
constexpr const char *DUCKDB_SHELL_PROMPT_QUERY_SENT =
    "SELECT (CASE WHEN current_schema() = 'main' THEN current_database() "
    "ELSE current_database() || '.' || current_schema() END)::varchar(65535) AS \"case\"";

//! The text CONNECT sends Redshift for `sql`, a statement DuckDB handed it: `sql` itself, except
//! DUCKDB_SHELL_PROMPT_QUERY, byte for byte, which becomes DUCKDB_SHELL_PROMPT_QUERY_SENT.
string ConnectedStatementText(const string &sql);

//! The redshift_query call that runs `sql`, a statement typed while CONNECT-ed to `catalog`'s handle,
//! on that handle and no other, with messages written for the user who typed it. It sends
//! ConnectedStatementText(sql). For RedshiftCatalog::RemoteExecute.
unique_ptr<TableRef> MakeConnectedQuery(RedshiftCatalog &catalog, const string &sql);

} // namespace duckdb
