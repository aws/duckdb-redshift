//===----------------------------------------------------------------------===//
//                         DuckDB - Redshift
//
// src/transport/odbc/trust_store_cert_file.hpp
//
// The SSL_CERT_FILE step every trust resolver starts with. Each platform checks the file its own
// way and passes what it found, so the rules for using or refusing the host's choice are one.
//===----------------------------------------------------------------------===//

#pragma once

#include "duckdb/common/optional.hpp"
#include "duckdb/common/string.hpp"
#include "odbc/trust_store.hpp"

#include <cstdint>

namespace duckdb {

//! The variable a process sets to choose which roots are trusted. The driver loader's environment
//! list records it as overridden by the CaFile keyword, with this step as its first trust source.
extern const char *const TRUST_VARIABLE;

//! What this platform's file checks found at SSL_CERT_FILE's path.
enum class CertFileState : uint8_t {
	//! Missing, or this process may not read it: passed over.
	UNREADABLE,
	//! Readable, but an empty file, a directory or a special file.
	NOT_A_BUNDLE,
	//! A readable, non-empty regular file.
	BUNDLE,
	//! On Windows, an open that failed for another reason, such as a program holding it unshared.
	OPEN_FAILED
};

//! SSL_CERT_FILE's `path` as the trust source, or nullopt when `state` is UNREADABLE, which the
//! caller logs and passes over. Any other file is the host's choice of roots, so one that cannot be
//! used is refused with IOException naming SSL_CERT_FILE and never the path.
optional<TrustSource> UseCertFile(const string &path, CertFileState state);

} // namespace duckdb
