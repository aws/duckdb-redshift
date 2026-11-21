// The generator of the aws-partition region allowlist that ValidateRegion checks against
// (region_validation_test.cpp). It runs as a subprocess on POSIX paths, so this file runs on Linux
// and macOS. The cases hand it tampered data and unusable paths, so losing any one of its refusals
// fails a case. The last cases, for the driver-lock read and the test-endpoint switch in the
// sources.cmake files and the Makefile, and for the client's reads of the environment, are here
// for this file's subprocess and source-file helpers.

// catch.hpp comes first, as the extension's C++ tests include it: DuckDB's copy declares the
// duckdb_base_std aliases it needs only while no DuckDB header has opened that namespace.
#include "catch.hpp"

#include "duckdb/common/string_util.hpp"
#include "odbc/connection_settings.hpp"

#include <algorithm>
#include <cctype>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <dirent.h>
#include <fstream>
#include <iterator>
#include <memory>
#include <regex>
#include <set>
#include <sstream>
#include <string>
#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>
#include <utility>
#include <utime.h>
#include <vector>

#ifndef REDSHIFT_SOURCE_DIR
#error "REDSHIFT_SOURCE_DIR must name the checkout; test/client/odbc/sources.cmake sets it"
#endif
#ifndef REDSHIFT_PYTHON
#error "REDSHIFT_PYTHON must name the interpreter; test/client/odbc/sources.cmake sets it"
#endif

namespace {

//! What a shell command exited with, and what it printed. `wait_status` is pclose's status, which
//! is 0 only for a command that ran and exited 0.
struct Ran {
	int wait_status;
	std::string output;
};

//! The status a command exited with, or -1 when it did not exit normally.
int ExitStatus(const Ran &ran) {
	return WIFEXITED(ran.wait_status) ? WEXITSTATUS(ran.wait_status) : -1;
}

//! Checks that the generator refused with `message`. Every refusal exits 2, so a crash's traceback,
//! which exits 1, does not pass for one.
void CheckRefusal(const Ran &ran, const std::string &message) {
	CHECK(ExitStatus(ran) == 2);
	CHECK(ran.output.find(message) != std::string::npos);
}

//! An open pipe to a command. The destructor closes a pipe a throw left open, which would otherwise
//! leak the descriptor and leave the child unreaped.
class Pipe {
public:
	explicit Pipe(const std::string &command) : stream(popen(command.c_str(), "r")) {
	}
	~Pipe() {
		if (stream != nullptr) {
			pclose(stream);
		}
	}
	Pipe(const Pipe &) = delete;
	Pipe &operator=(const Pipe &) = delete;

	//! The stream to read the output from, or null when the command could not be started.
	FILE *Stream() const {
		return stream;
	}

	//! pclose's status, which is 0 only for a command that ran and exited 0.
	int Close() {
		FILE *closing = stream;
		stream = nullptr;
		return pclose(closing);
	}

private:
	FILE *stream;
};

//! Run `command` through the shell, with stderr folded in so a failing check prints the reason,
//! and stdin from /dev/null so nothing can block on it.
Ran Run(const std::string &command) {
	Ran ran = {-1, std::string()};
	Pipe pipe(command + " </dev/null 2>&1");
	if (pipe.Stream() == nullptr) {
		ran.output = "could not run: " + command;
		return ran;
	}
	char buffer[512];
	while (std::fgets(buffer, sizeof(buffer), pipe.Stream()) != nullptr) {
		ran.output += buffer;
	}
	ran.wait_status = pipe.Close();
	return ran;
}

//! The shell command that runs `arguments` under the python3 this build was configured with and
//! `-I`, so no PYTHONPATH, user site or PATH lookup changes what runs. The build does not need
//! python3, so this suite is where a missing one is reported.
std::string PythonCommand(const std::string &arguments) {
	INFO("REDSHIFT_PYTHON is \"" << REDSHIFT_PYTHON
	                             << "\"; CMake found no python3 when it is empty: install python3 and "
	                                "re-run CMake");
	REQUIRE_FALSE(std::string(REDSHIFT_PYTHON).empty());
	return std::string("'") + REDSHIFT_PYTHON + "' -I " + arguments;
}

//! The generator script, quoted for a command line.
std::string QuotedGenerator() {
	return std::string("'") + REDSHIFT_SOURCE_DIR + "/src/transport/odbc/tools/gen-region-allowlist.py'";
}

//! The shell command that runs the generator with `arguments`.
std::string GeneratorCommand(const std::string &arguments) {
	return PythonCommand(QuotedGenerator() + " " + arguments);
}

//! Run the generator with `arguments`.
Ran RunGenerator(const std::string &arguments) {
	return Run(GeneratorCommand(arguments));
}

//! A dry run of `make <arguments>` in the checkout, without this run's make flags, which it would
//! pass on, or the caller's variables that make the pinned Makefile add a configure step or flags.
Ran DryRun(const std::string &arguments) {
	return Run("cd '" + std::string(REDSHIFT_SOURCE_DIR) +
	           "' && env -u MAKEFLAGS -u MFLAGS -u MAKELEVEL -u EXTENSION_CONFIG_STEP -u USE_MERGED_VCPKG_MANIFEST "
	           "-u DUCKDB_NEW_EXTENSION_BUILD -u EXT_FLAGS -u EXT_RELEASE_FLAGS make -n " +
	           arguments);
}

//! A mkstemp or mkdtemp template under TMPDIR when it is set, as mktemp does, or /tmp. A trailing
//! '/', as macOS's TMPDIR has, is dropped: the generator's paths do not keep the doubled slash.
std::string TempTemplate() {
	const char *directory = std::getenv("TMPDIR");
	std::string base = directory && *directory ? directory : "/tmp";
	while (!base.empty() && base.back() == '/') {
		base.pop_back();
	}
	return base + "/redshift_region_allowlist_XXXXXX";
}

//! A file the generator can be pointed at, holding what a case wants it to read, and removed when
//! the case ends.
class TempFile {
public:
	//! A failed write removes the file before the REQUIRE throws, since ~TempFile does not run when
	//! the constructor threw.
	explicit TempFile(const std::string &content) : path(TempTemplate()) {
		// The name is built before mkstemp, so nothing between creating the file and the checks can
		// throw and leave it behind.
		int descriptor = mkstemp(&path[0]);
		REQUIRE(descriptor != -1);
		FILE *handle = fdopen(descriptor, "w");
		if (!handle) {
			close(descriptor);
		}
		const bool written = handle && std::fwrite(content.data(), 1, content.size(), handle) == content.size();
		const bool closed = handle && std::fclose(handle) == 0;
		if (!written || !closed) {
			std::remove(path.c_str());
		}
		REQUIRE(written);
		REQUIRE(closed);
	}
	~TempFile() {
		std::remove(path.c_str());
	}
	TempFile(const TempFile &) = delete;
	TempFile &operator=(const TempFile &) = delete;

	//! The path to quote into a generator command line.
	const std::string &Path() const {
		return path;
	}

private:
	std::string path;
};

//! A new directory, removed with everything in it when the case ends.
class TempDirectory {
public:
	TempDirectory() : path(TempTemplate()) {
		REQUIRE(mkdtemp(&path[0]) != nullptr);
	}
	~TempDirectory() {
		Run("rm -rf -- '" + path + "'");
	}
	TempDirectory(const TempDirectory &) = delete;
	TempDirectory &operator=(const TempDirectory &) = delete;

