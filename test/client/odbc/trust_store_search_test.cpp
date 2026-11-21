// The trust search Linux and macOS share, over files the cases create (and /dev/null), so no case
// depends on which CA bundles the host has installed. Refusals are checked against the whole
// message.

// catch.hpp comes first, as the other C++ suites include it: DuckDB's copy declares the
// duckdb_base_std aliases it needs only while no DuckDB header has opened that namespace.
#include "catch.hpp"

#include "duckdb/common/exception.hpp"
#include "odbc/trust_store_search.hpp"
#include "thrown.hpp"
#include "trust_store_test_support.hpp"

#include <string>
#include <sys/stat.h>
#include <unistd.h>
#include <vector>

using duckdb::ExceptionType;
using duckdb::IsReadableBundle;
using duckdb::ResolveTrustSourceFrom;
using redshift_test::Capture;
using redshift_test::Thrown;
using redshift_trust_test::BUNDLE;
using redshift_trust_test::Locations;
using redshift_trust_test::NO_BUNDLE_MESSAGE;
using redshift_trust_test::NoTrustSourceMessage;
using redshift_trust_test::RecordingLogSink;
using redshift_trust_test::TemporaryDirectory;
using redshift_trust_test::UNREADABLE_LINE;

namespace {

//! The refusal of a readable SSL_CERT_FILE whose path the driver cannot be given, for `reason`.
std::string UnusablePathMessage(const std::string &reason) {
	return "SSL_CERT_FILE names a CA bundle whose path " + reason +
	       ", so the Redshift driver cannot be given it. Move the bundle to a path of at most 255 bytes, with no "
	       "leading or trailing space and without an ASCII control character or '}', or unset SSL_CERT_FILE to use "
	       "the system CA certificates";
}

bool Mentions(const std::string &text, const std::string &part) {
	return text.find(part) != std::string::npos;
}

} // namespace

TEST_CASE("The first readable system location is the trust source", "[odbc]") {
	TemporaryDirectory directory;
	const std::vector<std::string> names = {"first.pem", "second.pem", "third.pem"};
	Locations locations;
	for (auto &name : names) {
		locations.push_back(directory.Path(name));
	}
	// The locations are created from the last to the first, so each turn finds a new first readable
	// one, which a search that kept an earlier answer would miss.
	for (size_t i = names.size(); i-- > 0;) {
		directory.File(names[i], BUNDLE);
		RecordingLogSink log_sink;
		CHECK(ResolveTrustSourceFrom(nullptr, locations, log_sink).path == locations[i]);
		CHECK(log_sink.lines.empty());
	}
}

TEST_CASE("A readable SSL_CERT_FILE is the trust source ahead of a readable system location", "[odbc]") {
	TemporaryDirectory directory;
	auto chosen = directory.File("chosen.pem", BUNDLE);
	RecordingLogSink log_sink;
	auto resolved = ResolveTrustSourceFrom(chosen.c_str(), {directory.File("system.pem", BUNDLE)}, log_sink);
	CHECK(resolved.path == chosen);
	CHECK(log_sink.lines.empty());
}

TEST_CASE("An SSL_CERT_FILE that is a symbolic link to a bundle is the trust source, as the link", "[odbc]") {
	TemporaryDirectory directory;
	auto link = directory.Link("link.pem", directory.File("bundle.pem", BUNDLE));
	RecordingLogSink log_sink;
	CHECK(ResolveTrustSourceFrom(link.c_str(), {directory.File("system.pem", BUNDLE)}, log_sink).path == link);
	CHECK(log_sink.lines.empty());
}

TEST_CASE("An SSL_CERT_FILE that names no readable file is logged, then the system locations are searched", "[odbc]") {
	TemporaryDirectory directory;
	auto system = directory.File("system.pem", BUNDLE);
	// An empty value counts as set, as it does for the variables the driver refuses. The last two
	// paths would be refused if they named readable files, so readability is what is checked first.
	for (auto &value : {directory.Path("absent.pem"), std::string(), directory.Path("absent}.pem"),
	                    directory.Path(std::string(300, 'c'))}) {
		RecordingLogSink log_sink;
		CHECK(ResolveTrustSourceFrom(value.c_str(), {system}, log_sink).path == system);
		CHECK(log_sink.lines == std::vector<std::string> {UNREADABLE_LINE});
	}
}

