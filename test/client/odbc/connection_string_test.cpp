// The one connection-string encoder, profile and target validation, and the connection string built from them.
//
// The output is checked by parsing it as the pinned driver does, not by searching it, because the
// driver matches keywords case-insensitively and accepts aliases such as TS for TrustStore. Parse
// is a port of driver 2.3.0.0's rsutil.c parseConnectionString, checked by the first case below.
//
// The port does not model a keyword of 65,536 bytes or more, which the driver measures in a
// uint16_t. It copies behaviour known only for the versions it was compared with, so a case fails
// when the driver lock names another, until the port is compared with that one.

// catch.hpp comes first, as the extension's C++ tests include it: DuckDB's copy declares the
// duckdb_base_std aliases it needs only while no DuckDB header has opened that namespace.
#include "catch.hpp"

#include "duckdb/common/exception.hpp"
#include "duckdb/common/string_util.hpp"
#include "duckdb/main/database.hpp"
#include "odbc/connection_settings.hpp"
#include "redshift_extension.hpp"
#include "settings_shape.hpp"
#include "thrown.hpp"

#include <algorithm>
#include <cctype>
#include <cstring>
#include <map>
#include <regex>
#include <string>
#include <utility>
#include <vector>

using duckdb::ApplicationName;
using duckdb::BuildConnectionString;
using duckdb::ConnectionStringEncoder;
using duckdb::DuckDB;
using duckdb::ExceptionType;
using duckdb::FindUnusableTrustPathReason;
using duckdb::InternalException;
using duckdb::InvalidInputException;
using duckdb::IsKerberosOrGssKeyword;
using duckdb::RedshiftConfig;
using duckdb::RedshiftExtension;
using duckdb::RedshiftTarget;
using duckdb::ResolveTarget;
using duckdb::TrustSource;
using duckdb::ValidateProfile;
using redshift_test::Capture;
using redshift_test::Thrown;

//! The generated list ValidateRegion checks against, for the case that bounds the derived host.
namespace allowlist {
#include "odbc/region_allowlist.inc"
} // namespace allowlist

namespace {

//! The driver's own whitespace set (rsutil.c WHITE_SPACES), trimmed from every keyword and value.
const char *const BLANKS = " \t\n\r\f\v";

//! The driver versions whose parseConnectionString the port below was compared with, line by line,
//! and whose other cited behaviour was re-checked. A list, so that a version can be added before
//! the driver lock moves to it.
//! 2.3.1.0's parseConnectionString differs from 2.3.0.0's only in dropping its process-wide cache
//! and returning an empty case-insensitive map on a parse error; the parse is the same.
const std::vector<std::string> PORT_COMPARED_DRIVER_VERSIONS = {"2.3.0.0", "2.3.1.0"};

std::string Trim(const std::string &value) {
	auto first = value.find_first_not_of(BLANKS);
	if (first == std::string::npos) {
		return std::string();
	}
	return value.substr(first, value.find_last_not_of(BLANKS) - first + 1);
}

std::string ToUpper(std::string value) {
	for (auto &character : value) {
		character = static_cast<char>(std::toupper(static_cast<unsigned char>(character)));
	}
	return value;
}

//! Keywords compared as the driver compares them: uppercased on both sides, as rsutil.c
//! createCaseInsensitiveMap does.
struct DriverKeywordLess {
	bool operator()(const std::string &left, const std::string &right) const {
		return ToUpper(left) < ToUpper(right);
	}
};

typedef std::map<std::string, std::string, DriverKeywordLess> DriverSettings;

//! A cursor over a connection string, reading it as driver 2.3.0.0's parseConnectionString does. It
//! walks a NUL-terminated buffer, as that function does, so a NUL ends the scan here too.
class DriverCursor {
public:
	explicit DriverCursor(const char *text) : at(text), gave_up(false) {
	}

	bool AtEnd() const {
		return *at == '\0';
	}

	char Peek() const {
		return *at;
	}

	void Step() {
		at++;
	}

	//! Steps over the driver's whitespace set, as parseConnectionString's SkipWhitespace does.
	void SkipBlanks() {
		// The NUL check comes first: strchr finds the terminator in BLANKS as well.
		while (*at != '\0' && std::strchr(BLANKS, *at) != nullptr) {
			at++;
		}
	}

	//! Whether the parser has given up, which loses every setting, not only the one being read.
	bool GaveUp() const {
		return gave_up;
	}

	//! The keyword at the cursor, trimmed, with its `=` and the blanks after it stepped over. Gives
	//! up where there is no `=` or the keyword is empty.
	std::string ReadKeyword() {
		const char *from = at;
		while (*at != '\0' && *at != '=') {
			at++;
		}
		if (*at == '\0' || at == from) {
			gave_up = true;
			return std::string();
		}
		auto keyword = Trim(std::string(from, static_cast<size_t>(at - from)));
		at++;
		SkipBlanks();
		return keyword;
	}

	//! The value at the cursor, trimmed, with its terminator stepped over. It is brace-quoted
	//! only when `{` is its first non-blank byte, and then ends at the first `}` that is not
	//! doubled. Gives up on an unterminated brace-quoted value.
	std::string ReadValue() {
		char ends_at = ';';
		if (*at == '{') {
			ends_at = '}';
			at++;
		}
		std::string value;
		while (*at != '\0') {
			if (ends_at == '}' && at[0] == '}' && at[1] == '}') {
				value += '}'; // A doubled `}` is one `}`.
				at += 2;
			} else if (*at == ends_at) {
				break;
			} else {
				value += *at++;
			}
		}
		if (ends_at == '}') {
			if (*at == '\0') {
				gave_up = true;
				return std::string();
			}
			at++;
		}
		return Trim(value); // The driver trims a braced value too.
	}

private:
	const char *at;
	bool gave_up;
};

//! The settings driver 2.3.0.0 would act on, or an empty map where its parser gives up. Every
//! turn of the loop either returns or consumes at least one byte (an empty pair's `;`, or a
//! keyword and its `=`), so it cannot spin.
DriverSettings Parse(const std::string &text) {
	DriverSettings settings;
	DriverCursor cursor(text.c_str());
	while (!cursor.AtEnd()) {
		cursor.SkipBlanks();
		if (cursor.Peek() == ';') {
			cursor.Step(); // An empty pair.
			continue;
		}
		auto keyword = cursor.ReadKeyword();
		if (cursor.GaveUp()) {
			return DriverSettings();
		}
		auto value = cursor.ReadValue();
		if (cursor.GaveUp()) {
			return DriverSettings();
		}
		settings[keyword] = value; // The last repetition wins.
		cursor.SkipBlanks();
		if (cursor.Peek() == ';') {
			cursor.Step();
		}
		cursor.SkipBlanks();
	}
	return settings;
}

//! Every keyword the driver would read, comma-separated in its own order, so a check that fails
//! prints the keyword an injected value added.
std::string JoinKeywords(const DriverSettings &settings) {
	std::string joined;
	for (auto &setting : settings) {
		if (!joined.empty()) {
			joined += ",";
		}
		joined += setting.first;
	}
	return joined;
}

std::string FindSetting(const DriverSettings &settings, const std::string &keyword) {
	auto found = settings.find(keyword);
	return found == settings.end() ? "(absent)" : found->second;
}

//! What ValidateProfile throws for `reason`. The rule is spelled out rather than read from the
//! code, so that changing one of the two fails this suite.
Thrown ProfileRefusal(const std::string &reason) {
	return Thrown {ExceptionType::INVALID_INPUT,
	               "Invalid value for parameter 'PROFILE': it " + reason +
	                   ". Use a profile name of at most 255 bytes, with no leading or trailing space and without NUL, "
	                   "an ASCII control character or '}'"};
}

//! What Add throws when it refuses a CaFile value for `reason`.
Thrown CaFileRefusal(const std::string &reason) {
	return Thrown {ExceptionType::INVALID_INPUT,
	               "Invalid value for the Redshift connection setting 'CaFile': it " + reason};
}

//! Values that close the braces to add a keyword the client refuses, plus `}` alone or hidden
//! in the middle or at an end. The first reopens the braces so its SSLMode wins; the second leaves
//! the closing `}` in SSLMode, an unknown mode, so rsconnect.cpp resolveSslPolicy uses verify-full.
std::vector<std::string> InjectingValues() {
	return {"};SSLMode=disable;Ignore={",
	        "};SSLMode=disable",
	        "};Host=evil.example",
	        "};EndPointUrl=https://evil.example",
	        "};IAM=0",
	        "};Profile=other",
	        "};LogLevel=6",
	        "}",
	        "}x",
	        "a}b"};
}

//! Values the driver uses exactly as written: several would add a keyword if the braces did not
//! hold, one has inner blanks, which the driver keeps, two are UTF-8 (no byte above 0x7F is a
//! control character), and the last is as long as the driver's profile buffer holds.
std::vector<std::string> AcceptedValues() {
	return {"analytics",
	        "",
	        ";SSLMode=disable",
	        "{",
	        "{IAM=0;Profile=other",
	        "a=b;c=d",
	        "team/analytics v1.2",
	        "caf\xc3\xa9",
	        "\xf0\x9f\x94\x92",
	        std::string(255, 'x')};
}

//! A version typed here, so ApplicationName is checked against a literal, not the build's value.
const char *const GIVEN_VERSION = "v0.4.1";

//! What ApplicationName names after the extension's part: the DuckDB the build is built with.
std::string BuiltDuckDBParts() {
	return std::string(" duckdb/") + DuckDB::LibraryVersion() + " " + DuckDB::Platform();
}

//! The ApplicationName a connection given GIVEN_VERSION sends.
std::string GivenApplicationName() {
	return "duckdb_redshift/v0.4.1" + BuiltDuckDBParts();
}

const char *const TRUST_PATH = "/etc/pki/tls/certs/ca-bundle.crt";

//! An account-backed Serverless workgroup in `region` with `profile`, in an account whose leading
//! zero must survive as text.
RedshiftConfig Config(const std::string &region, const std::string &profile) {
	RedshiftConfig config;
	config.region = region;
	config.profile = profile;
	config.target = RedshiftTarget::SERVERLESS;
	config.workgroup = "analytics";
	config.account_id = "012345678901";
	return config;
}

//! Config in us-east-1 with no profile and `database`.
RedshiftConfig ConfigWithDatabase(const std::string &database) {
	auto config = Config("us-east-1", "");
	config.database = database;
	return config;
}

//! What ResolveTarget throws for an invalid workgroup name, spelled out as ProfileRefusal is.
const Thrown WORKGROUP_REFUSAL {ExceptionType::INVALID_INPUT,
                                "Invalid value for parameter 'WORKGROUP': use a Redshift Serverless workgroup name "
                                "of 3 to 64 characters, each a lower-case letter, a digit or '-'"};

//! The same for an invalid account ID.
const Thrown ACCOUNT_ID_REFUSAL {
    ExceptionType::INVALID_INPUT,
    "Invalid value for parameter 'ACCOUNT_ID': use the 12-digit AWS account ID that owns the workgroup"};

//! The same for a database refused for `reason`.
Thrown DatabaseRefusal(const std::string &reason) {
	return Thrown {ExceptionType::INVALID_INPUT,
	               "Invalid value for parameter 'DATABASE': it " + reason +
	                   ". Use a database name of at most 127 bytes, with no leading or trailing space and without NUL, "
	                   "an ASCII control character or '}'"};
}

std::string ConnectionStringFor(const std::string &region, const std::string &profile) {
	return BuildConnectionString(Config(region, profile), TrustSource {TRUST_PATH}, GIVEN_VERSION);
}

//! What ValidateRegion throws, spelled out as ProfileRefusal is.
Thrown RegionRefusal() {
	return Thrown {ExceptionType::INVALID_INPUT,
	               "Invalid value for parameter 'REGION': it is not an aws-partition region name. Use a lower-case "
	               "region name from the aws partition, such as 'us-east-1'; the China, GovCloud, isolated and "
	               "European Sovereign Cloud partitions are not supported, and a region newer than this extension's "
	               "region list needs a newer extension"};
}

//! The keepalive interval for this platform: Windows sends 10 probes whatever
//! KeepAliveCount says, so its interval is shorter for the same 120-second bound.
#ifdef _WIN32
const char *const KEEPALIVE_INTERVAL = "8";
#else
const char *const KEEPALIVE_INTERVAL = "10";
#endif

//! The settings the client gives a connection in us-east-1 with no profile, written out
//! here rather than read from the code, so that changing one of the two fails this suite.
std::vector<std::pair<std::string, std::string>> TableSettings() {
	return {{"Host", "analytics.012345678901.us-east-1.redshift-serverless.amazonaws.com"},
	        {"EndPointUrl", "https://redshift-serverless.us-east-1.amazonaws.com"},
	        {"Region", "us-east-1"},
	        {"Port", "5439"},
	        {"Database", "dev"},
	        {"Workgroup", "analytics"},
	        {"IAM", "true"},
	        {"Serverless", "true"},
	        {"SSLMode", "verify-full"},
	        {"TrustStore", TRUST_PATH},
	        {"CaFile", TRUST_PATH},
	        {"LoginTimeout", "45"},
	        {"StreamingCursorRows", "2048"},
	        {"UseDeclareFetch", "0"},
	        {"CscEnable", "0"},
	        {"Compression", "zstd"},
	        {"MaxVarcharSize", "0"},
	        {"KeepAlive", "1"},
	        {"KeepAliveIdle", "30"},
	        {"KeepAliveInterval", KEEPALIVE_INTERVAL},
	        {"KeepAliveCount", "6"},
	        {"LogLevel", "0"},
	        {"ApplicationName", GivenApplicationName()},
	        {"DisableCache", "true"}};
}

//! A provisioned cluster in `region` with `profile`.
RedshiftConfig ClusterConfig(const std::string &region, const std::string &profile = "") {
	RedshiftConfig config;
	config.region = region;
	config.profile = profile;
	config.target = RedshiftTarget::PROVISIONED;
	config.cluster = "warehouse-1";
	return config;
}

//! ClusterConfig in us-east-1 with `cluster` and `database`.
RedshiftConfig ClusterConfigWith(const std::string &cluster, const std::string &database = "") {
	auto config = ClusterConfig("us-east-1");
	config.cluster = cluster;
	config.database = database;
	return config;
}

//! A workgroup endpoint with no account, as a redshift:// URI or a long-form workgroup ARN names it.
static RedshiftConfig LabelledConfig(const std::string &label = "lbl", const std::string &workgroup = "wg-a") {
	RedshiftConfig config;
	config.region = "eu-north-1";
	config.target = RedshiftTarget::SERVERLESS;
	config.workgroup = workgroup;
	config.endpoint_label = label;
	return config;
}

//! A cluster endpoint, as a redshift:// URI names it: ClusterConfig's cluster with a label and a port.
static RedshiftConfig ClusterEndpointWithLabel(const std::string &label = "abc123xyz", int port = 5439) {
	auto config = ClusterConfig("eu-north-1");
	config.endpoint_label = label;
	config.endpoint_port = port;
	return config;
}

//! The settings the client gives a provisioned ClusterConfig("us-east-1"), written out, then the common
//! settings TableSettings ends with, from SSLMode on.
std::vector<std::pair<std::string, std::string>> ClusterTableSettings() {
	std::vector<std::pair<std::string, std::string>> settings = {
	    {"EndPointUrl", "https://redshift.us-east-1.amazonaws.com"},
	    {"Region", "us-east-1"},
	    {"Database", "dev"},
	    {"ClusterId", "warehouse-1"},
	    {"IAM", "true"},
	    {"group_federation", "1"}};
	const auto all = TableSettings();
	const auto common = std::find_if(all.begin(), all.end(), [](const std::pair<std::string, std::string> &setting) {
		return setting.first == "SSLMode";
	});
	settings.insert(settings.end(), common, all.end());
	return settings;
}

//! What ResolveTarget throws for an invalid cluster identifier.
const Thrown CLUSTER_REFUSAL {ExceptionType::INVALID_INPUT,
                              "Invalid value for parameter 'CLUSTER': use a Redshift cluster identifier of 1 to 63 "
                              "characters, each a lower-case letter, a digit or '-', starting with a letter, with no "
                              "two '-' in a row and none at the end"};

} // namespace