	const std::string &Path() const {
		return path;
	}

private:
	std::string path;
};

//! Write `content` to `path`, replacing what is there.
void WriteFile(const std::string &path, const std::string &content) {
	std::ofstream file(path, std::ios::binary);
	file << content;
	file.close();
	REQUIRE(file.good());
}

//! The bytes of `path`, read whole.
std::string ReadFile(const std::string &path) {
	std::ifstream file(path, std::ios::binary);
	REQUIRE(file.good());
	return std::string((std::istreambuf_iterator<char>(file)), std::istreambuf_iterator<char>());
}

//! The names beside `path` that start with its own name and a dot, which a write beside it that it
//! did not clean up would leave.
std::vector<std::string> FilesBeside(const std::string &path) {
	auto slash = path.rfind('/');
	REQUIRE(slash != std::string::npos);
	const std::string prefix = path.substr(slash + 1) + ".";
	std::vector<std::string> names;
	DIR *directory = opendir(path.substr(0, slash).c_str());
	REQUIRE(directory != nullptr);
	for (struct dirent *entry = readdir(directory); entry != nullptr; entry = readdir(directory)) {
		if (std::string(entry->d_name).compare(0, prefix.size(), prefix) == 0) {
			names.push_back(path.substr(0, slash + 1) + entry->d_name);
		}
	}
	closedir(directory);
	return names;
}

//! The checked-in list, as the generator writes it.
std::string CheckedInList() {
	return ReadFile(std::string(REDSHIFT_SOURCE_DIR) + "/src/transport/odbc/region_allowlist.inc");
}

//! The text on the checked-in list's banner line that starts with `prefix`, up to `end`. Read, not
//! spelled out, so a bump of the pin changes no test; the --check case ties the banner to the pin.
std::string BannerField(const std::string &prefix, char end) {
	const std::string list = CheckedInList();
	auto start = list.find("\n" + prefix);
	REQUIRE(start != std::string::npos);
	start += 1 + prefix.size();
	auto stop = list.find(end, start);
	REQUIRE(stop != std::string::npos);
	REQUIRE(stop > start);
	return list.substr(start, stop - start);
}

//! The SDK tag the list was generated from.
std::string PinnedTag() {
	return BannerField("// Source: aws/aws-sdk-cpp tag ", ',');
}

//! The SHA-256 of the published endpoint data the list was generated from.
std::string PinnedDigest() {
	return BannerField("// SHA-256 of the published file: ", '\n');
}

//! The extract's record of the published file it came from, naming the pinned one.
std::string PinnedProvenance() {
	return "\"extractedFrom\": {\"sdkTag\": \"" + PinnedTag() + "\", \"sha256\": \"" + PinnedDigest() + "\"}";
}

//! The `aws` partition's own region pattern, as the published data spells it in JSON. A fixture
//! carries this rather than a pattern written for the test, so a case shows the published rule
//! refusing a name.
const char *const PUBLISHED_REGION_REGEX = R"(^(us|eu|ap|sa|ca|me|af|il|mx)\\-\\w+\\-\\d+$)";

//! The fields of one partition, as the published data holds them.
struct PartitionFields {
	std::string partition;
	std::string dns_suffix;
	std::string region_regex;
	std::string regions;
};

//! One partition in the shape the generator reads, with the given name, suffix, region pattern
//! and `regions` body.
std::string PartitionObject(const PartitionFields &fields) {
	return "{\n      \"dnsSuffix\": \"" + fields.dns_suffix + "\",\n      \"partition\": \"" + fields.partition +
	       "\",\n      \"regionRegex\": \"" + fields.region_regex + "\",\n      \"regions\": {" + fields.regions +
	       "}\n    }";
}

//! An extract holding `partitions`, each an object from PartitionObject or one written out, and
//! recording the pinned published file.
std::string EndpointsFixture(const std::string &partitions) {
	return "{\n  " + PinnedProvenance() + ",\n  \"partitions\": [\n    " + partitions + "\n  ]\n}\n";
}

//! Endpoint data in the shape the generator reads, with the one partition `fields` describes.
std::string EndpointFixture(const PartitionFields &fields) {
	return EndpointsFixture(PartitionObject(fields));
}

//! An extract shaped like the vendored one, differing only in `regions`, given here: the `aws`
//! partition, its real suffix and pattern, and the pinned record.
std::string AwsFixture(const std::string &regions) {
	return EndpointFixture(PartitionFields {"aws", "amazonaws.com", PUBLISHED_REGION_REGEX, regions});
}

//! One region entry, as the published data holds it.
std::string RegionEntry(const std::string &name) {
	return "\n        \"" + name + "\": {\n          \"description\": \"a region\"\n        }\n      ";
}

//! The vendored extract the generator reads by default.
std::string VendoredExtract() {
	return ReadFile(std::string(REDSHIFT_SOURCE_DIR) + "/src/transport/odbc/tools/endpoints-aws.json");
}

//! `text` with its one `from` replaced by `to`.
std::string ReplacedOnce(const std::string &text, const std::string &from, const std::string &to) {
	auto at = text.find(from);
	REQUIRE(at != std::string::npos);
	REQUIRE(text.find(from, at + 1) == std::string::npos);
	return text.substr(0, at) + to + text.substr(at + from.size());
}

//! Endpoint data in the published file's format, small: its keys out of order, and a second
//! partition, the aws partition's other keys and the file's version, all of which the extract drops.
const char *const PUBLISHED_FORMAT_FIXTURE = R"json({
  "version": 3,
  "partitions": [
    {
      "services": {"redshift": {"endpoints": {"us-east-1": {}}}},
      "regions": {
        "us-west-2": {"description": "US West (Oregon)"},
        "us-east-1": {"description": "US East (N. Virginia)"}
      },
      "regionRegex": "^(us|eu|ap|sa|ca|me|af|il|mx)\\-\\w+\\-\\d+$",
      "partitionName": "AWS Standard",
      "partition": "aws",
      "dnsSuffix": "amazonaws.com",
      "defaults": {"hostname": "{service}.{region}.{dnsSuffix}"}
    },
    {
      "partition": "aws-cn",
      "dnsSuffix": "amazonaws.com.cn",
      "regionRegex": "^cn\\-\\w+\\-\\d+$",
      "regions": {"cn-north-1": {"description": "China (Beijing)"}}
    }
  ]
}
)json";

//! The extract of PUBLISHED_FORMAT_FIXTURE, spelled out.
std::string ExpectedExtract() {
	return R"json({
  "extractedFrom": {
    "sdkTag": ")json" +
	       PinnedTag() + R"json(",
    "sha256": ")json" +
	       PinnedDigest() + R"json("
  },
  "partitions": [
    {
      "dnsSuffix": "amazonaws.com",
      "partition": "aws",
      "regionRegex": "^(us|eu|ap|sa|ca|me|af|il|mx)\\-\\w+\\-\\d+$",
      "regions": {
        "us-east-1": {
          "description": "US East (N. Virginia)"
        },
        "us-west-2": {
          "description": "US West (Oregon)"
        }
      }
    }
  ]
}
)json";
}

//! The shell command that runs the generator's render_extract on the published-format data at
//! `path`, loading the script without running its command line.
std::string RenderExtractCommand(const std::string &path) {
	return PythonCommand(
	    "-c 'import json, pathlib, runpy, sys; generator = runpy.run_path(sys.argv[1]); "
	    "sys.stdout.write(generator[\"render_extract\"](json.loads(pathlib.Path(sys.argv[2]).read_bytes())))' " +
	    QuotedGenerator() + " '" + path + "'");
}

//! The shell command that writes the list at `path`, printing the file each os.replace moves.
std::string RecordedWriteCommand(const std::string &path) {
	return PythonCommand("-c 'import os, runpy, sys; replace = os.replace; os.replace = lambda source, target: "
	                     "(print(\"replacing \" + source), replace(source, target)); sys.argv = sys.argv[1:]; "
	                     "runpy.run_path(sys.argv[0], run_name=\"__main__\")' " +
	                     QuotedGenerator() + " --list '" + path + "'");
}

//! The refusal --check gives for a list that is not the generated one.
std::string ListMismatchMessage() {
	return "is not what aws-sdk-cpp " + PinnedTag() + "'s endpoint data generates";
}

//! The checked-in list with one more name in it, as a hand edit to the file would leave it.
std::string ListWithExtraName() {
	std::string text = CheckedInList();
	auto closing = text.rfind("};");
	REQUIRE(closing != std::string::npos);
	return text.substr(0, closing) + "\t\"evil-1\",\n" + text.substr(closing);
}

} // namespace

TEST_CASE("The checked-in allowlist is the list the generator writes from the pinned data", "[odbc]") {
	// A hand edit to the list does not survive a test run. No network: the extract is vendored,
	// and tools/README.md records the published file and how to re-derive the extract.
	auto ran = RunGenerator("--check");
	INFO(ran.output);
	CHECK(ExitStatus(ran) == 0);
}

TEST_CASE("The generator refuses a list that is not the one the pinned data generates", "[odbc]") {
	// The positive control for the check above: without it, a typed-in name would reach a host
	// name with nothing having published it.
	TempFile tampered(ListWithExtraName());
	auto ran = RunGenerator("--check --list '" + tampered.Path() + "'");
	INFO(ran.output);
	CheckRefusal(ran, ListMismatchMessage());
}

TEST_CASE("The generator compares the list byte for byte", "[odbc]") {
	// CRLF line ends are not folded into the list's own, and bytes that are not UTF-8 are a
	// mismatch rather than a crash in the read.
	std::string crlf;
	for (auto byte : CheckedInList()) {
		crlf += byte == '\n' ? std::string("\r\n") : std::string(1, byte);
	}
	for (auto &content : std::vector<std::string> {crlf, "\xff\n"}) {
		TempFile list(content);
		auto ran = RunGenerator("--check --list '" + list.Path() + "'");
		INFO(ran.output);
		CheckRefusal(ran, ListMismatchMessage());
	}
}

TEST_CASE("The generator writes the list that --check expects", "[odbc]") {
	// Bumping the pin regenerates the list, so write mode must write exactly what --check expects.
	TempFile list("");
	auto ran = RunGenerator("--list '" + list.Path() + "'");
	INFO(ran.output);
	CHECK(ExitStatus(ran) == 0);
	CHECK(ReadFile(list.Path()) == CheckedInList());
}

