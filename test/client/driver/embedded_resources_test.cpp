// Windows only. The set is the RCDATA resources of each image that links the loader, this suite
// included, and the loader hashes them before it uses a byte. A changed or missing resource is made
// in a copy of this binary with UpdateResourceW and read from the copy mapped as data, so no case
// changes the image under test.
#include "windows_temp_dir.hpp"

#include "driver_info.h"
#include "embedded_resources.hpp"
#include "sha256.hpp"

#include "catch.hpp"

#include <string>
#include <utility>
#include <vector>

extern "C" IMAGE_DOS_HEADER __ImageBase;

using duckdb::redshift_driver::DriverError;
using duckdb::redshift_driver::DriverErrorKind;
using duckdb::redshift_driver::DriverOrigin;
using duckdb::redshift_driver::OverriddenVariable;
using duckdb::redshift_driver::detail::EmbeddedConfig;
using duckdb::redshift_driver::detail::EmbeddedResource;
using duckdb::redshift_driver::detail::ReadEmbeddedResources;
using duckdb::redshift_driver::detail::Sha256Hex;

namespace {

HMODULE ThisImage() {
	return reinterpret_cast<HMODULE>(&__ImageBase);
}

//! Each file's resource name, as the resource script writes it: its stem in upper case.
std::vector<std::wstring> ResourceNames() {
	std::vector<std::string> stems;
#define REDSHIFT_STEM(name, size, sha256, stem) stems.push_back(#stem);
	REDSHIFT_DRIVER_FILES(REDSHIFT_STEM)
#undef REDSHIFT_STEM
	std::vector<std::wstring> names;
	for (const auto &stem : stems) {
		std::wstring name;
		for (char c : stem) {
			name.push_back(static_cast<wchar_t>(c >= 'a' && c <= 'z' ? c - 'a' + 'A' : c));
		}
		names.push_back(name);
	}
	return names;
}

//! A copy of this binary under the temporary directory, deleted when the case ends.
struct ImageCopy {
	std::wstring path;
	ImageCopy() {
		std::wstring self(32768, L'\0');
		const DWORD length = GetModuleFileNameW(ThisImage(), &self[0], static_cast<DWORD>(self.size()));
		REQUIRE((length > 0 && length < self.size()));
		self.resize(length);
		std::wstring directory(MAX_PATH + 1, L'\0');
		const DWORD directory_length = GetTempPathW(static_cast<DWORD>(directory.size()), &directory[0]);
		REQUIRE((directory_length > 0 && directory_length < directory.size()));
		directory.resize(directory_length);
		path = directory + L"redshift_embedded_resources_" + std::to_wstring(GetCurrentProcessId()) + L".exe";
		REQUIRE(CopyFileW(self.c_str(), path.c_str(), FALSE));
	}
	~ImageCopy() {
		DeleteFileW(path.c_str());
	}
	ImageCopy(const ImageCopy &) = delete;
	ImageCopy &operator=(const ImageCopy &) = delete;

	//! Replaces the named RCDATA resource in the neutral language, the script's, or removes it.
	void Update(const std::wstring &name, const std::string *bytes) const {
		const HANDLE update = BeginUpdateResourceW(path.c_str(), FALSE);
		REQUIRE(update != nullptr);
		const BOOL updated = UpdateResourceW(
		    update, MAKEINTRESOURCEW(10), name.c_str(), MAKELANGID(LANG_NEUTRAL, SUBLANG_NEUTRAL),
		    bytes ? const_cast<char *>(bytes->data()) : nullptr, bytes ? static_cast<DWORD>(bytes->size()) : 0);
		REQUIRE(EndUpdateResourceW(update, updated ? FALSE : TRUE));
		REQUIRE(updated);
	}

	//! The copy's set as the loader reads it.
	bool Read(std::vector<EmbeddedResource> &files, DriverError &error) const {
		const HMODULE module =
		    LoadLibraryExW(path.c_str(), nullptr, LOAD_LIBRARY_AS_DATAFILE | LOAD_LIBRARY_AS_IMAGE_RESOURCE);
		REQUIRE(module != nullptr);
		const bool read = ReadEmbeddedResources(module, files, error);
		FreeLibrary(module);
		return read;
	}
};

} // namespace

