// The driver's dependencies: the search list's empty or relative
// elements, the library cache that names the one file each needed library is loaded from, the checks of that
// file and its directory, and the load of each before the driver, which the platform loader then finds loaded
// and searches no directory for. The rules are tested in this process, with the list and the cache given; what
// the process started with (LD_LIBRARY_PATH, which the platform loader parses once) and every load of a library
// are tested in freshly exec'd children.
#include "driver_info.h"
#include "driver_loader_core.hpp"
#include "embedded_test_helpers.hpp"
#include "loader_payload.hpp"
#include "temp_dir.hpp"

#include "catch.hpp"

#include <algorithm>
#include <climits>
#include <csignal>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <functional>
#include <iterator>
#include <map>
#include <sstream>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

#include <dlfcn.h>
#include <fcntl.h>
#include <link.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>

using namespace duckdb::redshift_driver;
using namespace duckdb::redshift_driver::detail;
using redshift_driver_test::InBoundedChild;
using redshift_driver_test::MakeDir;
using redshift_driver_test::PlatformLoaderCalls;
using redshift_driver_test::ReadWholeFile;
using redshift_driver_test::ScopedEnv;
using redshift_driver_test::TempDir;

namespace {

// Where the assembler read the embedded files from, the driver first.
#define REDSHIFT_EMBED_PATH(stem, path) path,
const char *const EMBEDDED_PATHS[] = {REDSHIFT_DRIVER_EMBED_FILES(REDSHIFT_EMBED_PATH)};
#undef REDSHIFT_EMBED_PATH

std::vector<std::string> g_list;
bool GivenList(std::vector<std::string> &directories) {
	directories = g_list;
	return true;
}
bool UnreadableList(std::vector<std::string> &) {
	return false;
}
bool AlreadyLoaded(const std::string &name) {
	return name == "libloaded.so.1";
}

struct Verdict {
	bool ok;
	SearchRefusal refusal;
	std::vector<NeededLibrary> preload;
};

// The list is given, the library cache is `cache` (none when null) and the system directories are `system` (none
// by default, so every name not loaded that the cache lacks is missing).
Verdict Check(const std::vector<std::string> &list, const std::vector<std::string> &needed = {},
              const char *cache = nullptr, const std::vector<std::string> &system = {}) {
	g_list = list;
	SearchInputs inputs;
	inputs.list = GivenList;
	inputs.loaded = AlreadyLoaded;
	inputs.cache_path = cache;
	inputs.system_directories = system;
	inputs.needed = needed;
	Verdict verdict {true, SearchRefusal(), {}};
	verdict.ok = CheckSearchList(inputs, verdict.refusal, verdict.preload);
	return verdict;
}

void Touch(const std::string &path, mode_t mode, const std::string &content = "x") {
	{
		std::ofstream out(path);
		out << content;
	}
	REQUIRE(::chmod(path.c_str(), mode) == 0);
}

void WriteFile(const std::string &path, const std::string &content, mode_t mode = 0600) {
	{
		std::ofstream out(path, std::ios::binary | std::ios::trunc);
		out << content;
	}
	REQUIRE(::chmod(path.c_str(), mode) == 0);
}

void Put32(std::string &out, uint32_t value) {
	out.append(reinterpret_cast<const char *>(&value), sizeof(value));
}

// An entry of a library cache: the key, the path it gives, its flags and its hardware capabilities.
struct CacheEntry {
	std::string key;
	std::string path;
	int32_t flags = LIBRARY_CACHE_FLAGS; // this platform's, unless a case says otherwise
	uint64_t hwcap = 0;
	CacheEntry(std::string key_, std::string path_) : key(std::move(key_)), path(std::move(path_)) {
	}
	CacheEntry(std::string key_, std::string path_, int32_t flags_, uint64_t hwcap_ = 0)
	    : key(std::move(key_)), path(std::move(path_)), flags(flags_), hwcap(hwcap_) {
	}
};

// A library cache in glibc's formats: the new one ("glibc-ld.so.cache1.1"), alone or after an old-format
// section holding `old_entries` placeholder entries, which the reader never reads. The new section's
// string offsets count from its own start, after an old section too, as glibc's reader takes them and ldconfig
// writes them (elf/dl-cache.c, elf/cache.c); a combined cache that ldconfig wrote is tested below.
std::string MakeCache(const std::vector<CacheEntry> &entries, size_t old_entries = 0, bool old_format = false) {
	std::string strings;
	std::vector<std::pair<uint32_t, uint32_t>> offsets;
	for (const auto &entry : entries) {
		offsets.emplace_back(static_cast<uint32_t>(strings.size()), 0);
		strings += entry.key;
		strings.push_back('\0');
		offsets.back().second = static_cast<uint32_t>(strings.size());
		strings += entry.path;
		strings.push_back('\0');
	}
	std::string out;
	if (old_format) {
		out += std::string("ld.so-1.7.0", 11);
		out.push_back('\0');
		Put32(out, static_cast<uint32_t>(old_entries));
		for (size_t i = 0; i < old_entries; i++) {
			Put32(out, 1);
			Put32(out, 0);
			Put32(out, 0);
		}
		while (out.size() % 8 != 0) {
			out.push_back('\0');
		}
	}
	const size_t base = out.size();
	out += "glibc-ld.so.cache1.1";
	Put32(out, static_cast<uint32_t>(entries.size()));
	Put32(out, static_cast<uint32_t>(strings.size()));
	for (int i = 0; i < 5; i++) {
		Put32(out, 0);
	}
	const size_t table = 48 + entries.size() * 24;
	for (size_t i = 0; i < entries.size(); i++) {
		Put32(out, static_cast<uint32_t>(entries[i].flags));
		Put32(out, static_cast<uint32_t>(table + offsets[i].first));
		Put32(out, static_cast<uint32_t>(table + offsets[i].second));
		Put32(out, 0); // os version
		Put32(out, static_cast<uint32_t>(entries[i].hwcap & 0xffffffffu));
		Put32(out, static_cast<uint32_t>(entries[i].hwcap >> 32));
	}
	REQUIRE(out.size() == base + table);
	out += strings;
	return out;
}

} // namespace

namespace {

// Stands in for the C library's realpath in this executable, as driver_directory_test.cpp's mkdir does: while
// g_realpath_of is set, realpath of that path answers g_realpath_to, as it would if the file had been swapped
// between the loader's look at it and its realpath; every other call goes to the C library's. With
// _FORTIFY_SOURCE, which some compilers turn on by default, a call into a buffer of known size becomes
// __realpath_chk, so that is stood in for too; g_realpath_answers counts the answers, so a call that reaches
// neither shows as such.
std::string g_realpath_of, g_realpath_to;
int g_realpath_answers = 0;

// The stand-in's answer for `path`, or nullptr when the C library's is wanted.
char *StandInRealpath(const char *path, char *resolved) {
	if (g_realpath_of.empty() || g_realpath_of != path) {
		return nullptr;
	}
	g_realpath_answers++;
	if (resolved == nullptr) {
		return ::strdup(g_realpath_to.c_str());
	}
	std::snprintf(resolved, PATH_MAX, "%s", g_realpath_to.c_str());
	return resolved;
}

} // namespace

extern "C" char *realpath(const char *path, char *resolved) noexcept {
	using Realpath = char *(*)(const char *, char *);
	static const auto real = reinterpret_cast<Realpath>(::dlsym(RTLD_NEXT, "realpath"));
	char *answer = StandInRealpath(path, resolved);
	return answer != nullptr ? answer : real(path, resolved);
}

extern "C" char *__realpath_chk(const char *path, char *resolved, size_t resolved_length) noexcept {
	using RealpathChk = char *(*)(const char *, char *, size_t);
	static const auto real = reinterpret_cast<RealpathChk>(::dlsym(RTLD_NEXT, "__realpath_chk"));
	char *answer = StandInRealpath(path, resolved);
	return answer != nullptr ? answer : real(path, resolved, resolved_length);
}

TEST_CASE("An empty or relative element of the search list is refused: it searches the current directory",
          "[driver][search]") {
	// The list shows an empty LD_LIBRARY_PATH element as ".".
	for (const char *element : {".", "", "a", "a/b", "../x", "./lib"}) {
		INFO("element '" << element << "'");
		const auto verdict = Check({"/lib64", element});
		CHECK_FALSE(verdict.ok);
		CHECK(verdict.refusal.cause == SearchCause::RELATIVE);
		CHECK(verdict.refusal.rule == "the directory is empty or relative, so it searches the current directory");
		CHECK(verdict.refusal.where == element);
		CHECK(verdict.preload.empty());
	}
	CHECK(Check({"/lib64", "/usr/lib64"}).ok);
	CHECK(Check({}).ok);
}

TEST_CASE("The other elements of the list are not audited: the library comes from the cache, not from them",
          "[driver][search]") {
	TempDir tmp;
	if (!redshift_driver_test::PrivateAncestry(tmp)) {
		return;
	}
	// A directory others can write, holding a file under the needed name that others can write: no refusal,
	// and it is not what would be loaded, which is the cache's checked file. (The list's other elements are not
	// audited.)
	MakeDir(tmp / "open", 0777);
	Touch(tmp / "open/libneeded.so.1", 0666);
	MakeDir(tmp / "lib", 0700);
	Touch(tmp / "lib/libneeded.so.1", 0600);
	WriteFile(tmp / "cache", MakeCache({{"libneeded.so.1", tmp / "lib/libneeded.so.1"}}));
	const auto verdict = Check({tmp / "open", "/lib64"}, {"libneeded.so.1"}, (tmp / "cache").c_str());
	CHECK(verdict.ok);
	REQUIRE(verdict.preload.size() == 1);
	CHECK(verdict.preload[0].name == "libneeded.so.1");
	CHECK(verdict.preload[0].path == tmp / "lib/libneeded.so.1");
	// Without the cache's entry it is missing, whatever the list holds.
	WriteFile(tmp / "cache", MakeCache({}));
	const auto missing = Check({tmp / "open"}, {"libneeded.so.1"}, (tmp / "cache").c_str());
	CHECK_FALSE(missing.ok);
	CHECK(missing.refusal.cause == SearchCause::MISSING);
	CHECK(missing.refusal.library == "libneeded.so.1");
	// A relative element is refused whatever comes before it.
	CHECK(Check({tmp / "open", "lib"}).refusal.cause == SearchCause::RELATIVE);
}