TEST_CASE("The test parser reads a connection string as driver 2.3.0.0 does", "[odbc]") {
	auto settings = Parse("SSLMode={verify-full};sslmode = {disable}; Host ={a}}b};;Empty={};Plain= x ;Blank={ p }");
	// The repeated SSLMode wins case-insensitively, under the keyword it was first read with,
	// which is why the encoder refuses to write a keyword twice.
	CHECK(JoinKeywords(settings) == "Blank,Empty,Host,Plain,SSLMode");
	CHECK(FindSetting(settings, "SSLMODE") == "disable");
	CHECK(FindSetting(settings, "Host") == "a}b");
	CHECK(FindSetting(settings, "Empty") == "");
	CHECK(FindSetting(settings, "Plain") == "x");
	// Trimmed on both sides even inside braces, which is why a padded value is refused.
	CHECK(FindSetting(settings, "Blank") == "p");
	// Text straight after a brace-quoted value is read as the next keyword, with no `;` needed,
	// so a value that closes the braces early does not have to supply one either.
	CHECK(JoinKeywords(Parse("A={x}junk;B={y}")) == "A,junk;B");
	// All six of the driver's whitespace bytes, not only the space the checks above use: skipped
	// before a keyword and before a value, and trimmed from both ends of each.
	CHECK(FindSetting(Parse("\tA\r\n=\f{x}\v;"), "A") == "x");
	CHECK(FindSetting(Parse("A\v= \f\t\r\nx\n ;"), "A") == "x");
	// Blanks before an empty pair are skipped as well, or its `;` would be read into the next keyword.
	CHECK(JoinKeywords(Parse(" ;A={x}")) == "A");
	// Each of the three ways the parser gives up, every one of which loses the settings already
	// read as well as the rest of the string. The encoder refuses the bytes that cause them.
	CHECK(Parse("A").empty());    // No `=` at all.
	CHECK(Parse("=x;").empty());  // An empty keyword.
	CHECK(Parse("A={x").empty()); // A brace-quoted value nothing closes.
	// A NUL ends the scan, so inside a brace-quoted value it leaves that value unterminated, and
	// the settings written after it are lost too.
	CHECK(Parse(std::string("A={a\0b};B={c};", 14)).empty());
	// What those refusals are for: written raw into a brace-quoted value, this one closes the
	// braces, reopens them around the closing brace that follows, and overrides the client's own
	// SSLMode, because the last repetition is the one the driver acts on.
	CHECK(FindSetting(Parse("SSLMode={verify-full};Profile={};SSLMode=disable;Ignore={};"), "SSLMode") == "disable");
}

TEST_CASE("The test parser was compared with the locked driver version's parser", "[odbc]") {
	// test/client/odbc/sources.cmake defines REDSHIFT_LOCKED_DRIVER_VERSION when CMake configures:
	// the version the driver lock names, or empty when the lock cannot be read, which fails here.
	INFO("The driver lock names '"
	     << REDSHIFT_LOCKED_DRIVER_VERSION
	     << "' (empty when CMake could not read it: see its configure warning): compare that version's "
	        "parseConnectionString with Parse here, re-check every driver source citation under src/transport/odbc/, "
	        "and test/client/odbc/ (rsutil.c, rsconnect.cpp, rsiam.h, rsodbc.h "
	        "and the others), check that its libpq still restarts the connect timer for each address "
	        "(fe-connect.c connectDBComplete), which LoginTimeout's sizing rests on, then "
	        "add the version to PORT_COMPARED_DRIVER_VERSIONS");
	CHECK(std::find(PORT_COMPARED_DRIVER_VERSIONS.begin(), PORT_COMPARED_DRIVER_VERSIONS.end(),
	                REDSHIFT_LOCKED_DRIVER_VERSION) != PORT_COMPARED_DRIVER_VERSIONS.end());
}

TEST_CASE("A profile crafted to add a connection-string keyword is refused", "[odbc]") {
	for (auto &profile : InjectingValues()) {
		CHECK_THROWS_AS(ValidateProfile(profile), InvalidInputException);
	}
}

TEST_CASE("A profile holding NUL or an ASCII control character is refused", "[odbc]") {
	CHECK_THROWS_AS(ValidateProfile(std::string("a\0b", 3)), InvalidInputException);
	for (int byte = 0x01; byte <= 0x1F; byte++) {
		CHECK_THROWS_AS(ValidateProfile("a" + std::string(1, static_cast<char>(byte))), InvalidInputException);
	}
	CHECK_THROWS_AS(ValidateProfile("a\x7f"), InvalidInputException);
	// 0x20 is the first byte that is not a control character, and 0x80 is a UTF-8 continuation.
	CHECK_NOTHROW(ValidateProfile("a b"));
	CHECK_NOTHROW(ValidateProfile("a\xc2\x80"));
}

TEST_CASE("A profile the driver would trim or truncate is refused", "[odbc]") {
	CHECK(Capture([] { ValidateProfile(" prod"); }) == ProfileRefusal("starts with a space, which the driver trims"));
	CHECK(Capture([] { ValidateProfile("prod "); }) == ProfileRefusal("ends with a space, which the driver trims"));
	CHECK(Capture([] { ValidateProfile(std::string(256, 'p')); }) ==
	      ProfileRefusal("is longer than the 255 bytes the driver keeps"));
	// 255 bytes is the longest the driver keeps, and a space between other bytes is left alone.
	CHECK_NOTHROW(ValidateProfile(std::string(255, 'p')));
	CHECK_NOTHROW(ValidateProfile("team prod"));
}

TEST_CASE("An empty profile is valid, because it asks for the default credential chain", "[odbc]") {
	CHECK_NOTHROW(ValidateProfile(""));
}

TEST_CASE("A profile the driver uses as written is accepted", "[odbc]") {
	for (auto &profile : AcceptedValues()) {
		CHECK_NOTHROW(ValidateProfile(profile));
	}
}

TEST_CASE("The profile refusal names the parameter, the reason and the rule, never the name", "[odbc]") {
	CHECK(Capture([] { ValidateProfile("secret-team}"); }) ==
	      ProfileRefusal("holds '}', which ends a brace-quoted value"));
	CHECK(Capture([] { ValidateProfile(std::string("a\0b", 3)); }) ==
	      ProfileRefusal("holds a NUL byte, which truncates the connection string"));
	CHECK(Capture([] { ValidateProfile("a\nb"); }) == ProfileRefusal("holds an ASCII control character (0x0a)"));
	// A refused byte is named even in a profile that is also too long.
	CHECK(Capture([] { ValidateProfile(std::string(300, 'p') + "}"); }) ==
	      ProfileRefusal("holds '}', which ends a brace-quoted value"));
	// DEL is a control character too, and its code is printed in the same two-digit form.
	CHECK(Capture([] { ValidateProfile("a\x7f"); }) == ProfileRefusal("holds an ASCII control character (0x7f)"));
}

TEST_CASE("A trust path over its limit that also holds '}' is refused for the '}', as a profile is", "[odbc]") {
	// The profile half is checked in "The profile refusal names the parameter, the reason and the
	// rule, never the name", with the same value.
	CHECK(FindUnusableTrustPathReason(std::string(300, 'p') + "}") == "holds '}', which ends a brace-quoted value");
}

