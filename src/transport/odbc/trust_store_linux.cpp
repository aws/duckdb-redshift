#include "odbc/trust_store_search.hpp"

namespace duckdb {

//! The distributions' CA bundle locations, in search order.
const vector<string> &SystemCaBundles() {
	static const vector<string> BUNDLES = {
	    "/etc/pki/tls/certs/ca-bundle.crt",
	    "/etc/pki/ca-trust/extracted/pem/tls-ca-bundle.pem",
	    "/etc/ssl/certs/ca-certificates.crt",
	    "/etc/ssl/ca-bundle.pem",
	    "/etc/ssl/cert.pem",
	};
	return BUNDLES;
}

} // namespace duckdb
