//===----------------------------------------------------------------------===//
//                         DuckDB - Redshift
//
// src/transport/odbc/trust_store_unsupported.cpp
//
// ResolveTrustSource for a platform with no trust resolver. The build links, as
// src/transport/driver/driver_not_built.cpp lets a platform without a driver loader link, and a
// connection fails closed rather than trusting the driver's own choice of CA file.
//===----------------------------------------------------------------------===//

#include "odbc/trust_store.hpp"

#include "duckdb/common/exception.hpp"

namespace duckdb {

TrustSource ResolveTrustSource(RedshiftLogSink &) {
	throw IOException("This build of the Redshift extension has no way to find this operating system's CA "
	                  "certificates, so it cannot verify a Redshift connection. Use a published release of the "
	                  "extension on a platform it supports");
}

} // namespace duckdb
