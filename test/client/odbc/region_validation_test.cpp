// ValidateRegion against the generated aws-partition region allowlist. It makes no driver call, so
// these cases need no driver and no network, and they run on every platform; that a refused
// region never reaches the driver is for a connect-path case to prove. The generator's own cases,
// which run it as a subprocess, are in region_allowlist_test.cpp.

// catch.hpp comes first, as in this directory's other tests.
#include "catch.hpp"

#include "duckdb/common/error_data.hpp"
#include "duckdb/common/exception.hpp"
#include "odbc/connection_settings.hpp"
#include "thrown.hpp"

#include <algorithm>
#include <string>
#include <vector>

using duckdb::ExceptionType;
using duckdb::ValidateRegion;
using redshift_test::Capture;
using redshift_test::CaptureError;
using redshift_test::Thrown;

namespace {

//! The message every region refusal carries, spelled out rather than read from the code, so that
//! changing one of the two fails this suite.
const std::string REGION_MESSAGE =
    "Invalid value for parameter 'REGION': it is not an aws-partition region name. Use a lower-case region name from "
    "the aws partition, such as 'us-east-1'; the China, GovCloud, isolated and European Sovereign Cloud partitions "
    "are not supported, and a region newer than this extension's region list needs a newer extension";

//! What ValidateRegion throws for every region it refuses.
Thrown RegionRefusal() {
	return Thrown {ExceptionType::INVALID_INPUT, REGION_MESSAGE};
}

//! What ValidateRegion(`region`) throws.
Thrown ValidateRegionThrown(const std::string &region) {
	return Capture([&] { ValidateRegion(region); });
}

//! Region names the endpoint data lists in every partition other than `aws`, which this release does not
//! support: the two China regions, the two GovCloud ones, the US isolated
//! regions of all three kinds, the European isolated one, and the European Sovereign Cloud one.
std::vector<std::string> OtherPartitionRegions() {
	return {"cn-north-1",     "cn-northwest-1",  "us-gov-east-1",  "us-gov-west-1",
	        "us-iso-east-1",  "us-iso-west-1",   "us-isob-east-1", "us-isob-west-1",
	        "us-isof-east-1", "us-isof-south-1", "eu-isoe-west-1", "eusc-de-east-1"};
}

//! The keys of the structured fields on `error`, where the extension reads a driver failure's SQLSTATE.
//! Empty for an error never thrown, which the type check beside it catches.
std::vector<std::string> ExtraInfoKeys(const duckdb::ErrorData &error) {
	std::vector<std::string> keys;
	for (auto &entry : error.ExtraInfo()) {
		keys.push_back(entry.first);
	}
	return keys;
}

} // namespace

TEST_CASE("A region name the AWS endpoint data lists in the aws partition is accepted", "[odbc]") {
	// Spelled out rather than read from the allowlist, so a list that lost one fails here too.
	CHECK_NOTHROW(ValidateRegion("us-east-1"));
	CHECK_NOTHROW(ValidateRegion("us-east-2"));
	CHECK_NOTHROW(ValidateRegion("us-west-1"));
	CHECK_NOTHROW(ValidateRegion("us-west-2"));
	CHECK_NOTHROW(ValidateRegion("eu-west-1"));
	CHECK_NOTHROW(ValidateRegion("eu-central-1"));
	CHECK_NOTHROW(ValidateRegion("ap-southeast-2"));
	CHECK_NOTHROW(ValidateRegion("ap-northeast-1"));
	CHECK_NOTHROW(ValidateRegion("sa-east-1"));
	CHECK_NOTHROW(ValidateRegion("af-south-1"));
	CHECK_NOTHROW(ValidateRegion("ca-west-1"));
	CHECK_NOTHROW(ValidateRegion("il-central-1"));
	CHECK_NOTHROW(ValidateRegion("me-central-1"));
	CHECK_NOTHROW(ValidateRegion("mx-central-1"));
}