TEST_CASE("The generator reads --extract - from standard input", "[odbc]") {
	// Standard input is empty, so a refusal naming the empty input's digest shows stdin was read
	// rather than a file named -.
	auto ran = RunGenerator("--extract -");
	INFO(ran.output);
	CheckRefusal(ran, "e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855");
}

TEST_CASE("The generator refuses endpoint data it will not take a region name from", "[odbc]") {
	// One row per refusal on the partition and its names, so losing any of them fails here: a region
	// name goes into the host name, so data unlike the published file is refused, not trusted for
	// where it sits.
	struct Fixture {
		const char *what;
		std::string data;
		const char *message;
	};
	const std::vector<Fixture> fixtures = {
	    {"no aws partition",
	     EndpointFixture(
	         PartitionFields {"aws-cn", "amazonaws.com.cn", PUBLISHED_REGION_REGEX, RegionEntry("cn-north-1")}),
	     "expected exactly one 'aws' partition"},
	    {"two aws partitions",
	     EndpointsFixture(PartitionObject(PartitionFields {"aws", "amazonaws.com", PUBLISHED_REGION_REGEX,
	                                                       RegionEntry("us-east-1")}) +
	                      ",\n    " +
	                      PartitionObject(PartitionFields {"aws", "amazonaws.com", PUBLISHED_REGION_REGEX,
	                                                       RegionEntry("us-west-2")})),
	     "expected exactly one 'aws' partition in the endpoint data, found 2"},
	    {"another dnsSuffix",
	     EndpointFixture(PartitionFields {"aws", "evil.example", PUBLISHED_REGION_REGEX, RegionEntry("us-east-1")}),
	     "partition's dnsSuffix is 'evil.example'"},
	    {"an unusable regionRegex",
	     EndpointFixture(PartitionFields {"aws", "amazonaws.com", "^(", RegionEntry("us-east-1")}),
	     "is not a usable pattern"},
	    {"no regionRegex",
	     EndpointsFixture("{\"dnsSuffix\": \"amazonaws.com\", \"partition\": \"aws\", \"regions\": {" +
	                      RegionEntry("us-east-1") + "}}"),
	     "regionRegex is not a usable pattern: None"},
	    {"a regionRegex with an inline flag that contradicts re.ASCII",
	     EndpointFixture(PartitionFields {"aws", "amazonaws.com", "(?u)^.+$", RegionEntry("us-east-1")}),
	     "is not a usable pattern"},
	    {"a regionRegex with a repeat count too large to compile",
	     EndpointFixture(PartitionFields {"aws", "amazonaws.com", "^a{99999999999}$", RegionEntry("us-east-1")}),
	     "is not a usable pattern"},
	    {"a regionRegex nested too deep to compile",
	     EndpointFixture(PartitionFields {"aws", "amazonaws.com", std::string(2000, '(') + std::string(2000, ')'),
	                                      RegionEntry("us-east-1")}),
	     "is not a usable pattern"},
	    {"a regionRegex that is not a string",
	     EndpointsFixture("{\"dnsSuffix\": \"amazonaws.com\", \"partition\": \"aws\", \"regionRegex\": 5, "
	                      "\"regions\": {" +
	                      RegionEntry("us-east-1") + "}}"),
	     "regionRegex is not a usable pattern: 5"},
	    {"no regions", AwsFixture(""), "lists no regions"},
	    {"a name the partition's own pattern refuses", AwsFixture(RegionEntry("aws-global")),
	     "is not matched in full by the 'aws' partition's own regionRegex"},
	    {"a fips spelling, which that pattern refuses too", AwsFixture(RegionEntry("fips-us-east-1")),
	     "is not matched in full by the 'aws' partition's own regionRegex"},
	    {"a name an unanchored pattern matches only a prefix of",
	     EndpointFixture(
	         PartitionFields {"aws", "amazonaws.com", R"((us|eu)\\-\\w+\\-\\d+)", RegionEntry("us-east-1x")}),
	     "'us-east-1x' is not matched in full by the 'aws' partition's own regionRegex"},
	    {"a name holding a character a host label may not", AwsFixture(RegionEntry("us-ea_st-1")),
	     "not a plain lower-case region token"},
	    {"an upper-case name that pattern matches", AwsFixture(RegionEntry("us-EAST-1")),
	     "not a plain lower-case region token"},
	    {"a first character a host label may not start with, under a pattern that admits anything",
	     EndpointFixture(PartitionFields {"aws", "amazonaws.com", "^.+$", RegionEntry("-us-east-1")}),
	     "not a plain lower-case region token"},
	    {"data that is not an object", "[1]\n", "not an object holding a list of partitions"},
	    {"a partitions value that is not a list", "{\"partitions\": \"aws\"}\n",
	     "not an object holding a list of partitions"},
	    {"a partition that is not an object", "{\"partitions\": [1]}\n",
	     "a partition in the endpoint data is not an object"},
	    {"regions that are not an object",
	     EndpointsFixture("{\"dnsSuffix\": \"amazonaws.com\", \"partition\": \"aws\", \"regionRegex\": \"" +
	                      std::string(PUBLISHED_REGION_REGEX) + "\", \"regions\": [\"us-east-1\"]}"),
	     "regions are not an object keyed by region name"},
	};
	for (auto &fixture : fixtures) {
		INFO(fixture.what);
		TempFile data(fixture.data);
		auto ran = RunGenerator("--check --endpoints '" + data.Path() + "'");
		INFO(ran.output);
		CheckRefusal(ran, fixture.message);
	}
}

TEST_CASE("The generator refuses an extract that does not record the pinned published file", "[odbc]") {
	// The banner names the pin, so names from an extract cut at another pin would go out under it.
	// Both modes, as each writes or checks a banner, and the list is left as it was.
	const std::string extract = VendoredExtract();
	const std::vector<std::pair<std::string, std::string>> tampered = {
	    {"another tag", ReplacedOnce(extract, "\"sdkTag\": \"" + PinnedTag() + "\"", "\"sdkTag\": \"0.0.0\"")},
	    {"another digest", ReplacedOnce(extract, PinnedDigest(), std::string(64, '0'))},
	    {"no record", ReplacedOnce(extract, "\"extractedFrom\"", "\"comment\"")},
	};
	for (auto &one : tampered) {
		for (auto mode : {"--check ", ""}) {
			INFO(one.first << (*mode ? ", --check" : ", write mode"));
			TempFile data(one.second);
			TempFile list("unchanged\n");
			auto ran =
			    RunGenerator(std::string(mode) + "--endpoints '" + data.Path() + "' --list '" + list.Path() + "'");
			INFO(ran.output);
			CheckRefusal(ran, "does not record the pinned published file (aws-sdk-cpp " + PinnedTag() + ", SHA-256 " +
			                      PinnedDigest() + ") in its extractedFrom field");
			CHECK(ReadFile(list.Path()) == "unchanged\n");
		}
	}
}

TEST_CASE("The generator refuses a vendored extract edited by hand under the pinned record", "[odbc]") {
	// A hand edit can keep extractedFrom, so only the extract's digest stops an added name. The
	// script runs from a copy of its directory, where its vendored extract can be swapped, and the
	// untouched copy passing shows that copying alone is not what is refused.
	TempDirectory scratch;
	const std::string tools = scratch.Path() + "/tools";
	REQUIRE(mkdir(tools.c_str(), 0700) == 0);
	const std::string generator = tools + "/gen-region-allowlist.py";
	const std::string extract = tools + "/endpoints-aws.json";
	const std::string list = scratch.Path() + "/region_allowlist.inc";
	WriteFile(generator,
	          ReadFile(std::string(REDSHIFT_SOURCE_DIR) + "/src/transport/odbc/tools/gen-region-allowlist.py"));
	auto run_copy = [&](const std::string &arguments) {
		return Run(PythonCommand("'" + generator + "' " + arguments + "--list '" + list + "'"));
	};

	WriteFile(extract, VendoredExtract());
	WriteFile(list, CheckedInList());
	auto untouched = run_copy("--check ");
	INFO(untouched.output);
	CHECK(ExitStatus(untouched) == 0);

	WriteFile(extract,
	          ReplacedOnce(VendoredExtract(), "\"us-west-2\": {", "\"us-fake-9\": {},\n        \"us-west-2\": {"));
	// The last mode names the same file by another spelling, which is still the vendored one.
	const std::string respelled = tools + "/../tools/endpoints-aws.json";
	for (auto &mode : std::vector<std::string> {"--check ", "", "--endpoints '" + respelled + "' "}) {
		INFO("mode: " << mode);
		WriteFile(list, "unchanged\n");
		auto ran = run_copy(mode);
		INFO(ran.output);
		CheckRefusal(ran, "has changed since it was reviewed: its SHA-256 is ");
		CHECK(ReadFile(list) == "unchanged\n");
	}
}