TEST_CASE("The encoder brace-quotes every value it writes, in the order they were added", "[odbc]") {
	ConnectionStringEncoder encoder;
	encoder.Add("Port", "5439");
	encoder.Add("Profile", "a;b=c");
	CHECK(encoder.Build() == "Port={5439};Profile={a;b=c};");
}

TEST_CASE("No accepted value adds a keyword to the connection string", "[odbc]") {
	for (auto &value : AcceptedValues()) {
		ConnectionStringEncoder encoder;
		encoder.Add("SSLMode", "verify-full");
		encoder.Add("Profile", value);
		encoder.Add("LogLevel", "0");
		auto settings = Parse(encoder.Build());
		CHECK(JoinKeywords(settings) == "LogLevel,Profile,SSLMode");
		CHECK(FindSetting(settings, "sslmode") == "verify-full");
		CHECK(FindSetting(settings, "loglevel") == "0");
		CHECK(FindSetting(settings, "profile") == value);
	}
}

TEST_CASE("The encoder refuses a value crafted to add a keyword without writing it", "[odbc]") {
	for (auto &value : InjectingValues()) {
		ConnectionStringEncoder encoder;
		CHECK_THROWS_AS(encoder.Add("Profile", value), InvalidInputException);
		CHECK(encoder.Build() == "");
	}
}

TEST_CASE("The encoder refuses a derived value holding NUL or a control character", "[odbc]") {
	ConnectionStringEncoder encoder;
	CHECK_THROWS_AS(encoder.Add("CaFile", std::string("/etc/pki/a\0b.pem", 16)), InvalidInputException);
	CHECK_THROWS_AS(encoder.Add("CaFile", "/etc/pki/a\nb.pem"), InvalidInputException);
	CHECK_THROWS_AS(encoder.Add("CaFile", "/etc/pki/a\x7f.pem"), InvalidInputException);
	CHECK(encoder.Build() == "");
}

TEST_CASE("The encoder refuses a derived value the driver would trim", "[odbc]") {
	ConnectionStringEncoder encoder;
	CHECK(Capture([&] { encoder.Add("CaFile", " /etc/pki/ca.pem"); }) ==
	      CaFileRefusal("starts with a space, which the driver trims"));
	CHECK(Capture([&] { encoder.Add("CaFile", "/etc/pki/ca.pem "); }) ==
	      CaFileRefusal("ends with a space, which the driver trims"));
	CHECK(encoder.Build() == "");
}

TEST_CASE("The encoder checks no length, because a length limit belongs to each setting", "[odbc]") {
	ConnectionStringEncoder encoder;
	encoder.Add("Profile", std::string(512, 'x'));
	CHECK(encoder.Build() == "Profile={" + std::string(512, 'x') + "};");
}

TEST_CASE("The encoder refusal names the setting and the reason, never the value or a fix", "[odbc]") {
	ConnectionStringEncoder encoder;
	CHECK(Capture([&] { encoder.Add("CaFile", "/etc/pki/secret}.pem"); }) ==
	      CaFileRefusal("holds '}', which ends a brace-quoted value"));
}

TEST_CASE("The encoder refuses a keyword that is not spelled as a bare keyword", "[odbc]") {
#ifdef DUCKDB_CRASH_ON_ASSERT
	return; // InternalException aborts in this build
#endif
	ConnectionStringEncoder encoder;
	// An empty keyword writes `={value};`, on which the driver's parser gives up and drops every
	// setting, SSLMode included.
	CHECK(Capture([&] { encoder.Add("", "verify-full"); }) ==
	      Thrown {ExceptionType::INTERNAL,
	              "Redshift connection setting keyword '' must be a letter followed by letters, "
	              "digits or underscores"});
	// The driver trims this one to SSLMode, so the duplicate check would not see the repetition.
	CHECK_THROWS_AS(encoder.Add("SSLMode ", "disable"), InternalException);
	CHECK_THROWS_AS(encoder.Add("SSL-Mode", "disable"), InternalException);
	CHECK_THROWS_AS(encoder.Add("2SSLMode", "disable"), InternalException);
	CHECK(encoder.Build() == "");
}

TEST_CASE("The encoder accepts a keyword of a letter, then letters, digits or underscores", "[odbc]") {
	ConnectionStringEncoder encoder;
	// Min_TLS is a driver keyword (rsodbc.h RS_MIN_TLS); Key2 is here only for its digit.
	encoder.Add("Min_TLS", "1.2");
	encoder.Add("Key2", "v");
	CHECK(encoder.Build() == "Min_TLS={1.2};Key2={v};");
}

TEST_CASE("The encoder refuses the same keyword twice, whatever case it is written in", "[odbc]") {
#ifdef DUCKDB_CRASH_ON_ASSERT
	return; // InternalException aborts in this build
#endif
	ConnectionStringEncoder encoder;
	encoder.Add("SSLMode", "verify-full");
	// The message names the keyword and nothing else: the refused value is text like any other,
	// which the client keeps out of an exception.
	CHECK(Capture([&] { encoder.Add("sslmode", "disable"); }) ==
	      Thrown {ExceptionType::INTERNAL, "Redshift connection setting keyword 'sslmode' was added twice"});
	CHECK(encoder.Build() == "SSLMode={verify-full};");
}

TEST_CASE("The encoder refuses a repeated keyword with other keywords added in between", "[odbc]") {
#ifdef DUCKDB_CRASH_ON_ASSERT
	return; // InternalException aborts in this build
#endif
	ConnectionStringEncoder encoder;
	encoder.Add("SSLMode", "verify-full");
	encoder.Add("Profile", "analytics");
	CHECK(Capture([&] { encoder.Add("SSLMODE", "disable"); }) ==
	      Thrown {ExceptionType::INTERNAL, "Redshift connection setting keyword 'SSLMODE' was added twice"});
	CHECK(encoder.Build() == "SSLMode={verify-full};Profile={analytics};");
}

TEST_CASE("The connection string writes the table's settings once each, in the table's order", "[odbc]") {
	std::string expected;
	for (auto &setting : TableSettings()) {
		expected += setting.first + "={" + setting.second + "};";
	}
	CHECK(ConnectionStringFor("us-east-1", "") == expected);
}

TEST_CASE("The driver reads exactly the table's settings from the connection string", "[odbc]") {
	auto settings = Parse(ConnectionStringFor("us-east-1", ""));
	CHECK(JoinKeywords(settings) ==
	      "ApplicationName,CaFile,Compression,CscEnable,Database,DisableCache,EndPointUrl,Host,IAM,KeepAlive,"
	      "KeepAliveCount,KeepAliveIdle,KeepAliveInterval,LoginTimeout,LogLevel,MaxVarcharSize,Port,Region,"
	      "Serverless,SSLMode,StreamingCursorRows,TrustStore,UseDeclareFetch,Workgroup");
	for (auto &setting : TableSettings()) {
		CHECK(FindSetting(settings, setting.first) == setting.second);
	}
}

TEST_CASE("No user name, password, trace or statement timeout setting is emitted", "[odbc]") {
	// The driver's own keywords for them (rsodbc.h RS_LOGON_ID to RS_TRACE_LEVEL, RS_DB_USER and
	// RS_LOG_PATH_OPTION_NAME), looked up whatever their case, as the driver looks them up.
	auto settings = Parse(ConnectionStringFor("us-east-1", "analytics"));
	for (auto keyword : {"LogonID", "UID", "User", "Password", "PWD", "DbUser", "QueryTimeout", "Trace", "TraceFile",
	                     "TraceLevel", "LogPath"}) {
		CHECK(FindSetting(settings, keyword) == "(absent)");
	}
}

TEST_CASE("No AWS key, token, auth type, credential plugin or instance-profile setting is emitted", "[odbc]") {
	// The driver's own names for them (rsodbc.h RS_AUTH_TYPE, RS_ACCESS_KEY_ID, RS_SECRET_ACCESS_KEY,
	// RS_SESSION_TOKEN, RS_WEB_IDENTITY_TOKEN, RS_PLUGIN_NAME and RS_INSTANCE_PROFILE), looked up as
	// the driver looks keywords up. With none of them set, the driver takes credentials from the
	// profile when one is given and otherwise from its default chain (iam/RsIamClient.cpp
	// InferCredentialsProvider).
	for (auto profile : {"", "analytics"}) {
		auto settings = Parse(ConnectionStringFor("us-east-1", profile));
		for (auto keyword : {"AuthType", "AccessKeyID", "SecretAccessKey", "SessionToken", "web_identity_token",
		                     "plugin_name", "InstanceProfile"}) {
			CHECK(FindSetting(settings, keyword) == "(absent)");
		}
	}
}

TEST_CASE("The keepalive settings fail a dead peer within 120 seconds on this platform", "[odbc]") {
	const auto settings = Parse(ConnectionStringFor("us-east-1", ""));
	const auto idle = std::stoi(FindSetting(settings, "KeepAliveIdle"));
	const auto interval = std::stoi(FindSetting(settings, "KeepAliveInterval"));
#ifdef _WIN32
	// Windows sends 10 probes whatever KeepAliveCount says.
	const int probes = 10;
#else
	const auto probes = std::stoi(FindSetting(settings, "KeepAliveCount"));
#endif
	CHECK(idle + probes * interval <= 120);
}

TEST_CASE("The Kerberos and GSS check finds the driver's options and their aliases in any case", "[odbc]") {
	// The settings list, each also lower- and upper-cased, as the driver compares keywords.
	for (std::string keyword :
	     {"KerberosServiceName", "KSN", "KerberosAPI", "KSA", "krbsrvname", "gsslib", "gssencmode"}) {
		for (const auto &spelling : {keyword, duckdb::StringUtil::Lower(keyword), duckdb::StringUtil::Upper(keyword)}) {
			INFO(spelling);
			CHECK(IsKerberosOrGssKeyword(spelling));
		}
	}
	// Mixed case, which neither case conversion produces from the recorded names.
	for (auto keyword : {"GssLib", "GssEncMode", "KrbSrvName", "KerberosApi", "kerberosServiceName", "Ksa"}) {
		INFO(keyword);
		CHECK(IsKerberosOrGssKeyword(keyword));
	}
	// Near misses: a setting the client sends, a prefix and an extension of a listed name.
	for (auto keyword : {"KeepAlive", "Kerberos", "KSAX", "gss"}) {
		INFO(keyword);
		CHECK_FALSE(IsKerberosOrGssKeyword(keyword));
	}
}

TEST_CASE("No target's connection string sets a Kerberos or GSS option", "[odbc]") {
	const auto workgroup = Parse(ConnectionStringFor("us-east-1", "analytics"));
	const auto cluster =
	    Parse(BuildConnectionString(ClusterConfig("us-east-1", "analytics"), TrustSource {TRUST_PATH}));
	REQUIRE_FALSE(workgroup.empty());
	REQUIRE_FALSE(cluster.empty());
	for (const auto &settings : {workgroup, cluster}) {
		for (const auto &setting : settings) {
			INFO(setting.first);
			CHECK_FALSE(IsKerberosOrGssKeyword(setting.first));
		}
	}
}

TEST_CASE("The encoder refuses a Kerberos or GSS option as a programming error, adding nothing", "[odbc]") {
#ifdef DUCKDB_CRASH_ON_ASSERT
	return; // InternalException aborts in this build
#endif
	ConnectionStringEncoder encoder;
	CHECK(Capture([&] { encoder.Add("KSA", "GSS"); }) ==
	      Thrown {ExceptionType::INTERNAL, "Redshift connection setting keyword 'KSA' is a Kerberos or GSS option"});
	CHECK(encoder.Build() == "");
}

