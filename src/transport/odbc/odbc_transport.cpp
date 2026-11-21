//===----------------------------------------------------------------------===//
// OpenRedshiftConnection for REDSHIFT_CLIENT=odbc (the ODBC client).
//
// The connection obtains the driver from the process's driver source when it opens, not at
// extension LOAD, and reaches it through LoadedDriver::api.
//===----------------------------------------------------------------------===//

#include "odbc/odbc_connection.hpp"
#include "redshift_transport.hpp"

namespace duckdb {

unique_ptr<RedshiftConnection> OpenRedshiftConnection(const RedshiftConfig &config,
                                                      optional_ptr<const RedshiftAwsCredentials> credentials,
                                                      shared_ptr<RedshiftLogSink> log_sink) {
	return make_uniq<OdbcConnection>(config, std::move(log_sink), credentials);
}

} // namespace duckdb