TEST_CASE("The extract keeps the aws partition's four keys and the pin, sorted and indented", "[odbc]") {
	// --extract renders only data whose digest is the pinned file's, so this drives the rendering
	// directly, with data shaped like that file: keys out of order, and keys and a partition to drop.
	TempFile published(PUBLISHED_FORMAT_FIXTURE);
	auto ran = Run(RenderExtractCommand(published.Path()));
	INFO(ran.output);
	CHECK(ExitStatus(ran) == 0);
	CHECK(ran.output == ExpectedExtract());
}

TEST_CASE("The extract rendering refuses data the list would not be generated from", "[odbc]") {
	// It runs the checks the list is generated under, so refused data is never vendored. Each row
	// edits the published-format data that the case above renders.
	const std::string published = PUBLISHED_FORMAT_FIXTURE;
	const std::vector<std::pair<std::string, std::string>> cases = {
	    {ReplacedOnce(published, "\"us-west-2\"", "\"aws-global\""),
	     "'aws-global' is not matched in full by the 'aws' partition's own regionRegex"},
	    {ReplacedOnce(published, "\"partition\": \"aws-cn\"", "\"partition\": \"aws\""),
	     "expected exactly one 'aws' partition in the endpoint data, found 2"},
	    {ReplacedOnce(published, "\"dnsSuffix\": \"amazonaws.com\",", "\"dnsSuffix\": \"evil.example\","),
	     "partition's dnsSuffix is 'evil.example'"},
	};
	for (auto &one : cases) {
		INFO(one.second);
		TempFile data(one.first);
		auto ran = Run(RenderExtractCommand(data.Path()));
		INFO(ran.output);
		CheckRefusal(ran, one.second);
	}
}

TEST_CASE("The generator refuses a published file whose digest is not the pinned one", "[odbc]") {
	// The digest is what makes the vendored extract provably the published file's aws partition.
	TempFile wrong("{\"partitions\": []}\n");
	auto ran = RunGenerator("--extract '" + wrong.Path() + "'");
	INFO(ran.output);
	CheckRefusal(ran, "not the " + PinnedDigest() + " published at");
}

TEST_CASE("An empty --extract path is refused rather than taken as write mode", "[odbc]") {
	// Taken as no mode, `--extract ''` would rewrite the list and report success instead of
	// re-deriving the extract.
	TempFile list("unchanged\n");
	auto ran = RunGenerator("--extract '' --list '" + list.Path() + "'");
	INFO(ran.output);
	CheckRefusal(ran, "cannot read");
	CHECK(ReadFile(list.Path()) == "unchanged\n");
}

TEST_CASE("The generator refuses a file it cannot read or cannot parse", "[odbc]") {
	TempFile not_json("}\n");
	// Deeper than any decoder recurses: unclosed, so a decoder that does not recurse refuses it too.
	TempFile too_deep(std::string(1000000, '['));
	const std::vector<std::pair<std::string, std::string>> cases = {
	    {"--check --endpoints '/nonexistent/endpoints-aws.json'", "cannot read"},
	    {"--check --list '/nonexistent/region_allowlist.inc'", "cannot read"},
	    {"--check --endpoints '" + not_json.Path() + "'", "does not hold JSON"},
	    {"--check --endpoints '" + too_deep.Path() + "'", "does not hold JSON"},
	};
	for (auto &one : cases) {
		INFO(one.first);
		auto ran = RunGenerator(one.first);
		INFO(ran.output);
		CheckRefusal(ran, one.second);
	}
}

TEST_CASE("The generator refuses a list it cannot write with its own message, not a traceback", "[odbc]") {
	// A directory that does not exist, so the write fails whoever runs the test.
	auto ran = RunGenerator("--list '/nonexistent/region_allowlist.inc'");
	INFO(ran.output);
	CheckRefusal(ran, "gen-region-allowlist: cannot write /nonexistent/region_allowlist.inc: ");
	CHECK(ran.output.find("Traceback") == std::string::npos);
}

TEST_CASE("A list write that fails leaves the old list whole", "[odbc]") {
	// `ulimit -f 0` makes every file write fail. SIGXFSZ is ignored first, as CPython also does, so
	// the write fails with an OSError rather than killing the generator. A list written in place would
	// be truncated; one written beside it and moved over is not.
	TempFile list("unchanged\n");
	auto ran = Run("trap '' XFSZ && ulimit -f 0 && " + GeneratorCommand("--list '" + list.Path() + "'"));
	INFO(ran.output);
	CheckRefusal(ran, "gen-region-allowlist: cannot write " + list.Path() + ": ");
	CHECK(ReadFile(list.Path()) == "unchanged\n");
	auto left = FilesBeside(list.Path());
	CHECK(left.empty());
	for (auto &name : left) {
		std::remove(name.c_str());
	}
}

TEST_CASE("The generator writes the list through a new file beside it, not a fixed name", "[odbc]") {
	// A write to a fixed name such as <list>.tmp would go through a symlink left there, then move it
	// over the list. TMPDIR names another directory, so a new file made there, not beside it, shows.
	TempFile victim("victim\n");
	TempFile list("unchanged\n");
	TempDirectory elsewhere;
	// Built before the symlink: building it can throw, which would leave the symlink behind.
	const std::string command = "TMPDIR='" + elsewhere.Path() + "' " + RecordedWriteCommand(list.Path());
	const std::string fixed = list.Path() + ".tmp";
	const bool linked = symlink(victim.Path().c_str(), fixed.c_str()) == 0;
	auto ran = linked ? Run(command) : Ran {-1, "could not make the symlink " + fixed};
	std::remove(fixed.c_str());
	INFO(ran.output);
	CHECK(ExitStatus(ran) == 0);
	CHECK(ran.output.find("replacing " + list.Path() + ".") != std::string::npos);
	CHECK(ReadFile(victim.Path()) == "victim\n");
	CHECK(ReadFile(list.Path()) == CheckedInList());
}

TEST_CASE("The written list has the mode a plain open gives under the umask", "[odbc]") {
	// A new file is made 0600, so the generator gives the list the mode a plain open would. A fixed
	// mode or umask fails a row, and 002 fails 0644 under the umask; no row's mode is mkstemp's 0600.
	const std::vector<std::pair<std::string, mode_t>> rows = {{"022", 0644}, {"027", 0640}, {"002", 0664}};
	for (auto &row : rows) {
		INFO("umask " << row.first);
		TempFile list("");
		auto ran = Run("umask " + row.first + " && " + GeneratorCommand("--list '" + list.Path() + "'"));
		INFO(ran.output);
		CHECK(ExitStatus(ran) == 0);
		struct stat status {};
		REQUIRE(stat(list.Path().c_str(), &status) == 0);
		CHECK((status.st_mode & 07777) == row.second);
	}
}

//! The configure's "locked [<version>]" line, or "no configure" when it did not run.
std::string LockedLine(const std::string &output) {
	auto start = output.find("locked [");
	return start == std::string::npos ? std::string("no configure")
	                                  : output.substr(start, output.find('\n', start) - start);
}

//! Moves `path`'s times by `seconds` from now: ahead of any output a configure wrote, whatever the
//! timestamp resolution, or, negative, behind them.
void TouchAt(const std::string &path, int seconds) {
	struct utimbuf at {};
	at.actime = at.modtime = std::time(nullptr) + seconds;
	REQUIRE(utime(path.c_str(), &at) == 0);
}

TEST_CASE("A plain rebuild reads the version from a relative RS_DRIVER_LOCK again", "[odbc]") {
	// A lock change re-runs CMake from the build tree, so sources.cmake runs check-lock.sh from the
	// checkout for a relative lock to name one file. A scratch project includes the real file.
	TempDirectory project;
	const std::string checkout = REDSHIFT_SOURCE_DIR;
	REQUIRE(symlink((checkout + "/third_party").c_str(), (project.Path() + "/third_party").c_str()) == 0);
	const std::string lock = project.Path() + "/copied.lock";
	WriteFile(lock, ReadFile(checkout + "/third_party/redshift_odbc/driver.lock"));
	WriteFile(project.Path() + "/CMakeLists.txt",
	          "cmake_minimum_required(VERSION 3.5)\nproject(probe NONE)\ninclude(\"" + checkout +
	              "/test/client/odbc/sources.cmake\")\nmessage(STATUS \"locked [${redshift_locked_version}]\")\n");
	// The cmake on PATH, as the Makefile runs it.
	const std::string cmake = "cd '" + project.Path() + "' && RS_DRIVER_LOCK=copied.lock cmake ";
	auto configured = Run(cmake + "-G 'Unix Makefiles' -S . -B build");
	INFO(configured.output);
	REQUIRE(ExitStatus(configured) == 0);
	REQUIRE(LockedLine(configured.output) != "locked []");
	TouchAt(lock, 10);
	auto rebuilt = Run(cmake + "--build build");
	INFO(rebuilt.output);
	CHECK(ExitStatus(rebuilt) == 0);
	CHECK(LockedLine(rebuilt.output) == LockedLine(configured.output));
}