#ifndef _WIN32
TEST_CASE("A Linux or macOS build registers the case that checks the Windows test-file list", "[odbc]") {
	// That case lives in a file a wrong platform guard in sources.cmake would also drop from this
	// build, leaving no case to notice; this file is in every build.
	const std::string name = "A Windows configure leaves out only the ODBC client test files test/README.md lists";
	bool registered = false;
	for (const auto &test_case : Catch::getAllTestCasesSorted(*Catch::getCurrentContext().getConfig())) {
		registered = registered || test_case.name == name;
	}
	CHECK(registered);
}
#endif

TEST_CASE("A non-empty profile is passed unchanged as the last setting", "[odbc]") {
	auto text = ConnectionStringFor("us-east-1", "team analytics");
	CHECK(text.substr(text.size() - std::strlen("Profile={team analytics};")) == "Profile={team analytics};");
	CHECK(FindSetting(Parse(text), "Profile") == "team analytics");
}

TEST_CASE("An empty profile emits no Profile setting, so the driver uses its default credential chain", "[odbc]") {
	CHECK(FindSetting(Parse(ConnectionStringFor("us-east-1", "")), "Profile") == "(absent)");
}

TEST_CASE("Both trust options are given the same trust source", "[odbc]") {
	auto path = std::string("/opt/team certs/ca bundle.pem");
	auto settings = Parse(BuildConnectionString(Config("us-east-1", ""), TrustSource {path}));
	CHECK(FindSetting(settings, "TrustStore") == path);
	CHECK(FindSetting(settings, "CaFile") == path);
}

TEST_CASE("The host carries the target and the region, and the endpoint and Region setting the region", "[odbc]") {
	auto settings = Parse(ConnectionStringFor("eu-west-1", ""));
	CHECK(FindSetting(settings, "Host") == "analytics.012345678901.eu-west-1.redshift-serverless.amazonaws.com");
	CHECK(FindSetting(settings, "EndPointUrl") == "https://redshift-serverless.eu-west-1.amazonaws.com");
	CHECK(FindSetting(settings, "Region") == "eu-west-1");
}

TEST_CASE("A region outside the allowlist is refused", "[odbc]") {
	CHECK(Capture([] { ConnectionStringFor("us-east-1.evil.example", ""); }) == RegionRefusal());
}

TEST_CASE("ResolveTarget refuses a region outside the allowlist, with an account or without one", "[odbc]") {
	auto config = Config("us-east-1.evil.example", "");
	CHECK(Capture([&] { ResolveTarget(config); }) == RegionRefusal());
	// Before a workgroup with no account is looked up, in either build.
	config.account_id.clear();
	CHECK(Capture([&] { ResolveTarget(config); }) == RegionRefusal());
}

TEST_CASE("A region the encoder would also refuse gets the region refusal, so it is checked before Host", "[odbc]") {
	// Host, the first setting, holds the region, so encoding it first would give the encoder's
	// message naming 'Host' instead.
	for (auto &region : {std::string("us-east-1\x7f"), std::string("us-east-1\n"), std::string("us-east-1\0", 10),
	                     std::string("us-east-1}")}) {
		INFO("length " << region.size());
		CHECK(Capture([&] { ConnectionStringFor(region, ""); }) == RegionRefusal());
	}
}

TEST_CASE("A profile crafted to add a keyword is refused by the profile check", "[odbc]") {
	// The profile check's message, not the encoder's, shows the check ran before anything was
	// encoded; the encoder would refuse the same value naming the setting instead.
	CHECK(Capture([] { ConnectionStringFor("us-east-1", "};SSLMode=disable"); }) ==
	      ProfileRefusal("holds '}', which ends a brace-quoted value"));
	for (auto &profile : InjectingValues()) {
		CHECK(Capture([&] { ConnectionStringFor("us-east-1", profile); }).type == ExceptionType::INVALID_INPUT);
	}
}

TEST_CASE("The region, then the profile, then the target is checked, so the first refused is reported", "[odbc]") {
	auto config = Config("us-east-1.evil.example", "};SSLMode=disable");
	config.workgroup = "};SSLMode={disable";
	CHECK(Capture([&] { BuildConnectionString(config, TrustSource {TRUST_PATH}); }) == RegionRefusal());
	config.region = "us-east-1";
	CHECK(Capture([&] { BuildConnectionString(config, TrustSource {TRUST_PATH}); }) ==
	      ProfileRefusal("holds '}', which ends a brace-quoted value"));
	config.profile = "";
	CHECK(Capture([&] { BuildConnectionString(config, TrustSource {TRUST_PATH}); }) == WORKGROUP_REFUSAL);
}

TEST_CASE("The workgroup, then the account ID, then the database is checked, so the first refused is reported",
          "[odbc]") {
	auto config = ConfigWithDatabase("};x");
	config.workgroup = "Sales";
	config.account_id = "0123";
	CHECK(Capture([&] { ResolveTarget(config); }) == WORKGROUP_REFUSAL);
	config.workgroup = "analytics";
	CHECK(Capture([&] { ResolveTarget(config); }) == ACCOUNT_ID_REFUSAL);
	config.account_id = "012345678901";
	CHECK(Capture([&] { ResolveTarget(config); }) == DatabaseRefusal("holds '}', which ends a brace-quoted value"));
}

TEST_CASE("No accepted profile changes a setting the client fixes", "[odbc]") {
	auto fixed = TableSettings();
	for (auto &profile : AcceptedValues()) {
		if (profile.empty()) {
			continue;
		}
		auto settings = Parse(ConnectionStringFor("us-east-1", profile));
		CHECK(settings.size() == fixed.size() + 1);
		CHECK(FindSetting(settings, "Profile") == profile);
		for (auto &setting : fixed) {
			CHECK(FindSetting(settings, setting.first) == setting.second);
		}
	}
}

TEST_CASE("Every target the client reaches sends DisableCache=true exactly once", "[odbc]") {
	// The account-backed workgroup and the cluster, each in two regions, with a profile and with a
	// database, a labelled workgroup endpoint and a cluster endpoint; the accountless one is tested with
	// the preview implementation, and a target added later joins this list.
	for (const auto &config :
	     {Config("us-east-1", ""), Config("eu-west-1", ""), Config("us-east-1", "analytics"),
	      ConfigWithDatabase("sales"), ClusterConfig("us-east-1"), ClusterConfig("eu-west-1", "analytics"),
	      ClusterConfigWith("warehouse-1", "sales"), LabelledConfig(), ClusterEndpointWithLabel()}) {
		INFO(config.Describe());
		const auto text = BuildConnectionString(config, TrustSource {TRUST_PATH}, GIVEN_VERSION);
		// Counted in the raw string, case folded as the driver folds keywords: the parser keeps only
		// the last repetition, so a second one would hide in the parsed settings.
		const auto folded = duckdb::StringUtil::Lower(text);
		size_t count = 0;
		for (auto at = folded.find("disablecache="); at != std::string::npos;
		     at = folded.find("disablecache=", at + 1)) {
			count++;
		}
		CHECK(count == 1);
		CHECK(FindSetting(Parse(text), "DisableCache") == "true");
	}
}

TEST_CASE("An account-backed workgroup resolves to its derived host, its workgroup, dev and its hints", "[odbc]") {
	const auto settings = ResolveTarget(Config("eu-west-1", "analytics"));
	CHECK(settings.host == "analytics.012345678901.eu-west-1.redshift-serverless.amazonaws.com");
	CHECK(settings.workgroup == "analytics");
	CHECK(settings.cluster.empty());
	CHECK(settings.database == "dev");
	CHECK(settings.hints.noun == "workgroup");
	CHECK(settings.hints.resumes_when_idle);
	CHECK(settings.hints.permission == "Grant it redshift-serverless:GetCredentials on "
	                                   "arn:aws:redshift-serverless:<region>:<account>:workgroup/<workgroup-id>.");
	CHECK(settings.hints.availability == "The Redshift Serverless workgroup may not exist or may not be available to "
	                                     "the account; check WORKGROUP, ACCOUNT_ID and REGION.");
}

TEST_CASE("A configured database is the Database setting, and none means dev", "[odbc]") {
	auto database = [](const std::string &name) {
		return FindSetting(
		    Parse(BuildConnectionString(ConfigWithDatabase(name), TrustSource {TRUST_PATH}, GIVEN_VERSION)),
		    "Database");
	};
	CHECK(database("sales") == "sales");
	CHECK(database("") == "dev");
}

TEST_CASE("A workgroup name of 3 to 64 characters from a-z, 0-9 or '-' is accepted", "[odbc]") {
	for (const auto &workgroup : {std::string("abc"), std::string(64, 'w'), std::string("sales-wg-2"),
	                              std::string("0123456789"), std::string("zone-z9")}) {
		auto config = Config("us-east-1", "");
		config.workgroup = workgroup;
		CHECK(ResolveTarget(config).workgroup == workgroup);
	}
}

TEST_CASE("A workgroup name the host could not carry as written is refused, naming no value", "[odbc]") {
	// Each would put a byte outside the label rule into the host, or move the host elsewhere; ':' and
	// '/' sit either side of the digits, so a range one off would admit them.
	for (const auto &workgroup :
	     {std::string("ab"), std::string(65, 'w'), std::string("Sales"), std::string("sales_wg"),
	      std::string("sales.evil.example"), std::string("sales wg"), std::string("};SSLMode={disable"),
	      std::string("wg}"), std::string("wg{"), std::string("wg`"), std::string("caf\xc3\xa9"),
	      std::string("abc\0d", 5), std::string("abc\n"), std::string("wg:1"), std::string("wg/x"), std::string("wg,x"),
	      std::string("wg@x")}) {
		INFO("length " << workgroup.size());
		auto config = Config("us-east-1", "");
		config.workgroup = workgroup;
		CHECK(Capture([&] { BuildConnectionString(config, TrustSource {TRUST_PATH}); }) == WORKGROUP_REFUSAL);
	}
}

TEST_CASE("The longest derived host fits a DNS name's 253 characters in every allowlisted region", "[odbc]") {
	// A DNS name is at most 253 characters (RFC 1035), well under the 1,024 bytes the driver keeps of
	// Host (rsiam.h MAX_IDEN_LEN, which rsodbc.h includes first), so a regenerated list stays inside.
	// Only the total is checked: a 64-character workgroup's label is one over DNS's 63.
	auto config = Config("us-east-1", "");
	config.workgroup = std::string(64, 'w');
	for (const auto *region : allowlist::AWS_REGIONS) {
		INFO(region);
		config.region = region;
		CHECK(ResolveTarget(config).host.size() <= 253);
	}
}

TEST_CASE("In every allowlisted region each derived host holds its region and the form the driver reads it as",
          "[odbc]") {
	// Driver 2.3.0.0's hostPattern and serverlessWorkgroupHostPattern (RsIamHelper.cpp): a cluster
	// endpoint must match the provisioned one, or the driver looks its host up as a custom domain. A
	// labelled workgroup matches the Serverless one, as an account-backed workgroup does, and the
	// Workgroup and Region sent take the place of what the driver would read from the host;
	// std::regex, as the driver's.
	const std::regex provisioned("(.+)\\.(.+)\\.(.+).redshift(-dev)?\\.amazonaws\\.com(.)*");
	const std::regex serverless("(.+)\\.(.+)\\.(.+).redshift-serverless(-dev)?\\.amazonaws\\.com(.)*");
	for (const auto *region : allowlist::AWS_REGIONS) {
		INFO(region);
		const auto workgroup = ResolveTarget(Config(region, "")).host;
		// The driver tries the provisioned pattern first, so a host matching both would be a cluster's.
		CHECK_FALSE(std::regex_match(workgroup, provisioned));
		CHECK(std::regex_match(workgroup, serverless));
		CHECK(duckdb::StringUtil::Split(workgroup, '.')[2] == region);
		auto endpoint_config = ClusterEndpointWithLabel();
		endpoint_config.region = region;
		const auto cluster_endpoint = ResolveTarget(endpoint_config).host;
		CHECK(std::regex_match(cluster_endpoint, provisioned));
		CHECK(duckdb::StringUtil::Split(cluster_endpoint, '.')[2] == region);
		auto labelled_config = LabelledConfig();
		labelled_config.region = region;
		const auto labelled = ResolveTarget(labelled_config).host;
		CHECK_FALSE(std::regex_match(labelled, provisioned));
		CHECK(std::regex_match(labelled, serverless));
		CHECK(duckdb::StringUtil::Split(labelled, '.')[2] == region);
	}
}

