//===----------------------------------------------------------------------===//
//                         DuckDB - Redshift
//
// test/client/odbc/trust_store_test_support.hpp
//
// The files, log sink and expected messages the trust search's tests share: the search's own cases
// and each platform's ResolveTrustSource cases.
//===----------------------------------------------------------------------===//

#pragma once

#include "catch.hpp"
#include "duckdb/common/vector.hpp"
#include "redshift_transport.hpp"

#include <climits>
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <ftw.h>
#include <string>
#include <sys/stat.h>
#include <unistd.h>
#include <vector>

namespace redshift_trust_test {

//! Enough of a PEM file to be non-empty: the search checks that a file is readable, not what it
//! holds.
inline const char *const BUNDLE = "-----BEGIN CERTIFICATE-----\n";

//! The line logged for an SSL_CERT_FILE that names no readable file, spelled out here rather than
//! read from the code, so that changing one of the two fails this suite.
inline const std::string UNREADABLE_LINE =
    "SSL_CERT_FILE is set but does not name a readable file; trying the system CA bundle locations";

typedef duckdb::vector<std::string> Locations;

class RecordingLogSink : public duckdb::RedshiftLogSink {
public:
	void Debug(const std::string &message) noexcept override {
		lines.push_back(message);
	}

	std::vector<std::string> lines;
};

//! The case's own files, in a directory removed with everything in it however the case ends. Only
//! POSIX calls, so the suite compiles on macOS as on Linux.
class TemporaryDirectory {
public:
	//! Made with mkdtemp under TMPDIR, or /tmp, and resolved, so its path holds no symbolic link.
	TemporaryDirectory() {
		const char *tmp = std::getenv("TMPDIR");
		std::string pattern = std::string(tmp && tmp[0] == '/' ? tmp : "/tmp") + "/redshift-trust-test-XXXXXX";
		REQUIRE(mkdtemp(&pattern[0]) != nullptr);
		char resolved[PATH_MAX];
		REQUIRE(realpath(pattern.c_str(), resolved) != nullptr);
		root = resolved;
	}
	~TemporaryDirectory() {
		// A directory left without permissions is made usable first, so all of it can be removed.
		nftw(root.c_str(), MakeUsable, 16, FTW_PHYS);
		nftw(root.c_str(), Remove, 16, FTW_DEPTH | FTW_PHYS);
	}
	TemporaryDirectory(const TemporaryDirectory &) = delete;
	TemporaryDirectory &operator=(const TemporaryDirectory &) = delete;

	//! The path `name` has in the directory, whether or not anything is there.
	std::string Path(const std::string &name) const {
		return root + "/" + name;
	}
	//! Writes `content` to `name` and returns its path.
	std::string File(const std::string &name, const std::string &content) {
		auto path = Path(name);
		std::ofstream(path.c_str()) << content;
		REQUIRE(IsWritten(path, content.size()));
		return path;
	}
	//! Makes `name` a directory and returns its path.
	std::string Directory(const std::string &name) {
		auto path = Path(name);
		REQUIRE(mkdir(path.c_str(), 0700) == 0);
		return path;
	}
	//! Makes `name` a symbolic link to `target` and returns its path.
	std::string Link(const std::string &name, const std::string &target) {
		auto path = Path(name);
		REQUIRE(symlink(target.c_str(), path.c_str()) == 0);
		return path;
	}
	//! The length of the directory's own path, so a case can build a path of an exact length.
	size_t RootLength() const {
		return root.size();
	}

private:
	static bool IsWritten(const std::string &path, size_t size) {
		struct stat info;
		return stat(path.c_str(), &info) == 0 && static_cast<size_t>(info.st_size) == size;
	}
	static int MakeUsable(const char *path, const struct stat *, int type, struct FTW *) {
		if (type == FTW_D || type == FTW_DNR) {
			chmod(path, 0700);
		}
		return 0;
	}
	static int Remove(const char *path, const struct stat *, int, struct FTW *) {
		std::remove(path);
		return 0;
	}

	std::string root;
};

//! The missing-trust error for `locations`, with or without SSL_CERT_FILE set, spelled out here.
inline std::string NoTrustSourceMessage(bool variable_set, const Locations &locations) {
	std::string tried;
	for (auto &location : locations) {
		tried += (tried.empty() ? "" : ", ") + location;
	}
	return std::string("No CA certificate bundle was found to verify the Redshift connection against. Tried ") +
	       (variable_set ? "the file SSL_CERT_FILE names, then " : "") + "the system locations " + tried +
	       ". Set SSL_CERT_FILE to a readable file of PEM CA certificates, or install the operating system's CA "
	       "certificates";
}

//! The refusal of a readable SSL_CERT_FILE that holds no CA bundle.
inline const std::string NO_BUNDLE_MESSAGE =
    "SSL_CERT_FILE names an empty file, a directory or a special file such as a device, so it holds no CA "
    "certificates to verify the Redshift connection against. Set SSL_CERT_FILE to a file of PEM CA certificates, or "
    "unset it to use the system CA certificates";

} // namespace redshift_trust_test
