// Linux's ResolveTrustSource: its system locations, and how it reads them and SSL_CERT_FILE from the
// host. The search itself is tested in trust_store_search_test.cpp.

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
using redshift_trust_test::NoTrustSourceMessage;
using redshift_trust_test::RecordingLogSink;
using redshift_trust_test::TemporaryDirectory;

TEST_CASE("The Linux system CA bundle locations are the client's list, in its order", "[odbc]") {
	CHECK(SystemCaBundles() ==
	      Locations({"/etc/pki/tls/certs/ca-bundle.crt", "/etc/pki/ca-trust/extracted/pem/tls-ca-bundle.pem",
	                 "/etc/ssl/certs/ca-certificates.crt", "/etc/ssl/ca-bundle.pem", "/etc/ssl/cert.pem"}));
}

TEST_CASE("Every system location is a path the driver can be given as written", "[odbc]") {
	// Only SSL_CERT_FILE's path is checked when it is resolved, because the system locations are
	// fixed: this case keeps each of them within the trust-path rules.
	for (auto &location : SystemCaBundles()) {
		CHECK(FindUnusableTrustPathReason(location).empty());
	}
}

TEST_CASE("ResolveTrustSource reads SSL_CERT_FILE from the environment", "[odbc]") {
	TemporaryDirectory directory;
	auto bundle = directory.File("bundle.pem", BUNDLE);
	ScopedEnvironmentVariable variable("SSL_CERT_FILE", bundle.c_str());
	RecordingLogSink log_sink;
	CHECK(ResolveTrustSource(log_sink).path == bundle);
	CHECK(log_sink.lines.empty());
}

TEST_CASE("ResolveTrustSource searches the system locations when SSL_CERT_FILE is unset", "[odbc]") {
	ScopedEnvironmentVariable variable("SSL_CERT_FILE", nullptr);
	RecordingLogSink log_sink;
	// Which locations this host has is not the test's to choose, so the expected answer is read
	// from the host: the first readable location, or the error naming them all.
	auto first = std::string();
	for (auto &location : SystemCaBundles()) {
		if (first.empty() && IsReadableBundle(location)) {
			first = location;
		}
	}
	// The call stays outside CHECK, which would record a throw as a failure and swallow it.
	duckdb::TrustSource source;
	auto resolved = Capture([&] { source = ResolveTrustSource(log_sink); });
	if (first.empty()) {
		CHECK(resolved == Thrown {ExceptionType::IO, NoTrustSourceMessage(false, SystemCaBundles())});
	} else {
		CHECK(resolved == Thrown {});
		CHECK(source.path == first);
	}
	CHECK(log_sink.lines.empty());
}