TEST_CASE("An SSL_CERT_FILE this process may not read is logged, then the system locations are searched", "[odbc]") {
	// The superuser may read a file whatever its mode, so the case means nothing when run as root.
	if (geteuid() == 0) {
		WARN("running as root: a file's mode cannot make it unreadable");
		return;
	}
	TemporaryDirectory directory;
	auto system = directory.File("system.pem", BUNDLE);
	auto unreadable = directory.File("unreadable.pem", BUNDLE);
	REQUIRE(chmod(unreadable.c_str(), 0) == 0);
	RecordingLogSink log_sink;
	CHECK(ResolveTrustSourceFrom(unreadable.c_str(), {system}, log_sink).path == system);
	CHECK(log_sink.lines == std::vector<std::string> {UNREADABLE_LINE});
}

TEST_CASE("The unreadable SSL_CERT_FILE is logged when no system location resolves either", "[odbc]") {
	TemporaryDirectory directory;
	auto absent = directory.Path("absent.pem");
	RecordingLogSink log_sink;
	Locations locations = {directory.Path("system.pem")};
	CHECK(Capture([&] { ResolveTrustSourceFrom(absent.c_str(), locations, log_sink); }) ==
	      Thrown {ExceptionType::IO, NoTrustSourceMessage(true, locations)});
	CHECK(log_sink.lines == std::vector<std::string> {UNREADABLE_LINE});
}

TEST_CASE("With no readable location the error names the mechanism, every location tried and the fix", "[odbc]") {
	TemporaryDirectory directory;
	Locations locations = {directory.Path("first.pem"), directory.Directory("second.pem"),
	                       directory.File("third.pem", "")};
	RecordingLogSink log_sink;
	CHECK(Capture([&] { ResolveTrustSourceFrom(nullptr, locations, log_sink); }) ==
	      Thrown {ExceptionType::IO, NoTrustSourceMessage(false, locations)});
	CHECK(log_sink.lines.empty());
}

TEST_CASE("A readable SSL_CERT_FILE the driver cannot be given as written is refused, not passed over", "[odbc]") {
	TemporaryDirectory directory;
	// A system location is readable, so a search that fell back would resolve rather than throw.
	Locations locations = {directory.File("system.pem", BUNDLE)};
	struct Case {
		std::string name;
		std::string reason;
	};
	for (auto &refused : {Case {"ca}.pem", "holds '}', which ends a brace-quoted value"},
	                      Case {"ca\n.pem", "holds an ASCII control character (0x0a)"},
	                      Case {"ca.pem ", "ends with a space, which the driver trims"}}) {
		auto path = directory.File(refused.name, BUNDLE);
		RecordingLogSink log_sink;
		CHECK(Capture([&] { ResolveTrustSourceFrom(path.c_str(), locations, log_sink); }) ==
		      Thrown {ExceptionType::IO, UnusablePathMessage(refused.reason)});
		CHECK(log_sink.lines.empty());
	}
}

TEST_CASE("A readable SSL_CERT_FILE that is no CA bundle is refused, not passed over", "[odbc]") {
	TemporaryDirectory directory;
	// A system location is readable, so a search that fell back would resolve rather than throw.
	// empty}.pem also breaks the trust-path rules, and gets this refusal, so the order is fixed.
	Locations locations = {directory.File("system.pem", BUNDLE)};
	for (auto &path : {directory.File("empty.pem", ""), directory.Directory("bundle.d"), std::string("/dev/null"),
	                   directory.File("empty}.pem", "")}) {
		RecordingLogSink log_sink;
		CHECK(Capture([&] { ResolveTrustSourceFrom(path.c_str(), locations, log_sink); }) ==
		      Thrown {ExceptionType::IO, NO_BUNDLE_MESSAGE});
		CHECK(log_sink.lines.empty());
	}
}