TEST_CASE("A region in any partition other than aws is refused", "[odbc]") {
	for (auto &region : OtherPartitionRegions()) {
		INFO(region);
		CHECK(ValidateRegionThrown(region) == RegionRefusal());
	}
}

TEST_CASE("A region that differs from an allowlisted name only in case is refused", "[odbc]") {
	// The value chooses the host name, and lower-casing it would connect to a name the customer
	// did not write.
	for (auto &region : {"US-EAST-1", "Us-East-1", "us-EAST-1", "US-east-1"}) {
		INFO(region);
		CHECK(ValidateRegionThrown(region) == RegionRefusal());
	}
}

TEST_CASE("A region with anything added to an allowlisted name is refused", "[odbc]") {
	// The first is the case the allowlist exists for: in the host name, it would aim the connection
	// at a name the customer does not control.
	for (auto &region : {"us-east-1.evil.example", "us-east-1.amazonaws.com", "us-east-1/../us-west-2", "us-east-1 ",
	                     " us-east-1", "us-east-11"}) {
		INFO(region);
		CHECK(ValidateRegionThrown(region) == RegionRefusal());
	}
}

TEST_CASE("A truncated name, or a pseudo-region the aws partition does not list, is refused", "[odbc]") {
	// A name cut short, which a match in the other direction would accept, and a pseudo-region the
	// data does not list among the aws partition's regions.
	for (auto &region : {"us-east-", "aws-global"}) {
		INFO(region);
		CHECK(ValidateRegionThrown(region) == RegionRefusal());
	}
}

TEST_CASE("An empty region is refused", "[odbc]") {
	// Unlike an empty profile, which asks for the default credential chain, an empty region has
	// no meaning: there is no host name to derive from it.
	CHECK(ValidateRegionThrown("") == RegionRefusal());
}

TEST_CASE("A region holding NUL or an ASCII control character is refused", "[odbc]") {
	std::vector<std::string> regions = {std::string("us-east-1\0us-west-2", 19), std::string("\0", 1), "us-east-1\x7f"};
	for (int byte = 0x01; byte <= 0x1F; byte++) {
		regions.push_back("us-east-1" + std::string(1, static_cast<char>(byte)));
	}
	for (auto &region : regions) {
		INFO("length " << region.size() << ", last byte "
		               << static_cast<int>(static_cast<unsigned char>(region.back())));
		CHECK(ValidateRegionThrown(region) == RegionRefusal());
	}
}

TEST_CASE("The region refusal names the parameter and the rule, and never the region", "[odbc]") {
	// A refused region may be a secret pasted into the wrong field, so the
	// message gives the rule, not the value.
	const std::string secret = "AKIAIOSFODNN7EXAMPLE";
	auto thrown = ValidateRegionThrown(secret);
	CHECK(thrown == RegionRefusal());
	CHECK(thrown.message.find(secret) == std::string::npos);
}

TEST_CASE("A region refusal does not compare equal to another type or message", "[odbc]") {
	// The control for the comparison the cases above rely on: if it ignored the type or the
	// message, a wrong refusal would pass them all.
	auto thrown = ValidateRegionThrown("");
	CHECK_FALSE(thrown == Thrown {ExceptionType::INVALID_INPUT, REGION_MESSAGE + "."});
	CHECK_FALSE(thrown == Thrown {ExceptionType::IO, REGION_MESSAGE});
}

TEST_CASE("The region refusal carries no driver diagnostic", "[odbc]") {
	// No driver diagnostic lies behind a value refused before the driver is called.
	// The ErrorData is named because ExtraInfo() returns a reference into it.
	auto error = CaptureError([] { ValidateRegion("us-east-1.evil.example"); });
	REQUIRE(error.Type() == ExceptionType::INVALID_INPUT);
	CHECK(error.RawMessage().find("SQLSTATE") == std::string::npos);
	auto keys = ExtraInfoKeys(error);
	CHECK(std::find(keys.begin(), keys.end(), "sqlstate") == keys.end());
	CHECK(std::find(keys.begin(), keys.end(), "sqlstates") == keys.end());
}