//! The checked-in driver lock's text, and the version its "version" line names.
std::string CheckedInLock() {
	return ReadFile(std::string(REDSHIFT_SOURCE_DIR) + "/third_party/redshift_odbc/driver.lock");
}
std::string CheckedInVersion() {
	const auto lock = CheckedInLock();
	const auto start = lock.find("\nversion ");
	REQUIRE(start != std::string::npos);
	const auto from = start + std::strlen("\nversion ");
	return lock.substr(from, lock.find('\n', from) - from);
}

//! The checked-in lock with its version, everywhere it appears, changed to the same version with
//! `last` as its final number, which lock.py accepts.
std::string LockOfVersion(const std::string &last) {
	const auto version = CheckedInVersion();
	const auto changed = version.substr(0, version.rfind('.') + 1) + last;
	auto lock = CheckedInLock();
	for (auto at = lock.find(version); at != std::string::npos; at = lock.find(version, at + changed.size())) {
		lock.replace(at, version.size(), changed);
	}
	return lock;
}

//! The version LockOfVersion(`last`) names.
std::string VersionEndingIn(const std::string &last) {
	const auto version = CheckedInVersion();
	return version.substr(0, version.rfind('.') + 1) + last;
}

//! A scratch project that includes the real sources.cmake, and the preview's where the tree has
//! it, with WIN32 and CMAKE_SYSTEM_NAME set as a Windows configure sets them, or as this host does.
//! It holds its own lock and lock.py, which a case may change, and prints the version and test
//! sources it read.
class ScratchProject {
public:
	explicit ScratchProject(bool windows = true, bool with_python = true) {
		const std::string checkout = REDSHIFT_SOURCE_DIR;
		REQUIRE(ExitStatus(Run("mkdir -p '" + ReaderDirectory() + "'")) == 0);
		WriteFile(Reader(), ReadFile(checkout + "/third_party/redshift_odbc/tools/lib/lock.py"));
		WriteFile(Path() + "/third_party/redshift_odbc/driver.lock", CheckedInLock());
		WriteFile(Path() + "/CMakeLists.txt",
		          "cmake_minimum_required(VERSION 3.5)\nproject(probe NONE)\n" +
		              std::string(windows ? "set(WIN32 1)\nset(CMAKE_SYSTEM_NAME Windows)\n" : "") +
		              std::string(with_python ? "find_package(Python3 COMPONENTS Interpreter)\n" : "") +
		              "set(REDSHIFT_DRIVER none)\nset(REDSHIFT_PREVIEW ON)\ninclude(\"" + checkout +
		              "/test/client/odbc/sources.cmake\")\n" + "include(\"" + checkout +
		              "/test/preview/client/sources.cmake\" OPTIONAL)\n" +
		              "message(STATUS \"locked [${redshift_locked_version}]\")\n"
		              "message(STATUS \"sources [${REDSHIFT_CLIENT_TEST_SOURCES}]\")\n");
	}

	const std::string &Path() const {
		return project.Path();
	}
	std::string ReaderDirectory() const {
		return Path() + "/third_party/redshift_odbc/tools/lib";
	}
	std::string Reader() const {
		return ReaderDirectory() + "/lock.py";
	}
	//! A first configure, or with `step` "--build build" the build that may configure again, with
	//! `environment` set for it, as the Makefile runs the cmake on PATH.
	Ran Cmake(const std::string &environment, const std::string &step = "-G 'Unix Makefiles' -S . -B build") const {
		return Run("cd '" + Path() + "' && " + environment + " cmake " + step);
	}

private:
	TempDirectory project;
};

TEST_CASE("A Windows configure reads the version from RS_DRIVER_LOCK, else from the checked-in lock", "[odbc]") {
	ScratchProject named;
	WriteFile(named.Path() + "/other.lock", LockOfVersion("91"));
	auto configured = named.Cmake("RS_DRIVER_LOCK=other.lock");
	INFO(configured.output);
	CHECK(ExitStatus(configured) == 0);
	CHECK(LockedLine(configured.output) == "locked [" + VersionEndingIn("91") + "]");

	ScratchProject unset;
	auto checked_in = unset.Cmake("env -u RS_DRIVER_LOCK");
	INFO(checked_in.output);
	CHECK(ExitStatus(checked_in) == 0);
	CHECK(LockedLine(checked_in.output) == "locked [" + CheckedInVersion() + "]");
}

TEST_CASE("A Windows configure with an empty RS_DRIVER_LOCK, a bad lock or no Python leaves the version empty",
          "[odbc]") {
	const std::string warning = "the driver lock could not be read";
	ScratchProject empty;
	ScratchProject rejected;
	WriteFile(rejected.Path() + "/bad.lock", "format 1\nversion x\n");
	ScratchProject no_python(true, false);
	for (auto &ran : {empty.Cmake("RS_DRIVER_LOCK="), rejected.Cmake("RS_DRIVER_LOCK=bad.lock"),
	                  no_python.Cmake("env -u RS_DRIVER_LOCK")}) {
		INFO(ran.output);
		CHECK(ExitStatus(ran) == 0);
		CHECK(LockedLine(ran.output) == "locked []");
		CHECK(ran.output.find(warning) != std::string::npos);
	}
}

TEST_CASE("A Windows build configures again when lock.py changes, reading a relative lock from the checkout",
          "[odbc]") {
	ScratchProject project;
	WriteFile(project.Path() + "/other.lock", LockOfVersion("91"));
	auto configured = project.Cmake("RS_DRIVER_LOCK=other.lock");
	INFO(configured.output);
	REQUIRE(LockedLine(configured.output) == "locked [" + VersionEndingIn("91") + "]");
	auto untouched = project.Cmake("RS_DRIVER_LOCK=other.lock", "--build build");
	INFO(untouched.output);
	CHECK(LockedLine(untouched.output) == "no configure");
	TouchAt(project.Reader(), 10);
	auto rebuilt = project.Cmake("RS_DRIVER_LOCK=other.lock", "--build build");
	INFO(rebuilt.output);
	CHECK(ExitStatus(rebuilt) == 0);
	CHECK(LockedLine(rebuilt.output) == "locked [" + VersionEndingIn("91") + "]");
}

TEST_CASE("A Windows build configures again when the lock RS_DRIVER_LOCK names changes", "[odbc]") {
	// Its own project, so the lock.py case's future timestamp cannot make this rebuild configure.
	ScratchProject project;
	const std::string lock = project.Path() + "/other.lock";
	WriteFile(lock, LockOfVersion("91"));
	auto configured = project.Cmake("RS_DRIVER_LOCK=other.lock");
	INFO(configured.output);
	REQUIRE(LockedLine(configured.output) == "locked [" + VersionEndingIn("91") + "]");
	WriteFile(lock, LockOfVersion("92"));
	TouchAt(lock, 10);
	auto rebuilt = project.Cmake("RS_DRIVER_LOCK=other.lock", "--build build");
	INFO(rebuilt.output);
	CHECK(ExitStatus(rebuilt) == 0);
	CHECK(LockedLine(rebuilt.output) == "locked [" + VersionEndingIn("92") + "]");
}

//! The base names in the configure's "sources [...]" line, but those of the entries under `skip`
//! when it is not empty.
std::set<std::string> ConfiguredSources(const std::string &output, const std::string &skip = "") {
	const auto start = output.find("sources [");
	REQUIRE(start != std::string::npos);
	const auto from = start + std::strlen("sources [");
	std::set<std::string> names;
	for (const auto &source : duckdb::StringUtil::Split(output.substr(from, output.find(']', from) - from), ';')) {
		if (skip.empty() || !duckdb::StringUtil::StartsWith(source, skip)) {
			names.insert(source.substr(source.rfind('/') + 1));
		}
	}
	return names;
}

//! The base names of the test files there. A shipped source the tests compile on one platform alone
//! is in the extension on another, so it is not one of them.
std::set<std::string> ConfiguredTestSources(const std::string &output) {
	return ConfiguredSources(output, "src/");
}

//! Every .cpp or .hpp name test/README.md's table of files left out on Windows names.
std::set<std::string> ReadmeLeftOut() {
	const auto readme = ReadFile(std::string(REDSHIFT_SOURCE_DIR) + "/test/README.md");
	const auto start = readme.find("| Left out on Windows | Why |");
	REQUIRE(start != std::string::npos);
	const auto table = readme.substr(start, readme.find("\n\n", start) - start);
	std::set<std::string> names;
	const std::regex name(R"(`(?:[\w/]*/)?(\w+\.[ch]pp)`)");
	for (std::sregex_iterator at(table.begin(), table.end(), name), end; at != end; ++at) {
		names.insert((*at)[1].str());
	}
	return names;
}