TEST_CASE("A workgroup name is checked before a workgroup with no account is looked up", "[odbc]") {
	// Either build's accountless settings would refuse it too, but in words that do not name the rule.
	auto config = Config("us-east-1", "");
	config.account_id.clear();
	config.workgroup = "};SSLMode={disable";
	CHECK(Capture([&] { BuildConnectionString(config, TrustSource {TRUST_PATH}); }) == WORKGROUP_REFUSAL);
}

TEST_CASE("An account ID of exactly 12 decimal digits is accepted as text", "[odbc]") {
	for (const auto &account : {"000000000000", "999999999999", "012345678901"}) {
		auto config = Config("us-east-1", "");
		config.account_id = account;
		CHECK(duckdb::StringUtil::StartsWith(ResolveTarget(config).host, std::string("analytics.") + account + "."));
	}
}

TEST_CASE("An account ID that is not exactly 12 decimal digits is refused, naming no value", "[odbc]") {
	for (const auto &account : {std::string("01234567890"), std::string("0123456789012"), std::string("01234567890a"),
	                            std::string("0123-4567-89"), std::string(" 12345678901"), std::string("};Host={evil"),
	                            std::string("01234567890\0", 12), std::string("12345678901."),
	                            std::string("01234567890:"), std::string("01234567890/")}) {
		INFO("length " << account.size());
		auto config = Config("us-east-1", "");
		config.account_id = account;
		CHECK(Capture([&] { BuildConnectionString(config, TrustSource {TRUST_PATH}); }) == ACCOUNT_ID_REFUSAL);
	}
}

TEST_CASE("A database the driver uses as written, up to 127 bytes, is accepted", "[odbc]") {
	for (const auto &database : {std::string("sales"), std::string(127, 'd'), std::string("team db"),
	                             std::string("caf\xc3\xa9"), std::string(";SSLMode=disable"), std::string("{IAM=0")}) {
		INFO("length " << database.size());
		CHECK(ResolveTarget(ConfigWithDatabase(database)).database == database);
	}
}

TEST_CASE("No accepted database changes a setting the client fixes", "[odbc]") {
	auto fixed = TableSettings();
	for (auto &database : AcceptedValues()) {
		if (database.empty() || database.size() > 127) {
			continue;
		}
		INFO("length " << database.size());
		auto settings =
		    Parse(BuildConnectionString(ConfigWithDatabase(database), TrustSource {TRUST_PATH}, GIVEN_VERSION));
		CHECK(settings.size() == fixed.size());
		CHECK(FindSetting(settings, "Database") == database);
		for (auto &setting : fixed) {
			if (setting.first != "Database") {
				CHECK(FindSetting(settings, setting.first) == setting.second);
			}
		}
	}
}

TEST_CASE("A database over 127 bytes, or one the driver would not use as written, is refused", "[odbc]") {
	CHECK(Capture([] { ResolveTarget(ConfigWithDatabase(std::string(128, 'd'))); }) ==
	      DatabaseRefusal("is longer than the 127 bytes a Redshift database name can have"));
	CHECK(Capture([] { ResolveTarget(ConfigWithDatabase(" sales")); }) ==
	      DatabaseRefusal("starts with a space, which the driver trims"));
	CHECK(Capture([] { ResolveTarget(ConfigWithDatabase("sales ")); }) ==
	      DatabaseRefusal("ends with a space, which the driver trims"));
	CHECK(Capture([] { ResolveTarget(ConfigWithDatabase(std::string("sa\0les", 6))); }) ==
	      DatabaseRefusal("holds a NUL byte, which truncates the connection string"));
	CHECK(Capture([] { ResolveTarget(ConfigWithDatabase("sa\nles")); }) ==
	      DatabaseRefusal("holds an ASCII control character (0x0a)"));
	// A refused byte is named even in a database that is also too long.
	CHECK(Capture([] { ResolveTarget(ConfigWithDatabase(std::string(200, 'd') + "}")); }) ==
	      DatabaseRefusal("holds '}', which ends a brace-quoted value"));
	for (auto &database : InjectingValues()) {
		CHECK(Capture([&] { BuildConnectionString(ConfigWithDatabase(database), TrustSource {TRUST_PATH}); }) ==
		      DatabaseRefusal("holds '}', which ends a brace-quoted value"));
	}
}

TEST_CASE("Without the preview settings, a Serverless workgroup with no account is refused",
          redshift_test::PUBLIC_SETTINGS_TAGS) {
	auto config = Config("us-east-1", "");
	config.account_id.clear();
	CHECK(Capture([&] { BuildConnectionString(config, TrustSource {TRUST_PATH}); }) ==
	      Thrown {ExceptionType::INVALID_INPUT,
	              "Invalid Redshift target: a Serverless workgroup needs the account that owns it"});
}

TEST_CASE("The connection string is refused for no target, naming no value", "[odbc]") {
	auto config = Config("us-east-1", "");
	config.target = RedshiftTarget::UNSPECIFIED;
	CHECK(Capture([&] { BuildConnectionString(config, TrustSource {TRUST_PATH}); }) ==
	      Thrown {ExceptionType::INVALID_INPUT,
	              "Redshift connection requires a target: a Serverless workgroup or a provisioned cluster"});
}

TEST_CASE("A trust source the trust search would refuse is a programming error", "[odbc]") {
#ifdef DUCKDB_CRASH_ON_ASSERT
	return; // InternalException aborts in this build
#endif
	const std::string prefix = "Redshift trust source did not come from the trust search: its path ";
	auto refusal = [](const std::string &path) {
		return Capture([&] { BuildConnectionString(Config("us-east-1", ""), TrustSource {path}); });
	};
	CHECK(refusal("") ==
	      Thrown {ExceptionType::INTERNAL, prefix + "is empty, so the driver would choose a CA file of its own"});
	CHECK(refusal("/" + std::string(255, 'a')) ==
	      Thrown {ExceptionType::INTERNAL, prefix + "is longer than the 255 bytes the driver keeps"});
	CHECK(refusal("/etc/pki/ca}.pem") ==
	      Thrown {ExceptionType::INTERNAL, prefix + "holds '}', which ends a brace-quoted value"});
}

//! A UTF-8 trust path, which Windows libpq would open in the ANSI code page.
const char *const NON_ASCII_TRUST_PATH = "C:\\Users\\j\xc3\xa9r\xc3\xb4me\\ca.pem";

#ifdef _WIN32
TEST_CASE("On Windows a trust path with a character outside ASCII is refused, naming no path", "[odbc]") {
	const std::string reason = "holds a character outside ASCII, and on Windows the driver needs an ASCII path";
	CHECK(FindUnusableTrustPathReason(NON_ASCII_TRUST_PATH) == reason);
	CHECK(FindUnusableTrustPathReason("C:\\certs\\ca bundle.pem") == "");
	// The value rule still comes first.
	CHECK(FindUnusableTrustPathReason(std::string(NON_ASCII_TRUST_PATH) + "}") ==
	      "holds '}', which ends a brace-quoted value");
#ifndef DUCKDB_CRASH_ON_ASSERT
	CHECK(Capture([] { BuildConnectionString(Config("us-east-1", ""), TrustSource {NON_ASCII_TRUST_PATH}); }) ==
	      Thrown {ExceptionType::INTERNAL,
	              "Redshift trust source did not come from the trust search: its path " + reason});
#endif
}
#else
TEST_CASE("On Linux and macOS a UTF-8 trust path is given to both options as written", "[odbc]") {
	const auto settings = Parse(BuildConnectionString(Config("us-east-1", ""), TrustSource {NON_ASCII_TRUST_PATH}));
	CHECK(FindSetting(settings, "TrustStore") == NON_ASCII_TRUST_PATH);
	CHECK(FindSetting(settings, "CaFile") == NON_ASCII_TRUST_PATH);
}
#endif

TEST_CASE("A 255-byte trust path, the longest the driver keeps whole, is given to both options", "[odbc]") {
	auto path = "/" + std::string(254, 'a');
	auto settings = Parse(BuildConnectionString(Config("us-east-1", ""), TrustSource {path}));
	CHECK(FindSetting(settings, "TrustStore") == path);
	CHECK(FindSetting(settings, "CaFile") == path);
}

TEST_CASE("ApplicationName names the extension version it is given, and the built DuckDB and its platform", "[odbc]") {
	auto named = [](const std::string &version) {
		return FindSetting(Parse(BuildConnectionString(Config("us-east-1", ""), TrustSource {TRUST_PATH}, version)),
		                   "ApplicationName");
	};
	REQUIRE(!std::string(DuckDB::LibraryVersion()).empty());
	REQUIRE(!DuckDB::Platform().empty());
	// The expected values join the parts whole, which holds while this build's fit the cap.
	REQUIRE(GivenApplicationName().size() <= duckdb::APPLICATION_NAME_MAX_BYTES);
	CHECK(named("v0.4.1") == GivenApplicationName());
	CHECK(named("") == "duckdb_redshift" + BuiltDuckDBParts());
}

TEST_CASE("ApplicationName joins the three parts with one space each, and leaves out an empty one", "[odbc]") {
	CHECK(ApplicationName("v0.1.0", "v2.0.0", "linux_amd64") == "duckdb_redshift/v0.1.0 duckdb/v2.0.0 linux_amd64");
	CHECK(ApplicationName("", "v2.0.0", "linux_amd64") == "duckdb_redshift duckdb/v2.0.0 linux_amd64");
	CHECK(ApplicationName("v0.1.0", "", "linux_amd64") == "duckdb_redshift/v0.1.0 linux_amd64");
	CHECK(ApplicationName("v0.1.0", "v2.0.0", "") == "duckdb_redshift/v0.1.0 duckdb/v2.0.0");
	CHECK(ApplicationName("", "", "linux_amd64") == "duckdb_redshift linux_amd64");
	CHECK(ApplicationName("", "", "") == "duckdb_redshift");
}

TEST_CASE("ApplicationName leaves out a part that is not one token of letters, digits, '.', '_' and '-'", "[odbc]") {
	// Each would blur where a part ends, hold a byte outside what a log reader expects, or hold one
	// the connection string treats as syntax.
	for (auto unusable : {"v0.1 beta", "linux/amd64", "caf\xc3\xa9", " v1", "v1+local", "a}b", "a{b", "a;b",
	                      "x;Foo=bar", "a=b", "a\tb"}) {
		INFO(unusable);
		CHECK(ApplicationName(unusable, "v2.0.0", "linux_amd64") == "duckdb_redshift duckdb/v2.0.0 linux_amd64");
		CHECK(ApplicationName("v0.1.0", unusable, "linux_amd64") == "duckdb_redshift/v0.1.0 linux_amd64");
		CHECK(ApplicationName("v0.1.0", "v2.0.0", unusable) == "duckdb_redshift/v0.1.0 duckdb/v2.0.0");
	}
	CHECK(ApplicationName("v2.1.0-RC1", "v2.0.0-dev42", "linux_amd64_musl") ==
	      "duckdb_redshift/v2.1.0-RC1 duckdb/v2.0.0-dev42 linux_amd64_musl");
}

