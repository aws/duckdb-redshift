//===----------------------------------------------------------------------===//
//                         DuckDB - Redshift
//
// redshift_target_hook.hpp
//
// A neutral hook on target resolution: whether a Serverless WORKGROUP given without ACCOUNT_ID is
// accepted. The build compiles exactly one implementation: the default,
// src/redshift_target_hook_default.cpp, which accepts none, or, in a tree that carries one, the
// preview implementation (redshift_select_hook in CMakeLists.txt).
//===----------------------------------------------------------------------===//

#pragma once

#include "duckdb/common/common.hpp"

namespace duckdb {

//! Whether this build accepts `workgroup`, given without ACCOUNT_ID, as a Serverless workgroup.
//! Depends only on the name, so every ATTACH with that name answers alike. An accepted workgroup takes
//! no DATABASE; ValidateRedshiftOptions refuses one.
bool AcceptsAccountlessWorkgroup(const string &workgroup);

} // namespace duckdb