TEST_CASE("A library the cache lacks is taken from the first system directory that has it, checked as a cached one "
          "is",
          "[driver][search][system]") {
	TempDir tmp;
	if (!redshift_driver_test::PrivateAncestry(tmp)) {
		return;
	}
	MakeDir(tmp / "first", 0700);
	MakeDir(tmp / "second", 0700);
	Touch(tmp / "second/libneeded.so.1", 0600);
	const std::vector<std::string> system {tmp / "first", tmp / "second"};
	// A host with no cache: the library sits in the second system directory.
	auto verdict = Check({}, {"libneeded.so.1"}, nullptr, system);
	CHECK(verdict.ok);
	REQUIRE(verdict.preload.size() == 1);
	CHECK(verdict.preload[0].path == tmp / "second/libneeded.so.1");
	// In both: the first in glibc's order is the one.
	Touch(tmp / "first/libneeded.so.1", 0600);
	verdict = Check({}, {"libneeded.so.1"}, nullptr, system);
	REQUIRE(verdict.preload.size() == 1);
	CHECK(verdict.preload[0].path == tmp / "first/libneeded.so.1");
	// A cache with no entry for it, and one whose entry's file is gone, are passed over the same way.
	WriteFile(tmp / "cache", MakeCache({}));
	verdict = Check({}, {"libneeded.so.1"}, (tmp / "cache").c_str(), system);
	REQUIRE(verdict.preload.size() == 1);
	CHECK(verdict.preload[0].path == tmp / "first/libneeded.so.1");
	WriteFile(tmp / "cache", MakeCache({{"libneeded.so.1", tmp / "gone/libneeded.so.1"}}));
	verdict = Check({}, {"libneeded.so.1"}, (tmp / "cache").c_str(), system);
	REQUIRE(verdict.preload.size() == 1);
	CHECK(verdict.preload[0].path == tmp / "first/libneeded.so.1");
	// So is one whose entry is a link that leads nowhere, as glibc's open finds it.
	MakeDir(tmp / "dangling", 0700);
	REQUIRE(::symlink((tmp / "gone/libneeded.so.1.2").c_str(), (tmp / "dangling/libneeded.so.1").c_str()) == 0);
	WriteFile(tmp / "cache", MakeCache({{"libneeded.so.1", tmp / "dangling/libneeded.so.1"}}));
	verdict = Check({}, {"libneeded.so.1"}, (tmp / "cache").c_str(), system);
	REQUIRE(verdict.preload.size() == 1);
	CHECK(verdict.preload[0].path == tmp / "first/libneeded.so.1");
	// A cache entry whose file is there comes first.
	MakeDir(tmp / "cached", 0700);
	Touch(tmp / "cached/libneeded.so.1", 0600);
	WriteFile(tmp / "cache", MakeCache({{"libneeded.so.1", tmp / "cached/libneeded.so.1"}}));
	verdict = Check({}, {"libneeded.so.1"}, (tmp / "cache").c_str(), system);
	REQUIRE(verdict.preload.size() == 1);
	CHECK(verdict.preload[0].path == tmp / "cached/libneeded.so.1");
	// A cached file that is there but refused stays refused: a system copy does not take its place.
	REQUIRE(::chmod((tmp / "cached").c_str(), 0777) == 0);
	verdict = Check({}, {"libneeded.so.1"}, (tmp / "cache").c_str(), system);
	REQUIRE(::chmod((tmp / "cached").c_str(), 0700) == 0);
	CHECK_FALSE(verdict.ok);
	CHECK(verdict.refusal.cause == SearchCause::CACHE);
	CHECK(verdict.refusal.where == tmp / "cached");
	// A system directory that is a file is passed over (the next one has it); one that cannot be searched is not:
	// the file in it cannot be looked at, so it is refused for its place, as a cached one is.
	Touch(tmp / "plain", 0600);
	verdict = Check({}, {"libneeded.so.1"}, nullptr, {tmp / "plain", tmp / "second"});
	REQUIRE(verdict.preload.size() == 1);
	CHECK(verdict.preload[0].path == tmp / "second/libneeded.so.1");
	if (::geteuid() != 0) {
		REQUIRE(::chmod((tmp / "first").c_str(), 0) == 0);
		verdict = Check({}, {"libneeded.so.1"}, nullptr, system);
		REQUIRE(::chmod((tmp / "first").c_str(), 0700) == 0);
		CHECK_FALSE(verdict.ok);
		CHECK(verdict.refusal.cause == SearchCause::CACHE);
	}
	// The file found there passes the same rules: one others can write is refused, and so is its directory.
	REQUIRE(::chmod((tmp / "first/libneeded.so.1").c_str(), 0666) == 0);
	verdict = Check({}, {"libneeded.so.1"}, nullptr, system);
	CHECK_FALSE(verdict.ok);
	CHECK(verdict.refusal.cause == SearchCause::CACHE);
	CHECK(verdict.refusal.rule == "a dependency of the driver can be written by another user");
	REQUIRE(::chmod((tmp / "first/libneeded.so.1").c_str(), 0600) == 0);
	REQUIRE(::chmod((tmp / "first").c_str(), 0777) == 0);
	verdict = Check({}, {"libneeded.so.1"}, nullptr, system);
	REQUIRE(::chmod((tmp / "first").c_str(), 0700) == 0);
	CHECK_FALSE(verdict.ok);
	CHECK(verdict.refusal.cause == SearchCause::CACHE);
	CHECK(verdict.refusal.where == tmp / "first");
}

TEST_CASE("A library in neither the cache nor a system directory is missing", "[driver][search][system]") {
	TempDir tmp;
	MakeDir(tmp / "empty", 0700);
	const std::string cache_file = tmp / "cache";
	WriteFile(cache_file, MakeCache({}));
	for (const char *cache : {static_cast<const char *>(nullptr), cache_file.c_str()}) {
		const auto verdict = Check({}, {"libneeded.so.1"}, cache, {tmp / "empty", tmp / "absent"});
		CHECK_FALSE(verdict.ok);
		CHECK(verdict.refusal.cause == SearchCause::MISSING);
		CHECK(verdict.refusal.library == "libneeded.so.1");
		CHECK(verdict.refusal.rule == "neither the library cache nor a system directory has a file for libneeded.so.1");
		CHECK(verdict.preload.empty());
	}
}

TEST_CASE("The system directories are glibc's for this platform, the multiarch pair first",
          "[driver][search][system]") {
#if defined(__x86_64__)
	const std::string triplet = "x86_64-linux-gnu";
#else
	const std::string triplet = "aarch64-linux-gnu";
#endif
	CHECK(SystemLibraryDirectories() ==
	      std::vector<std::string> {"/lib/" + triplet, "/usr/lib/" + triplet, "/lib64", "/usr/lib64"});
	CHECK(SearchInputs().system_directories == SystemLibraryDirectories());
}

TEST_CASE("A search list that cannot be read is refused, never allowed", "[driver][search]") {
	SearchInputs inputs;
	inputs.list = UnreadableList;
	SearchRefusal refusal;
	std::vector<NeededLibrary> preload;
	CHECK_FALSE(CheckSearchList(inputs, refusal, preload));
	CHECK(refusal.cause == SearchCause::UNREADABLE);
	CHECK(refusal.rule == "the search list cannot be read");
}

TEST_CASE("Only the libraries no loaded object answers to are looked for, by their names alone", "[driver][search]") {
	// Already loaded, or not a library name: nothing to look for, no cache read.
	CHECK(Check({}, {"libloaded.so.1"}).ok);
	CHECK(Check({}, {"../libx.so", "a/b", ""}).ok);
	CHECK(Check({}, {"lib x.so.1", "lib$ORIGIN.so"}).ok);
	CHECK(Check({}, {}).ok);
	// Anything else, with no cache: missing, the first one named.
	const auto verdict = Check({}, {"libloaded.so.1", "libother.so.1", "libthird.so.1"});
	CHECK_FALSE(verdict.ok);
	CHECK(verdict.refusal.cause == SearchCause::MISSING);
	CHECK(verdict.refusal.library == "libother.so.1");
	CHECK(verdict.refusal.where.empty()); // no cache path to name
	CHECK(verdict.refusal.rule == "neither the library cache nor a system directory has a file for libother.so.1");
	CHECK(verdict.preload.empty());
}

TEST_CASE("Each refusal has its own sentence, with no path in it", "[driver][search]") {
	SearchRefusal refusal;
	refusal.where = "/home/someone/private-looking-path";
	refusal.cause = SearchCause::RELATIVE;
	refusal.rule = "the directory is empty or relative, so it searches the current directory";
	CHECK(SearchRefusalMessage(refusal) ==
	      "the platform loader's search list holds an empty or relative directory, which searches the current "
	      "directory, where the platform loader could find a library another user put there. Restart the process "
	      "without it (an empty element in LD_LIBRARY_PATH, such as a trailing ':', is one), or have an administrator "
	      "correct the run path that names it, to connect");
	refusal.rule = "a parent directory is writable by another user";
	refusal.cause = SearchCause::CACHE;
	CHECK(SearchRefusalMessage(refusal) ==
	      "the platform loader's library cache or system directory has a library the Redshift driver needs in a place "
	      "that cannot be used (a parent directory is writable by another user). An administrator must fix that file "
	      "or its directory (or, where the library cache names it, rebuild the cache with ldconfig). (In a user "
	      "namespace that does not map the host's root user, as under `unshare -r`, the system's directories look "
	      "owned by another user: run outside such a namespace.)");
	refusal.cause = SearchCause::CACHE_UNREADABLE;
	CHECK(SearchRefusalMessage(refusal) ==
	      "the platform loader's library cache cannot be read, so the libraries the Redshift driver needs cannot be "
	      "found and checked. An administrator must look at this host's library cache (ldconfig)");
	refusal.cause = SearchCause::UNREADABLE;
	CHECK(SearchRefusalMessage(refusal) ==
	      "the platform loader's search list cannot be read, so the Redshift driver is not loaded, as it cannot be "
	      "checked that the list does not search the current directory. An administrator must look at this host's "
	      "dynamic loader");
	for (const auto cause : {SearchCause::RELATIVE, SearchCause::CACHE, SearchCause::UNREADABLE,
	                         SearchCause::CACHE_UNREADABLE, SearchCause::MISSING}) {
		refusal.cause = cause;
		CHECK(SearchRefusalMessage(refusal).find("/home") == std::string::npos);
		// Neither the environment nor a value of it is part of a sentence.
		CHECK(SearchRefusalMessage(refusal).find("LD_LIBRARY_PATH=") == std::string::npos);
	}
}

TEST_CASE("ElfNeededLibraries reads the DT_NEEDED names from the bytes, and survives anything else",
          "[driver][search]") {
	const auto driver = ReadWholeFile(redshift_driver_test::TestInputPath(REDSHIFT_TEST_DRIVER_NEEDS_ABSENT));
	const auto names = ElfNeededLibraries(reinterpret_cast<const unsigned char *>(driver.data()), driver.size());
	CHECK(std::find(names.begin(), names.end(), "libredshift_absent_test.so.1") != names.end());
	CHECK(std::find(names.begin(), names.end(), "libc.so.6") != names.end());
	// The embedded driver needs libkeyutils.so.1, the one library that is not in a base system.
	const auto embedded = std::vector<std::string> {REDSHIFT_DRIVER_HOST_PREREQUISITES};
	const auto real = ReadWholeFile(EMBEDDED_PATHS[0]);
	const auto real_names = ElfNeededLibraries(reinterpret_cast<const unsigned char *>(real.data()), real.size());
	for (const auto &prerequisite : embedded) {
		CHECK(std::find(real_names.begin(), real_names.end(), prerequisite) != real_names.end());
	}
	// Not an ELF file, nothing, a short file, and every truncation and a flipped byte of a real header.
	CHECK(ElfNeededLibraries(nullptr, 0).empty());
	CHECK(ElfNeededLibraries(reinterpret_cast<const unsigned char *>("hello"), 5).empty());
	const std::string text(200, 'x');
	CHECK(ElfNeededLibraries(reinterpret_cast<const unsigned char *>(text.data()), text.size()).empty());
	for (size_t cut = 0; cut < 4096 && cut < driver.size(); cut += 7) {
		(void)ElfNeededLibraries(reinterpret_cast<const unsigned char *>(driver.data()), cut);
	}
	auto damaged = driver;
	for (size_t at = 0; at < 1024; at++) {
		const char old = damaged[at];
		damaged[at] = static_cast<char>(old ^ 0xff);
		(void)ElfNeededLibraries(reinterpret_cast<const unsigned char *>(damaged.data()), damaged.size());
		damaged[at] = old;
	}
	// A name with a '/' is a path, not a library name: patch the dependency's name in a copy.
	auto patched = driver;
	const std::string needed_name = "libredshift_absent_test.so.1";
	const auto at = patched.find(needed_name);
	REQUIRE(at != std::string::npos);
	patched[at + 3] = '/';
	const auto patched_names =
	    ElfNeededLibraries(reinterpret_cast<const unsigned char *>(patched.data()), patched.size());
	CHECK(std::find(patched_names.begin(), patched_names.end(), "lib/edshift_absent_test.so.1") == patched_names.end());
	CHECK(patched_names.size() + 1 == names.size());
	// A name with an odd byte, a blank or a '$', is no library name either.
	for (const char odd : {' ', '$'}) {
		patched[at + 3] = odd;
		const auto odd_names =
		    ElfNeededLibraries(reinterpret_cast<const unsigned char *>(patched.data()), patched.size());
		auto odd_name = needed_name;
		odd_name[3] = odd;
		INFO("byte '" << odd << "'");
		CHECK(std::find(odd_names.begin(), odd_names.end(), odd_name) == odd_names.end());
		CHECK(odd_names.size() + 1 == names.size());
	}
}