TEST_CASE("Past 63 bytes ApplicationName drops the platform, then DuckDB, then the extension's version", "[odbc]") {
	CHECK(duckdb::APPLICATION_NAME_MAX_BYTES == 63);
	// "duckdb_redshift/" + version + " duckdb/v2.0.0 linux_amd64" is 42 bytes plus the version.
	const std::string fits(21, 'a');
	CHECK(ApplicationName(fits, "v2.0.0", "linux_amd64") == "duckdb_redshift/" + fits + " duckdb/v2.0.0 linux_amd64");
	CHECK(ApplicationName(fits, "v2.0.0", "linux_amd64").size() == 63);
	CHECK(ApplicationName(fits + "a", "v2.0.0", "linux_amd64") == "duckdb_redshift/" + fits + "a duckdb/v2.0.0");
	// Without the platform the name is 30 bytes plus the version.
	const std::string fits_without_platform(33, 'b');
	CHECK(ApplicationName(fits_without_platform, "v2.0.0", "linux_amd64") ==
	      "duckdb_redshift/" + fits_without_platform + " duckdb/v2.0.0");
	CHECK(ApplicationName(fits_without_platform + "b", "v2.0.0", "linux_amd64") ==
	      "duckdb_redshift/" + fits_without_platform + "b");
	// With no DuckDB version the platform is the part that goes first: 28 bytes plus the version.
	const std::string fits_alone_with_platform(35, 'd');
	CHECK(ApplicationName(fits_alone_with_platform, "", "linux_amd64") ==
	      "duckdb_redshift/" + fits_alone_with_platform + " linux_amd64");
	CHECK(ApplicationName(fits_alone_with_platform + "d", "", "linux_amd64") ==
	      "duckdb_redshift/" + fits_alone_with_platform + "d");
	// The extension's part alone: kept at 63 bytes, its version dropped past them.
	const std::string longest(47, 'c');
	CHECK(ApplicationName(longest, "v2.0.0", "linux_amd64") == "duckdb_redshift/" + longest);
	CHECK(ApplicationName(longest + "c", "v2.0.0", "linux_amd64") == "duckdb_redshift");
	CHECK(ApplicationName(longest + "c", "", "") == "duckdb_redshift");
}

TEST_CASE("The build's identity is the extension's version and the DuckDB it is built with", "[odbc]") {
	const auto build = duckdb::ThisBuild();
	CHECK(build.extension_version == RedshiftExtension().Version());
	CHECK(build.duckdb_version == DuckDB::LibraryVersion());
	CHECK(build.platform == DuckDB::Platform());
}

TEST_CASE("Without a version given, the connection string names the extension's own version", "[odbc]") {
	// The extension's Version(), not the macro, so a build that defines it for some sources only
	// (set_extension_version_sources) fails here. A build with no version can only compare "" with "".
	if (RedshiftExtension().Version().empty()) {
		WARN("this build sets no extension version, so both sides name an empty one");
	}
	auto config = Config("us-east-1", "");
	CHECK(BuildConnectionString(config, TrustSource {TRUST_PATH}) ==
	      BuildConnectionString(config, TrustSource {TRUST_PATH}, RedshiftExtension().Version()));
}

TEST_CASE("A cluster's connection string is the settings table's provisioned column, once each, in order", "[odbc]") {
	std::string expected;
	for (auto &setting : ClusterTableSettings()) {
		expected += setting.first + "={" + setting.second + "};";
	}
	CHECK(BuildConnectionString(ClusterConfig("us-east-1"), TrustSource {TRUST_PATH}, GIVEN_VERSION) == expected);
}

TEST_CASE("The driver reads no Host, Port, Workgroup, Serverless or DbUser for a cluster named by CLUSTER alone",
          "[odbc]") {
	// With no cluster endpoint the driver looks up the host and port with DescribeClusters, and
	// group_federation=1 needs no database user.
	const auto settings =
	    Parse(BuildConnectionString(ClusterConfig("us-east-1", "analytics"), TrustSource {TRUST_PATH}, GIVEN_VERSION));
	CHECK(JoinKeywords(settings) ==
	      "ApplicationName,CaFile,ClusterId,Compression,CscEnable,Database,DisableCache,EndPointUrl,group_federation,"
	      "IAM,KeepAlive,KeepAliveCount,KeepAliveIdle,KeepAliveInterval,LoginTimeout,LogLevel,MaxVarcharSize,Profile,"
	      "Region,SSLMode,StreamingCursorRows,TrustStore,UseDeclareFetch");
	for (auto keyword : {"Host", "Server", "Port", "Workgroup", "Serverless", "DbUser", "UID", "PWD"}) {
		CHECK(FindSetting(settings, keyword) == "(absent)");
	}
}

TEST_CASE("A cluster's endpoint and Region follow the region, and a profile is last", "[odbc]") {
	const auto text =
	    BuildConnectionString(ClusterConfig("eu-west-1", "team prod"), TrustSource {TRUST_PATH}, GIVEN_VERSION);
	CHECK(duckdb::StringUtil::StartsWith(text, "EndPointUrl={https://redshift.eu-west-1.amazonaws.com};"
	                                           "Region={eu-west-1};Database={dev};ClusterId={warehouse-1};"));
	CHECK(duckdb::StringUtil::EndsWith(text, ";DisableCache={true};Profile={team prod};"));
}

TEST_CASE("A cluster resolves to its identifier, dev and its hints, with no host and no resume from idle", "[odbc]") {
	const auto settings = ResolveTarget(ClusterConfig("eu-west-1"));
	CHECK(settings.host.empty());
	CHECK(settings.workgroup.empty());
	CHECK(settings.cluster == "warehouse-1");
	CHECK(settings.database == "dev");
	CHECK(settings.hints.noun == "cluster");
	CHECK_FALSE(settings.hints.resumes_when_idle);
	CHECK(settings.hints.permission ==
	      "Grant it redshift:DescribeClusters on arn:aws:redshift:<region>:<account>:cluster:<cluster> and "
	      "redshift:GetClusterCredentialsWithIAM on arn:aws:redshift:<region>:<account>:dbname:<cluster>/<database>.");
	CHECK(settings.hints.availability ==
	      "The Redshift cluster may not exist or may be paused; check CLUSTER and REGION.");
}

TEST_CASE("A cluster identifier of 1 to 63 characters that starts with a letter is accepted", "[odbc]") {
	for (const auto &cluster : {std::string("a"), std::string("z"), std::string(63, 'c'), std::string("a-b-c"),
	                            std::string("warehouse-1"), std::string("z0123456789"), "a" + std::string(62, '9')}) {
		INFO("length " << cluster.size());
		CHECK(ResolveTarget(ClusterConfigWith(cluster)).cluster == cluster);
		CHECK(FindSetting(Parse(BuildConnectionString(ClusterConfigWith(cluster), TrustSource {TRUST_PATH})),
		                  "ClusterId") == cluster);
	}
}

TEST_CASE("A cluster identifier outside Redshift's rule is refused, naming no value", "[odbc]") {
	// Each range's neighbours: '`' and '{' beside a-z, '/' and ':' beside 0-9, ',' and '.' beside '-',
	// first as the leading character and then after it.
	for (const auto &cluster : {std::string(64, 'c'),      std::string("1abc"),  std::string("9abc"),
	                            std::string("0"),          std::string("-abc"),  std::string("abc-"),
	                            std::string("a-"),         std::string("ab--c"), std::string("a---b"),
	                            std::string("Abc"),        std::string("abC"),   std::string("`a"),
	                            std::string("{a"),         std::string("a`"),    std::string("a{"),
	                            std::string("a/"),         std::string("a:"),    std::string("a,"),
	                            std::string("a."),         std::string("/a"),    std::string(":a"),
	                            std::string(",a"),         std::string(".a"),    std::string("a_b"),
	                            std::string("a b"),        std::string("a}"),    std::string("};Host={evil.example"),
	                            std::string("a\0b", 3),    std::string("a\n"),   std::string(" a"),
	                            std::string("caf\xc3\xa9")}) {
		INFO("length " << cluster.size());
		CHECK(Capture([&] { BuildConnectionString(ClusterConfigWith(cluster), TrustSource {TRUST_PATH}); }) ==
		      CLUSTER_REFUSAL);
	}
	for (auto &cluster : InjectingValues()) {
		CHECK(Capture([&] { ResolveTarget(ClusterConfigWith(cluster)); }) == CLUSTER_REFUSAL);
	}
}

TEST_CASE("A cluster's database follows the database rules, and none means dev", "[odbc]") {
	for (const auto &database : {std::string("sales"), std::string(127, 'd'), std::string("team db")}) {
		INFO("length " << database.size());
		CHECK(ResolveTarget(ClusterConfigWith("warehouse-1", database)).database == database);
	}
	CHECK(ResolveTarget(ClusterConfigWith("warehouse-1")).database == "dev");
	CHECK(Capture([] { ResolveTarget(ClusterConfigWith("warehouse-1", std::string(128, 'd'))); }) ==
	      DatabaseRefusal("is longer than the 127 bytes a Redshift database name can have"));
	for (auto &database : InjectingValues()) {
		CHECK(Capture([&] {
			      BuildConnectionString(ClusterConfigWith("warehouse-1", database), TrustSource {TRUST_PATH});
		      }) == DatabaseRefusal("holds '}', which ends a brace-quoted value"));
	}
}

TEST_CASE("The region, then the cluster identifier, then the database is checked for a cluster", "[odbc]") {
	auto config = ClusterConfigWith("Warehouse", "};x");
	config.region = "us-east-1.evil.example";
	CHECK(Capture([&] { ResolveTarget(config); }) == RegionRefusal());
	config.region = "us-east-1";
	CHECK(Capture([&] { ResolveTarget(config); }) == CLUSTER_REFUSAL);
	config.cluster = "warehouse";
	CHECK(Capture([&] { ResolveTarget(config); }) == DatabaseRefusal("holds '}', which ends a brace-quoted value"));
}

TEST_CASE("No accepted profile or database changes a setting a cluster's connection fixes", "[odbc]") {
	const auto fixed = ClusterTableSettings();
	for (auto &value : AcceptedValues()) {
		if (value.empty() || value.size() > 127) {
			continue;
		}
		INFO("length " << value.size());
		auto config = ClusterConfigWith("warehouse-1", value);
		config.profile = value;
		const auto settings = Parse(BuildConnectionString(config, TrustSource {TRUST_PATH}, GIVEN_VERSION));
		CHECK(settings.size() == fixed.size() + 1);
		CHECK(FindSetting(settings, "Profile") == value);
		CHECK(FindSetting(settings, "Database") == value);
		for (auto &setting : fixed) {
			if (setting.first != "Database") {
				CHECK(FindSetting(settings, setting.first) == setting.second);
			}
		}
	}
}

TEST_CASE("A labelled workgroup endpoint connects to exactly its host, in every build", "[odbc]") {
	const auto settings = ResolveTarget(LabelledConfig());
	// An account-backed workgroup's host form, with the label in the account's place.
	CHECK(settings.host == "wg-a.lbl.eu-north-1.redshift-serverless.amazonaws.com");
	CHECK(settings.workgroup == "wg-a");
	CHECK(settings.cluster.empty());
	CHECK(settings.database == "dev");
	CHECK(settings.hints.noun == "workgroup");
	CHECK(settings.hints.resumes_when_idle);
	CHECK(settings.hints.permission == "Grant it redshift-serverless:GetCredentials on "
	                                   "arn:aws:redshift-serverless:<region>:<account>:workgroup/lbl/wg-a; a workgroup "
	                                   "may need further IAM actions.");
	CHECK(settings.hints.availability ==
	      "Check the region and the endpoint, and that this host can resolve and reach it.");
	// The usual Serverless settings, with the host the endpoint names and the caller's database.
	auto config = LabelledConfig();
	config.database = "analytics";
	const auto parsed = Parse(BuildConnectionString(config, TrustSource {TRUST_PATH}, GIVEN_VERSION));
	CHECK(FindSetting(parsed, "Host") == "wg-a.lbl.eu-north-1.redshift-serverless.amazonaws.com");
	CHECK(FindSetting(parsed, "Workgroup") == "wg-a");
	CHECK(FindSetting(parsed, "Serverless") == "true");
	CHECK(FindSetting(parsed, "IAM") == "true");
	CHECK(FindSetting(parsed, "Port") == "5439");
	CHECK(FindSetting(parsed, "Region") == "eu-north-1");
	CHECK(FindSetting(parsed, "EndPointUrl") == "https://redshift-serverless.eu-north-1.amazonaws.com");
	CHECK(FindSetting(parsed, "Database") == "analytics");
	CHECK(FindSetting(parsed, "SSLMode") == "verify-full");
}

