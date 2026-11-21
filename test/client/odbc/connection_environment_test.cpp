// The environment checks a connect runs once the driver has loaded and before any handle exists:
// a variable the driver refuses, and a keyword the driver's overridden-environment list needs the
// connection string to set. Each refusal names a variable or a keyword, never a
// value.

// catch.hpp comes first, as in this directory's other tests.
#include "catch.hpp"

#include "../interface/scoped_environment_variable.hpp"
#include "duckdb/common/unordered_map.hpp"
#include "duckdb/main/database.hpp"
#include "fake_driver_fixture.hpp"
#include "thrown.hpp"

#include <cstdlib>
#include <string>
#include <vector>

using namespace duckdb;
using redshift_driver::DriverErrorKind;
using redshift_driver::DriverLoadResult;
using redshift_driver::OverriddenVariable;
using redshift_driver::testing::FakeOdbcDriver;
using redshift_driver::testing::ScopedEnvironmentVariable;
using redshift_test::FakeDriverFixture;
using redshift_test::Thrown;

namespace {

//! A value that must not reach any message.
const char *const SECRET_VALUE = "/home/analyst/do-not-print.crt";

const char *const REFUSED_MESSAGE = "PGSSLROOTCERT is set; the Redshift driver would read it, and it can change the "
                                    "driver's connection or security settings. Unset it to connect.";

//! The driver loader's load-phase refusal, in the wording both layers share.
const char *const LOAD_PHASE_MESSAGE = "PGSYSCONFDIR is set; the Redshift driver would read it, and it can change the "
                                       "driver's connection or security settings. Unset it to connect.";

//! The calls LoadPhaseCheckingSource has answered since a test reset it.
int load_phase_source_calls = 0;

//! A driver source that refuses while PGSYSCONFDIR is set, as the driver loader refuses a load-phase variable
//! before the platform loader, and otherwise answers as the fake does.
DriverLoadResult LoadPhaseCheckingSource() {
	load_phase_source_calls++;
	if (std::getenv("PGSYSCONFDIR") != nullptr) {
		DriverLoadResult refused;
		refused.error.kind = DriverErrorKind::REFUSED_ENVIRONMENT;
		refused.error.message = LOAD_PHASE_MESSAGE;
		return refused;
	}
	return FakeOdbcDriver::Source();
}

} // namespace

TEST_CASE("A set refused variable fails the first connect naming it, before any handle is allocated", "[odbc]") {
	FakeDriverFixture fixture;
	fixture.fake.SetRefusedEnvironment({"PGSERVICE", "PGSSLROOTCERT"});
	ScopedEnvironmentVariable unset("PGSERVICE", nullptr);
	ScopedEnvironmentVariable refused("PGSSLROOTCERT", SECRET_VALUE);

	CHECK(redshift_test::Capture([&] { fixture.OpenConnection(); }) == Thrown {ExceptionType::IO, REFUSED_MESSAGE});
	CHECK(fixture.fake.Calls().empty());
}

TEST_CASE("A refused variable set to an empty value is refused as a set one is", "[odbc]") {
	FakeDriverFixture fixture;
	fixture.fake.SetRefusedEnvironment({"PGSSLROOTCERT"});
	ScopedEnvironmentVariable refused("PGSSLROOTCERT", "");

	CHECK(redshift_test::Capture([&] { fixture.OpenConnection(); }) == Thrown {ExceptionType::IO, REFUSED_MESSAGE});
	CHECK(fixture.fake.Calls().empty());
}

TEST_CASE("An environment refusal's extra_info holds the loaded driver's identity alone", "[odbc]") {
	FakeDriverFixture fixture;
	fixture.fake.SetRefusedEnvironment({"PGSSLROOTCERT"});
	ScopedEnvironmentVariable refused("PGSSLROOTCERT", SECRET_VALUE);

	const auto extra_info = redshift_test::CaptureError([&] { fixture.OpenConnection(); }).ExtraInfo();
	const unordered_map<string, string> expected {{"redshift_driver", "test-fake fake " + DuckDB::Platform()}};
	CHECK(extra_info == expected);
}