namespace {

void PutLittleEndian(std::string &image, size_t at, uint64_t value, size_t bytes) {
	if (image.size() < at + bytes) {
		image.resize(at + bytes, '\0');
	}
	for (size_t i = 0; i < bytes; i++) {
		image[at + i] = static_cast<char>((value >> (8 * i)) & 0xff);
	}
}

// How a test shapes the image ElfImage builds.
struct ElfPlan {
	bool write_strsz = true; // a DT_STRSZ entry; when not, an entry of a tag the reader does not know
	uint64_t strsz = 0;      // its value; 0 is the whole string table
};

// A minimal ELF64 image for ElfNeededLibraries: one PT_LOAD over the whole file at virtual address 0, a
// PT_DYNAMIC with a DT_NEEDED for each of `needed`, DT_STRTAB and DT_STRSZ, and the strings; with `decoy`
// names, a second PT_DYNAMIC after the first, with a table of its own.
std::string ElfImage(const std::vector<std::string> &needed, const std::vector<std::string> &decoy = {},
                     const ElfPlan &plan = ElfPlan()) {
	std::string image(64, '\0');
	image.replace(0, 4, "\177ELF");
	image[4] = 2; // 64-bit
	image[5] = 1; // little endian
	const size_t phoff = 64, phentsize = 56, headers = decoy.empty() ? 2 : 3;
	PutLittleEndian(image, 0x20, phoff, 8);
	PutLittleEndian(image, 0x36, phentsize, 2);
	PutLittleEndian(image, 0x38, headers, 2);
	std::string strings(1, '\0');
	const auto add = [&](const std::string &name) {
		const size_t index = strings.size();
		strings += name;
		strings.push_back('\0');
		return index;
	};
	std::vector<size_t> first, second;
	for (const auto &name : needed) {
		first.push_back(add(name));
	}
	for (const auto &name : decoy) {
		second.push_back(add(name));
	}
	const size_t first_at = phoff + headers * phentsize, first_size = (first.size() + 3) * 16;
	const size_t second_at = first_at + first_size, second_size = decoy.empty() ? 0 : (second.size() + 3) * 16;
	const size_t strings_at = second_at + second_size, total = strings_at + strings.size();
	const auto header = [&](size_t index, uint32_t type, uint64_t at, uint64_t size) {
		const size_t base = phoff + index * phentsize;
		PutLittleEndian(image, base, type, 4);
		PutLittleEndian(image, base + 8, at, 8);
		PutLittleEndian(image, base + 16, at, 8); // the virtual address is the file offset
		PutLittleEndian(image, base + 32, size, 8);
		PutLittleEndian(image, base + 40, size, 8);
	};
	header(0, 1, 0, total);
	header(1, 2, first_at, first_size);
	if (!decoy.empty()) {
		header(2, 2, second_at, second_size);
	}
	const auto table = [&](size_t at, const std::vector<size_t> &indexes) {
		for (const auto index : indexes) {
			PutLittleEndian(image, at, 1, 8); // DT_NEEDED
			PutLittleEndian(image, at + 8, index, 8);
			at += 16;
		}
		PutLittleEndian(image, at, 5, 8); // DT_STRTAB
		PutLittleEndian(image, at + 8, strings_at, 8);
		PutLittleEndian(image, at + 16, plan.write_strsz ? 10 : 0x70000000, 8); // DT_STRSZ
		PutLittleEndian(image, at + 24, plan.strsz != 0 ? plan.strsz : strings.size(), 8);
		PutLittleEndian(image, at + 32, 0, 16); // DT_NULL
	};
	table(first_at, first);
	if (!decoy.empty()) {
		table(second_at, second);
	}
	image.resize(strings_at);
	image += strings;
	return image;
}

// An image whose program header entries are 32 bytes apart, so that a reader which accepted that size
// would find the dynamic section: the entries' fields overlap, which is how this lays them out. The
// string table's file offset is the first entry's, its virtual address 0, and the dynamic table follows
// the headers.
std::string CompactElfImage(const std::string &name) {
	std::string image(64, '\0');
	image.replace(0, 4, "\177ELF");
	image[4] = 2;
	image[5] = 1;
	PutLittleEndian(image, 0x20, 64, 8);
	PutLittleEndian(image, 0x36, 32, 2);
	PutLittleEndian(image, 0x38, 2, 2);
	const size_t dynamic_at = 136, dynamic_size = 3 * 16, strings_at = dynamic_at + dynamic_size;
	// Entry 0, a PT_LOAD at 64: its file size is entry 1's type, 2, bytes enough to hold virtual address 0.
	PutLittleEndian(image, 64, 1, 4);
	PutLittleEndian(image, 72, strings_at, 8); // file offset
	PutLittleEndian(image, 80, 0, 8);          // virtual address
	// Entry 1, a PT_DYNAMIC at 96; its file size is the 8 bytes at 128.
	PutLittleEndian(image, 96, 2, 4);
	PutLittleEndian(image, 104, dynamic_at, 8);
	PutLittleEndian(image, 112, 0, 8);
	PutLittleEndian(image, 128, dynamic_size, 8);
	PutLittleEndian(image, dynamic_at, 1, 8); // DT_NEEDED, at string index 1
	PutLittleEndian(image, dynamic_at + 8, 1, 8);
	PutLittleEndian(image, dynamic_at + 16, 5, 8); // DT_STRTAB at virtual address 0
	PutLittleEndian(image, dynamic_at + 24, 0, 8);
	PutLittleEndian(image, dynamic_at + 32, 0, 16);
	image.resize(strings_at);
	image += std::string(1, '\0') + name + std::string(1, '\0');
	return image;
}

std::vector<std::string> NeededOf(const std::string &image) {
	return ElfNeededLibraries(reinterpret_cast<const unsigned char *>(image.data()), image.size());
}

} // namespace

TEST_CASE(
    "ElfNeededLibraries keeps to its bounds: 64 names, 255 bytes, DT_STRSZ, the first PT_DYNAMIC and 56-byte headers",
    "[driver][search]") {
	// The images read as they are meant to.
	CHECK(NeededOf(ElfImage({"libfirst.so.1", "libsecond.so.1"})) ==
	      std::vector<std::string> {"libfirst.so.1", "libsecond.so.1"});
	// At most 64 names, the first 64.
	std::vector<std::string> many;
	for (int i = 0; i < 65; i++) {
		many.push_back("libn" + std::to_string(i) + ".so.1");
	}
	auto kept = NeededOf(ElfImage(many));
	CHECK(kept.size() == 64);
	CHECK(kept.back() == "libn63.so.1");
	many.pop_back();
	CHECK(NeededOf(ElfImage(many)).size() == 64);
	// A name of 255 bytes is a name; one of 256 is not.
	const std::string longest(255, 'a'), too_long(256, 'a');
	CHECK(NeededOf(ElfImage({longest, too_long, "libok.so.1"})) == std::vector<std::string> {longest, "libok.so.1"});
	// DT_STRSZ bounds the string table: a name that starts at or past it is ignored even if the file goes on,
	// and with no DT_STRSZ the table runs to the end of the file.
	ElfPlan bounded;
	bounded.strsz = std::string("libfirst.so.1").size() + 2; // the empty name, the first name and its NUL
	CHECK(NeededOf(ElfImage({"libfirst.so.1", "libsecond.so.1"}, {}, bounded)) ==
	      std::vector<std::string> {"libfirst.so.1"});
	ElfPlan unbounded;
	unbounded.write_strsz = false;
	CHECK(NeededOf(ElfImage({"libfirst.so.1", "libsecond.so.1"}, {}, unbounded)).size() == 2);
	// A name that runs past DT_STRSZ is dropped, not cut.
	ElfPlan cut;
	cut.strsz = std::string("libfirst.so.1").size() + 2 + 3;
	CHECK(NeededOf(ElfImage({"libfirst.so.1", "libsecond.so.1"}, {}, cut)) ==
	      std::vector<std::string> {"libfirst.so.1"});
	// Of two PT_DYNAMIC headers the first counts.
	CHECK(NeededOf(ElfImage({"libfirst.so.1"}, {"libdecoy.so.1"})) == std::vector<std::string> {"libfirst.so.1"});
	// Program header entries smaller than Elf64_Phdr are no ELF64 file.
	CHECK(NeededOf(CompactElfImage("libcompact.so.1")).empty());
}

TEST_CASE("A cache key answers a name only when it is the name", "[driver][search][cache]") {
	TempDir tmp;
	std::vector<std::string> paths;
	const auto lookup = [&](const std::string &name, const std::string &key) {
		WriteFile(tmp / "cache", MakeCache({{key, "/opt/evil/" + key}}));
		REQUIRE(CachePaths((tmp / "cache").c_str(), {name}, paths) == CacheRead::PARSED);
		return paths.size() == 1 && !paths[0].empty();
	};
	CHECK(lookup("libkeyutils.so.1", "libkeyutils.so.1"));
	// glibc's own lookup compares runs of digits as numbers (_dl_cache_libcmp), so it takes these keys for the
	// name; a file under such a key has that key as its soname, and would not answer to the name once loaded.
	for (const char *key : {"libkeyutils.so.01", "libkeyutils.so.0001", "libkeyutils.so.4294967297", "libkeyutils.so.2",
	                        "libkeyutils.so.1.1", "libkeyutils.so", "libKeyutils.so.1", ""}) {
		INFO(key);
		CHECK_FALSE(lookup("libkeyutils.so.1", key));
	}
}

namespace {

const char *const HELPER_TAG = "[search-helper]";

struct HelperAnswer {
	int exit_status = -1;
	std::map<std::string, std::string> values;
};

// What a helper run is given besides LD_LIBRARY_PATH: the needed names to check, and for a load the base,
// the test-input driver, its listed prerequisites and the library cache (none when empty).
struct HelperRun {
	std::string mode;
	std::string needed;
	std::string base;
	std::string payload;
	std::string prerequisites;
	std::string cache;
	std::string again_cache; // the second load's cache, when it differs
};

// Runs this executable's hidden case afresh with `library_path` as LD_LIBRARY_PATH (not set when
// `set` is false), so the platform loader parses it at that process's start.
HelperAnswer RunHelper(bool set, const std::string &library_path, const HelperRun &run) {
	TempDir tmp;
	const auto answer_file = tmp / "answer";
	std::fflush(nullptr);
	const pid_t child = ::fork();
	REQUIRE(child >= 0);
	if (child == 0) {
		// The helper bounds itself with alarm(20); an ignored SIGALRM would survive the exec and leave it unbounded.
		::signal(SIGALRM, SIG_DFL);
		if (set) {
			::setenv("LD_LIBRARY_PATH", library_path.c_str(), 1);
		} else {
			::unsetenv("LD_LIBRARY_PATH");
		}
		::setenv("RS_SEARCH_HELPER_OUT", answer_file.c_str(), 1);
		::setenv("RS_SEARCH_HELPER_MODE", run.mode.c_str(), 1);
		::setenv("RS_SEARCH_HELPER_NEEDED", run.needed.c_str(), 1);
		::setenv("RS_SEARCH_HELPER_DIR", run.base.c_str(), 1);
		::setenv("RS_SEARCH_HELPER_PAYLOAD", run.payload.c_str(), 1);
		::setenv("RS_SEARCH_HELPER_PREREQ", run.prerequisites.c_str(), 1);
		::setenv("RS_SEARCH_HELPER_CACHE", run.cache.c_str(), 1);
		::setenv("RS_SEARCH_HELPER_AGAIN_CACHE", run.again_cache.c_str(), 1);
		const int null = ::open("/dev/null", O_WRONLY | O_CLOEXEC);
		if (null >= 0) {
			::dup2(null, STDOUT_FILENO);
		}
		::execl("/proc/self/exe", "redshift_driver_tests", HELPER_TAG, static_cast<char *>(nullptr));
		::_exit(127);
	}
	int status = 0;
	REQUIRE(::waitpid(child, &status, 0) == child);
	HelperAnswer answer;
	answer.exit_status = WIFEXITED(status) ? WEXITSTATUS(status) : -1;
	std::ifstream in(answer_file);
	std::string line;
	while (std::getline(in, line)) {
		const auto at = line.find('=');
		if (at != std::string::npos) {
			answer.values[line.substr(0, at)] = line.substr(at + 1);
		}
	}
	return answer;
}

// The file the object loaded under `name` was loaded from, or empty when none is.
std::string LoadedFrom(const std::string &name) {
	if (!LibraryIsMapped(name)) {
		return std::string();
	}
	void *handle = ::dlopen(name.c_str(), RTLD_LAZY | RTLD_NOLOAD);
	struct link_map *map = nullptr;
	std::string path;
	if (handle != nullptr && ::dlinfo(handle, RTLD_DI_LINKMAP, &map) == 0 && map != nullptr) {
		path = map->l_name;
	}
	if (handle != nullptr) {
		::dlclose(handle);
	}
	return path;
}

const std::string ABSENT = "libredshift_absent_test.so.1";

// For the helper's "load-throw" mode: the "already loaded" test, which throws once the library it is asked about
// is mapped, as it is only after the loader has loaded it first: an exception after the platform loader call.
bool LoadedOrThrow(const std::string &name) {
	if (LibraryIsMapped(name)) {
		if (name == ABSENT) {
			throw std::runtime_error("a failure after the library was loaded first");
		}
		return true;
	}
	return false;
}

} // namespace

