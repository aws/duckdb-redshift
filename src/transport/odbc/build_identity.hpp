//===----------------------------------------------------------------------===//
//                         DuckDB - Redshift
//
// src/transport/odbc/build_identity.hpp
//
// What this build is, for the ApplicationName every connection sends. Its own file, so the
// connection settings stay pure and only this one includes DuckDB's database header.
//===----------------------------------------------------------------------===//

#pragma once

#include "duckdb/common/string.hpp"

namespace duckdb {

//! The extension's version, empty when the build sets none, and the version and platform of the
//! DuckDB it is built with, which LOAD checks against the DuckDB that loads it.
struct BuildIdentity {
	string extension_version;
	string duckdb_version;
	string platform;
};

BuildIdentity ThisBuild();

} // namespace duckdb