TEST_CASE("A refused variable set after a connect fails the next connect before it calls the driver", "[odbc]") {
	FakeDriverFixture fixture;
	fixture.fake.SetRefusedEnvironment({"PGSSLROOTCERT"});
	auto first = fixture.OpenConnection();
	const auto calls = fixture.fake.Calls().size();
	ScopedEnvironmentVariable refused("PGSSLROOTCERT", SECRET_VALUE);

	CHECK(redshift_test::Capture([&] { fixture.OpenConnection(); }) == Thrown {ExceptionType::IO, REFUSED_MESSAGE});
	CHECK(fixture.fake.Calls().size() == calls);
}

TEST_CASE("A load-phase refusal from the driver source gives IOException with its message unchanged", "[odbc]") {
	FakeDriverFixture fixture;
	load_phase_source_calls = 0;
	SetDriverSourceForTesting(&LoadPhaseCheckingSource);
	ScopedEnvironmentVariable refused("PGSYSCONFDIR", SECRET_VALUE);

	CHECK(redshift_test::Capture([&] { fixture.OpenConnection(); }) ==
	      Thrown {ExceptionType::IO, std::string("the Redshift driver could not be loaded: ") + LOAD_PHASE_MESSAGE});
	CHECK(load_phase_source_calls == 1);
	CHECK(fixture.fake.Calls().empty());
}

TEST_CASE("A connect after a load-phase refusal, with the variable unset, calls the driver source again", "[odbc]") {
	FakeDriverFixture fixture;
	load_phase_source_calls = 0;
	SetDriverSourceForTesting(&LoadPhaseCheckingSource);
	{
		ScopedEnvironmentVariable refused("PGSYSCONFDIR", SECRET_VALUE);
		CHECK_THROWS_AS(fixture.OpenConnection(), IOException);
	}

	ScopedEnvironmentVariable unset("PGSYSCONFDIR", nullptr);
	auto connection = fixture.OpenConnection();
	CHECK(load_phase_source_calls == 2);
	CHECK(fixture.fake.AllocatedHandles() == "environment, connection");
}

TEST_CASE("A connection string lacking a keyword the overridden list names is refused before any handle", "[odbc]") {
	FakeDriverFixture fixture;
	fixture.fake.SetOverriddenEnvironment(
	    {OverriddenVariable {"SSL_CERT_FILE", "CaFile"}, OverriddenVariable {"PGCLIENTENCODING", "ClientEncoding"}});

	CHECK(redshift_test::Capture([&] { fixture.OpenConnection(); }) ==
	      Thrown {ExceptionType::IO, "the Redshift connection settings do not set ClientEncoding, so the driver "
	                                 "would read PGCLIENTENCODING from the environment. This is a defect in the "
	                                 "extension; report it with the extension and driver versions."});
	CHECK(fixture.fake.Calls().empty());
}

TEST_CASE("A missing overridden keyword's refusal carries the loaded driver's identity alone", "[odbc]") {
	FakeDriverFixture fixture;
	fixture.fake.SetOverriddenEnvironment({OverriddenVariable {"PGCLIENTENCODING", "ClientEncoding"}});

	const auto extra_info = redshift_test::CaptureError([&] { fixture.OpenConnection(); }).ExtraInfo();
	const unordered_map<string, string> expected {{"redshift_driver", "test-fake fake " + DuckDB::Platform()}};
	CHECK(extra_info == expected);
}

TEST_CASE("A later connect whose settings lack an overridden keyword is refused before it calls the driver", "[odbc]") {
	FakeDriverFixture fixture;
	// Profile is set only for a named profile, so the default chain's connect lacks it.
	fixture.fake.SetOverriddenEnvironment({OverriddenVariable {"AWS_PROFILE", "Profile"}});
	auto config = redshift_test::UsEast1Config();
	config.profile = "dev";
	auto first = OpenRedshiftConnection(config, fixture.log_sink);
	const auto calls = fixture.fake.Calls().size();

	CHECK(redshift_test::Capture([&] { fixture.OpenConnection(); }) ==
	      Thrown {ExceptionType::IO, "the Redshift connection settings do not set Profile, so the driver would read "
	                                 "AWS_PROFILE from the environment. This is a defect in the extension; report it "
	                                 "with the extension and driver versions."});
	CHECK(fixture.fake.Calls().size() == calls);
}