TEST_CASE("The search-list check helper", "[.][search-helper]") {
	const char *out = std::getenv("RS_SEARCH_HELPER_OUT");
	if (out == nullptr) {
		return; // not started by RunHelper
	}
	// A check that blocks ends here, with no answer, instead of stopping the suite.
	::alarm(20);
	const std::string mode = std::getenv("RS_SEARCH_HELPER_MODE");
	const std::string cache = std::getenv("RS_SEARCH_HELPER_CACHE");
	if (mode == "unset-first") {
		// The platform loader parsed the variable at start; the list still has what it held.
		::unsetenv("LD_LIBRARY_PATH");
	}
	if (mode == "load" || mode == "load-throw") {
		// The whole sequence, twice, on a test-input driver, in a process whose platform loader parsed
		// LD_LIBRARY_PATH at its start.
		const std::string base = std::getenv("RS_SEARCH_HELPER_DIR");
		redshift_driver_test::TestPayload payload(std::getenv("RS_SEARCH_HELPER_PAYLOAD"));
		std::istringstream names(std::getenv("RS_SEARCH_HELPER_PREREQ"));
		for (std::string name; names >> name;) {
			payload.config.prerequisites.push_back(name);
		}
		DriverLoader loader(payload.config);
		LoadInputs load_inputs;
		load_inputs.bases = redshift_driver_test::Inputs(base, "");
		load_inputs.search.cache_path = cache.empty() ? nullptr : cache.c_str();
		if (mode == "load-throw") {
			load_inputs.search.loaded = LoadedOrThrow;
		}
		load_inputs.hooks.on_platform_loader_call = redshift_driver_test::CountLoaderCall;
		const int calls = PlatformLoaderCalls();
		const auto result = loader.Load(load_inputs);
		const int first_calls = PlatformLoaderCalls() - calls;
		const std::string again_cache = std::getenv("RS_SEARCH_HELPER_AGAIN_CACHE");
		if (!again_cache.empty()) {
			load_inputs.search.cache_path = again_cache.c_str();
		}
		const auto again = loader.Load(load_inputs);
		std::ofstream answer(out);
		answer << "loaded=" << (result.ok() ? 1 : 0) << "\nkind=" << static_cast<int>(result.error.kind)
		       << "\nmessage=" << result.error.message << "\ncalls=" << first_calls
		       << "\nagain_message=" << again.error.message << "\nagain_calls=" << (PlatformLoaderCalls() - calls)
		       << "\nwritten=" << (::access((base + "/.redshift-driver").c_str(), F_OK) == 0 ? 1 : 0)
		       << "\nfrom=" << LoadedFrom(ABSENT) << "\nattempt=";
		if (!result.error.attempts.empty()) {
			answer << result.error.attempts[0].directory << " | " << result.error.attempts[0].reason;
		}
		answer << "\n";
		return;
	}
	SearchInputs inputs;
	inputs.cache_path = cache.empty() ? nullptr : cache.c_str();
	std::istringstream needed(std::getenv("RS_SEARCH_HELPER_NEEDED"));
	for (std::string name; needed >> name;) {
		inputs.needed.push_back(name);
	}
	SearchRefusal refusal;
	std::vector<NeededLibrary> preload;
	std::vector<std::string> list;
	const bool read = PlatformSearchList(list);
	const bool ok = CheckSearchList(inputs, refusal, preload);
	std::ofstream answer(out);
	answer << "read=" << (read ? 1 : 0) << "\nok=" << (ok ? 1 : 0) << "\ncause=" << static_cast<int>(refusal.cause)
	       << "\nrule=" << refusal.rule << "\nwhere=" << refusal.where << "\npreload=";
	for (const auto &library : preload) {
		answer << library.name << ">" << library.path << ":";
	}
	answer << "\nlist=";
	for (const auto &directory : list) {
		answer << directory << ":";
	}
	answer << "\n";
}

TEST_CASE("The process's own search list is read from the platform loader, as it started", "[driver][search]") {
	TempDir tmp;
	if (!redshift_driver_test::PrivateAncestry(tmp)) {
		return;
	}
	MakeDir(tmp / "one", 0700);
	MakeDir(tmp / "two", 0700);
	MakeDir(tmp / "open", 0777);
	MakeDir(tmp / "group", 0770);
	HelperRun check;
	check.mode = "check";
	const std::string cause_relative = std::to_string(static_cast<int>(SearchCause::RELATIVE));
	// Refused: an empty or relative element, however it is spelled, whatever the process does to the
	// variable after it started.
	for (const auto &value : {std::string(":/a"), std::string("a"), std::string("/a::/b"), std::string("/a:"),
	                          std::string("/a;b"), tmp / "one" + ":rel"}) {
		INFO("LD_LIBRARY_PATH='" << value << "'");
		auto answer = RunHelper(true, value, check);
		REQUIRE(answer.exit_status == 0);
		CHECK(answer.values.at("read") == "1");
		CHECK(answer.values.at("ok") == "0");
		CHECK(answer.values.at("cause") == cause_relative);
		HelperRun unset = check;
		unset.mode = "unset-first";
		answer = RunHelper(true, value, unset);
		CHECK(answer.values.at("ok") == "0");
		CHECK(answer.values.at("cause") == cause_relative);
	}
	// Absolute directories are not refused, whoever can write them: nothing the driver needs is taken from them.
	for (const auto &value : {tmp / "one" + ":" + tmp / "two", tmp / "open", tmp / "group", tmp / "group/missing"}) {
		INFO("LD_LIBRARY_PATH='" << value << "'");
		const auto answer = RunHelper(true, value, check);
		CHECK(answer.values.at("ok") == "1");
		CHECK(answer.values.at("list").find(value.substr(0, value.find(':'))) == 0);
	}
	CHECK(RunHelper(false, "", check).values.at("ok") == "1");
	CHECK(RunHelper(true, "", check).values.at("ok") == "1");
}

TEST_CASE("A library the driver needs is loaded from the file the cache names and was checked, never from "
          "LD_LIBRARY_PATH",
          "[driver][search]") {
	TempDir tmp;
	if (!redshift_driver_test::PrivateAncestry(tmp)) {
		return;
	}
	const std::string kind_refused = std::to_string(static_cast<int>(DriverErrorKind::REFUSED_ENVIRONMENT));
	const std::string kind_prerequisite = std::to_string(static_cast<int>(DriverErrorKind::MISSING_PREREQUISITE));
	const std::string library = ReadWholeFile(redshift_driver_test::TestInputPath(REDSHIFT_TEST_ABSENT_LIBRARY));
	// A copy in a directory others can write, on LD_LIBRARY_PATH, which the platform loader would take, and
	// a private copy elsewhere.
	MakeDir(tmp / "planted", 0777);
	WriteFile(tmp / ("planted/" + ABSENT), library, 0666);
	MakeDir(tmp / "good", 0700);
	WriteFile(tmp / ("good/" + ABSENT), library, 0500);
	const auto fresh = [&](const std::string &name) {
		MakeDir(tmp / name, 0700);
		return tmp / name;
	};
	HelperRun load;
	load.mode = "load";
	load.payload = redshift_driver_test::TestInputPath(REDSHIFT_TEST_DRIVER_NEEDS_ABSENT);
	load.prerequisites = ABSENT;
	// No cache names it: missing, before anything is loaded or written, and final: a second load, by then with a
	// cache that names a good copy, gets the stored answer. The planted copy is not loaded, though the platform
	// loader would have found it.
	WriteFile(tmp / "good-cache", MakeCache({{ABSENT, tmp / ("good/" + ABSENT)}}));
	load.again_cache = tmp / "good-cache";
	load.base = fresh("base-missing");
	auto answer = RunHelper(true, tmp / "planted", load);
	REQUIRE(answer.exit_status == 0);
	CHECK(answer.values.at("loaded") == "0");
	CHECK(answer.values.at("kind") == kind_prerequisite);
	CHECK(answer.values.at("message").find("the Redshift ODBC driver needs " + ABSENT) == 0);
	CHECK(answer.values.at("calls") == "0");
	CHECK(answer.values.at("again_calls") == "0");
	CHECK(answer.values.at("again_message") == answer.values.at("message"));
	CHECK(answer.values.at("written") == "0");
	CHECK(answer.values.at("from").empty());
	load.again_cache.clear();
	// A cache that names the private copy: that file is loaded first, and the driver loads, the planted copy
	// on LD_LIBRARY_PATH notwithstanding.
	WriteFile(tmp / "cache", MakeCache({{ABSENT, tmp / ("good/" + ABSENT)}}));
	load.cache = tmp / "cache";
	load.base = fresh("base-good");
	answer = RunHelper(true, tmp / "planted", load);
	REQUIRE(answer.exit_status == 0);
	CHECK(answer.values.at("loaded") == "1");
	CHECK(answer.values.at("calls") == "1");
	CHECK(answer.values.at("from") == tmp / ("good/" + ABSENT));
	// A cache that names the planted copy: refused for it, nothing loaded or written, and not final.
	WriteFile(tmp / "cache", MakeCache({{ABSENT, tmp / ("planted/" + ABSENT)}}));
	load.base = fresh("base-planted");
	answer = RunHelper(true, tmp / "planted", load);
	REQUIRE(answer.exit_status == 0);
	CHECK(answer.values.at("loaded") == "0");
	CHECK(answer.values.at("kind") == kind_refused);
	CHECK(answer.values.at("message").find("the platform loader's library cache or system directory has a library the "
	                                       "Redshift driver needs in a place that cannot be used (the directory is "
	                                       "writable by another user)") == 0);
	CHECK(answer.values.at("message").find(tmp.path()) == std::string::npos);
	CHECK(answer.values.at("calls") == "0");
	CHECK(answer.values.at("written") == "0");
	CHECK(answer.values.at("from").empty());
	// A relative element, whatever the cache says.
	WriteFile(tmp / "cache", MakeCache({{ABSENT, tmp / ("good/" + ABSENT)}}));
	load.base = fresh("base-relative");
	answer = RunHelper(true, ":" + tmp / "planted", load);
	CHECK(answer.values.at("kind") == kind_refused);
	CHECK(answer.values.at("written") == "0");
	CHECK(answer.values.at("message").find("the platform loader's search list holds an empty or relative directory") ==
	      0);
}

TEST_CASE("From the first library loaded before the driver on, the outcome is final", "[driver][search]") {
	TempDir tmp;
	if (!redshift_driver_test::PrivateAncestry(tmp)) {
		return;
	}
	const std::string kind_prerequisite = std::to_string(static_cast<int>(DriverErrorKind::MISSING_PREREQUISITE));
	const std::string kind_failed = std::to_string(static_cast<int>(DriverErrorKind::LOAD_FAILED));
	MakeDir(tmp / "lib", 0700);
	HelperRun load;
	load.mode = "load";
	load.payload = redshift_driver_test::TestInputPath(REDSHIFT_TEST_DRIVER_NEEDS_ABSENT);
	load.cache = tmp / "cache";
	WriteFile(tmp / "cache", MakeCache({{ABSENT, tmp / ("lib/" + ABSENT)}}));
	// A private file that is no library, behind the name as a distribution lays it out (a link to a versioned
	// file): the platform loader refuses it, naming the versioned file. A listed prerequisite's failure is
	// MISSING_PREREQUISITE, another LOAD_FAILED; each final, with the loader's text in the attempts.
	WriteFile(tmp / ("lib/" + ABSENT + ".10"), "this is not a shared library", 0600);
	REQUIRE(::symlink((ABSENT + ".10").c_str(), (tmp / ("lib/" + ABSENT)).c_str()) == 0);
	for (const bool listed : {true, false}) {
		INFO((listed ? "a listed prerequisite" : "not listed"));
		load.prerequisites = listed ? ABSENT : "";
		load.base = tmp / (listed ? "base-listed" : "base-unlisted");
		MakeDir(load.base, 0700);
		const auto answer = RunHelper(false, "", load);
		REQUIRE(answer.exit_status == 0);
		CHECK(answer.values.at("loaded") == "0");
		CHECK(answer.values.at("kind") == (listed ? kind_prerequisite : kind_failed));
		CHECK(answer.values.at("message") ==
		      (listed ? "the Redshift ODBC driver needs " + ABSENT +
		                    ", which this host does not have; install it, then restart the program and connect again"
		              : "the Redshift ODBC driver could not be loaded (the extension's directory): the platform loader "
		                "refused " +
		                    ABSENT + ", a library the driver needs"));
		CHECK(answer.values.at("calls") == "1");
		CHECK(answer.values.at("again_calls") == "1");
		CHECK(answer.values.at("again_message") == answer.values.at("message"));
		CHECK(answer.values.at("attempt").find(tmp / ("lib/" + ABSENT + ".10") + " | ") == 0);
		CHECK(answer.values.at("message").find(tmp.path()) == std::string::npos);
	}
	// A library under the name whose soname is another: it loads, but the platform loader would not take it for
	// the name and would search for it, so the driver is not loaded, and that is final too.
	REQUIRE(::unlink((tmp / ("lib/" + ABSENT)).c_str()) == 0);
	WriteFile(tmp / ("lib/" + ABSENT), ReadWholeFile(redshift_driver_test::TestInputPath(REDSHIFT_TEST_RUNPATH_PROBE)),
	          0600);
	load.prerequisites = ABSENT;
	load.base = tmp / "base-soname";
	MakeDir(load.base, 0700);
	const auto answer = RunHelper(false, "", load);
	REQUIRE(answer.exit_status == 0);
	CHECK(answer.values.at("loaded") == "0");
	CHECK(answer.values.at("kind") == kind_failed);
	CHECK(answer.values.at("message") ==
	      "the Redshift ODBC driver could not be loaded (the extension's directory): the file found for " + ABSENT +
	          " does not answer to that name");
	CHECK(answer.values.at("calls") == "1");
	CHECK(answer.values.at("again_calls") == "1");
	CHECK(answer.values.at("again_message") == answer.values.at("message"));
	CHECK(answer.values.at("from").empty());
	// An exception once a library was loaded first (here from the "already loaded" test asked after it) is an
	// error after the platform loader call: LOAD_FAILED, final.
	WriteFile(tmp / ("lib/" + ABSENT), ReadWholeFile(redshift_driver_test::TestInputPath(REDSHIFT_TEST_ABSENT_LIBRARY)),
	          0600);
	load.mode = "load-throw";
	load.base = tmp / "base-throw";
	MakeDir(load.base, 0700);
	const auto thrown = RunHelper(false, "", load);
	REQUIRE(thrown.exit_status == 0);
	CHECK(thrown.values.at("loaded") == "0");
	CHECK(thrown.values.at("kind") == kind_failed);
	CHECK(thrown.values.at("calls") == "1");
	CHECK(thrown.values.at("again_calls") == "1");
	CHECK(thrown.values.at("again_message") == thrown.values.at("message"));
}

