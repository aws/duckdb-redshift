// The Windows refusals that come before the driver source, through the extension:
// an invalid region, and an end-to-end test variable set in the environment block alone, each with
// its own message and no driver call. Windows only.

// catch.hpp comes first, as in this directory's other tests.
#include "catch.hpp"

#include "duckdb/common/windows.hpp"
#include "extension_database.hpp"
#include "fake_driver_fixture.hpp"
#include "odbc/windows_text.hpp"

#include <string>

using namespace duckdb;
using redshift_test::ExtensionDatabase;
using redshift_test::FakeDriverFixture;

namespace {

//! The refusals, spelled out rather than read from the code, so that changing one fails this suite.
const std::string REGION_REFUSAL =
    "Invalid value for parameter 'REGION': it is not an aws-partition region name. Use a lower-case region name from "
    "the aws partition, such as 'us-east-1'; the China, GovCloud, isolated and European Sovereign Cloud partitions "
    "are not supported, and a region newer than this extension's region list needs a newer extension";
const std::string TEST_VARIABLE_REFUSAL =
    "DUCKDB_REDSHIFT_TEST_E2E is set, but this build has no test-endpoint override, so the connection would go to "
    "the endpoint the target derives and is refused. Unset it to connect.";

//! What `attach` reported, its open refused, after checking that the fake's driver source was never
//! asked for a driver and that the refusal names the handle.
ErrorData AttachError(const FakeDriverFixture &fixture, const std::string &attach) {
	ExtensionDatabase database;
	auto result = database.con.Query(attach);
	REQUIRE(result->HasError());
	CHECK(fixture.fake.SourceCalls() == 0);
	CHECK(fixture.fake.Calls().empty());
	CHECK(result->GetErrorObject().RawMessage().rfind("Cannot attach rs: ", 0) == 0);
	return result->GetErrorObject();
}

bool Holds(const std::string &text, const std::string &part) {
	return text.find(part) != std::string::npos;
}

//! `name` set to `value` in the process's environment block alone, as a host's own runtime sets it
//! after the extension has loaded, and removed again at the end of the scope.
class BlockVariable {
public:
	BlockVariable(const char *name, const char *value) : wide_name(Utf8ToWide(name)) {
		REQUIRE(SetEnvironmentVariableW(wide_name.c_str(), Utf8ToWide(value).c_str()) != 0);
	}
	~BlockVariable() {
		SetEnvironmentVariableW(wide_name.c_str(), nullptr);
	}
	BlockVariable(const BlockVariable &) = delete;
	BlockVariable &operator=(const BlockVariable &) = delete;

private:
	std::wstring wide_name;
};

} // namespace

TEST_CASE("ATTACH on the load-only build refuses an invalid region before the driver source", "[odbc]") {
	FakeDriverFixture fixture;
	const auto error = AttachError(fixture, redshift_test::AttachRs("xx-nowhere-1", "analytics", "012345678901"));
	INFO(error.RawMessage());
	CHECK(error.Type() == ExceptionType::INVALID_INPUT);
	CHECK(Holds(error.RawMessage(), REGION_REFUSAL));
}

TEST_CASE("ATTACH on the load-only build refuses a test variable set in the block before the driver source", "[odbc]") {
	FakeDriverFixture fixture;
	BlockVariable test_case(TEST_CASE_VARIABLE, "1");
	const auto error = AttachError(fixture, redshift_test::ATTACH_RS);
	INFO(error.RawMessage());
	CHECK(error.Type() == ExceptionType::INVALID_INPUT);
	CHECK(Holds(error.RawMessage(), TEST_VARIABLE_REFUSAL));
}