TEST_CASE("A Windows configure leaves out only the ODBC client test files test/README.md lists", "[odbc]") {
	ScratchProject here(false);
	ScratchProject windows;
	auto host = here.Cmake("env -u RS_DRIVER_LOCK");
	auto configured = windows.Cmake("env -u RS_DRIVER_LOCK");
	INFO(host.output);
	INFO(configured.output);
	REQUIRE(ExitStatus(host) == 0);
	REQUIRE(ExitStatus(configured) == 0);
	const auto all = ConfiguredTestSources(host.output);
	const auto kept = ConfiguredTestSources(configured.output);
	std::set<std::string> left_out;
	std::set_difference(all.begin(), all.end(), kept.begin(), kept.end(), std::inserter(left_out, left_out.end()));
	const auto listed = ReadmeLeftOut();
	// A file that opens a connection, directly or through redshift_query, connects through the fake
	// on Windows too, unless it needs a POSIX facility, which the table names.
	const std::regex connects(R"(\b(OpenConnection|OpenRedshiftConnection|redshift_query)\s*\()");
	for (const std::string directory : {"test/client/odbc", "test/preview/client"}) {
		DIR *listing = opendir((std::string(REDSHIFT_SOURCE_DIR) + "/" + directory).c_str());
		// Absent where the tree carries no preview tests.
		if (listing == nullptr && directory == "test/preview/client") {
			continue;
		}
		REQUIRE(listing != nullptr);
		for (struct dirent *entry = readdir(listing); entry != nullptr; entry = readdir(listing)) {
			const std::string name = entry->d_name;
			const auto path = std::string(REDSHIFT_SOURCE_DIR) + "/" + directory + "/" + name;
			if (!duckdb::StringUtil::EndsWith(name, "_test.cpp") || !std::regex_search(ReadFile(path), connects)) {
				continue;
			}
			// Built here, or on Windows alone, as the Windows resolver's own cases are, or on Linux alone, as
			// the cases that trace a new process with seccomp, /proc and inotify are (sources.cmake), where
			// this host is not Linux.
			INFO(name);
#ifndef __linux__
			if (name == "environment_gate_test.cpp" || name == "known_environment_live_test.cpp") {
				continue;
			}
#endif
			CHECK(all.count(name) + kept.count(name) >= 1);
			CHECK((kept.count(name) == 1) == (listed.count(name) == 0));
		}
		closedir(listing);
	}
	for (const auto &name : left_out) {
		INFO(name);
		CHECK(listed.count(name) == 1);
	}
	CHECK(kept.count("process_environment_test.cpp") == 1);
	CHECK(kept.count("region_validation_test.cpp") == 1);
	CHECK(kept.count("trust_store_windows_test.cpp") == 1);
	CHECK(all.count("trust_store_windows_test.cpp") == 0);
	CHECK(kept.count("load_only_refusal_test.cpp") == 1);
	CHECK(all.count("load_only_refusal_test.cpp") == 0);
	if (all.count("accountless_settings_test.cpp") == 1) {
		CHECK(kept.count("accountless_settings_test.cpp") == 1);
	}
}

TEST_CASE("A Windows configure with the test-endpoint switch leaves out only the load-only build's refusals",
          "[odbc]") {
	ScratchProject off;
	ScratchProject on;
	auto without = off.Cmake("env -u RS_DRIVER_LOCK");
	auto with = on.Cmake("env -u RS_DRIVER_LOCK", "-G 'Unix Makefiles' -S . -B build -DREDSHIFT_TEST_ENDPOINTS=ON");
	INFO(without.output);
	INFO(with.output);
	REQUIRE(ExitStatus(without) == 0);
	REQUIRE(ExitStatus(with) == 0);
	auto kept = ConfiguredTestSources(without.output);
	REQUIRE(kept.erase("load_only_refusal_test.cpp") == 1);
	CHECK(ConfiguredTestSources(with.output) == kept);
}

//! DuckDB's layout under `root`, as the Makefile configures it: top/ is a top-level project that
//! adds the extension's, named through the symlink link/, and extension/src/ is the real src/. The
//! extension's CMakeLists.txt includes the real sources.cmake, then runs `after`.
void WriteNestedProject(const std::string &root, const std::string &after) {
	const std::string top = root + "/top";
	const std::string extension = root + "/extension";
	REQUIRE(mkdir(top.c_str(), 0700) == 0);
	REQUIRE(mkdir(extension.c_str(), 0700) == 0);
	REQUIRE(symlink((std::string(REDSHIFT_SOURCE_DIR) + "/src").c_str(), (extension + "/src").c_str()) == 0);
	REQUIRE(symlink(extension.c_str(), (root + "/link").c_str()) == 0);
	WriteFile(top + "/CMakeLists.txt", "cmake_minimum_required(VERSION 3.5)\nproject(top NONE)\nadd_subdirectory(\"" +
	                                       root + "/link\" extension_build)\n");
	// sources.cmake pairs a hook through CMakeLists.txt's redshift_select_hook, which this project
	// lacks, so a stand-in appends the default, as a tree without a preview implementation compiles.
	WriteFile(extension + "/CMakeLists.txt",
	          "cmake_minimum_required(VERSION 3.5)\nproject(extension NONE)\nfunction(redshift_select_hook list)\n"
	          "cmake_parse_arguments(HOOK \"\" \"DEFAULT;PREVIEW\" \"\" ${ARGN})\n"
	          "set(${list} ${${list}} ${HOOK_DEFAULT} PARENT_SCOPE)\nendfunction()\n"
	          "include(src/transport/odbc/sources.cmake)\n" +
	              after);
}

TEST_CASE("Only build/driver-e2e of the project may be configured with the test-endpoint switch", "[odbc]") {
	// The extension is named through a symlink, so only resolved paths accept its build/driver-e2e
	// named by the real path. A build compiles the one file the switch reaches.
	TempDirectory scratch;
	const std::string extension = scratch.Path() + "/extension";
	WriteNestedProject(scratch.Path(),
	                   "get_property(definitions SOURCE src/transport/odbc/connection_settings.cpp PROPERTY "
	                   "COMPILE_DEFINITIONS)\nmessage(STATUS \"definitions [${definitions}]\")\n"
	                   "if(REDSHIFT_COMPILE)\nenable_language(CXX)\nset(CMAKE_CXX_STANDARD 17)\n"
	                   "include_directories(src/include src/transport \"" +
	                       std::string(REDSHIFT_SOURCE_DIR) +
	                       "/duckdb/src/include\")\nadd_library(settings OBJECT "
	                       "src/transport/odbc/connection_settings.cpp)\nendif()\n");
	// The cmake on PATH, as the Makefile runs it. CMake wraps a message, so runs of white space are
	// read as one space.
	auto configure = [&](const std::string &build, const std::string &option) {
		auto ran = Run("cd '" + scratch.Path() + "' && cmake -G 'Unix Makefiles' -S top -B " + build + " " + option);
		std::string spaced;
		for (const char character : ran.output) {
			const bool blank = std::isspace(static_cast<unsigned char>(character)) != 0;
			if (!blank || spaced.empty() || spaced.back() != ' ') {
				spaced += blank ? ' ' : character;
			}
		}
		ran.output = spaced;
		return ran;
	};
	const std::string refused = "REDSHIFT_TEST_ENDPOINTS is on in the build directory";

	auto off = configure("extension/build/release", "");
	INFO(off.output);
	CHECK(ExitStatus(off) == 0);
	CHECK(off.output.find("definitions [REDSHIFT_TEST_ENDPOINTS_CONFIGURED=0]") != std::string::npos);
	const std::string on_definitions = "definitions [REDSHIFT_TEST_ENDPOINTS;REDSHIFT_TEST_ENDPOINTS_CONFIGURED=1]";
	auto on = configure("link/build/driver-e2e", "-DREDSHIFT_TEST_ENDPOINTS=ON");
	INFO(on.output);
	CHECK(ExitStatus(on) == 0);
	CHECK(on.output.find(on_definitions) != std::string::npos);
	CHECK(on.output.find("this build is for testing only and must not be shipped") != std::string::npos);
	// build/release as a recipe's later flags would turn it on there; top's is DuckDB's own.
	for (const auto &build : {"extension/build/release", "extension/build/shipped", "extension/out/driver-e2e",
	                          "extension/build/driver-e2e/driver-e2e", "top/build/driver-e2e"}) {
		auto other = configure(build, "-DREDSHIFT_TEST_ENDPOINTS=ON");
		INFO(build << ": " << other.output);
		CHECK(ExitStatus(other) != 0);
		CHECK(other.output.find(refused) != std::string::npos);
	}

	// Compiler flags that define the macro with the switch off, as the Makefile passes it, configure,
	// and the file then fails to compile.
	auto compile = [&](const std::string &build, const std::string &options) {
		auto configured = configure(build, "-DREDSHIFT_COMPILE=ON " + options);
		INFO(build << ": " << configured.output);
		REQUIRE(ExitStatus(configured) == 0);
		return Run("cd '" + scratch.Path() + "' && cmake --build " + build);
	};
	auto plain = compile("extension/build/plain", "");
	INFO(plain.output);
	CHECK(ExitStatus(plain) == 0);
	auto switched = compile("link/build/driver-e2e", "-DREDSHIFT_TEST_ENDPOINTS=ON");
	INFO(switched.output);
	CHECK(ExitStatus(switched) == 0);
	const std::string mismatch = "REDSHIFT_TEST_ENDPOINTS does not match the configure's switch";
	auto flagged = compile("extension/build/release",
	                       "-DREDSHIFT_TEST_ENDPOINTS=OFF '-DCMAKE_CXX_FLAGS=-DREDSHIFT_TEST_ENDPOINTS'");
	INFO(flagged.output);
	CHECK(ExitStatus(flagged) != 0);
	CHECK(flagged.output.find(mismatch) != std::string::npos);
	// CMake puts the definitions before CMAKE_CXX_FLAGS, so a -U there removes one: the switch on
	// without its macro, and a compile not told whether the configure set the switch. In a new
	// build/driver-e2e: the one above built connection_settings.cpp moments ago, and GNU make 3.81,
	// macOS's, compares whole seconds, so it can keep that object.
	REQUIRE(ExitStatus(Run("rm -rf '" + scratch.Path() + "/link/build/driver-e2e'")) == 0);
	auto unflagged =
	    compile("link/build/driver-e2e", "-DREDSHIFT_TEST_ENDPOINTS=ON '-DCMAKE_CXX_FLAGS=-UREDSHIFT_TEST_ENDPOINTS'");
	INFO(unflagged.output);
	CHECK(ExitStatus(unflagged) != 0);
	CHECK(unflagged.output.find(mismatch) != std::string::npos);
	auto untold = compile("extension/build/untold", "'-DCMAKE_CXX_FLAGS=-UREDSHIFT_TEST_ENDPOINTS_CONFIGURED'");
	INFO(untold.output);
	CHECK(ExitStatus(untold) != 0);
	CHECK(untold.output.find("must say whether the configure set the switch") != std::string::npos);

	// The same build/driver-e2e named by its real path, while the extension is named through the
	// symlink: only resolving both paths makes them equal. Its cache names the other path, so it goes.
	REQUIRE(ExitStatus(Run("rm -rf '" + extension + "/build/driver-e2e'")) == 0);
	auto real = configure("extension/build/driver-e2e", "-DREDSHIFT_TEST_ENDPOINTS=ON");
	INFO(real.output);
	CHECK(ExitStatus(real) == 0);
	CHECK(real.output.find(on_definitions) != std::string::npos);
}