TEST_CASE("A 255-byte SSL_CERT_FILE path is the trust source and a 256-byte one is refused", "[odbc]") {
	TemporaryDirectory directory;
	Locations locations = {directory.File("system.pem", BUNDLE)};
	// A TMPDIR this long leaves no room for a name, and the subtraction below would wrap.
	REQUIRE(directory.RootLength() + 2 <= 255);
	auto name_bytes = 255 - directory.RootLength() - 1;
	auto longest = directory.File(std::string(name_bytes, 'a'), BUNDLE);
	auto too_long = directory.File(std::string(name_bytes + 1, 'b'), BUNDLE);
	REQUIRE(longest.size() == 255);
	REQUIRE(too_long.size() == 256);
	RecordingLogSink log_sink;
	CHECK(ResolveTrustSourceFrom(longest.c_str(), locations, log_sink).path == longest);
	CHECK(Capture([&] { ResolveTrustSourceFrom(too_long.c_str(), locations, log_sink); }) ==
	      Thrown {ExceptionType::IO, UnusablePathMessage("is longer than the 255 bytes the driver keeps")});
	CHECK(log_sink.lines.empty());
}

TEST_CASE("Neither the log line nor an error carries SSL_CERT_FILE's value", "[odbc]") {
	TemporaryDirectory directory;
	auto private_directory = directory.Directory("user-9-private");
	auto absent = private_directory + "/ca.pem";
	auto refused = directory.File("user-9-private/ca}.pem", BUNDLE);
	auto empty = directory.File("user-9-private/empty.pem", "");
	RecordingLogSink log_sink;
	auto unreadable = Capture([&] { ResolveTrustSourceFrom(absent.c_str(), {}, log_sink); });
	auto unusable = Capture([&] { ResolveTrustSourceFrom(refused.c_str(), {}, log_sink); });
	auto no_bundle = Capture([&] { ResolveTrustSourceFrom(empty.c_str(), {}, log_sink); });
	REQUIRE(unreadable.type == ExceptionType::IO);
	REQUIRE(unusable.type == ExceptionType::IO);
	REQUIRE(no_bundle.type == ExceptionType::IO);
	REQUIRE(log_sink.lines.size() == 1);
	CHECK(!Mentions(unreadable.message, "user-9-private"));
	CHECK(!Mentions(unusable.message, "user-9-private"));
	CHECK(!Mentions(no_bundle.message, "user-9-private"));
	CHECK(!Mentions(log_sink.lines[0], "user-9-private"));
}

TEST_CASE("A non-empty regular file is a readable bundle, directly or through a symbolic link", "[odbc]") {
	TemporaryDirectory directory;
	auto bundle = directory.File("bundle.pem", BUNDLE);
	CHECK(IsReadableBundle(bundle));
	CHECK(IsReadableBundle(directory.Link("link.pem", bundle)));
}

TEST_CASE("A directory, an empty file or a missing path is not a readable bundle", "[odbc]") {
	TemporaryDirectory directory;
	CHECK(!IsReadableBundle(directory.Directory("bundle.d")));
	CHECK(!IsReadableBundle(directory.File("empty.pem", "")));
	CHECK(!IsReadableBundle(directory.Path("absent.pem")));
	CHECK(!IsReadableBundle(directory.Link("dangling.pem", directory.Path("absent.pem"))));
	CHECK(!IsReadableBundle(""));
}

TEST_CASE("A file this process may not read is not a readable bundle", "[odbc]") {
	// The superuser may read a file whatever its mode, so the case means nothing when run as root.
	if (geteuid() == 0) {
		WARN("running as root: a file's mode cannot make it unreadable");
		return;
	}
	TemporaryDirectory directory;
	auto bundle = directory.File("bundle.pem", BUNDLE);
	REQUIRE(chmod(bundle.c_str(), 0) == 0);
	CHECK(!IsReadableBundle(bundle));
}
