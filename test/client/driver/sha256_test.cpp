// SHA-256 of memory and of files, against the FIPS 180-4 examples and inputs
// at the padding boundaries.
#include "sha256.hpp"
#include "temp_dir.hpp"

#include "catch.hpp"

#include <cstdio>
#include <cstring>
#include <fstream>
#include <string>
#include <thread>

#include <dlfcn.h>
#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>

using duckdb::redshift_driver::detail::ErrnoMessage;
using duckdb::redshift_driver::detail::ErrnoMessageFrom;
using duckdb::redshift_driver::detail::Sha256;
using duckdb::redshift_driver::detail::Sha256File;
using duckdb::redshift_driver::detail::Sha256Hex;
using redshift_driver_test::TempDir;

namespace {

std::string Hex(const std::string &data) {
	return Sha256Hex(data.data(), data.size());
}

void WriteFile(const std::string &path, const std::string &data) {
	std::ofstream out(path, std::ios::binary);
	out << data;
	REQUIRE(out.good());
}

// Stand-ins of each strerror_r form: GNU returns its text, XSI writes it and returns a status.
char *GnuStrerror(int, char *, size_t) {
	static char text[] = "text the GNU form returns";
	return text;
}
int XsiStrerror(int, char *buffer, size_t size) {
	std::snprintf(buffer, size, "text the XSI form writes");
	return 0;
}
int XsiStrerrorFailing(int, char *, size_t) {
	return EINVAL;
}

// Stands in for the C library's strerror in this executable: while armed it returns a sentinel, so
// a test can tell whether ErrnoMessage calls it; otherwise it is the C library's.
bool g_strerror_armed = false;
char g_strerror_sentinel[] = "strerror was called";

} // namespace

// The C library's own exception specification, which a redeclaration must repeat: glibc declares strerror
// __THROW (noexcept in C++), Apple's SDK and musl with none.
#if defined(__GLIBC__)
#define REDSHIFT_STRERROR_NOEXCEPT noexcept
#else
#define REDSHIFT_STRERROR_NOEXCEPT
#endif

extern "C" char *strerror(int error_number) REDSHIFT_STRERROR_NOEXCEPT {
	using Strerror = char *(*)(int);
	static const auto real = reinterpret_cast<Strerror>(::dlsym(RTLD_NEXT, "strerror"));
	return g_strerror_armed ? g_strerror_sentinel : real(error_number);
}

TEST_CASE("SHA-256 matches the FIPS 180-4 examples", "[driver][sha256]") {
	CHECK(Hex("") == "e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855");
	CHECK(Hex("abc") == "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad");
	CHECK(Hex("abcdbcdecdefdefgefghfghighijhijkijkljklmklmnlmnomnopnopq") ==
	      "248d6a61d20638b8e5c026930c3e6039a33ce45964ff2167f6ecedd419db06c1");
	CHECK(Hex("abcdefghbcdefghicdefghijdefghijkefghijklfghijklmghijklmnhijklmnoijklmnopjklmnopqklmnopqrlmnopqrsmnopqrst"
	          "nopqrstu") == "cf5b16a778af8380036ce59e7b0492370b249b11e8f07a51afac45037afee9d1");
	CHECK(Hex(std::string(1000000, 'a')) == "cdc76e5c9914fb9281a1c7e284d73e67f1809a48a497200e046d39ccc7112cd0");
}

TEST_CASE("SHA-256 pads correctly at every block boundary", "[driver][sha256]") {
	// Lengths around 56 and 64 bytes, where the length field moves into a second block.
	CHECK(Hex(std::string(55, 'a')) == "9f4390f8d30c2dd92ec9f095b65e2b9ae9b0a925a5258e241c9f1e910f734318");
	CHECK(Hex(std::string(56, 'a')) == "b35439a4ac6f0948b6d6f9e3c6af0f5f590ce20f1bde7090ef7970686ec6738a");
	CHECK(Hex(std::string(63, 'a')) == "7d3e74a05d7db15bce4ad9ec0658ea98e3f06eeecf16b4c6fff2da457ddc2f34");
	CHECK(Hex(std::string(64, 'a')) == "ffe054fe7ae0cb6dc65c3af9b61d5209f439851db43d0ba5997337df154668eb");
	CHECK(Hex(std::string(65, 'a')) == "635361c48bb9eab14198e76ea8ab7f1a41685d6ad62aa9146d301d4f17eb0ae0");
	CHECK(Hex(std::string(119, 'a')) == "31eba51c313a5c08226adf18d4a359cfdfd8d2e816b13f4af952f7ea6584dcfb");
	CHECK(Hex(std::string(120, 'a')) == "2f3d335432c70b580af0e8e1b3674a7c020d683aa5f73aaaedfdc55af904c21c");
}

