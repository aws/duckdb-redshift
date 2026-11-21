// The driver source a shipped build has: LoadEmbeddedDriver, which in a REDSHIFT_DRIVER=none build
// is the interface's NOT_BUILT placeholder. Listed for that build alone.

// catch.hpp comes first, as in this directory's other tests.
#include "catch.hpp"

#include "driver_source_hook.hpp"
#include "duckdb/main/database.hpp"
#include "fake_driver_fixture.hpp"
#include "thrown.hpp"

#if defined(_WIN32)
#include "duckdb/common/windows.hpp"
#include "extension_database.hpp"

#include <algorithm>
#include <string>
#include <vector>
#endif

using namespace duckdb;
using redshift_test::Thrown;

namespace {

//! Opens a connection with the process state a fresh process has, so through the shipped source,
//! with a CA bundle for the trust search that runs before it and no end-to-end test variable.
ErrorData ShippedConnectError() {
	ResetProcessStateForTesting();
	redshift_test::ScopedCaBundle ca_bundle;
	redshift_test::NoTestVariables no_test_variables;
	return redshift_test::CaptureError([] {
		OpenRedshiftConnection(redshift_test::UsEast1Config(), make_shared_ptr<redshift_test::CapturingLogSink>());
	});
}

} // namespace

TEST_CASE("Without a test source, a connection reports the NOT_BUILT placeholder's failure as IOException", "[odbc]") {
	const auto error = ShippedConnectError();
	CHECK(Thrown {error.Type(), error.RawMessage()} ==
	      Thrown {ExceptionType::IO, "the Redshift driver could not be loaded: this build of the extension carries no "
	                                 "Redshift ODBC driver for " +
	                                     DuckDB::Platform() +
	                                     " (it was built with REDSHIFT_DRIVER=none, or for a platform without a driver "
	                                     "loader); install a published release of the extension on a platform it "
	                                     "supports"});
}

TEST_CASE("The NOT_BUILT failure carries no redshift_driver, sqlstate or sqlstates key", "[odbc]") {
	const auto extra_info = ShippedConnectError().ExtraInfo();
	CHECK(extra_info.count("redshift_driver") == 0);
	CHECK(extra_info.count("sqlstate") == 0);
	CHECK(extra_info.count("sqlstates") == 0);
}

#if defined(_WIN32)
namespace {

//! The names of the .pem files directly in `dir`, sorted; none when it does not exist.
std::vector<std::wstring> PemFiles(const std::wstring &dir) {
	std::vector<std::wstring> names;
	WIN32_FIND_DATAW entry;
	const HANDLE find = FindFirstFileW((dir + L"\\*.pem").c_str(), &entry);
	if (find == INVALID_HANDLE_VALUE) {
		return names;
	}
	do {
		names.push_back(entry.cFileName);
	} while (FindNextFileW(find, &entry));
	FindClose(find);
	std::sort(names.begin(), names.end());
	return names;
}

//! The trust directories a load could use: beside this binary, and in the per-user cache.
std::vector<std::wstring> TrustDirectories() {
	std::vector<wchar_t> module(32768);
	const DWORD length = GetModuleFileNameW(nullptr, module.data(), static_cast<DWORD>(module.size()));
	std::wstring beside(module.data(), length < module.size() ? length : 0);
	beside = beside.substr(0, beside.find_last_of(L'\\')) + L"\\trust";
	std::vector<wchar_t> local(32768);
	const DWORD local_length = GetEnvironmentVariableW(L"LOCALAPPDATA", local.data(), static_cast<DWORD>(local.size()));
	const std::wstring cache =
	    std::wstring(local.data(), local_length < local.size() ? local_length : 0) + L"\\duckdb-redshift\\trust";
	return {beside, cache};
}

} // namespace

TEST_CASE("On Windows, with no CA bundle, ATTACH gives NOT_BUILT and writes no trust file", "[odbc]") {
	// The ROOT store is exported only into the trust directory a loaded driver names, so the
	// placeholder's failure comes first and nothing is written.
	ResetProcessStateForTesting();
	redshift_test::NoTestVariables no_test_variables;
	duckdb::redshift_driver::testing::ScopedEnvironmentVariable no_bundle {"SSL_CERT_FILE", nullptr};
	std::vector<std::vector<std::wstring>> before;
	for (const auto &dir : TrustDirectories()) {
		before.push_back(PemFiles(dir));
	}
	redshift_test::ExtensionDatabase database;
	// The connect is the ATTACH's, so the placeholder's failure is the ATTACH's.
	auto result = database.con.Query(redshift_test::ATTACH_RS);
	REQUIRE(result->HasError());
	const auto error = result->GetErrorObject();
	INFO(error.RawMessage());
	CHECK(error.Type() == ExceptionType::IO);
	CHECK(error.RawMessage().find("Cannot attach rs: ") == 0);
	CHECK(error.RawMessage().find("this build of the extension carries no Redshift ODBC driver") != std::string::npos);
	CHECK(error.ExtraInfo().count("sqlstate") == 0);
	std::vector<std::vector<std::wstring>> after;
	for (const auto &dir : TrustDirectories()) {
		after.push_back(PemFiles(dir));
	}
	CHECK(after == before);
}
#endif