// ---- The library cache -------------------------------------------------------------------------

TEST_CASE("The library cache gives, for each name, the first entry for this platform outside any hardware "
          "subdirectory",
          "[driver][search][cache]") {
	TempDir tmp;
	const std::vector<std::string> names {"libneeded.so.1", "libalso.so.2", "libnone.so.3"};
	std::vector<std::string> paths;
	// An entry for a name nobody asked for, a relative path, another platform's entry, one with hardware
	// capabilities, then the entries that count, then a later one for the same name.
	WriteFile(tmp / "new",
	          MakeCache({{"libunasked.so.1", "/opt/b/libunasked.so.1"},
	                     {"libalso.so.2", "relative/libalso.so.2"},
	                     {"libneeded.so.1", "/opt/i386/libneeded.so.1", 0x0003},
	                     {"libneeded.so.1", "/opt/hw/libneeded.so.1", LIBRARY_CACHE_FLAGS, 1},
	                     {"libneeded.so.1", "/opt/hw3/libneeded.so.1", LIBRARY_CACHE_FLAGS, uint64_t {1} << 62},
	                     {"libneeded.so.1", "/opt/a/libneeded.so.1"},
	                     {"libalso.so.2", "/opt/d/libalso.so.2"},
	                     {"libneeded.so.1", "/opt/c/libneeded.so.1"}}));
	CHECK(CachePaths((tmp / "new").c_str(), names, paths) == CacheRead::PARSED);
	CHECK(paths == std::vector<std::string> {"/opt/a/libneeded.so.1", "/opt/d/libalso.so.2", ""});
	// After an old-format section, whose entries are not used and whose size moves the new one.
	for (const size_t old_entries : {size_t {0}, size_t {1}, size_t {2}, size_t {3}}) {
		INFO("old entries " << old_entries);
		WriteFile(tmp / "both", MakeCache({{"libalso.so.2", "/opt/d/libalso.so.2"}}, old_entries, true));
		CHECK(CachePaths((tmp / "both").c_str(), names, paths) == CacheRead::PARSED);
		CHECK(paths == std::vector<std::string> {"", "/opt/d/libalso.so.2", ""});
	}
	// One that AlmaLinux 8's ldconfig wrote (glibc 2.28 with RHEL's backports of 2.33's shared string table and
	// cache extension, so a key can be the tail of its path), as `ldconfig -r <root> -c compat` over a root
	// holding libkeyutils.so.1, libz.so.1 and libuuid.so.1, as a host whose ldconfig writes both formats has:
	// the old section's 3 entries rounded up to 4, the new section at byte 64, its offsets from there. Its
	// entries are x86-64's; elsewhere they are given this platform's flags, the one thing the reader compares.
	// 16 bytes a line, as `xxd` shows the file.
	// clang-format off
	static const unsigned char LDCONFIG_2_28_COMPAT[] = {
	    0x6c, 0x64, 0x2e, 0x73, 0x6f, 0x2d, 0x31, 0x2e, 0x37, 0x2e, 0x30, 0x00, 0x04, 0x00, 0x00, 0x00,
	    0x03, 0x03, 0x00, 0x00, 0x83, 0x00, 0x00, 0x00, 0x78, 0x00, 0x00, 0x00, 0x03, 0x03, 0x00, 0x00,
	    0xb4, 0x00, 0x00, 0x00, 0xa9, 0x00, 0x00, 0x00, 0x03, 0x03, 0x00, 0x00, 0x98, 0x00, 0x00, 0x00,
	    0x8d, 0x00, 0x00, 0x00, 0x03, 0x03, 0x00, 0x00, 0x98, 0x00, 0x00, 0x00, 0x8d, 0x00, 0x00, 0x00,
	    0x67, 0x6c, 0x69, 0x62, 0x63, 0x2d, 0x6c, 0x64, 0x2e, 0x73, 0x6f, 0x2e, 0x63, 0x61, 0x63, 0x68,
	    0x65, 0x31, 0x2e, 0x31, 0x03, 0x00, 0x00, 0x00, 0x49, 0x00, 0x00, 0x00, 0x02, 0x00, 0x00, 0x00,
	    0x04, 0x01, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
	    0x03, 0x03, 0x00, 0x00, 0x83, 0x00, 0x00, 0x00, 0x78, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
	    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x03, 0x03, 0x00, 0x00, 0xb4, 0x00, 0x00, 0x00,
	    0xa9, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
	    0x03, 0x03, 0x00, 0x00, 0x98, 0x00, 0x00, 0x00, 0x8d, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
	    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x2f, 0x75, 0x73, 0x72, 0x2f, 0x6c, 0x69, 0x62,
	    0x36, 0x34, 0x2f, 0x6c, 0x69, 0x62, 0x7a, 0x2e, 0x73, 0x6f, 0x2e, 0x31, 0x00, 0x2f, 0x75, 0x73,
	    0x72, 0x2f, 0x6c, 0x69, 0x62, 0x36, 0x34, 0x2f, 0x6c, 0x69, 0x62, 0x6b, 0x65, 0x79, 0x75, 0x74,
	    0x69, 0x6c, 0x73, 0x2e, 0x73, 0x6f, 0x2e, 0x31, 0x00, 0x2f, 0x75, 0x73, 0x72, 0x2f, 0x6c, 0x69,
	    0x62, 0x36, 0x34, 0x2f, 0x6c, 0x69, 0x62, 0x75, 0x75, 0x69, 0x64, 0x2e, 0x73, 0x6f, 0x2e, 0x31,
	    0x00, 0x00, 0x00, 0x00, 0x74, 0x21, 0xa4, 0xea, 0x01, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
	    0x00, 0x00, 0x00, 0x00, 0x1c, 0x01, 0x00, 0x00, 0x2f, 0x00, 0x00, 0x00, 0x6c, 0x64, 0x63, 0x6f,
	    0x6e, 0x66, 0x69, 0x67, 0x20, 0x28, 0x47, 0x4e, 0x55, 0x20, 0x6c, 0x69, 0x62, 0x63, 0x29, 0x20,
	    0x73, 0x74, 0x61, 0x62, 0x6c, 0x65, 0x20, 0x72, 0x65, 0x6c, 0x65, 0x61, 0x73, 0x65, 0x20, 0x76,
	    0x65, 0x72, 0x73, 0x69, 0x6f, 0x6e, 0x20, 0x32, 0x2e, 0x32, 0x38,
	};
	// clang-format on
	std::string real(reinterpret_cast<const char *>(LDCONFIG_2_28_COMPAT), sizeof(LDCONFIG_2_28_COMPAT));
	REQUIRE(real.size() == 331);
	REQUIRE(real.compare(0, 11, "ld.so-1.7.0") == 0);
	REQUIRE(real.compare(64, 20, "glibc-ld.so.cache1.1") == 0);
	for (size_t entry = 0; entry < 3; entry++) {
		const int32_t flags = LIBRARY_CACHE_FLAGS;
		std::memcpy(&real[64 + 48 + entry * 24], &flags, sizeof(flags));
	}
	WriteFile(tmp / "ldconfig", real);
	CHECK(CachePaths((tmp / "ldconfig").c_str(), {"libkeyutils.so.1", "libuuid.so.1", "libz.so.1", "libc.so.6"},
	                 paths) == CacheRead::PARSED);
	CHECK(paths == std::vector<std::string> {"/usr/lib64/libkeyutils.so.1", "/usr/lib64/libuuid.so.1",
	                                         "/usr/lib64/libz.so.1", ""});
	// Nothing asked for.
	CHECK(CachePaths((tmp / "new").c_str(), {}, paths) == CacheRead::PARSED);
	CHECK(paths.empty());
}

TEST_CASE("A library cache entry whose key only compares equal to the name, as glibc compares digits, is no entry "
          "for it: the library is missing and nothing is loaded",
          "[driver][search][cache]") {
	TempDir tmp;
	if (!redshift_driver_test::PrivateAncestry(tmp)) {
		return;
	}
	// The planted case: a key glibc's own lookup takes for 'libneeded.so.1' (leading zeros, a number that wraps
	// to 1 in 32 bits), in a directory others can write.
	MakeDir(tmp / "evil", 0777);
	for (const char *key : {"libneeded.so.01", "libneeded.so.4294967297", "libneeded.so.18446744073709551617"}) {
		INFO(key);
		const std::string planted = tmp / (std::string("evil/") + key);
		WriteFile(planted, "x", 0666);
		WriteFile(tmp / "cache", MakeCache({{key, planted}}));
		const auto verdict = Check({}, {"libneeded.so.1"}, (tmp / "cache").c_str());
		CHECK_FALSE(verdict.ok);
		CHECK(verdict.refusal.cause == SearchCause::MISSING);
		CHECK(verdict.preload.empty());
	}
}

TEST_CASE("A cache that is absent names nothing; one that cannot be read or understood is refused, never skipped",
          "[driver][search][cache]") {
	TempDir tmp;
	const std::vector<std::string> names {"libneeded.so.1"};
	std::vector<std::string> paths;
	CHECK(CachePaths((tmp / "absent").c_str(), names, paths) == CacheRead::ABSENT);
	WriteFile(tmp / "file", "x");
	CHECK(CachePaths((tmp / "file/deeper").c_str(), names, paths) == CacheRead::ABSENT); // ENOTDIR
	// Not a regular file: a directory, and a FIFO, which must not stop the call.
	MakeDir(tmp / "dir", 0700);
	CHECK(CachePaths((tmp / "dir").c_str(), names, paths) == CacheRead::UNREADABLE);
	REQUIRE(::mkfifo((tmp / "fifo").c_str(), 0600) == 0);
	CHECK(InBoundedChild([&] { return CachePaths((tmp / "fifo").c_str(), names, paths) == CacheRead::UNREADABLE; }));
	// In no format, empty, and damaged copies of a good one.
	WriteFile(tmp / "text", "this is not a cache");
	CHECK(CachePaths((tmp / "text").c_str(), names, paths) == CacheRead::UNREADABLE);
	WriteFile(tmp / "empty", "");
	CHECK(CachePaths((tmp / "empty").c_str(), names, paths) == CacheRead::UNREADABLE);
	const auto good = MakeCache({{"libneeded.so.1", "/opt/a/libneeded.so.1"}});
	for (size_t cut = 0; cut < 48 + 24; cut += 3) {
		WriteFile(tmp / "cut", good.substr(0, cut));
		INFO("cut at " << cut);
		CHECK(CachePaths((tmp / "cut").c_str(), names, paths) == CacheRead::UNREADABLE);
	}
	// The entry count claims more than the file holds.
	auto many = good;
	many[20] = '\xff';
	many[21] = '\xff';
	WriteFile(tmp / "many", many);
	CHECK(CachePaths((tmp / "many").c_str(), names, paths) == CacheRead::UNREADABLE);
	// A new-format header cut short, with no entries: 24 bytes reach the count (0), but not the rest of the
	// 48-byte header, alone or after an old-format section. Not an empty cache: the file is damaged.
	const auto none = MakeCache({});
	REQUIRE(none.size() == 48);
	const auto after_old = MakeCache({}, 1, true);
	const size_t old_part = after_old.size() - 48;
	for (size_t cut = 24; cut < 48; cut++) {
		INFO("header cut at " << cut);
		WriteFile(tmp / "short", none.substr(0, cut));
		CHECK(CachePaths((tmp / "short").c_str(), names, paths) == CacheRead::UNREADABLE);
		WriteFile(tmp / "short-after-old", after_old.substr(0, old_part + cut));
		CHECK(CachePaths((tmp / "short-after-old").c_str(), names, paths) == CacheRead::UNREADABLE);
	}
	WriteFile(tmp / "whole", none);
	CHECK(CachePaths((tmp / "whole").c_str(), names, paths) == CacheRead::PARSED);
	// The old format alone (`ldconfig -c old`), with no new section after it: not read, so refused.
	WriteFile(tmp / "old-only", after_old.substr(0, old_part));
	CHECK(CachePaths((tmp / "old-only").c_str(), names, paths) == CacheRead::UNREADABLE);
	// An old section followed by room for a new one that holds no new magic: refused by the magic check alone, since
	// zeros would otherwise read as an empty new section.
	WriteFile(tmp / "old-then-zeros", after_old.substr(0, old_part) + std::string(64, '\0'));
	CHECK(CachePaths((tmp / "old-then-zeros").c_str(), names, paths) == CacheRead::UNREADABLE);
	// A string offset past the end: that entry is skipped, and nothing is read out of bounds.
	auto wild = good;
	wild[48 + 4] = '\xff';
	wild[48 + 5] = '\xff';
	wild[48 + 6] = '\xff';
	wild[48 + 7] = '\x7f';
	WriteFile(tmp / "wild", wild);
	CHECK(CachePaths((tmp / "wild").c_str(), names, paths) == CacheRead::PARSED);
	CHECK(paths == std::vector<std::string> {""});
	// A string that never ends.
	auto unterminated = good;
	unterminated.back() = 'x';
	WriteFile(tmp / "unterminated", unterminated);
	CHECK(CachePaths((tmp / "unterminated").c_str(), names, paths) == CacheRead::PARSED);
	CHECK(paths == std::vector<std::string> {""});
	// What the check makes of them: a cache that cannot be read is refused with its own sentence, and an
	// absent one names nothing, so a library not loaded is missing.
	const auto unreadable = Check({}, names, (tmp / "text").c_str());
	CHECK_FALSE(unreadable.ok);
	CHECK(unreadable.refusal.cause == SearchCause::CACHE_UNREADABLE);
	const auto absent = Check({}, names, (tmp / "absent").c_str());
	CHECK_FALSE(absent.ok);
	CHECK(absent.refusal.cause == SearchCause::MISSING);
	// Nothing is read when no name is looked for.
	CHECK(Check({}, {}, (tmp / "text").c_str()).ok);
	CHECK(Check({}, {"libloaded.so.1"}, (tmp / "text").c_str()).ok);
}