TEST_CASE("This suite's image holds each file of the set, as driver_info.h records it", "[driver][embed]") {
	std::vector<EmbeddedResource> files;
	DriverError error;
	REQUIRE(ReadEmbeddedResources(ThisImage(), files, error));
	struct Recorded {
		std::string name;
		size_t size;
		std::string sha256;
	};
	std::vector<Recorded> recorded;
#define REDSHIFT_RECORDED(file_name, file_size, file_sha256, stem)                                                     \
	recorded.push_back({file_name, file_size, file_sha256});
	REDSHIFT_DRIVER_FILES(REDSHIFT_RECORDED)
#undef REDSHIFT_RECORDED
	REQUIRE(!files.empty());
	REQUIRE(files.size() == recorded.size());
	// Each field the Windows loader is handed, in the header's order, and the bytes themselves.
	for (size_t i = 0; i < files.size(); i++) {
		CHECK(files[i].name == recorded[i].name);
		CHECK(files[i].size == recorded[i].size);
		CHECK(files[i].sha256 == recorded[i].sha256);
		CHECK(Sha256Hex(files[i].data, files[i].size) == recorded[i].sha256);
	}
}

TEST_CASE("A copy of the image with its resources unchanged reads as the same set", "[driver][embed]") {
	// The control for the two cases below: the copy and the data mapping alone change nothing.
	const ImageCopy copy;
	std::vector<EmbeddedResource> files;
	DriverError error;
	CHECK(copy.Read(files, error));
}

TEST_CASE("A changed resource gives EMBEDDED_BYTES_CORRUPT, naming the file and no path", "[driver][embed]") {
	std::vector<EmbeddedResource> files;
	DriverError error;
	REQUIRE(ReadEmbeddedResources(ThisImage(), files, error));
	std::string changed(reinterpret_cast<const char *>(files[0].data), files[0].size);
	changed[changed.size() / 2] ^= 1;
	const ImageCopy copy;
	copy.Update(ResourceNames()[0], &changed);
	std::vector<EmbeddedResource> read;
	CHECK_FALSE(copy.Read(read, error));
	CHECK(error.kind == DriverErrorKind::EMBEDDED_BYTES_CORRUPT);
	CHECK(error.message.rfind(files[0].name + " in the extension does not match the hash", 0) == 0);
	CHECK(error.message.find('\\') == std::string::npos);
}

TEST_CASE("A missing resource gives EMBEDDED_BYTES_CORRUPT", "[driver][embed]") {
	const auto names = ResourceNames();
	const ImageCopy copy;
	copy.Update(names.back(), nullptr);
	std::vector<EmbeddedResource> files;
	DriverError error;
	CHECK_FALSE(copy.Read(files, error));
	CHECK(error.kind == DriverErrorKind::EMBEDDED_BYTES_CORRUPT);
}

TEST_CASE("The loader is handed driver_info.h's identity, environment lists and VC++ runtime minimums",
          "[driver][embed]") {
	std::vector<EmbeddedResource> files;
	DriverError error;
	REQUIRE(ReadEmbeddedResources(ThisImage(), files, error));
	const auto config = EmbeddedConfig(files);
	CHECK(config.files.size() == files.size());
	CHECK(config.set_sha256 == REDSHIFT_DRIVER_SET_SHA256);
	CHECK(config.identity.origin == DriverOrigin::EMBEDDED);
	CHECK(config.identity.version == REDSHIFT_DRIVER_VERSION);
	CHECK(config.identity.platform == REDSHIFT_DRIVER_PLATFORM);
	CHECK(config.identity.development == (REDSHIFT_DRIVER_DEVELOPMENT != 0));
	CHECK(config.identity.set_sha256 == REDSHIFT_DRIVER_SET_SHA256);
	const std::vector<std::string> refused = {REDSHIFT_DRIVER_REFUSED_ENVIRONMENT};
	CHECK(config.refused_environment == refused);
	const std::vector<OverriddenVariable> overridden = {REDSHIFT_DRIVER_OVERRIDDEN_ENVIRONMENT};
	REQUIRE(config.overridden_environment.size() == overridden.size());
	for (size_t i = 0; i < overridden.size(); i++) {
		CHECK(config.overridden_environment[i].name == overridden[i].name);
		CHECK(config.overridden_environment[i].keyword == overridden[i].keyword);
	}
	// An empty list passes CheckPrerequisites, so each minimum is compared, and the list must hold one.
	std::vector<std::pair<std::string, std::string>> minimums;
#define REDSHIFT_MINIMUM(dll, a, b, c, d) minimums.emplace_back(dll, #a "." #b "." #c "." #d);
	REDSHIFT_DRIVER_PREREQUISITE_VERSIONS(REDSHIFT_MINIMUM)
#undef REDSHIFT_MINIMUM
	REQUIRE_FALSE(minimums.empty());
	REQUIRE(config.prerequisites.size() == minimums.size());
	for (size_t i = 0; i < minimums.size(); i++) {
		CHECK(config.prerequisites[i].name == minimums[i].first);
		CHECK(config.prerequisites[i].minimum == minimums[i].second);
	}
}

