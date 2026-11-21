// ResolveTrustSource on a platform without a trust resolver.

// catch.hpp comes first, as the extension's C++ tests include it: DuckDB's copy declares the
// duckdb_base_std aliases it needs only while no DuckDB header has opened that namespace.
#include "catch.hpp"

#include "duckdb/common/exception.hpp"
#include "odbc/trust_store.hpp"
#include "thrown.hpp"

#include <string>

namespace {

class DiscardingLogSink : public duckdb::RedshiftLogSink {
public:
	void Debug(const std::string &) noexcept override {
	}
};

} // namespace

TEST_CASE("Without a trust resolver for the platform, ResolveTrustSource refuses the connection", "[odbc]") {
	DiscardingLogSink log_sink;
	CHECK(redshift_test::Capture([&] { duckdb::ResolveTrustSource(log_sink); }) ==
	      redshift_test::Thrown {duckdb::ExceptionType::IO,
	                             "This build of the Redshift extension has no way to find this operating system's "
	                             "CA certificates, so it cannot verify a Redshift connection. Use a published "
	                             "release of the extension on a platform it supports"});
}
