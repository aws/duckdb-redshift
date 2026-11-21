// The connect error for a workgroup that may still be resuming from idle suspension: what it says
// about the attempts made and the time they took, and that a cause the user can
// fix still wins over it. Every expected message is written out here.

// catch.hpp comes first, as the extension's C++ tests include it: DuckDB's copy declares the
// duckdb_base_std aliases it needs only while no DuckDB header has opened that namespace.
#include "catch.hpp"

#include "diagnostic_fixtures.hpp"
#include "duckdb/common/exception.hpp"
#include "odbc/odbc_errors.hpp"
#include "thrown.hpp"

#include <string>
#include <vector>

using duckdb::ExceptionType;
using duckdb::redshift_driver::testing::FakeDiag;
using redshift_test::Capture;
using redshift_test::ConnectRecords;
using redshift_test::Thrown;

namespace {

//! What ThrowConnectFailure throws for a connect that failed with `diags` after `attempts`
//! attempts over `elapsed_seconds`.
Thrown ConnectFailure(std::vector<FakeDiag> diags, duckdb::idx_t attempts, duckdb::idx_t elapsed_seconds) {
	const auto records = ConnectRecords(SQL_ERROR, std::move(diags));
	const duckdb::ConnectFailureContext context {"wg.example",
	                                             {"workgroup", true, "Grant it access.", ""},
	                                             duckdb::redshift_driver::DriverIdentity(),
	                                             attempts,
	                                             elapsed_seconds};
	return Capture([&]() { duckdb::ThrowConnectFailure(records, context); });
}

const std::string HINT =
    "It may still be resuming from idle suspension, which can take a few minutes; retry. If it keeps "
    "timing out, the workgroup may not be publicly reachable from this host's network.";

} // namespace

TEST_CASE("A resume timeout reports the attempts made and the seconds they took", "[odbc]") {
	CHECK(ConnectFailure({{"HY000", 0, "timeout expired"}}, 1, 61) ==
	      Thrown {ExceptionType::IO, "Could not connect to Redshift at wg.example: the Redshift Serverless workgroup "
	                                 "did not answer within 61 seconds over 1 connect attempt.\n" +
	                                     HINT + "\nSQLSTATE: HY000"});
	CHECK(ConnectFailure({{"HYT00", 0, "Timeout"}}, 3, 181) ==
	      Thrown {ExceptionType::IO, "Could not connect to Redshift at wg.example: the Redshift Serverless workgroup "
	                                 "did not answer within 181 seconds over 3 connect attempts.\n" +
	                                     HINT + "\nSQLSTATE: HYT00"});
}

TEST_CASE("A resume timeout of one second reads in the singular", "[odbc]") {
	CHECK(ConnectFailure({{"HYT01", 0, "Connection timeout"}}, 2, 1) ==
	      Thrown {ExceptionType::IO, "Could not connect to Redshift at wg.example: the Redshift Serverless workgroup "
	                                 "did not answer within 1 second over 2 connect attempts.\n" +
	                                     HINT + "\nSQLSTATE: HYT01"});
}

TEST_CASE("A credentials record beside a timeout gives the credentials message, without the attempts", "[odbc]") {
	CHECK(ConnectFailure({{"HY000", 0, "timeout expired"}, {"HY000", 0, "No AWS profile found: p"}}, 3, 181) ==
	      Thrown {ExceptionType::IO,
	              "Could not connect to Redshift at wg.example: no usable AWS credentials were found through the AWS "
	              "credential chain.\nCheck the credentials this process can reach: the default AWS credential chain, "
	              "or the profile PROFILE names.\nSQLSTATE: HY000"});
}