TEST_CASE("A cache larger than 256 MiB is refused without reading it; one of 256 MiB is read",
          "[driver][search][cache]") {
	TempDir tmp;
	const std::vector<std::string> names {"libneeded.so.1"};
	const auto good = MakeCache({{"libneeded.so.1", "/opt/a/libneeded.so.1"}});
	// A good cache extended by a hole, so neither file takes space on disk.
	constexpr off_t LIMIT = off_t {256} << 20;
	for (const off_t size : {LIMIT, LIMIT + 1}) {
		const auto path = tmp / std::to_string(size);
		WriteFile(path, good);
		REQUIRE(::truncate(path.c_str(), size) == 0);
		INFO("size " << size);
		std::vector<std::string> paths;
		const auto read = CachePaths(path.c_str(), names, paths);
		CHECK(read == (size == LIMIT ? CacheRead::PARSED : CacheRead::UNREADABLE));
		CHECK(paths ==
		      (size == LIMIT ? std::vector<std::string> {"/opt/a/libneeded.so.1"} : std::vector<std::string> {}));
		REQUIRE(::unlink(path.c_str()) == 0);
	}
}

TEST_CASE("The reader understands this host's own library cache", "[driver][search][cache]") {
	std::vector<std::string> paths;
	const auto read = CachePaths(LIBRARY_CACHE_PATH, {"libc.so.6"}, paths);
	if (read == CacheRead::ABSENT) {
		WARN("this host has no " << LIBRARY_CACHE_PATH << "; not tested");
		return;
	}
	REQUIRE(read == CacheRead::PARSED);
	REQUIRE(paths.size() == 1);
	REQUIRE_FALSE(paths[0].empty());
	CHECK(paths[0][0] == '/');
	// What ldconfig itself lists for the name, for this platform, where it can be run.
	FILE *listing = ::popen("ldconfig -p 2>/dev/null", "r");
	std::string text;
	if (listing != nullptr) {
		char buffer[4096];
		size_t got;
		while ((got = std::fread(buffer, 1, sizeof(buffer), listing)) > 0) {
			text.append(buffer, got);
		}
		::pclose(listing);
	}
	if (text.empty()) {
		WARN("ldconfig -p gave nothing here; the path was not compared");
		return;
	}
#if defined(__x86_64__)
	const std::string platform = "x86-64";
#else
	const std::string platform = "AArch64";
#endif
	const auto line_end = text.find(" => " + paths[0] + "\n");
	REQUIRE(line_end != std::string::npos);
	const auto line_start = text.rfind('\n', line_end);
	const auto line = text.substr(line_start + 1, line_end - line_start - 1);
	INFO(line);
	CHECK(line.find("libc.so.6 (libc6," + platform) != std::string::npos);
	// And the check takes it from there for a name that no loaded object answered to.
	SearchInputs inputs;
	inputs.list = GivenList;
	g_list.clear();
	inputs.loaded = AlreadyLoaded;
	inputs.needed = {"libc.so.6"};
	SearchRefusal refusal;
	std::vector<NeededLibrary> preload;
	REQUIRE(CheckSearchList(inputs, refusal, preload));
	REQUIRE(preload.size() == 1);
	char real[PATH_MAX];
	REQUIRE(::realpath(paths[0].c_str(), real) != nullptr);
	CHECK(preload[0].path == real);
}

TEST_CASE("A library the cache names where another user could change it is refused", "[driver][search][cache]") {
	TempDir tmp;
	if (!redshift_driver_test::PrivateAncestry(tmp)) {
		return;
	}
	MakeDir(tmp / "private", 0700);
	MakeDir(tmp / "loose", 0777);
	MakeDir(tmp / "group", 0770);
	Touch(tmp / "private/libneeded.so.1", 0600);
	Touch(tmp / "loose/libneeded.so.1", 0600);
	Touch(tmp / "group/libneeded.so.1", 0600);
	const auto cache = tmp / "cache";
	const auto check = [&](const std::vector<CacheEntry> &entries,
	                       const std::vector<std::string> &wanted = {"libneeded.so.1"}) {
		WriteFile(cache, MakeCache(entries));
		return Check({}, wanted, cache.c_str());
	};
	// A private directory and library: fine, and that is the file to load.
	auto verdict = check({{"libneeded.so.1", tmp / "private/libneeded.so.1"}});
	CHECK(verdict.ok);
	REQUIRE(verdict.preload.size() == 1);
	CHECK(verdict.preload[0].path == tmp / "private/libneeded.so.1");
	// In a directory others, or the group, can write.
	for (const char *dir : {"loose", "group"}) {
		INFO(dir);
		verdict = check({{"libneeded.so.1", tmp / (std::string(dir) + "/libneeded.so.1")}});
		CHECK_FALSE(verdict.ok);
		CHECK(verdict.refusal.cause == SearchCause::CACHE);
		CHECK(verdict.refusal.rule == "the directory is writable by another user");
		CHECK(verdict.refusal.where == tmp / dir);
		CHECK(verdict.preload.empty());
	}
	// Only the first entry for the name is taken, as glibc does: a later one is not looked at.
	CHECK(check({{"libneeded.so.1", tmp / "private/libneeded.so.1"}, {"libneeded.so.1", tmp / "loose/libneeded.so.1"}})
	          .ok);
	CHECK_FALSE(
	    check({{"libneeded.so.1", tmp / "loose/libneeded.so.1"}, {"libneeded.so.1", tmp / "private/libneeded.so.1"}})
	        .ok);
	// A private directory, a library others or the group can write.
	for (const mode_t mode : {mode_t {0666}, mode_t {0620}}) {
		REQUIRE(::chmod((tmp / "private/libneeded.so.1").c_str(), mode) == 0);
		verdict = check({{"libneeded.so.1", tmp / "private/libneeded.so.1"}});
		CHECK_FALSE(verdict.ok);
		CHECK(verdict.refusal.cause == SearchCause::CACHE);
		CHECK(verdict.refusal.rule == "a dependency of the driver can be written by another user");
		CHECK(verdict.refusal.where == tmp / "private/libneeded.so.1");
	}
	REQUIRE(::chmod((tmp / "private/libneeded.so.1").c_str(), 0600) == 0);
	// A name not looked for, or one that is loaded already: ignored, whatever the path.
	CHECK(check({{"libother.so.1", tmp / "loose/libother.so.1"}}, {}).ok);
	CHECK(check({{"libloaded.so.1", tmp / "loose/libloaded.so.1"}}, {"libloaded.so.1"}).ok);
	// A file the cache names that is not there, or below a file: missing, not a refusal of the place.
	verdict = check({{"libneeded.so.1", tmp / "private/gone.so.1"}});
	CHECK_FALSE(verdict.ok);
	CHECK(verdict.refusal.cause == SearchCause::MISSING);
	CHECK(verdict.refusal.where == cache);
	verdict = check({{"libneeded.so.1", tmp / "private/libneeded.so.1/below"}});
	CHECK(verdict.refusal.cause == SearchCause::MISSING);
	// One that cannot be looked at is not missing: refused for its place, which an administrator can fix.
	if (::geteuid() != 0) {
		MakeDir(tmp / "locked", 0700);
		Touch(tmp / "locked/libneeded.so.1", 0600);
		REQUIRE(::chmod((tmp / "locked").c_str(), 0) == 0);
		verdict = check({{"libneeded.so.1", tmp / "locked/libneeded.so.1"}});
		REQUIRE(::chmod((tmp / "locked").c_str(), 0700) == 0);
		CHECK_FALSE(verdict.ok);
		CHECK(verdict.refusal.cause == SearchCause::CACHE);
	}
	// The directory is walked as given: a link of the user's own to a private directory, from a directory
	// others can write, which could swap it.
	REQUIRE(::symlink((tmp / "private").c_str(), (tmp / "loose/link").c_str()) == 0);
	verdict = check({{"libneeded.so.1", tmp / "loose/link/libneeded.so.1"}});
	CHECK_FALSE(verdict.ok);
	CHECK(verdict.refusal.cause == SearchCause::CACHE);
	CHECK(verdict.refusal.rule == "a parent directory is writable by another user");
	CHECK(verdict.refusal.where == tmp / "loose");
}

TEST_CASE("A link the cache names is followed, each link and the file the user's or root's, and the file loaded is "
          "its real path",
          "[driver][search][cache]") {
	TempDir tmp;
	if (!redshift_driver_test::PrivateAncestry(tmp)) {
		return;
	}
	MakeDir(tmp / "lib", 0700);
	MakeDir(tmp / "elsewhere", 0700);
	MakeDir(tmp / "open", 0777);
	const auto name = tmp / "lib/libneeded.so.1";
	WriteFile(tmp / "cache", MakeCache({{"libneeded.so.1", name}}));
	const auto check = [&] {
		return Check({}, {"libneeded.so.1"}, (tmp / "cache").c_str());
	};
	// To a private file: fine, relative or absolute, and through two links; the real path is what is loaded.
	Touch(tmp / "elsewhere/real.so", 0600);
	REQUIRE(::symlink("../elsewhere/real.so", name.c_str()) == 0);
	auto verdict = check();
	CHECK(verdict.ok);
	REQUIRE(verdict.preload.size() == 1);
	CHECK(verdict.preload[0].path == tmp / "elsewhere/real.so");
	REQUIRE(::unlink(name.c_str()) == 0);
	REQUIRE(::symlink((tmp / "elsewhere/real.so").c_str(), (tmp / "elsewhere/hop").c_str()) == 0);
	REQUIRE(::symlink((tmp / "elsewhere/hop").c_str(), name.c_str()) == 0);
	CHECK(check().ok);
	// To a file others can write.
	Touch(tmp / "elsewhere/loose.so", 0666);
	REQUIRE(::unlink(name.c_str()) == 0);
	REQUIRE(::symlink((tmp / "elsewhere/loose.so").c_str(), name.c_str()) == 0);
	CHECK_FALSE(check().ok);
	// To a private file in a directory others can change.
	Touch(tmp / "open/swappable.so", 0600);
	REQUIRE(::unlink(name.c_str()) == 0);
	REQUIRE(::symlink((tmp / "open/swappable.so").c_str(), name.c_str()) == 0);
	verdict = check();
	CHECK_FALSE(verdict.ok);
	CHECK(verdict.refusal.rule == "a dependency of the driver is in a directory another user could change");
	// To a directory, or a loop, or nothing.
	REQUIRE(::unlink(name.c_str()) == 0);
	REQUIRE(::symlink((tmp / "elsewhere").c_str(), name.c_str()) == 0);
	CHECK(check().refusal.rule == "a dependency of the driver is not a regular file");
	REQUIRE(::unlink(name.c_str()) == 0);
	REQUIRE(::symlink("libneeded.so.1", name.c_str()) == 0);
	CHECK(InBoundedChild([&] {
		return check().refusal.rule == "a dependency of the driver is a symbolic link that cannot be followed";
	}));
	REQUIRE(::unlink(name.c_str()) == 0);
	REQUIRE(::symlink((tmp / "elsewhere/missing.so").c_str(), name.c_str()) == 0);
	verdict = check();
	CHECK_FALSE(verdict.ok); // dangling: the file is gone, as glibc's open finds it, so the system directories
	CHECK(verdict.refusal.cause == SearchCause::MISSING); // are next, and here there are none

	// A FIFO at the name: refused, and nothing opens it (it would block).
	REQUIRE(::unlink(name.c_str()) == 0);
	REQUIRE(::mkfifo(name.c_str(), 0600) == 0);
	CHECK(InBoundedChild([&] {
		const auto v = check();
		return !v.ok && v.refusal.rule == "a dependency of the driver is not a regular file";
	}));
}

