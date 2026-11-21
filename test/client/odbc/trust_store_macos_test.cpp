// macOS's ResolveTrustSource: SSL_CERT_FILE under the Linux rules, then /etc/ssl/cert.pem. The
// search itself is tested in trust_store_search_test.cpp. Unlike the search's cases, two of these
// need /etc/ssl/cert.pem readable, as every supported macOS ships it.

// catch.hpp comes first, as the other C++ suites include it: DuckDB's copy declares the
// duckdb_base_std aliases it needs only while no DuckDB header has opened that namespace.
#include "catch.hpp"

#include "../interface/scoped_environment_variable.hpp"
#include "duckdb/common/exception.hpp"
#include "odbc/connection_settings.hpp"
#include "odbc/trust_store_search.hpp"
#include "thrown.hpp"
#include "trust_store_test_support.hpp"

#include <string>
#include <vector>

using duckdb::ExceptionType;
using duckdb::FindUnusableTrustPathReason;
using duckdb::IsReadableBundle;
using duckdb::ResolveTrustSource;
using duckdb::SystemCaBundles;
using duckdb::redshift_driver::testing::ScopedEnvironmentVariable;
using redshift_test::Capture;
using redshift_test::Thrown;
using redshift_trust_test::BUNDLE;
using redshift_trust_test::Locations;
using redshift_trust_test::NO_BUNDLE_MESSAGE;
using redshift_trust_test::RecordingLogSink;
using redshift_trust_test::TemporaryDirectory;
using redshift_trust_test::UNREADABLE_LINE;

namespace {

const std::string SYSTEM_BUNDLE = "/etc/ssl/cert.pem";

} // namespace

TEST_CASE("The macOS system CA bundle is /etc/ssl/cert.pem alone, a path the driver can be given", "[odbc]") {
	CHECK(SystemCaBundles() == Locations({SYSTEM_BUNDLE}));
	CHECK(FindUnusableTrustPathReason(SYSTEM_BUNDLE).empty());
}

TEST_CASE("On macOS, ResolveTrustSource trusts /etc/ssl/cert.pem when SSL_CERT_FILE is unset", "[odbc]") {
	ScopedEnvironmentVariable variable("SSL_CERT_FILE", nullptr);
	REQUIRE(IsReadableBundle(SYSTEM_BUNDLE));
	RecordingLogSink log_sink;
	duckdb::TrustSource source;
	// The call stays outside CHECK, which would record a throw as a failure and swallow it.
	CHECK(Capture([&] { source = ResolveTrustSource(log_sink); }) == Thrown {});
	CHECK(source.path == SYSTEM_BUNDLE);
	CHECK(log_sink.lines.empty());
}

TEST_CASE("On macOS, a readable SSL_CERT_FILE replaces /etc/ssl/cert.pem as the trust source", "[odbc]") {
	// This checks the resolved path only. That a handshake against an unrelated CA then fails is a
	// live case.
	TemporaryDirectory directory;
	auto bundle = directory.File("unrelated.pem", BUNDLE);
	ScopedEnvironmentVariable variable("SSL_CERT_FILE", bundle.c_str());
	RecordingLogSink log_sink;
	CHECK(ResolveTrustSource(log_sink).path == bundle);
	CHECK(log_sink.lines.empty());
}

TEST_CASE("On macOS, an SSL_CERT_FILE that is no CA bundle is refused, not swapped for the system one", "[odbc]") {
	ScopedEnvironmentVariable variable("SSL_CERT_FILE", "/dev/null");
	RecordingLogSink log_sink;
	CHECK(Capture([&] { ResolveTrustSource(log_sink); }) == Thrown {ExceptionType::IO, NO_BUNDLE_MESSAGE});
	CHECK(log_sink.lines.empty());
}

TEST_CASE("On macOS, an unreadable SSL_CERT_FILE is logged, then /etc/ssl/cert.pem is the trust source", "[odbc]") {
	TemporaryDirectory directory;
	auto absent = directory.Path("absent.pem");
	ScopedEnvironmentVariable variable("SSL_CERT_FILE", absent.c_str());
	RecordingLogSink log_sink;
	CHECK(ResolveTrustSource(log_sink).path == SYSTEM_BUNDLE);
	CHECK(log_sink.lines == std::vector<std::string> {UNREADABLE_LINE});
}