TEST_CASE("Once the set's resources pass, the load reaches the Windows loader's base checks", "[driver][embed]") {
	// A relative REDSHIFT_DRIVER_DIR is the only base, refused before anything is written or loaded, and
	// that refusal is not final, so the case leaves the process's loader as it found it.
	REQUIRE(SetEnvironmentVariableW(L"REDSHIFT_DRIVER_DIR", L"relative"));
	const auto result = duckdb::redshift_driver::LoadEmbeddedDriver();
	SetEnvironmentVariableW(L"REDSHIFT_DRIVER_DIR", nullptr);
	CHECK(result.error.kind == DriverErrorKind::NO_USABLE_DIRECTORY);
	CHECK(result.error.message.find("is not rooted at a drive letter") != std::string::npos);
}

TEST_CASE("A refused name that driver_info.h lists stops the load before any base is used", "[driver][embed]") {
	// The loader's refused names come from driver_info.h's list, as EmbeddedConfig copies it; the
	// refusal is not final, so the case leaves the process's loader as it found it.
	const std::vector<std::string> refused = {REDSHIFT_DRIVER_REFUSED_ENVIRONMENT};
	REQUIRE_FALSE(refused.empty());
	REQUIRE(SetEnvironmentVariableA(refused[0].c_str(), "x"));
	const auto result = duckdb::redshift_driver::LoadEmbeddedDriver();
	SetEnvironmentVariableA(refused[0].c_str(), nullptr);
	CHECK(result.error.kind == DriverErrorKind::REFUSED_ENVIRONMENT);
	CHECK(result.error.message.find(refused[0] + " is set") == 0);
}

TEST_CASE("A DbgHelp.dll from another folder stops the load exactly when driver_info.h turns its check on",
          "[driver][embed]") {
	// EmbeddedConfig copies REDSHIFT_DRIVER_SYSTEM_DBGHELP; with it off, the load passes the check and stops
	// at a relative REDSHIFT_DRIVER_DIR. Neither refusal is final, so the process's loader is left as found.
	const redshift_driver_test::WindowsTempDir temp;
	wchar_t system[MAX_PATH];
	const UINT length = GetSystemDirectoryW(system, MAX_PATH);
	REQUIRE((length > 0 && length < MAX_PATH));
	const auto name = L"\\" + redshift_driver_test::Widen(duckdb::redshift_driver::windows_rules::DBGHELP);
	const auto planted = redshift_driver_test::Widen(temp.path()) + name;
	REQUIRE(CopyFileW((std::wstring(system, length) + name).c_str(), planted.c_str(), TRUE));
	const HMODULE module =
	    LoadLibraryExW(planted.c_str(), nullptr, LOAD_LIBRARY_SEARCH_DLL_LOAD_DIR | LOAD_LIBRARY_SEARCH_SYSTEM32);
	REQUIRE(module != nullptr);
	REQUIRE(SetEnvironmentVariableW(L"REDSHIFT_DRIVER_DIR", L"relative"));
	const auto result = duckdb::redshift_driver::LoadEmbeddedDriver();
	SetEnvironmentVariableW(L"REDSHIFT_DRIVER_DIR", nullptr);
	FreeLibrary(module);
#if REDSHIFT_DRIVER_SYSTEM_DBGHELP
	CHECK(result.error.kind == DriverErrorKind::REFUSED_ENVIRONMENT);
	CHECK(result.error.message == duckdb::redshift_driver::windows_rules::DBGHELP_MESSAGE);
#else
	CHECK(result.error.kind == DriverErrorKind::NO_USABLE_DIRECTORY);
#endif
}