TEST_CASE("A workgroup endpoint label that could change the host is refused before any driver call", "[odbc]") {
	const Thrown refusal {ExceptionType::INVALID_INPUT,
	                      "Invalid Redshift target: a workgroup endpoint label is 1 to 63 lower-case letters, digits "
	                      "or '-', not starting or ending with '-' and not digits alone"};
	// Digits alone in the label's place are an account, so such a label would reach an account's workgroup.
	for (const auto &label :
	     {std::string("-lbl"), std::string("lbl-"), std::string("LBL"), std::string("l.b"), std::string("l}b"),
	      std::string(64, 'l'), std::string("l\0b", 3), std::string("012345678901"), std::string("1")}) {
		INFO(label.size());
		CHECK(Capture([&] { ResolveTarget(LabelledConfig(label)); }) == refusal);
	}
	// The longest and shortest labels pass, and so do a '-' inside one and digits beside a letter.
	for (const auto &label :
	     {std::string(63, 'l'), std::string("l"), std::string("l-1"), std::string("01234567890a")}) {
		INFO(label);
		CHECK(ResolveTarget(LabelledConfig(label)).host ==
		      "wg-a." + label + ".eu-north-1.redshift-serverless.amazonaws.com");
	}
	// The workgroup name and the database keep their own rules.
	CHECK(Capture([&] { ResolveTarget(LabelledConfig("lbl", "};SSLMode={disable")); }) == WORKGROUP_REFUSAL);
	auto config = LabelledConfig();
	config.database = "dev}";
	CHECK(Capture([&] { ResolveTarget(config); }) == DatabaseRefusal("holds '}', which ends a brace-quoted value"));
}

TEST_CASE("A workgroup endpoint label goes only with a workgroup given without an account", "[odbc]") {
	auto with_account = LabelledConfig();
	with_account.account_id = "012345678901";
	CHECK(Capture([&] { ResolveTarget(with_account); }) ==
	      Thrown {ExceptionType::INVALID_INPUT, "Invalid Redshift target: a workgroup endpoint label goes only with a "
	                                            "workgroup given without an account"});
	auto cluster = ClusterConfig("us-east-1");
	cluster.endpoint_label = "lbl";
	CHECK(Capture([&] { ResolveTarget(cluster); }) ==
	      Thrown {ExceptionType::INVALID_INPUT,
	              "Invalid Redshift target: a cluster endpoint's label and port go together"});
}

TEST_CASE("A cluster endpoint sends Host and Port with ClusterId, so the driver looks nothing up", "[odbc]") {
	auto config = ClusterEndpointWithLabel("abc123xyz", 5440);
	const auto text = BuildConnectionString(config, TrustSource {TRUST_PATH}, GIVEN_VERSION);
	// Host and Port together: the driver calls DescribeClusters only when one is missing.
	CHECK(duckdb::StringUtil::StartsWith(text, "Host={warehouse-1.abc123xyz.eu-north-1.redshift.amazonaws.com};"
	                                           "EndPointUrl={https://redshift.eu-north-1.amazonaws.com};"
	                                           "Region={eu-north-1};Port={5440};Database={dev};"
	                                           "ClusterId={warehouse-1};IAM={true};group_federation={1};"
	                                           "SSLMode={verify-full};"));
	const auto settings = Parse(text);
	CHECK(JoinKeywords(settings) ==
	      "ApplicationName,CaFile,ClusterId,Compression,CscEnable,Database,DisableCache,EndPointUrl,group_federation,"
	      "Host,IAM,KeepAlive,KeepAliveCount,KeepAliveIdle,KeepAliveInterval,LoginTimeout,LogLevel,MaxVarcharSize,Port,"
	      "Region,SSLMode,StreamingCursorRows,TrustStore,UseDeclareFetch");
	for (auto keyword : {"Server", "Workgroup", "Serverless", "DbUser", "UID", "PWD"}) {
		CHECK(FindSetting(settings, keyword) == "(absent)");
	}
	// The port the path gave, its default included, reaches Port unchanged.
	for (auto port : {1, 5439, 65535}) {
		INFO(port);
		CHECK(FindSetting(
		          Parse(BuildConnectionString(ClusterEndpointWithLabel("abc123xyz", port), TrustSource {TRUST_PATH})),
		          "Port") == std::to_string(port));
	}
}

TEST_CASE("A cluster endpoint's host is rebuilt from its checked identifier, label and region", "[odbc]") {
	const auto settings = ResolveTarget(ClusterEndpointWithLabel("abc123xyz", 5440));
	CHECK(settings.host == "warehouse-1.abc123xyz.eu-north-1.redshift.amazonaws.com");
	CHECK(settings.port == 5440);
	CHECK(settings.cluster == "warehouse-1");
	CHECK(settings.workgroup.empty());
	CHECK(settings.database == "dev");
	CHECK(settings.hints.noun == "cluster");
	CHECK_FALSE(settings.hints.resumes_when_idle);
	// The connection's target still names the cluster.
	CHECK(TargetName(settings, duckdb::TestEndpoints()) == "warehouse-1");
	auto other = ClusterEndpointWithLabel("q0", 5439);
	other.cluster = "a";
	other.region = "us-west-2";
	CHECK(ResolveTarget(other).host == "a.q0.us-west-2.redshift.amazonaws.com");
	// The longest and shortest labels pass.
	for (const auto &label : {std::string(63, 'l'), std::string("l"), std::string("0")}) {
		INFO(label);
		CHECK(ResolveTarget(ClusterEndpointWithLabel(label)).host ==
		      "warehouse-1." + label + ".eu-north-1.redshift.amazonaws.com");
	}
}

TEST_CASE("A cluster endpoint's hints name no DescribeClusters, and CLUSTER alone keeps it", "[odbc]") {
	const auto endpoint = ResolveTarget(ClusterEndpointWithLabel());
	CHECK(endpoint.hints.permission == "Grant it redshift:GetClusterCredentialsWithIAM on "
	                                   "arn:aws:redshift:<region>:<account>:dbname:<cluster>/<database>.");
	CHECK(endpoint.hints.availability == "The Redshift cluster may not exist in the account of these credentials, or "
	                                     "may be paused; check the endpoint and the credentials.");
	// A login the endpoint's cluster refuses may be another account's credentials for a cluster of the same name.
	CHECK(endpoint.hints.login_refused == "The credentials may be for another account that also has a cluster of "
	                                      "this name; use credentials for the account of the endpoint.");
	const auto by_identifier = ResolveTarget(ClusterConfig("eu-north-1"));
	CHECK(by_identifier.host.empty());
	CHECK(by_identifier.port == 0);
	CHECK(by_identifier.hints.permission ==
	      "Grant it redshift:DescribeClusters on arn:aws:redshift:<region>:<account>:cluster:<cluster> and "
	      "redshift:GetClusterCredentialsWithIAM on arn:aws:redshift:<region>:<account>:dbname:<cluster>/<database>.");
	// Only a cluster endpoint names another account for a refused login; a cluster named by its identifier
	// gets the general message, and a workgroup its own hint, as its login uses a password GetCredentials
	// has just issued.
	CHECK(by_identifier.hints.login_refused.empty());
	CHECK(ResolveTarget(Config("us-east-1", "")).hints.login_refused ==
	      "The workgroup refused the temporary database credentials the driver got from GetCredentials. Retry; if "
	      "it keeps failing, check that this AWS identity may sign in to the workgroup's database.");
}

TEST_CASE("A cluster endpoint label or port that could change the endpoint is refused before any driver call",
          "[odbc]") {
	const Thrown label_refusal {ExceptionType::INVALID_INPUT,
	                            "Invalid Redshift target: a cluster endpoint label is 1 to 63 lower-case letters or "
	                            "digits"};
	for (const auto &label : {std::string("Abc"), std::string("a-b"), std::string("-ab"), std::string("a.b"),
	                          std::string("a}b"), std::string("a_b"), std::string(64, 'l'), std::string("a\0b", 3)}) {
		INFO(label.size());
		CHECK(Capture([&] { ResolveTarget(ClusterEndpointWithLabel(label)); }) == label_refusal);
	}
	const Thrown port_refusal {ExceptionType::INVALID_INPUT,
	                           "Invalid Redshift target: a cluster endpoint port is a number from 1 to 65535"};
	for (auto port : {-1, 65536, 70000}) {
		INFO(port);
		CHECK(Capture([&] { ResolveTarget(ClusterEndpointWithLabel("abc123xyz", port)); }) == port_refusal);
	}
	// The cluster identifier and the database keep their own rules.
	auto config = ClusterEndpointWithLabel();
	config.cluster = "};SSLMode={disable";
	CHECK_THROWS_WITH(ResolveTarget(config), Catch::Contains("Invalid value for parameter 'CLUSTER'"));
	config = ClusterEndpointWithLabel();
	config.database = "dev}";
	CHECK(Capture([&] { ResolveTarget(config); }) == DatabaseRefusal("holds '}', which ends a brace-quoted value"));
}

TEST_CASE("A cluster endpoint's label and port go together, and a workgroup endpoint takes a port", "[odbc]") {
	const Thrown apart {ExceptionType::INVALID_INPUT,
	                    "Invalid Redshift target: a cluster endpoint's label and port go together"};
	CHECK(Capture([&] { ResolveTarget(ClusterEndpointWithLabel("abc123xyz", 0)); }) == apart);
	CHECK(Capture([&] { ResolveTarget(ClusterEndpointWithLabel("", 5439)); }) == apart);
	// A workgroup with its account or label takes its endpoint's port; the cluster rule does not apply.
	for (auto workgroup : {Config("us-east-1", ""), LabelledConfig()}) {
		workgroup.endpoint_port = 5440;
		CHECK_NOTHROW(ResolveTarget(workgroup));
	}
}

TEST_CASE("A workgroup with no account and no label still reaches only the build's accountless settings", "[odbc]") {
	auto config = LabelledConfig("");
	// The public build refuses every one, and no preview implementation accepts this one.
	CHECK_THROWS_AS(ResolveTarget(config), InvalidInputException);
}

//===--------------------------------------------------------------------===//
// AWS credentials from an AWS secret, and a workgroup's port
//===--------------------------------------------------------------------===//

namespace {

const char *const KEY_ID = "AKIDEXAMPLEKEYID0001";
const char *const SECRET_KEY = "wJalrXUtnFEMIK7MDENGbPxRfiCYEXAMPLEKEY";
const char *const SESSION_TOKEN = "IQoJb3JpZ2luX2VjEXAMPLESESSIONTOKEN";

duckdb::RedshiftAwsCredentials Credentials(const std::string &session_token = SESSION_TOKEN) {
	return duckdb::RedshiftAwsCredentials {KEY_ID, SECRET_KEY, session_token};
}

RedshiftConfig AwsSecretConfig(RedshiftConfig config) {
	config.aws_secret = "creds";
	return config;
}

Thrown CredentialRefusal(const RedshiftConfig &config, const duckdb::RedshiftAwsCredentials *credentials) {
	return Capture([&] { BuildConnectionString(config, TrustSource {TRUST_PATH}, GIVEN_VERSION, credentials); });
}

} // namespace