TEST_CASE("A library whose real path leads to another file than the one checked is refused",
          "[driver][search][cache]") {
	// The file the cache names is looked at with lstat, then resolved with realpath, which is what is loaded; a
	// swap between the two would load a file no check saw. The swap is made by the realpath stand-in above, to a
	// file that passes every other check, so only the device and inode comparison can refuse it.
	TempDir tmp;
	if (!redshift_driver_test::PrivateAncestry(tmp)) {
		return;
	}
	MakeDir(tmp / "lib", 0700);
	Touch(tmp / "lib/libneeded.so.1", 0600);
	Touch(tmp / "lib/other.so.1", 0600);
	const auto name = tmp / "lib/libneeded.so.1";
	WriteFile(tmp / "cache", MakeCache({{"libneeded.so.1", name}}));
	g_realpath_of = name;
	g_realpath_to = tmp / "lib/other.so.1";
	g_realpath_answers = 0;
	const auto swapped = Check({}, {"libneeded.so.1"}, (tmp / "cache").c_str());
	const int answered = g_realpath_answers;
	g_realpath_to = name; // the control: the stand-in answers the file itself
	const auto same = Check({}, {"libneeded.so.1"}, (tmp / "cache").c_str());
	g_realpath_of.clear();
	REQUIRE(answered == 1); // the loader's realpath reached the stand-in
	CHECK_FALSE(swapped.ok);
	CHECK(swapped.refusal.cause == SearchCause::CACHE);
	CHECK(swapped.refusal.rule == "a dependency of the driver cannot be resolved to the file that was checked");
	CHECK(swapped.preload.empty());
	CHECK(same.ok);
	REQUIRE(same.preload.size() == 1);
	CHECK(same.preload[0].path == name);
}

TEST_CASE("Another user's library, or link, is refused (root only)", "[driver][search][cache]") {
	if (::geteuid() != 0) {
		WARN("not running as root, which is needed to give a file to another user; not tested");
		return;
	}
	TempDir tmp;
	if (!redshift_driver_test::PrivateAncestry(tmp)) {
		return;
	}
	MakeDir(tmp / "lib", 0700);
	Touch(tmp / "lib/libneeded.so.1", 0644);
	REQUIRE(::chown((tmp / "lib/libneeded.so.1").c_str(), redshift_driver_test::OTHER_USER, 0) == 0);
	WriteFile(tmp / "cache", MakeCache({{"libneeded.so.1", tmp / "lib/libneeded.so.1"}}));
	// Called as another user than the file's, and not root.
	SearchInputs inputs;
	inputs.list = GivenList;
	g_list.clear();
	inputs.loaded = AlreadyLoaded;
	const auto cache = tmp / "cache";
	inputs.cache_path = cache.c_str();
	inputs.needed = {"libneeded.so.1"};
	inputs.user = 4242;
	SearchRefusal refusal;
	std::vector<NeededLibrary> preload;
	// The user's own directories are what the walk accepts, so as user 4242 the private root-owned
	// directory is accepted and the file, which belongs to a third user, is refused.
	REQUIRE(::chown((tmp / "lib").c_str(), 4242, 0) == 0);
	CHECK_FALSE(CheckSearchList(inputs, refusal, preload));
	CHECK(refusal.rule == "a dependency of the driver is owned by another user");
	// A link another user owns.
	REQUIRE(::unlink((tmp / "lib/libneeded.so.1").c_str()) == 0);
	Touch(tmp / "real.so", 0600);
	REQUIRE(::chown((tmp / "real.so").c_str(), 4242, 0) == 0);
	REQUIRE(::symlink((tmp / "real.so").c_str(), (tmp / "lib/libneeded.so.1").c_str()) == 0);
	REQUIRE(::lchown((tmp / "lib/libneeded.so.1").c_str(), redshift_driver_test::OTHER_USER, 0) == 0);
	CHECK_FALSE(CheckSearchList(inputs, refusal, preload));
	CHECK(refusal.rule == "a dependency of the driver is a symbolic link another user owns");
}

TEST_CASE("An object's own run path is in its search list, and so in what a loadable extension is checked for",
          "[driver][search]") {
	// A loadable extension is a library of its own, with its own run path that the platform loader also
	// searches, so its elements are checked for being relative too. A library with one stands in for it: its list is
	// read from an address in it, with no file opened.
	void *handle =
	    ::dlopen(redshift_driver_test::TestInputPath(REDSHIFT_TEST_RUNPATH_PROBE).c_str(), RTLD_NOW | RTLD_LOCAL);
	REQUIRE(handle != nullptr);
	void *symbol = ::dlsym(handle, "redshift_test_runpath_probe");
	REQUIRE(symbol != nullptr);
	std::vector<std::string> own;
	REQUIRE(ObjectSearchList(symbol, own));
	CHECK(std::find(own.begin(), own.end(), "/redshift-runpath-probe-dir") != own.end());
	// The platform's list for a loadable extension is the main program's and then its own: the system's
	// directories come first, and the extension's run path is added without repeats.
	std::vector<std::string> program, whole;
	REQUIRE(PlatformSearchList(program));
	REQUIRE(PlatformSearchListOf(symbol, whole));
	CHECK(std::find(program.begin(), program.end(), "/redshift-runpath-probe-dir") == program.end());
	REQUIRE(whole.size() > program.size());
	CHECK(std::equal(program.begin(), program.end(), whole.begin()));
	CHECK(std::find(whole.begin(), whole.end(), "/redshift-runpath-probe-dir") != whole.end());
	// An address in no object has no list.
	std::vector<std::string> none;
	CHECK_FALSE(ObjectSearchList(nullptr, none));
	CHECK(none.empty());
	// So the list of a program and an extension that cannot be read is not a list: the load is refused,
	// not allowed on the main program's alone.
	CHECK_FALSE(PlatformSearchListOf(nullptr, none));
	SearchInputs unreadable_extension;
	unreadable_extension.list = [](std::vector<std::string> &directories) {
		return PlatformSearchListOf(nullptr, directories);
	};
	unreadable_extension.cache_path = nullptr;
	SearchRefusal refusal;
	std::vector<NeededLibrary> preload;
	CHECK_FALSE(CheckSearchList(unreadable_extension, refusal, preload));
	CHECK(refusal.cause == SearchCause::UNREADABLE);
	// The other half: an unreadable main-program list refuses it too, though the extension's reads fine.
	std::vector<std::string> from_program;
	CHECK_FALSE(PlatformSearchListFrom([](std::vector<std::string> &) { return false; }, symbol, from_program));
	CHECK(from_program.empty());
	CHECK(PlatformSearchListFrom(&ProgramSearchList, symbol, from_program));
	CHECK_FALSE(from_program.empty());
	SearchInputs unreadable_program;
	unreadable_program.list = [](std::vector<std::string> &directories) {
		return PlatformSearchListFrom([](std::vector<std::string> &) { return false; },
		                              reinterpret_cast<const void *>(&PlatformSearchListOf), directories);
	};
	unreadable_program.cache_path = nullptr;
	CHECK_FALSE(CheckSearchList(unreadable_program, refusal, preload));
	CHECK(refusal.cause == SearchCause::UNREADABLE);
	// Without repeats.
	std::vector<std::string> twice = own;
	REQUIRE(ObjectSearchList(symbol, twice));
	CHECK(twice == own);
	::dlclose(handle);
}

TEST_CASE("Whether a library is already loaded is decided from the loaded objects, opening nothing",
          "[driver][search]") {
	// libc is always loaded, under a path whose last component is its name.
	CHECK(LibraryIsMapped("libc.so.6"));
	CHECK_FALSE(LibraryIsMapped("libzz-not-loaded.so.7"));
	CHECK_FALSE(LibraryIsMapped(""));
	// A name that is only part of a loaded name is not it.
	CHECK_FALSE(LibraryIsMapped("c.so.6"));
	CHECK_FALSE(LibraryIsMapped("libc.so"));
	// No directory is searched and no file opened: a FIFO under the name in LD_LIBRARY_PATH-like places
	// is of no concern to this call (the case above runs it with the variable set).
}

TEST_CASE("A library counts as loaded under a name as glibc matches it: its soname, or its path", "[driver][search]") {
	TempDir tmp;
	// glibc matches a bare name with an object's own name and its DT_SONAME, and the file's last path
	// component is neither. A file loaded by its path answers to its soname only; whether the name is
	// loaded is what dlopen with RTLD_NOLOAD says (its search finds nothing in these tests' directories).
	const auto glibc_says = [](const std::string &name) {
		void *handle = ::dlopen(name.c_str(), RTLD_LAZY | RTLD_NOLOAD);
		if (handle != nullptr) {
			::dlclose(handle);
		}
		return handle != nullptr;
	};
	// A copy of a library under a name that is not its soname.
	const std::string source = redshift_driver_test::TestInputPath(REDSHIFT_TEST_RUNPATH_PROBE);
	const std::string soname = source.substr(source.rfind('/') + 1);
	const std::string renamed = "libredshift-renamed-copy.so.7";
	WriteFile(tmp / renamed, ReadWholeFile(source), 0700);
	void *handle = ::dlopen((tmp / renamed).c_str(), RTLD_LAZY | RTLD_LOCAL);
	REQUIRE(handle != nullptr);
	CHECK(LibraryIsMapped(soname)); // its soname
	CHECK(LibraryIsMapped(soname) == glibc_says(soname));
	CHECK_FALSE(LibraryIsMapped(renamed)); // the last component of its path: not a name it answers to
	CHECK(LibraryIsMapped(renamed) == glibc_says(renamed));
	CHECK(LibraryIsMapped(tmp / renamed)); // the path it was loaded by
	::dlclose(handle);
	// A library with no soname at all, loaded by path.
	const std::string unnamed_source = redshift_driver_test::TestInputPath(REDSHIFT_TEST_NO_SONAME_PROBE);
	const std::string unnamed = unnamed_source.substr(unnamed_source.rfind('/') + 1);
	void *without = ::dlopen(unnamed_source.c_str(), RTLD_LAZY | RTLD_LOCAL);
	REQUIRE(without != nullptr);
	CHECK_FALSE(LibraryIsMapped(unnamed));
	CHECK(LibraryIsMapped(unnamed) == glibc_says(unnamed));
	CHECK(LibraryIsMapped(unnamed_source));
	::dlclose(without);
}

TEST_CASE("An object that another namespace holds is not one the driver's dependencies would reuse",
          "[driver][search]") {
	// glibc reuses an already loaded object only from the namespace that is loading, and dl_iterate_phdr
	// reports only the namespace of its caller, so a library a dlmopen put in a namespace of its own does
	// not make its name loaded: the name stays one that is looked for in the cache and loaded first.
	const std::string source = redshift_driver_test::TestInputPath(REDSHIFT_TEST_RUNPATH_PROBE);
	const std::string soname = source.substr(source.rfind('/') + 1);
	REQUIRE_FALSE(LibraryIsMapped(soname));
	void *other = ::dlmopen(LM_ID_NEWLM, source.c_str(), RTLD_LAZY | RTLD_LOCAL);
	if (other == nullptr) {
		WARN("dlmopen is unavailable here (" << ::dlerror() << "); not tested");
		return;
	}
	Lmid_t namespace_id = LM_ID_BASE;
	REQUIRE(::dlinfo(other, RTLD_DI_LMID, &namespace_id) == 0);
	REQUIRE(namespace_id != LM_ID_BASE);
	CHECK_FALSE(LibraryIsMapped(soname));
	CHECK_FALSE(LibraryIsMapped(source));
	// Loaded here too, it counts.
	void *here = ::dlopen(source.c_str(), RTLD_LAZY | RTLD_LOCAL);
	REQUIRE(here != nullptr);
	CHECK(LibraryIsMapped(soname));
	::dlclose(here);
	::dlclose(other);
}