TEST_CASE(
    "Only an MSVC configure compiles each of the ODBC client's own files at /W4 with its narrowing warnings as errors",
    "[odbc]") {
	// DuckDB's layout, configured for Windows: DuckDB's include directories are top's, the vendored
	// ODBC headers the extension's, and the list ends with the trust store's Windows file. MSVC is set
	// by hand, as a Windows compiler would set it; the path is resolved, as CMake's is.
	TempDirectory scratch;
	const std::unique_ptr<char, decltype(&std::free)> resolved(realpath(scratch.Path().c_str(), nullptr), std::free);
	REQUIRE(resolved != nullptr);
	const std::string root = resolved.get();
	WriteNestedProject(root, "message(STATUS \"sources [${REDSHIFT_ODBC_SOURCES}]\")\n"
	                         "foreach(source ${REDSHIFT_ODBC_SOURCES})\n"
	                         "get_property(options SOURCE ${source} PROPERTY COMPILE_OPTIONS)\n"
	                         "message(STATUS \"options ${source} [${options}]\")\nendforeach()\n");
	// Each source's compile options, as "<source> [<options>]".
	auto options_lines = [&](const std::string &build, const std::string &option) {
		auto ran = Run("cd '" + root + "' && cmake -G 'Unix Makefiles' -S top -B " + build +
		               " -DCMAKE_SYSTEM_NAME=Windows " + option);
		INFO(ran.output);
		REQUIRE(ExitStatus(ran) == 0);
		const std::string prefix = "-- options ";
		std::vector<std::string> lines;
		std::istringstream output(ran.output);
		for (std::string line; std::getline(output, line);) {
			if (line.rfind(prefix, 0) == 0) {
				lines.push_back(line.substr(prefix.size()));
			}
		}
		const auto sources = ConfiguredSources(ran.output);
		CHECK(sources.count("trust_store_windows.cpp") == 1);
		CHECK(sources.count("accountless_settings.cpp") == 1);
		CHECK(lines.size() == sources.size());
		return lines;
	};
	// Exactly these, so a swapped source directory or a flag dropped from any file fails.
	const std::string flags = "/W4;/WX;/w44244;/w44267;/external:W0;/external:anglebrackets;/external:I" + root +
	                          "/top/src/include;/external:I" + root + "/top/third_party;/external:I" + root +
	                          "/link/third_party/unixodbc";
	for (const auto &line : options_lines("build/plain", "")) {
		INFO(line);
		CHECK(line.substr(line.find(" [")) == " []");
	}
	for (const auto &line : options_lines("build/msvc", "-DMSVC=1")) {
		INFO(line);
		CHECK(line.substr(line.find(" [")) == " [" + flags + "]");
	}
}

TEST_CASE("Only make driver_e2e configures a build with the test-endpoint switch on", "[odbc]") {
	// Each configure line of a dry run, as "<build directory> <client> <switch>". CMake takes the last
	// -D of a name, so that one is the build's.
	auto configures = [](const std::string &arguments) {
		auto ran = DryRun(arguments);
		INFO(ran.output);
		REQUIRE(ExitStatus(ran) == 0);
		const std::string build_option = " -B ";
		std::vector<std::string> found;
		std::istringstream lines(ran.output);
		for (std::string line; std::getline(lines, line);) {
			const auto build = line.find(build_option);
			if (line.rfind("cmake ", 0) != 0 || build == std::string::npos) {
				continue;
			}
			auto last = [&line](const std::string &name) {
				const auto option = "-D" + name + "=";
				const auto at = line.rfind(option);
				if (at == std::string::npos) {
					return std::string("unset");
				}
				const auto value = at + option.size();
				return line.substr(value, line.find(' ', value) - value);
			};
			const auto directory = build + build_option.size();
			found.push_back(line.substr(directory, line.find(' ', directory) - directory) + " " +
			                last("REDSHIFT_CLIENT") + " " + last("REDSHIFT_TEST_ENDPOINTS"));
		}
		return found;
	};
	using Lines = std::vector<std::string>;

	CHECK(configures("driver_e2e REDSHIFT_CLIENT=mock") == Lines {"build/driver-e2e odbc ON"});
	CHECK(configures("test_client_e2e") == Lines {"build/driver-e2e odbc ON"});
	CHECK(configures("release REDSHIFT_CLIENT=odbc REDSHIFT_TEST_ENDPOINTS=ON") == Lines {"build/release odbc OFF"});
	CHECK(configures("release REDSHIFT_CLIENT=odbc EXT_FLAGS=-DREDSHIFT_TEST_ENDPOINTS=ON") ==
	      Lines {"build/release odbc OFF"});
	CHECK(configures("tsan REDSHIFT_CLIENT=odbc REDSHIFT_TEST_ENDPOINTS=ON") == Lines {"build/tsan odbc OFF"});
}

TEST_CASE("make test_client_e2e checks build/driver-e2e's graph, then builds and runs the suite there", "[odbc]") {
	auto ran = DryRun("test_client_e2e");
	INFO(ran.output);
	REQUIRE(ExitStatus(ran) == 0);
	const std::vector<std::string> wanted {
	    "check-build-graph-offline.sh build/driver-e2e",
	    "cmake --build build/driver-e2e --config Release --target redshift_client_tests",
	    "./build/driver-e2e/extension/redshift/redshift_client_tests -w NoTests"};
	std::vector<std::string> found;
	std::istringstream lines(ran.output);
	for (std::string line; std::getline(lines, line);) {
		for (const auto &step : wanted) {
			if (line.find(step) != std::string::npos) {
				found.push_back(step);
			}
		}
	}
	CHECK(found == wanted);
}

TEST_CASE("make test_driver_e2e runs the offline suite on Linux after run-e2e.sh, failing a driver case's skip",
          "[odbc]") {
	auto ran = DryRun("test_driver_e2e");
	INFO(ran.output);
	REQUIRE(ExitStatus(ran) == 0);
	// The recipe's shell lines as the dry run prints them, each without its indent: spaces, and the tab
	// that GNU make 3.81, macOS's make, keeps after a backslash-newline where later makes drop it.
	std::vector<std::string> lines;
	std::istringstream output(ran.output);
	for (std::string line; std::getline(output, line);) {
		lines.push_back(line.erase(0, line.find_first_not_of(" \t")));
	}
	const auto arm = std::find(lines.begin(), lines.end(), "linux_amd64 | linux_arm64) \\");
	REQUIRE(arm - lines.begin() >= 2);
	REQUIRE(lines.end() - arm >= 5);
	// Only once run-e2e.sh, and with it its no-network check, has passed.
	CHECK(duckdb::StringUtil::EndsWith(arm[-2], "run-e2e.sh build/driver-e2e \"$stock\" && \\"));
	CHECK(arm[-1] == "case \"$platform\" in \\");
	CHECK(arm[1] == "cmake --build build/driver-e2e --config Release --target redshift_client_tests && \\");
	// Through the driver suite's environment, which leaves out the caller's LD_LIBRARY_PATH.
	CHECK(duckdb::StringUtil::StartsWith(arm[2], "env -u LD_LIBRARY_PATH "));
	CHECK(duckdb::StringUtil::EndsWith(arm[2], " REDSHIFT_REQUIRE_DRIVER=1 ./build/driver-e2e/extension/redshift/"
	                                           "redshift_client_tests -w NoTests '[odbc]~[live]' ;; \\"));
	// A build that records no platform fails rather than leaving the suite unrun.
	CHECK(duckdb::StringUtil::StartsWith(arm[3], "\"\") echo "));
	CHECK(duckdb::StringUtil::EndsWith(arm[3], " >&2; exit 1 ;; \\"));
	CHECK(arm[4] == "esac");
}