TEST_CASE("AWS credentials reach the driver as its static keys, with no Profile", "[odbc]") {
	const auto credentials = Credentials();
	for (auto config : {Config("us-east-1", ""), LabelledConfig(), ClusterConfig("us-east-1")}) {
		INFO(config.Describe());
		config = AwsSecretConfig(config);
		const auto parsed = Parse(BuildConnectionString(config, TrustSource {TRUST_PATH}, GIVEN_VERSION, &credentials));
		CHECK(FindSetting(parsed, "AccessKeyID") == KEY_ID);
		CHECK(FindSetting(parsed, "SecretAccessKey") == SECRET_KEY);
		CHECK(FindSetting(parsed, "SessionToken") == SESSION_TOKEN);
		CHECK(FindSetting(parsed, "Profile") == "(absent)");
		CHECK(FindSetting(parsed, "IAM") == "true");
		CHECK(FindSetting(parsed, "SSLMode") == "verify-full");
		CHECK(FindSetting(parsed, "DisableCache") == "true");
	}
	// Long-lived keys carry no session token, so none is sent.
	const auto long_lived = Credentials("");
	const auto parsed = Parse(BuildConnectionString(AwsSecretConfig(Config("us-east-1", "")), TrustSource {TRUST_PATH},
	                                                GIVEN_VERSION, &long_lived));
	CHECK(FindSetting(parsed, "AccessKeyID") == KEY_ID);
	CHECK(FindSetting(parsed, "SessionToken") == "(absent)");
	// Without an AWS secret, nothing credential-shaped is sent.
	const auto chain = Parse(BuildConnectionString(Config("us-east-1", ""), TrustSource {TRUST_PATH}, GIVEN_VERSION));
	CHECK(FindSetting(chain, "AccessKeyID") == "(absent)");
	CHECK(FindSetting(chain, "SecretAccessKey") == "(absent)");
}

TEST_CASE("AWS credentials come exactly with their AWS secret, and never beside a profile", "[odbc]") {
	const auto credentials = Credentials();
	const Thrown mismatch {ExceptionType::INVALID_INPUT, "Invalid Redshift configuration: AWS credentials are given "
	                                                     "exactly when an AWS secret is named"};
	CHECK(CredentialRefusal(Config("us-east-1", ""), &credentials) == mismatch);
	CHECK(CredentialRefusal(AwsSecretConfig(Config("us-east-1", "")), nullptr) == mismatch);
	CHECK(CredentialRefusal(AwsSecretConfig(Config("us-east-1", "etl")), &credentials) ==
	      Thrown {ExceptionType::INVALID_INPUT, "Invalid Redshift configuration: a profile cannot be given with an AWS "
	                                            "secret"});
	for (auto empty :
	     {duckdb::RedshiftAwsCredentials {"", SECRET_KEY, ""}, duckdb::RedshiftAwsCredentials {KEY_ID, "", ""}}) {
		CHECK(CredentialRefusal(AwsSecretConfig(Config("us-east-1", "")), &empty) ==
		      Thrown {ExceptionType::INVALID_INPUT,
		              "Invalid AWS credentials from AWS secret 'creds': the key ID or the "
		              "secret key is empty"});
	}
	// A name messages repeat passes the encoder's rules.
	auto bad_name = Config("us-east-1", "");
	bad_name.aws_secret = "a}b";
	const auto refusal = CredentialRefusal(bad_name, &credentials);
	CHECK(refusal.type == ExceptionType::INVALID_INPUT);
	CHECK(refusal.message.find("'SECRET'") != std::string::npos);
	CHECK(refusal.message.find("a}b") == std::string::npos);
}

TEST_CASE("An AWS credential the driver would cut short, or that could add a keyword, is refused unrepeated",
          "[odbc]") {
	const auto config = AwsSecretConfig(Config("us-east-1", ""));
	auto fits = duckdb::RedshiftAwsCredentials {std::string(255, 'A'), std::string(255, 's'), std::string(2047, 't')};
	CHECK_NOTHROW(BuildConnectionString(config, TrustSource {TRUST_PATH}, GIVEN_VERSION, &fits));
	const std::pair<duckdb::RedshiftAwsCredentials, std::string> too_long[] = {
	    {{std::string(256, 'A'), SECRET_KEY, ""}, "AccessKeyID is longer than the 255 bytes"},
	    {{KEY_ID, std::string(256, 's'), ""}, "SecretAccessKey is longer than the 255 bytes"},
	    {{KEY_ID, SECRET_KEY, std::string(2048, 't')}, "SessionToken is longer than the 2047 bytes"},
	};
	for (auto &entry : too_long) {
		INFO(entry.second);
		const auto refusal = CredentialRefusal(config, &entry.first);
		CHECK(refusal.type == ExceptionType::INVALID_INPUT);
		CHECK(refusal.message.find(entry.second) != std::string::npos);
		// Each row's own offending value: 200 bytes of it would show any echo.
		const auto &offending = entry.first.access_key_id.size() > 255       ? entry.first.access_key_id
		                        : entry.first.secret_access_key.size() > 255 ? entry.first.secret_access_key
		                                                                     : entry.first.session_token;
		CHECK(refusal.message.find(offending.substr(0, 200)) == std::string::npos);
	}
	const char *const keywords[] = {"AccessKeyID", "SecretAccessKey", "SessionToken"};
	for (auto &value : InjectingValues()) {
		INFO(value);
		for (int field = 0; field < 3; field++) {
			auto credentials = Credentials();
			(field == 0   ? credentials.access_key_id
			 : field == 1 ? credentials.secret_access_key
			              : credentials.session_token) = value;
			const auto refusal = CredentialRefusal(config, &credentials);
			CHECK(refusal.type == ExceptionType::INVALID_INPUT);
			// Refused by the field's own value rule, which names it, not by a later step.
			const auto prefix =
			    std::string("Invalid AWS credentials from AWS secret 'creds': ") + keywords[field] + " ";
			CHECK(refusal.message.rfind(prefix, 0) == 0);
			// A value as short as `}` is in the rule the message states; a longer one is never repeated.
			if (value.size() >= 3) {
				CHECK(refusal.message.find(value) == std::string::npos);
			}
		}
	}
	for (auto value : {std::string(" ") + KEY_ID, std::string(KEY_ID) + " ", std::string(KEY_ID) + "\n",
	                   std::string(KEY_ID) + std::string(1, '\0')}) {
		auto credentials = Credentials();
		credentials.access_key_id = value;
		const auto refusal = CredentialRefusal(config, &credentials);
		CHECK(refusal.type == ExceptionType::INVALID_INPUT);
		CHECK(refusal.message.rfind("Invalid AWS credentials from AWS secret 'creds': AccessKeyID ", 0) == 0);
		CHECK(refusal.message.find(KEY_ID) == std::string::npos);
	}
}

TEST_CASE("An AWS secret name the driver's messages could not carry is refused unrepeated", "[odbc]") {
	const auto credentials = Credentials();
	auto config = AwsSecretConfig(Config("us-east-1", ""));
	// Every byte a name may hold, at the longest.
	config.aws_secret = std::string(255, 'n');
	CHECK_NOTHROW(BuildConnectionString(config, TrustSource {TRUST_PATH}, GIVEN_VERSION, &credentials));
	const std::string name = "secret_name_canary";
	const std::pair<std::string, std::string> refused[] = {
	    {name + std::string(256 - name.size(), 'n'), "is longer than the 255 bytes"},
	    {" " + name, "starts with a space, which the driver trims"},
	    {name + " ", "ends with a space, which the driver trims"},
	    {name + std::string(1, '\0'), "holds a NUL byte"},
	    {name + "\n", "holds an ASCII control character (0x0a)"},
	    {name + "}", "holds '}', which ends a brace-quoted value"},
	};
	for (auto &entry : refused) {
		INFO(entry.second);
		config.aws_secret = entry.first;
		const auto refusal = CredentialRefusal(config, &credentials);
		CHECK(refusal.type == ExceptionType::INVALID_INPUT);
		CHECK(duckdb::StringUtil::StartsWith(refusal.message, "Invalid value for parameter 'SECRET': it "));
		CHECK(refusal.message.find(entry.second) != std::string::npos);
		CHECK(refusal.message.find("at most 255 bytes") != std::string::npos);
		CHECK(refusal.message.find(name) == std::string::npos);
	}
}

TEST_CASE("A workgroup's port reaches the driver, 5439 when none is given", "[odbc]") {
	for (auto config : {Config("us-east-1", ""), LabelledConfig()}) {
		for (auto port : {0, 1, 5439, 5440, 65535}) {
			INFO(config.Describe() << " port " << port);
			config.endpoint_port = static_cast<uint16_t>(port);
			const auto parsed = Parse(BuildConnectionString(config, TrustSource {TRUST_PATH}, GIVEN_VERSION));
			CHECK(FindSetting(parsed, "Port") == (port == 0 ? "5439" : std::to_string(port)));
		}
	}
	auto described = Config("us-east-1", "");
	described.endpoint_port = 5440;
	described.aws_secret = "creds";
	CHECK(described.Describe() ==
	      "region us-east-1, credentials from AWS secret creds, workgroup analytics, account 012345678901, port 5440");
}

TEST_CASE("A workgroup endpoint port outside 1 to 65535 is refused before any driver call", "[odbc]") {
	const Thrown refusal {ExceptionType::INVALID_INPUT,
	                      "Invalid Redshift target: a workgroup endpoint port is a number from 1 to 65535"};
	for (auto config : {Config("us-east-1", ""), LabelledConfig()}) {
		for (auto port : {-1, 65536, 70000}) {
			INFO(config.Describe() << " port " << port);
			config.endpoint_port = port;
			CHECK(Capture([&] { BuildConnectionString(config, TrustSource {TRUST_PATH}); }) == refusal);
		}
	}
}

TEST_CASE("A port is refused for a target whose port is not the user's", "[odbc]") {
	// A cluster named alone has no endpoint label, so it takes no port: only a cluster endpoint carries one.
	const Thrown refusal {ExceptionType::INVALID_INPUT,
	                      "Invalid Redshift target: a cluster endpoint's label and port go together"};
	auto cluster = ClusterConfig("us-east-1");
	cluster.endpoint_port = 5439;
	CHECK(Capture([&] { BuildConnectionString(cluster, TrustSource {TRUST_PATH}); }) == refusal);
	// A workgroup with neither account nor label goes to the target hook's settings: refused
	// by its hook in this build.
	auto accountless = LabelledConfig("");
	accountless.endpoint_port = 5440;
	CHECK(Capture([&] { BuildConnectionString(accountless, TrustSource {TRUST_PATH}); }).type ==
	      ExceptionType::INVALID_INPUT);
}

TEST_CASE("Only a target reached on 5439 for want of a port says how to give another", "[odbc]") {
	// A workgroup by name with its account, and a labelled workgroup endpoint, with no port.
	CHECK(ResolveTarget(Config("us-east-1", "")).hints.default_port);
	CHECK(ResolveTarget(LabelledConfig()).hints.default_port);
	// The same with a port: the path gave it.
	auto workgroup = Config("us-east-1", "");
	workgroup.endpoint_port = 5440;
	CHECK_FALSE(ResolveTarget(workgroup).hints.default_port);
	auto labelled = LabelledConfig();
	labelled.endpoint_port = 5440;
	CHECK_FALSE(ResolveTarget(labelled).hints.default_port);
	// A cluster endpoint, whose label always comes with its port.
	CHECK_FALSE(ResolveTarget(ClusterEndpointWithLabel("abc123xyz", 5440)).hints.default_port);
	// A cluster by its identifier: the driver looks its port up.
	CHECK_FALSE(ResolveTarget(ClusterConfig("us-east-1")).hints.default_port);
}