TEST_CASE("A connection string setting every keyword the overridden list names connects", "[odbc]") {
	FakeDriverFixture fixture;
	fixture.fake.SetOverriddenEnvironment({OverriddenVariable {"SSL_CERT_FILE", "CaFile"},
	                                       OverriddenVariable {"PGCOMPRESSION", "Compression"},
	                                       OverriddenVariable {"PGHOST", "Host"}});

	auto connection = fixture.OpenConnection();
	CHECK(fixture.fake.AllocatedHandles() == "environment, connection");
}

TEST_CASE("An overridden keyword is matched case-insensitively, as the driver matches keywords", "[odbc]") {
	FakeDriverFixture fixture;
	fixture.fake.SetOverriddenEnvironment({OverriddenVariable {"SSL_CERT_FILE", "cafile"}});

	auto connection = fixture.OpenConnection();
	CHECK(fixture.fake.AllocatedHandles() == "environment, connection");
}

TEST_CASE("An overridden keyword written only inside a value is not taken as set", "[odbc]") {
	FakeDriverFixture fixture;
	fixture.fake.SetOverriddenEnvironment({OverriddenVariable {"PGCLIENTENCODING", "ClientEncoding"}});
	auto config = redshift_test::UsEast1Config();
	config.profile = "a;ClientEncoding={b";

	CHECK(redshift_test::Capture([&] { OpenRedshiftConnection(config, fixture.log_sink); }) ==
	      Thrown {ExceptionType::IO, "the Redshift connection settings do not set ClientEncoding, so the driver "
	                                 "would read PGCLIENTENCODING from the environment. This is a defect in the "
	                                 "extension; report it with the extension and driver versions."});
}

TEST_CASE("An overridden keyword that only prefixes a set keyword is not taken as set", "[odbc]") {
	FakeDriverFixture fixture;
	// The connection string sets Serverless and no Server.
	fixture.fake.SetOverriddenEnvironment({OverriddenVariable {"PGHOST", "Server"}});

	CHECK(redshift_test::Capture([&] { fixture.OpenConnection(); }) ==
	      Thrown {ExceptionType::IO, "the Redshift connection settings do not set Server, so the driver would read "
	                                 "PGHOST from the environment. This is a defect in the extension; report it "
	                                 "with the extension and driver versions."});
	CHECK(fixture.fake.Calls().empty());
}

TEST_CASE("AWS_PROFILE adds the role profile hint to a credentials failure under the default chain, not under PROFILE",
          "[odbc]") {
	const std::string no_credentials = "No AWS profile found: analyst";
	const std::string hint = "If AWS_PROFILE names a profile that assumes a role";
	auto failure_with = [&](const duckdb::RedshiftConfig &config) {
		FakeDriverFixture fixture;
		fixture.FailNextCall("SQLDriverConnect", SQL_ERROR, {{"HY000", 0, no_credentials}});
		return redshift_test::Capture([&] { duckdb::OpenRedshiftConnection(config, fixture.log_sink); }).message;
	};
	auto named = redshift_test::UsEast1Config();
	named.profile = "analyst";
	{
		ScopedEnvironmentVariable selected("AWS_PROFILE", "analyst");
		CHECK(failure_with(redshift_test::UsEast1Config()).find(hint) != std::string::npos);
		CHECK(failure_with(named).find(hint) == std::string::npos);
	}
	{
		ScopedEnvironmentVariable unset("AWS_PROFILE", nullptr);
		CHECK(failure_with(redshift_test::UsEast1Config()).find(hint) == std::string::npos);
	}
	{
		ScopedEnvironmentVariable empty("AWS_PROFILE", "");
		CHECK(failure_with(redshift_test::UsEast1Config()).find(hint) == std::string::npos);
	}
}
