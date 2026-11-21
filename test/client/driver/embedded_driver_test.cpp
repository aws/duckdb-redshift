// The driver set embedded by embedded_driver_elf.S or embedded_driver_macho.S: the files driver_info.h lists,
// with the sizes it records, read from the paths it names. embedded-build_test.sh (Linux) and
// embedded-macho_test.sh (macOS) check each embedded range's SHA-256 against driver_info.h, which fetch-driver.sh
// made from the lock.
#include "driver_info.h"
#include "embedded_test_helpers.hpp"

#include "catch.hpp"

#include <algorithm>
#include <cstring>
#include <cstddef>
#include <cstdint>
#include <fstream>
#include <iterator>
#include <string>
#include <vector>

#define REDSHIFT_DECLARE_FILE(name, size, sha256, stem)                                                                \
	extern "C" const unsigned char redshift_driver_##stem##_begin[];                                                   \
	extern "C" const unsigned char redshift_driver_##stem##_end[];
REDSHIFT_DRIVER_FILES(REDSHIFT_DECLARE_FILE)

namespace {

struct EmbeddedFile {
	std::string name;
	std::size_t size;
	const unsigned char *begin;
	const unsigned char *end;
	std::string path; // where the assembler read it from
};

#define REDSHIFT_LIST_FILE(name, size, sha256, stem)                                                                   \
	{name, size, redshift_driver_##stem##_begin, redshift_driver_##stem##_end, ""},
#define REDSHIFT_LIST_PATH(stem, path) paths.push_back(path);
const std::vector<EmbeddedFile> &EmbeddedFiles() {
	static const std::vector<EmbeddedFile> files = [] {
		std::vector<EmbeddedFile> result {REDSHIFT_DRIVER_FILES(REDSHIFT_LIST_FILE)};
		std::vector<std::string> paths;
		REDSHIFT_DRIVER_EMBED_FILES(REDSHIFT_LIST_PATH)
		REQUIRE(paths.size() == result.size());
		for (std::size_t i = 0; i < result.size(); i++) {
			result[i].path = paths[i];
		}
		return result;
	}();
	return files;
}

std::vector<unsigned char> ReadFile(const std::string &path) {
	std::ifstream in(path, std::ios::binary);
	REQUIRE(in.good());
	return std::vector<unsigned char>(std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>());
}

} // namespace

TEST_CASE("The embedded set is the driver, then the three notices", "[driver][embed]") {
	const auto &files = EmbeddedFiles();
	REQUIRE(files.size() == 4);
#if defined(__APPLE__)
	CHECK(files[0].name == "librsodbc64.dylib");
#else
	CHECK(files[0].name == "librsodbc64.so");
#endif
	CHECK(files[1].name == "LICENSE");
	CHECK(files[2].name == "NOTICE");
	CHECK(files[3].name == "THIRD_PARTY_LICENSES");
}

TEST_CASE("Every embedded file has the size driver_info.h records and the bytes at its path", "[driver][embed]") {
	for (const auto &file : EmbeddedFiles()) {
		INFO(file.name);
		CHECK(static_cast<std::size_t>(file.end - file.begin) == file.size);
		// Each file starts on the 64-byte boundary both embed sources give it (.balign 64 for ELF, .p2align 6 for
		// Mach-O).
		CHECK(reinterpret_cast<std::uintptr_t>(file.begin) % 64 == 0);
		// Absolute, or the assembler would look for the name in its working directory first.
		REQUIRE(!file.path.empty());
		CHECK(file.path.front() == '/');
		CHECK(file.path.size() > file.name.size());
		CHECK(file.path.compare(file.path.size() - file.name.size(), file.name.size(), file.name) == 0);
		const auto checked = ReadFile(file.path);
		REQUIRE(checked.size() == file.size);
		CHECK(std::vector<unsigned char>(file.begin, file.end) == checked);
	}
}

#if defined(__APPLE__)
TEST_CASE("The embedded driver is the universal Mach-O file, with exactly the x86_64 and arm64 slices",
          "[driver][embed]") {
	// Both macOS builds embed the one signed universal file unchanged.
	const auto &driver = EmbeddedFiles().at(0);
	REQUIRE(driver.size > 8);
	CHECK(redshift_driver_test::BigEndian32(driver.begin) == FAT_MAGIC);
	auto types = redshift_driver_test::MachOCpuTypes(driver.begin, driver.size);
	std::sort(types.begin(), types.end());
	const std::vector<uint32_t> both {static_cast<uint32_t>(CPU_TYPE_X86_64), static_cast<uint32_t>(CPU_TYPE_ARM64)};
	CHECK(types == both);
	// The platform driver_info.h names is the architecture this test was compiled for.
#if defined(__x86_64__)
	CHECK(std::strcmp(REDSHIFT_DRIVER_PLATFORM, "osx_amd64") == 0);
#elif defined(__arm64__)
	CHECK(std::strcmp(REDSHIFT_DRIVER_PLATFORM, "osx_arm64") == 0);
#endif
}
#else
TEST_CASE("The embedded driver is the platform's ELF file", "[driver][embed]") {
	const auto &driver = EmbeddedFiles().at(0);
	REQUIRE(driver.size > 20);
	const unsigned char elf_magic[] = {0x7f, 'E', 'L', 'F'};
	CHECK(std::equal(elf_magic, elf_magic + sizeof(elf_magic), driver.begin));
	// The architecture this test, and so the extension, was compiled for, not the platform's label.
#if defined(__x86_64__)
	const unsigned compiled_for = 62; // EM_X86_64
#elif defined(__aarch64__)
	const unsigned compiled_for = 183; // EM_AARCH64
#else
	const unsigned compiled_for = 0;
#endif
	const unsigned machine = driver.begin[18] | (driver.begin[19] << 8);
	CHECK(machine == compiled_for);
}
#endif
