//===----------------------------------------------------------------------===//
//                         DuckDB - Redshift
//
// src/transport/odbc/trust_store.hpp
//
// Which CA certificates the connection's TLS is verified against. The host can change which roots
// are trusted, through SSL_CERT_FILE, but not whether certificates are checked.
// Linux and macOS share trust_store_search.cpp, each giving its own SystemCaBundles(); Windows
// exports its root store (trust_store_windows.cpp); any other platform links
// trust_store_unsupported.cpp. sources.cmake chooses.
//===----------------------------------------------------------------------===//

#pragma once

#include "duckdb/common/string.hpp"
#include "redshift_transport.hpp"

namespace duckdb {

//! The resolved CA bundle. One path rather than one per driver option, so that both trust options
//! are given the same source.
struct TrustSource {
	//! A readable file whose path the driver can be given as written. On Windows, empty when the
	//! store's export is used, until PlaceTrustSource writes it.
	string path;
#ifdef _WIN32
	//! The Windows root store's export, as PEM, when SSL_CERT_FILE chose no file. Its folder comes
	//! from the driver source, so the file is written after it.
	string export_content;
#endif
};

//! Reads this platform's trust source from the host on every call. Call it at connect, not at load,
//! so a host without a CA bundle can still load the extension. Throws IOException
//! when nothing resolves, naming the mechanism and the locations tried, when the
//! bundle the host chose cannot be used, or when the platform has no resolver.
TrustSource ResolveTrustSource(RedshiftLogSink &log_sink);

} // namespace duckdb