// ---- The shipped configuration -----------------------------------------------------------------

TEST_CASE("The shipped loader takes the real driver's libraries from the cache, not from the search list",
          "[driver][search][shipped]") {
	TempDir tmp;
	if (!redshift_driver_test::PrivateAncestry(tmp)) {
		return;
	}
	const auto config = EmbeddedConfig();
	// The libraries come from the embedded driver's own bytes.
	for (const char *prerequisite : {REDSHIFT_DRIVER_HOST_PREREQUISITES}) {
		INFO(prerequisite);
		CHECK(std::find(config.needed.begin(), config.needed.end(), prerequisite) != config.needed.end());
	}
	if (!redshift_driver_test::HostHasDriverPrerequisites()) {
		return;
	}
	// A directory on the search list holds a copy of each that other users can write: it is not what is
	// loaded, and it refuses nothing.
	MakeDir(tmp / "lib", 0777);
	for (const char *prerequisite : {REDSHIFT_DRIVER_HOST_PREREQUISITES}) {
		Touch(tmp / (std::string("lib/") + prerequisite), 0666);
	}
	SearchInputs search;
	g_list = {tmp / "lib"};
	search.list = GivenList;
	search.loaded = AlreadyLoaded; // as if none were loaded, so each is looked for
	search.needed = config.needed;
	SearchRefusal refusal;
	std::vector<NeededLibrary> preload;
	REQUIRE(CheckSearchList(search, refusal, preload));
	for (const char *prerequisite : {REDSHIFT_DRIVER_HOST_PREREQUISITES}) {
		INFO(prerequisite);
		const auto found = std::find_if(preload.begin(), preload.end(),
		                                [&](const NeededLibrary &library) { return library.name == prerequisite; });
		REQUIRE(found != preload.end());
		CHECK(found->path.find(tmp.path()) != 0);
		CHECK(found->path[0] == '/');
	}
	CHECK(preload.size() == config.needed.size());
}

TEST_CASE("The shipped configuration names the prerequisite and how to get it", "[driver][search][shipped]") {
	const auto config = EmbeddedConfig();
	const std::vector<std::string> generated {REDSHIFT_DRIVER_HOST_PREREQUISITES};
	CHECK(config.prerequisites == generated);
	CHECK(config.prerequisite_hint ==
	      "it comes from the host's keyutils package (for example keyutils-libs or libkeyutils1)");
	// The platform loader's refusal for the library is recognised from the configuration's list.
	const std::string text = "libkeyutils.so.1: cannot open shared object file: No such file or directory";
	const auto *missing = MissingPrerequisite(text.c_str(), config.prerequisites);
	REQUIRE(missing != nullptr);
	CHECK(*missing == "libkeyutils.so.1");
	// On a host without the library, the whole sequence says so before anything is written (a host with it
	// loads the driver, which other cases do in a child of their own).
	if (redshift_driver_test::HostHasDriverPrerequisites()) {
		return;
	}
	TempDir tmp;
	if (!redshift_driver_test::PrivateAncestry(tmp)) {
		return;
	}
	MakeDir(tmp / "base", 0700);
	DriverLoader loader(config);
	auto inputs = redshift_driver_test::LoadFrom(tmp / "base");
	inputs.search.cache_path = LIBRARY_CACHE_PATH;
	const int calls = PlatformLoaderCalls();
	const auto result = loader.Load(inputs);
	REQUIRE_FALSE(result.ok());
	CHECK(result.error.kind == DriverErrorKind::MISSING_PREREQUISITE);
	CHECK(result.error.message == "the Redshift ODBC driver needs libkeyutils.so.1, which this host does not have; " +
	                                  config.prerequisite_hint +
	                                  "; install it (or, if it is installed, have an administrator run ldconfig), then "
	                                  "restart the program and connect again");
	CHECK(PlatformLoaderCalls() == calls);
	CHECK(::access((tmp / "base/.redshift-driver").c_str(), F_OK) != 0);
	REQUIRE(result.error.attempts.size() == 1);
	CHECK(result.error.attempts[0].reason.find("libkeyutils.so.1") != std::string::npos);
}

// ---- The cache, through the real ldconfig (a disposable root only) ----------------------------

namespace {

// Undoes the case below however it ends: the configuration file removed and the cache rebuilt.
struct CacheCleanup {
	std::string conf;
	~CacheCleanup() {
		if (!conf.empty()) {
			::unlink(conf.c_str());
			if (std::system("ldconfig >/dev/null 2>&1") != 0) {
				std::fprintf(stderr, "ldconfig failed while restoring the library cache\n");
			}
		}
	}
};

} // namespace

TEST_CASE("A library planted where ldconfig's cache points is refused, though the search list never shows it",
          "[driver][search][cache][root]") {
	// The case that opens an administrator's library cache: it changes /etc/ld.so.conf.d and runs
	// ldconfig, so it runs only where the caller says the root is disposable (run-loader-tests-as-root.sh,
	// a container that is removed afterwards).
	if (::geteuid() != 0 || std::getenv("REDSHIFT_TEST_DISPOSABLE_ROOT") == nullptr) {
		WARN("not a disposable root (REDSHIFT_TEST_DISPOSABLE_ROOT unset, or not root); the real cache is not tested");
		return;
	}
	if (::access("/etc/ld.so.conf.d", W_OK) != 0 || std::system("ldconfig -V >/dev/null 2>&1") != 0) {
		WARN("no writable /etc/ld.so.conf.d or no ldconfig here; not tested");
		return;
	}
	const std::string name = "libkeyutils.so.1";
	if (LibraryIsMapped(name)) {
		WARN("this process has " << name << " loaded already, which is then not looked for; not tested");
		return;
	}
	TempDir tmp;
	MakeDir(tmp / "evil", 0777);
	REQUIRE(::chown((tmp / "evil").c_str(), 1234, 1234) == 0);
	WriteFile(tmp / ("evil/" + name), ReadWholeFile(redshift_driver_test::TestInputPath(REDSHIFT_TEST_FAKE_KEYUTILS)),
	          0666);
	REQUIRE(::chown((tmp / ("evil/" + name)).c_str(), 1234, 1234) == 0);
	CacheCleanup cleanup;
	cleanup.conf = "/etc/ld.so.conf.d/zz-redshift-driver-test.conf";
	WriteFile(cleanup.conf, tmp / "evil\n", 0644);
	REQUIRE(std::system("ldconfig") == 0);

	// The search list is blind to it (the main program's list holds only the system's directories).
	std::vector<std::string> list;
	REQUIRE(PlatformSearchList(list));
	for (const auto &directory : list) {
		CHECK(directory != tmp / "evil");
	}
	// The host's own copy, where there is one, may come first in the cache; then it is what would be loaded.
	std::vector<std::string> paths;
	REQUIRE(CachePaths(LIBRARY_CACHE_PATH, {name}, paths) == CacheRead::PARSED);
	REQUIRE(paths.size() == 1);
	if (paths[0] != tmp / ("evil/" + name)) {
		WARN("the host's own " << name << " comes first in the cache here; only the shipped load is tested");
	}
	SearchInputs inputs;
	inputs.needed = {name};
	inputs.loaded = AlreadyLoaded;
	SearchRefusal refusal;
	std::vector<NeededLibrary> preload;
	const bool ok = CheckSearchList(inputs, refusal, preload);
	if (paths[0] == tmp / ("evil/" + name)) {
		CHECK_FALSE(ok);
		CHECK(refusal.cause == SearchCause::CACHE);
		CHECK(refusal.where.find(tmp / "evil") == 0);
	}
	for (const auto &library : preload) {
		CHECK(library.path.find(tmp / "evil") != 0);
	}

	// And so is the shipped loader, whole: the planted library is never loaded, and nothing is installed
	// unless the host's own copy is what the cache gives.
	const auto config = EmbeddedConfig();
	REQUIRE(std::find(config.needed.begin(), config.needed.end(), name) != config.needed.end());
	MakeDir(tmp / "base", 0700);
	std::fflush(nullptr);
	const pid_t child = ::fork();
	REQUIRE(child >= 0);
	if (child == 0) {
		DriverLoader loader(config);
		auto load = redshift_driver_test::LoadFrom(tmp / "base");
		load.search.cache_path = LIBRARY_CACHE_PATH;
		const auto result = loader.Load(load);
		const bool planted_loaded = LibraryIsMapped(tmp / ("evil/" + name));
		const bool refused_right =
		    !result.ok() && result.error.kind == DriverErrorKind::REFUSED_ENVIRONMENT &&
		    result.error.message.find("the platform loader's library cache or system directory has a library "
		                              "the Redshift driver needs in a place that cannot be used (") == 0 &&
		    result.error.message.find(tmp.path()) == std::string::npos &&
		    ::access((tmp / "base/.redshift-driver").c_str(), F_OK) != 0;
		::_exit(!planted_loaded && (paths[0] == tmp / ("evil/" + name) ? refused_right : true) ? 0 : 1);
	}
	int status = 0;
	REQUIRE(::waitpid(child, &status, 0) == child);
	CHECK((WIFEXITED(status) && WEXITSTATUS(status) == 0));
}

namespace {

// Puts the host's library cache aside for one case, and a planted file away, and both back on scope exit.
struct CacheAside {
	std::string aside, planted;
	~CacheAside() {
		if (!planted.empty()) {
			::unlink(planted.c_str());
		}
		if (!aside.empty() && ::rename(aside.c_str(), LIBRARY_CACHE_PATH) != 0) {
			std::fprintf(stderr, "could not put the library cache back from %s\n", aside.c_str());
		}
	}
};

} // namespace

TEST_CASE("With no library cache, a library in the host's own system directory is taken from there, checked as a "
          "cached one is",
          "[driver][search][system][root]") {
	// It moves /etc/ld.so.cache aside and writes into a system directory, so it runs only where the caller says the
	// root is disposable (run-loader-tests-as-root.sh).
	if (::geteuid() != 0 || std::getenv("REDSHIFT_TEST_DISPOSABLE_ROOT") == nullptr) {
		WARN("not a disposable root (REDSHIFT_TEST_DISPOSABLE_ROOT unset, or not root); the real directories are not "
		     "tested");
		return;
	}
	const std::string name = "libkeyutils.so.1";
	std::string dir;
	for (const auto &candidate : SystemLibraryDirectories()) {
		struct stat st;
		if (::stat((candidate + "/" + name).c_str(), &st) == 0) {
			WARN("this host has " << name << " in " << candidate << " already; not tested");
			return;
		}
		if (dir.empty() && ::stat(candidate.c_str(), &st) == 0 && S_ISDIR(st.st_mode)) {
			dir = candidate;
		}
	}
	if (dir.empty() || LibraryIsMapped(name) || ::access(LIBRARY_CACHE_PATH, F_OK) != 0) {
		WARN("no system directory, " << name << " loaded already, or no library cache here; not tested");
		return;
	}
	CacheAside restore;
	restore.aside = std::string(LIBRARY_CACHE_PATH) + ".redshift-test-aside";
	REQUIRE(::rename(LIBRARY_CACHE_PATH, restore.aside.c_str()) == 0);
	restore.planted = dir + "/" + name;
	WriteFile(restore.planted, ReadWholeFile(redshift_driver_test::TestInputPath(REDSHIFT_TEST_FAKE_KEYUTILS)), 0644);
	SearchInputs inputs; // the shipped cache path and system directories
	inputs.list = GivenList;
	g_list.clear();
	inputs.loaded = AlreadyLoaded;
	inputs.needed = {name};
	SearchRefusal refusal;
	std::vector<NeededLibrary> preload;
	REQUIRE(CheckSearchList(inputs, refusal, preload));
	REQUIRE(preload.size() == 1);
	char real[PATH_MAX];
	REQUIRE(::realpath(restore.planted.c_str(), real) != nullptr);
	CHECK(preload[0].path == real);
	// The same rules: a file others can write is refused, and one that is not there is missing.
	REQUIRE(::chmod(restore.planted.c_str(), 0666) == 0);
	CHECK_FALSE(CheckSearchList(inputs, refusal, preload));
	CHECK(refusal.cause == SearchCause::CACHE);
	REQUIRE(::unlink(restore.planted.c_str()) == 0);
	CHECK_FALSE(CheckSearchList(inputs, refusal, preload));
	CHECK(refusal.cause == SearchCause::MISSING);
}
