//===----------------------------------------------------------------------===//
//                         DuckDB - Redshift
//
// src/transport/odbc/trust_store_search.hpp
//
// The POSIX search behind ResolveTrustSource that Linux and macOS share; each platform's file gives
// only its system locations. Declared apart from trust_store.hpp so no other platform sees a
// declaration without a definition, and given its inputs so a test can search files of its own.
//===----------------------------------------------------------------------===//

#pragma once

#include "duckdb/common/string.hpp"
#include "duckdb/common/vector.hpp"
#include "odbc/trust_store.hpp"

namespace duckdb {

//! This platform's system CA bundle locations, in search order. Each platform's file defines it.
const vector<string> &SystemCaBundles();

//! Whether `path` is a non-empty regular file this process may read, following symbolic links:
//! the only file the search counts, since a directory or an empty file holds no certificate. The
//! search passes over a system location that is anything else.
bool IsReadableBundle(const string &path);

//! The search: `ssl_cert_file` (SSL_CERT_FILE's value; nullptr when unset, and empty counts as
//! set), then the first readable bundle in `system_bundles`. An unreadable `ssl_cert_file` is
//! logged at debug and passed over; a readable one is the answer, or an IOException when it is no
//! bundle or a path the driver cannot be given as written. Also throws IOException when nothing
//! resolves.
TrustSource ResolveTrustSourceFrom(const char *ssl_cert_file, const vector<string> &system_bundles,
                                   RedshiftLogSink &log_sink);

} // namespace duckdb