TEST_CASE("make test_client runs the SQL tests without the end-to-end test variables", "[odbc]") {
	// The Makefile names the variables itself, so its list is compared with the client's.
	std::string unset_all = "env";
	for (const auto name : duckdb::TEST_VARIABLES) {
		unset_all += std::string(" -u ") + name;
	}
	// test_client runs test_client_shape in each preview shape's tree, the public shape's in a copy
	// of the tree, so the recipe is dry-run by name here: this suite runs in both trees, and
	// REDSHIFT_SOURCE_DIR is the one it was built from.
	auto ran = DryRun("test_client_shape");
	INFO(ran.output);
	REQUIRE(ExitStatus(ran) == 0);
	std::vector<std::string> sql_runs;
	std::istringstream lines(ran.output);
	for (std::string line; std::getline(lines, line);) {
		if (line.find("client/sql/*") != std::string::npos) {
			sql_runs.push_back(line.substr(0, line.find(" bash ")));
		}
	}
	CHECK(sql_runs == std::vector<std::string> {unset_all});
}

//! The ODBC client's sources: every .cpp and .hpp in src/transport/odbc/, and the odbc_* hooks of a preview
//! implementation where the tree carries one, each as "<directory>/<name>", sorted.
std::vector<std::string> OdbcClientSources(const std::string &root = REDSHIFT_SOURCE_DIR) {
	std::vector<std::string> sources;
	for (const std::string directory : {"src/transport/odbc", "src/preview"}) {
		DIR *listing = opendir((root + "/" + directory).c_str());
		// Absent where the tree carries no preview implementation.
		if (listing == nullptr && directory == "src/preview") {
			continue;
		}
		REQUIRE(listing != nullptr);
		for (struct dirent *entry = readdir(listing); entry != nullptr; entry = readdir(listing)) {
			const std::string name = entry->d_name;
			const bool code = duckdb::StringUtil::EndsWith(name, ".cpp") || duckdb::StringUtil::EndsWith(name, ".hpp");
			if (code && (directory != "src/preview" || duckdb::StringUtil::StartsWith(name, "odbc_"))) {
				sources.push_back(directory + "/" + name);
			}
		}
		closedir(listing);
	}
	std::sort(sources.begin(), sources.end());
	return sources;
}

TEST_CASE("In the ODBC client's sources only ReadEnvironmentVariable reads the environment", "[odbc]") {
	// Any other read could race the setenv the environment's allocation makes under the process
	// state's lock, which ReadEnvironmentVariable takes, or, on Windows, read the static C runtime's
	// copy rather than the block. Each C runtime and Win32 reader counts, wide forms included.
	const std::string reader = "optional<string> ReadEnvironmentVariable(const char *name) {";
	// The C runtime's environ arrays are read without a call, so they count by name alone. DuckDB's
	// FileSystem::GetEnvVariable and GetHomeDirectory read the environment too.
	const std::regex call(
	    R"(\w*(getenv|dupenv|GetEnvironmentVariable|GetEnvironmentStrings|ExpandEnvironmentStrings|GetEnvVariable|GetHomeDirectory)\w*(?=\s*\()|\b_{0,2}(p__)?w?environ\b)");
	for (auto reader_call :
	     {"getenv (", "_wgetenv(", "getenv_s(", "_wgetenv_s(", "_dupenv_s(", "_wdupenv_s(", "GetEnvironmentVariableA(",
	      "GetEnvironmentVariableW(", "GetEnvironmentStringsW(", "ExpandEnvironmentStringsW(", "environ[0]", "_environ",
	      "_wenviron", "__p__environ()", "FileSystem::GetEnvVariable(", "GetHomeDirectory("}) {
		INFO(reader_call);
		CHECK(std::regex_search(reader_call, call));
	}
	const auto sources = OdbcClientSources();
	CHECK(sources.size() > 10);
	std::vector<std::string> calls;
	for (const auto &source : sources) {
		const auto text = ReadFile(std::string(REDSHIFT_SOURCE_DIR) + "/" + source);
		const auto body = text.find(reader);
		const auto body_end = body == std::string::npos ? body : text.find("\n}\n", body);
		for (std::sregex_iterator at(text.begin(), text.end(), call), end; at != end; ++at) {
			const auto position = static_cast<size_t>(at->position());
			const bool inside = body != std::string::npos && body < position && position < body_end;
			calls.push_back(source + ": " + at->str() + (inside ? " in ReadEnvironmentVariable" : " elsewhere"));
		}
	}
	CHECK(calls == std::vector<std::string> {
	                   "src/transport/odbc/odbc_process_state.cpp: GetEnvironmentVariableW in ReadEnvironmentVariable",
	                   "src/transport/odbc/odbc_process_state.cpp: getenv in ReadEnvironmentVariable"});
}

//! Each loader call in the ODBC client's sources under `root`, as "<source>: <name>". The driver is reached
//! only through the interface, so the ODBC client may include <windows.h>, which declares the loader, but never
//! loads, looks up or frees a module itself, nor through DuckDB's dl.hpp wrappers, the dl* loader
//! (dlmopen and dlvsym included) or ntdll's loader.
std::vector<std::string> LoaderCalls(const std::string &root) {
	const std::regex call(R"(\b(LoadLibrary|LoadPackagedLibrary|LoadModule|GetProcAddress|)"
	                      R"(GetModuleHandle|FreeLibrary|LdrLoadDll|LdrGetDllHandle|)"
	                      R"(LdrGetProcedureAddress|LdrUnloadDll|)"
	                      R"(dlopen|dlmopen|dlsym|dlvsym|dlclose)\w*(?=\s*\())");
	std::vector<std::string> calls;
	for (const auto &source : OdbcClientSources(root)) {
		const auto text = ReadFile(root + "/" + source);
		for (std::sregex_iterator at(text.begin(), text.end(), call), end; at != end; ++at) {
			calls.push_back(source + ": " + at->str());
		}
	}
	return calls;
}

TEST_CASE("No source of the ODBC client calls the Windows loader", "[odbc]") {
	CHECK(OdbcClientSources().size() > 10);
	CHECK(LoaderCalls(REDSHIFT_SOURCE_DIR).empty());
}

TEST_CASE("The loader scan finds each loader call in a planted source", "[odbc]") {
	TempDirectory tree;
	REQUIRE(mkdir((tree.Path() + "/src").c_str(), 0700) == 0);
	REQUIRE(mkdir((tree.Path() + "/src/transport").c_str(), 0700) == 0);
	REQUIRE(mkdir((tree.Path() + "/src/transport/odbc").c_str(), 0700) == 0);
	REQUIRE(mkdir((tree.Path() + "/src/preview").c_str(), 0700) == 0);
	const std::vector<std::string> loader_calls {"LoadLibraryW(",
	                                             "LoadLibraryExW (",
	                                             "LoadPackagedLibrary(",
	                                             "LoadModule(",
	                                             "GetProcAddress(",
	                                             "GetModuleHandleExW(",
	                                             "FreeLibrary(",
	                                             "LdrLoadDll(",
	                                             "LdrGetDllHandleEx(",
	                                             "LdrGetProcedureAddress (",
	                                             "LdrUnloadDll(",
	                                             "dlopen(",
	                                             "dlmopen(",
	                                             "dlsym (",
	                                             "dlvsym (",
	                                             "dlclose("};
	for (const auto &loader_call : loader_calls) {
		const auto name = loader_call.substr(0, loader_call.find_first_of(" ("));
		INFO(loader_call);
		WriteFile(tree.Path() + "/src/transport/odbc/planted.cpp", "void Planted() {\n\t" + loader_call + ");\n}\n");
		CHECK(LoaderCalls(tree.Path()) == std::vector<std::string> {"src/transport/odbc/planted.cpp: " + name});
	}
	// A name in a comment or a longer identifier is not a call.
	WriteFile(tree.Path() + "/src/transport/odbc/planted.cpp", "// LdrLoadDll is never called\nvoid MyLdrLoadDll();\n");
	CHECK(LoaderCalls(tree.Path()).empty());
}