TEST_CASE("SHA-256 gives the same digest however the input is split", "[driver][sha256]") {
	std::string data;
	for (int i = 0; i < 1000; i++) {
		data += static_cast<char>(i * 7);
	}
	const auto whole = Hex(data);
	for (size_t piece : {1, 3, 55, 63, 64, 65, 200}) {
		Sha256 hash;
		for (size_t at = 0; at < data.size(); at += piece) {
			hash.Update(data.data() + at, std::min(piece, data.size() - at));
		}
		CHECK(hash.FinishHex() == whole);
	}
	// FinishHex resets the object for another digest.
	Sha256 hash;
	hash.Update("abc", 3);
	hash.FinishHex();
	CHECK(hash.FinishHex() == Hex(""));
}

TEST_CASE("A file's SHA-256 equals its bytes' SHA-256", "[driver][sha256]") {
	TempDir dir;
	// Larger than the 64 KiB read buffer, and not a multiple of it.
	std::string data(200000, 'x');
	data[12345] = '\0';
	WriteFile(dir / "file", data);
	std::string hex, error;
	REQUIRE(Sha256File(dir / "file", hex, error));
	CHECK(hex == Hex(data));
	WriteFile(dir / "empty", "");
	REQUIRE(Sha256File(dir / "empty", hex, error));
	CHECK(hex == Hex(""));
}

TEST_CASE("A file is hashed only if it is a regular file reached without a symbolic link", "[driver][sha256]") {
	TempDir dir;
	WriteFile(dir / "target", "abc");
	REQUIRE(::symlink((dir / "target").c_str(), (dir / "link").c_str()) == 0);
	REQUIRE(::mkfifo((dir / "fifo").c_str(), 0600) == 0);
	REQUIRE(::mkdir((dir / "subdir").c_str(), 0700) == 0);

	std::string hex = "unchanged", error;
	CHECK_FALSE(Sha256File(dir / "link", hex, error));
	CHECK(error == "is a symbolic link");
	// A FIFO with no writer would block a plain open; this one fails at once.
	CHECK_FALSE(Sha256File(dir / "fifo", hex, error));
	CHECK(error == "is not a regular file");
	CHECK_FALSE(Sha256File(dir / "subdir", hex, error));
	CHECK(error == "is not a regular file");
	CHECK_FALSE(Sha256File(dir / "missing", hex, error));
	CHECK(error.find("cannot be opened: ") == 0);
	CHECK(hex == "unchanged");
}

#if defined(__linux__)
TEST_CASE("A file that cannot be read is reported, not hashed in part", "[driver][sha256]") {
	// /proc/self/mem is a regular file whose first page is not mapped, so reading it fails.
	std::string hex = "unchanged", error;
	CHECK_FALSE(Sha256File("/proc/self/mem", hex, error));
	CHECK(error == "cannot be read: Input/output error");
	CHECK(hex == "unchanged");
}
#endif

TEST_CASE("ErrnoMessage takes the text from strerror_r, never strerror", "[driver][sha256]") {
	g_strerror_armed = true;
	const bool interposed = std::strcmp(std::strerror(ENOENT), g_strerror_sentinel) == 0;
	const auto known = ErrnoMessage(ENOENT);
	const auto unknown = ErrnoMessage(100000);
	g_strerror_armed = false;
	REQUIRE(interposed);
	CHECK(known == "No such file or directory");
	CHECK(unknown.find("100000") != std::string::npos);
	CHECK(ErrnoMessage(EACCES) == "Permission denied");
}

TEST_CASE("ErrnoMessage reads either strerror_r form", "[driver][sha256]") {
	// Each platform compiles one form, so the other is reached only through a stand-in.
	CHECK(ErrnoMessageFrom(GnuStrerror, 5) == "text the GNU form returns");
	CHECK(ErrnoMessageFrom(XsiStrerror, 5) == "text the XSI form writes");
	CHECK(ErrnoMessageFrom(XsiStrerrorFailing, 5) == "error 5");
}

TEST_CASE("ErrnoMessage gives each errno value its own text, from any thread", "[driver][sha256]") {
	// Unknown values are where std::strerror shares one buffer: every thread must get back the text
	// for its own value. Only glibc before 2.32 shares it, so this can fail only there; on later
	// glibc the strerror test above is what fails if ErrnoMessage goes back to strerror.
	std::vector<std::string> texts(16);
	std::vector<std::thread> threads;
	for (size_t i = 0; i < texts.size(); i++) {
		threads.emplace_back([&texts, i] {
			for (int round = 0; round < 1000; round++) {
				texts[i] = ErrnoMessage(100000 + static_cast<int>(i));
			}
		});
	}
	for (auto &thread : threads) {
		thread.join();
	}
	for (size_t i = 0; i < texts.size(); i++) {
		CHECK(texts[i].find(std::to_string(100000 + i)) != std::string::npos);
	}
}
