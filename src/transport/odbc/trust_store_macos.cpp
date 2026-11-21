#include "odbc/trust_store_search.hpp"

namespace duckdb {

//! The system CA bundle, also the default the locked macOS driver's libcurl was built with (its
//! strings). Both trust options take a PEM file, and neither Redshift hop reads the Keychain.
const vector<string> &SystemCaBundles() {
	static const vector<string> BUNDLES = {"/etc/ssl/cert.pem"};
	return BUNDLES;
}

} // namespace duckdb
