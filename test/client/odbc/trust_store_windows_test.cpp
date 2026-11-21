// ResolveTrustSource and the trust file on Windows, against the real stores and files: the checks
// of SSL_CERT_FILE, the trust file's access list and holding, and a connect through the export.

// catch.hpp comes before any DuckDB header, as in this directory's other tests. The Windows SDK
// headers come next, before any project or DuckDB header: odbc_driver.hpp removes Win32 macros,
// such as OPTIONAL, that aclapi.h's declarations need.
#include "catch.hpp"

#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef _WINSOCKAPI_
#define _WINSOCKAPI_ // as DuckDB's windows.hpp and odbc_driver.hpp read windows.h
#endif
#include <windows.h>

#include <aclapi.h>
#include <bcrypt.h>
#include <sddl.h>
#include <wincrypt.h>
#include <winioctl.h>

#include "duckdb/common/string_util.hpp"
#include "fake_driver_fixture.hpp"
#include "odbc/trust_store_export.hpp"
#include "thrown.hpp"

#include <algorithm>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <future>
#include <memory>
#include <random>

#pragma comment(lib, "bcrypt.lib")

using namespace duckdb;
using duckdb::redshift_driver::DriverErrorKind;
using duckdb::redshift_driver::testing::FakeDiag;
using duckdb::redshift_driver::testing::FakeGate;
using duckdb::redshift_driver::testing::ScopedEnvironmentVariable;
using redshift_test::Capture;
using redshift_test::Thrown;

namespace {

const std::string BEGIN = "-----BEGIN CERTIFICATE-----\n";
const std::string NAME = std::string(64, 'a') + ".pem";
const std::string CONTENT = BEGIN + "AAEC\n-----END CERTIFICATE-----\n";

//! A new folder in the user's temporary directory, removed with what it holds.
class TemporaryFolder {
public:
	TemporaryFolder() {
		std::random_device random;
		path = std::filesystem::temp_directory_path() /
		       ("redshift-trust-" + std::to_string(random()) + std::to_string(random()));
		REQUIRE(std::filesystem::create_directory(path));
		path = std::filesystem::canonical(path);
	}
	~TemporaryFolder() {
		std::error_code ignored;
		std::filesystem::remove_all(path, ignored);
	}
	std::string Utf8(const std::wstring &name = std::wstring()) const {
		return *WideToUtf8((name.empty() ? path : path / name).wstring());
	}
	std::string File(const std::wstring &name, const std::string &content) const {
		std::ofstream(path / name, std::ios::binary) << content;
		return Utf8(name);
	}
	std::vector<std::wstring> Names() const {
		std::vector<std::wstring> names;
		for (const auto &entry : std::filesystem::directory_iterator(path)) {
			names.push_back(entry.path().filename().wstring());
		}
		return names;
	}

	std::filesystem::path path;
};

std::string Resolved(const char *ssl_cert_file, redshift_test::CapturingLogSink &log_sink) {
	ScopedEnvironmentVariable variable("SSL_CERT_FILE", ssl_cert_file);
	const auto source = ResolveTrustSource(log_sink);
	return source.path.empty() ? source.export_content : source.path;
}

std::vector<BYTE> TokenUserSid() {
	HANDLE token = nullptr;
	REQUIRE(OpenProcessToken(GetCurrentProcess(), TOKEN_QUERY, &token));
	DWORD size = 0;
	GetTokenInformation(token, TokenUser, nullptr, 0, &size);
	std::vector<BYTE> user(size);
	REQUIRE(GetTokenInformation(token, TokenUser, user.data(), size, &size));
	CloseHandle(token);
	const PSID sid = reinterpret_cast<TOKEN_USER *>(user.data())->User.Sid;
	return std::vector<BYTE>(static_cast<BYTE *>(sid), static_cast<BYTE *>(sid) + GetLengthSid(sid));
}

//! Sets `path`'s last write eight days back, past the pruning age.
void MakeStale(const std::filesystem::path &path) {
	const HANDLE file =
	    CreateFileW(path.c_str(), FILE_WRITE_ATTRIBUTES, FILE_SHARE_READ, nullptr, OPEN_EXISTING, 0, nullptr);
	REQUIRE(file != INVALID_HANDLE_VALUE);
	FILETIME now;
	GetSystemTimeAsFileTime(&now);
	ULARGE_INTEGER time;
	time.LowPart = now.dwLowDateTime;
	time.HighPart = now.dwHighDateTime;
	time.QuadPart -= 8ULL * 24 * 3600 * 10000000;
	FILETIME old {time.LowPart, time.HighPart};
	CHECK(SetFileTime(file, nullptr, nullptr, &old));
	CloseHandle(file);
}

//! The SHA-256 of `bytes` in lower-case hex, from CNG, apart from the code under test.
std::string Sha256Hex(const std::string &bytes) {
	BCRYPT_ALG_HANDLE algorithm = nullptr;
	REQUIRE(BCryptOpenAlgorithmProvider(&algorithm, BCRYPT_SHA256_ALGORITHM, nullptr, 0) == 0);
	BCRYPT_HASH_HANDLE hash = nullptr;
	UCHAR digest[32];
	const bool hashed = BCryptCreateHash(algorithm, &hash, nullptr, 0, nullptr, 0, 0) == 0 &&
	                    BCryptHashData(hash, reinterpret_cast<PUCHAR>(const_cast<char *>(bytes.data())),
	                                   static_cast<ULONG>(bytes.size()), 0) == 0 &&
	                    BCryptFinishHash(hash, digest, sizeof(digest), 0) == 0;
	if (hash != nullptr) {
		BCryptDestroyHash(hash);
	}
	BCryptCloseAlgorithmProvider(algorithm, 0);
	REQUIRE(hashed);
	static const char HEX[] = "0123456789abcdef";
	std::string hex;
	for (auto byte : digest) {
		hex += HEX[byte >> 4];
		hex += HEX[byte & 15];
	}
	return hex;
}

std::string Base64(PCCERT_CONTEXT certificate) {
	DWORD length = 0;
	CryptBinaryToStringA(certificate->pbCertEncoded, certificate->cbCertEncoded,
	                     CRYPT_STRING_BASE64 | CRYPT_STRING_NOCRLF, nullptr, &length);
	std::string base64(length, '\0');
	CryptBinaryToStringA(certificate->pbCertEncoded, certificate->cbCertEncoded,
	                     CRYPT_STRING_BASE64 | CRYPT_STRING_NOCRLF, &base64[0], &length);
	base64.resize(length);
	return base64;
}

const std::string NOT_OURS = "The Redshift trust folder already holds " + NAME +
                             ", but it is not the file this extension writes: its content, owner or access list "
                             "differs, so it is not used. Delete it to let the extension write it again, or set "
                             "SSL_CERT_FILE to a readable file of PEM CA certificates";

const std::string FOLDER_FIX =
    "Set REDSHIFT_DRIVER_DIR to a folder you can write whose path is ASCII, has no leading or trailing space, holds "
    "no ASCII control character or '}', and leaves the trust file's path within 255 bytes, or set SSL_CERT_FILE to a "
    "readable file of PEM CA certificates";

//! How long a case waits for a call it expects. It only bounds how long a regression takes to fail.
const std::chrono::milliseconds EXPECTED_WAIT(10000);

//! What a connect did while its SQLDriverConnect was held.
struct HeldConnect {
	bool held;
	bool delete_refused;
	bool opened;
};

//! Connects with the fake's SQLDriverConnect call `held_call` held, and tries to delete the trust
//! file, the folder's one file, while it is. Releases the gate before any assert, so no case hangs.
HeldConnect ConnectWhileHeld(redshift_test::FakeDriverFixture &fixture, const TemporaryFolder &folder, int held_call) {
	auto gate = std::make_shared<FakeGate>();
	fixture.fake.BlockCall("SQLDriverConnect", held_call, gate);
	auto connect = std::async(std::launch::async, [&fixture]() { return fixture.OpenConnection(); });
	HeldConnect answers {};
	answers.held = gate->WaitUntilHeldFor(EXPECTED_WAIT);
	const auto names = folder.Names();
	answers.delete_refused = answers.held && names.size() == 1 && !DeleteFileW((folder.path / names[0]).c_str()) &&
	                         GetLastError() == ERROR_SHARING_VIOLATION;
	gate->Release();
	answers.opened = redshift_test::ReadyWithin(connect, EXPECTED_WAIT) && connect.get() != nullptr;
	return answers;
}

} // namespace

TEST_CASE("On Windows, an SSL_CERT_FILE naming a directory or an empty file is refused", "[odbc]") {
	TemporaryFolder folder;
	redshift_test::CapturingLogSink log_sink;
	const Thrown refused {ExceptionType::IO,
	                      "SSL_CERT_FILE names an empty file, a directory or a special file such as a device, so it "
	                      "holds no CA certificates to verify the Redshift connection against. Set SSL_CERT_FILE to a "
	                      "file of PEM CA certificates, or unset it to use the system CA certificates"};
	CHECK(Capture([&] { Resolved(folder.Utf8().c_str(), log_sink); }) == refused);
	CHECK(Capture([&] { Resolved(folder.File(L"empty.pem", "").c_str(), log_sink); }) == refused);
}

TEST_CASE("On Windows, an SSL_CERT_FILE another program holds unshared is refused, not passed over", "[odbc]") {
	TemporaryFolder folder;
	const auto path = folder.File(L"held.pem", CONTENT);
	const HANDLE held =
	    CreateFileW((folder.path / L"held.pem").c_str(), GENERIC_READ, 0, nullptr, OPEN_EXISTING, 0, nullptr);
	REQUIRE(held != INVALID_HANDLE_VALUE);
	redshift_test::CapturingLogSink log_sink;
	const auto thrown = Capture([&] { Resolved(path.c_str(), log_sink); });
	CloseHandle(held);
	CHECK(
	    thrown ==
	    Thrown {ExceptionType::IO,
	            "SSL_CERT_FILE names a file that could not be checked or opened: it may be on an unreachable drive "
	            "or share, have a name Windows refuses, or be held open by another program, such as a virus "
	            "scanner. Try again once it can be opened, or unset SSL_CERT_FILE to use the system CA certificates"});
}

TEST_CASE("On Windows, an SSL_CERT_FILE naming no file is passed over to the root store, with a log line", "[odbc]") {
	TemporaryFolder folder;
	redshift_test::CapturingLogSink log_sink;
	CHECK(StringUtil::StartsWith(Resolved(folder.Utf8(L"missing.pem").c_str(), log_sink), BEGIN));
	CHECK(log_sink.Lines() ==
	      std::vector<std::string> {
	          "SSL_CERT_FILE is set but does not name a readable file; using the Windows root store"});
}

TEST_CASE("On Windows, an SSL_CERT_FILE the user may not read, or under no folder, is passed over", "[odbc]") {
	TemporaryFolder folder;
	const auto denied = folder.File(L"denied.pem", CONTENT);
	PSECURITY_DESCRIPTOR descriptor = nullptr;
	// Read data denied to everyone; the owner keeps the right to change the list back.
	REQUIRE(ConvertStringSecurityDescriptorToSecurityDescriptorW(L"D:P(D;;0x1;;;WD)(A;;FA;;;WD)", SDDL_REVISION_1,
	                                                             &descriptor, nullptr));
	BOOL present = FALSE, defaulted = FALSE;
	PACL dacl = nullptr;
	REQUIRE(GetSecurityDescriptorDacl(descriptor, &present, &dacl, &defaulted));
	REQUIRE(SetNamedSecurityInfoW(const_cast<wchar_t *>((folder.path / L"denied.pem").c_str()), SE_FILE_OBJECT,
	                              DACL_SECURITY_INFORMATION | PROTECTED_DACL_SECURITY_INFORMATION, nullptr, nullptr,
	                              dacl, nullptr) == ERROR_SUCCESS);
	LocalFree(descriptor);
	const std::vector<std::string> passed_over = {
	    "SSL_CERT_FILE is set but does not name a readable file; using the Windows root store"};
	redshift_test::CapturingLogSink denied_log;
	CHECK(StringUtil::StartsWith(Resolved(denied.c_str(), denied_log), BEGIN));
	CHECK(denied_log.Lines() == passed_over);
	redshift_test::CapturingLogSink no_folder_log;
	CHECK(StringUtil::StartsWith(Resolved(folder.Utf8(L"no-such-folder\\x.pem").c_str(), no_folder_log), BEGIN));
	CHECK(no_folder_log.Lines() == passed_over);
}

TEST_CASE("On Windows, a non-ASCII SSL_CERT_FILE is refused naming the variable, not the path", "[odbc]") {
	TemporaryFolder folder;
	const auto path = folder.File(L"caf\u00e9.pem", CONTENT);
	redshift_test::CapturingLogSink log_sink;
	CHECK(Capture([&] { Resolved(path.c_str(), log_sink); }) ==
	      Thrown {ExceptionType::IO,
	              "SSL_CERT_FILE names a CA bundle whose path holds a character outside ASCII, and on Windows the "
	              "driver needs an ASCII path, so the Redshift driver cannot be given it. Move the bundle to a path of "
	              "at most 255 bytes, with no leading or trailing space and without an ASCII control character or "
	              "'}', or unset SSL_CERT_FILE to use the system CA certificates"});
}

TEST_CASE("On Windows, the root store's export is the same at every read", "[odbc]") {
	redshift_test::CapturingLogSink log_sink;
	const auto first = Resolved(nullptr, log_sink);
	CHECK(StringUtil::StartsWith(first, BEGIN));
	CHECK(Resolved(nullptr, log_sink) == first);
}

TEST_CASE("On Windows, a new trust file grants only its owner, the user, whatever its folder inherits", "[odbc]") {
	TemporaryFolder folder;
	PSECURITY_DESCRIPTOR inheritable = nullptr;
	REQUIRE(ConvertStringSecurityDescriptorToSecurityDescriptorW(L"D:(A;OICI;FA;;;WD)", SDDL_REVISION_1, &inheritable,
	                                                             nullptr));
	SECURITY_ATTRIBUTES attributes {sizeof(attributes), inheritable, FALSE};
	const auto directory = folder.path / L"inherits";
	REQUIRE(CreateDirectoryW(directory.c_str(), &attributes));
	LocalFree(inheritable);

	auto files = MakeWindowsTrustFiles();
	auto placed = PlaceTrustExport(*files, *WideToUtf8(directory.wstring()), NAME, CONTENT);
	PSID owner = nullptr;
	PACL acl = nullptr;
	PSECURITY_DESCRIPTOR descriptor = nullptr;
	REQUIRE(GetNamedSecurityInfoW((directory / NAME).c_str(), SE_FILE_OBJECT,
	                              OWNER_SECURITY_INFORMATION | DACL_SECURITY_INFORMATION, &owner, nullptr, &acl,
	                              nullptr, &descriptor) == ERROR_SUCCESS);
	auto user = TokenUserSid();
	SECURITY_DESCRIPTOR_CONTROL control = 0;
	DWORD revision = 0;
	void *entry = nullptr;
	CHECK(EqualSid(owner, user.data()));
	CHECK(GetSecurityDescriptorControl(descriptor, &control, &revision));
	CHECK((control & SE_DACL_PROTECTED) != 0);
	REQUIRE(acl->AceCount == 1);
	REQUIRE(GetAce(acl, 0, &entry));
	CHECK(EqualSid(&static_cast<ACCESS_ALLOWED_ACE *>(entry)->SidStart, user.data()));
	LocalFree(descriptor);

	// Held, so it cannot be deleted before the driver has read it.
	CHECK_FALSE(DeleteFileW((directory / NAME).c_str()));
	CHECK(GetLastError() == ERROR_SHARING_VIOLATION);
	placed.held.reset();
	CHECK(DeleteFileW((directory / NAME).c_str()));
}

TEST_CASE("On Windows, a trust file of the name with the folder's default access list is refused", "[odbc]") {
	TemporaryFolder folder;
	folder.File(std::wstring(NAME.begin(), NAME.end()), CONTENT);
	auto files = MakeWindowsTrustFiles();
	CHECK(Capture([&] { PlaceTrustExport(*files, folder.Utf8(), NAME, CONTENT); }) ==
	      Thrown {ExceptionType::IO, NOT_OURS});
}

TEST_CASE("On Windows, a trust file of the name over 64 MiB is refused unread, even with the owner-only list",
          "[odbc]") {
	TemporaryFolder folder;
	auto files = MakeWindowsTrustFiles();
	PlaceTrustExport(*files, folder.Utf8(), NAME, CONTENT).held.reset();
	// Sparse, so the size costs no disk; the owner-only list it was written with is kept.
	const auto path = folder.path / NAME;
	const HANDLE file = CreateFileW(path.c_str(), GENERIC_WRITE, 0, nullptr, OPEN_EXISTING, 0, nullptr);
	REQUIRE(file != INVALID_HANDLE_VALUE);
	DWORD returned = 0;
	LARGE_INTEGER end;
	end.QuadPart = 64LL * 1024 * 1024 + 1;
	const bool grown = DeviceIoControl(file, FSCTL_SET_SPARSE, nullptr, 0, nullptr, 0, &returned, nullptr) &&
	                   SetFilePointerEx(file, end, nullptr, FILE_BEGIN) && SetEndOfFile(file);
	CloseHandle(file);
	REQUIRE(grown);

	auto opened = files->Open(*WideToUtf8(path.wstring()));
	CHECK(opened.status.outcome == TrustFileOutcome::DONE);
	CHECK_FALSE(opened.owner_only);
	CHECK(opened.content.empty());
	opened.held.reset();
	CHECK(Capture([&] { PlaceTrustExport(*files, folder.Utf8(), NAME, CONTENT); }) ==
	      Thrown {ExceptionType::IO, NOT_OURS});
}

TEST_CASE("On Windows, a new trust file prunes a stale temporary file but not a held export", "[odbc]") {
	TemporaryFolder folder;
	const std::wstring old_export(64, L'b');
	folder.File(old_export + L".pem", CONTENT);
	folder.File(L"tmp-1-1.tmp", CONTENT);
	MakeStale(folder.path / (old_export + L".pem"));
	MakeStale(folder.path / L"tmp-1-1.tmp");
	const HANDLE held = CreateFileW((folder.path / (old_export + L".pem")).c_str(), GENERIC_READ, FILE_SHARE_READ,
	                                nullptr, OPEN_EXISTING, 0, nullptr);
	REQUIRE(held != INVALID_HANDLE_VALUE);
	auto files = MakeWindowsTrustFiles();
	PlaceTrustExport(*files, folder.Utf8(), NAME, CONTENT);
	CloseHandle(held);
	auto names = folder.Names();
	std::sort(names.begin(), names.end());
	CHECK(names == std::vector<std::wstring> {std::wstring(NAME.begin(), NAME.end()), old_export + L".pem"});
}

TEST_CASE("On Windows, a new trust file keeps a temporary file and an export written within the week", "[odbc]") {
	TemporaryFolder folder;
	const std::wstring recent_export(64, L'b');
	folder.File(recent_export + L".pem", CONTENT);
	folder.File(L"tmp-2-1.tmp", CONTENT);
	auto files = MakeWindowsTrustFiles();
	PlaceTrustExport(*files, folder.Utf8(), NAME, CONTENT);
	auto names = folder.Names();
	std::sort(names.begin(), names.end());
	CHECK(names ==
	      std::vector<std::wstring> {std::wstring(NAME.begin(), NAME.end()), recent_export + L".pem", L"tmp-2-1.tmp"});
}

TEST_CASE("On Windows, a connect without SSL_CERT_FILE gives the driver the export as both trust options", "[odbc]") {
	TemporaryFolder folder;
	redshift_test::FakeDriverFixture fixture;
	ScopedEnvironmentVariable unset("SSL_CERT_FILE", nullptr);
	fixture.fake.SetTrustDirectory(folder.Utf8());
	fixture.OpenConnection();
	fixture.OpenConnection();

	// The second connect found the first one's file and wrote nothing.
	const auto names = folder.Names();
	REQUIRE(names.size() == 1);
	const auto path = folder.Utf8(names[0]);
	const auto text = fixture.fake.LastConnectionString();
	CHECK(text.find("TrustStore={" + path + "};") != std::string::npos);
	CHECK(text.find("CaFile={" + path + "};") != std::string::npos);
	// Named by the SHA-256 of its bytes.
	std::ifstream in(folder.path / names[0], std::ios::binary);
	const std::string bytes((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
	const auto expected = Sha256Hex(bytes) + ".pem";
	CHECK(names[0] == std::wstring(expected.begin(), expected.end()));
}

TEST_CASE("On Windows, a connect whose driver gives no trust folder is refused after the driver source", "[odbc]") {
	redshift_test::FakeDriverFixture fixture;
	ScopedEnvironmentVariable unset("SSL_CERT_FILE", nullptr);
	CHECK(Capture([&] { fixture.OpenConnection(); }) ==
	      Thrown {ExceptionType::IO, "No folder could hold the Redshift trust file, the CA certificates exported "
	                                 "from the Windows root store: neither the driver's folder nor the per-user cache "
	                                 "could have one. " +
	                                     FOLDER_FIX});
	CHECK(fixture.fake.SourceCalls() == 1);
	CHECK(fixture.CallsTo("SQLDriverConnect") == 0);
}

TEST_CASE("On Windows, a non-ASCII trust folder is refused naming REDSHIFT_DRIVER_DIR, not the path", "[odbc]") {
	TemporaryFolder folder;
	const auto non_ascii = folder.path / L"caf\u00e9";
	REQUIRE(CreateDirectoryW(non_ascii.c_str(), nullptr));
	redshift_test::FakeDriverFixture fixture;
	ScopedEnvironmentVariable unset("SSL_CERT_FILE", nullptr);
	fixture.fake.SetTrustDirectory(folder.Utf8(L"caf\u00e9"));
	CHECK(Capture([&] { fixture.OpenConnection(); }) ==
	      Thrown {ExceptionType::IO, "The path of the Redshift trust file, the CA certificates exported from the "
	                                 "Windows root store, holds a character outside ASCII, and on Windows the driver "
	                                 "needs an ASCII path, so the driver cannot be given it. " +
	                                     FOLDER_FIX});
	CHECK(fixture.CallsTo("SQLDriverConnect") == 0);
	CHECK(std::filesystem::is_empty(non_ascii));
}

TEST_CASE("On Windows, the trust file cannot be deleted while the driver connects with it", "[odbc]") {
	TemporaryFolder folder;
	redshift_test::FakeDriverFixture fixture;
	ScopedEnvironmentVariable unset("SSL_CERT_FILE", nullptr);
	fixture.fake.SetTrustDirectory(folder.Utf8());
	const auto connect = ConnectWhileHeld(fixture, folder, 1);
	CHECK(connect.held);
	CHECK(connect.delete_refused);
	CHECK(connect.opened);
	// Let go once Open returns, so the refusal above was the connect's hold.
	const auto names = folder.Names();
	REQUIRE(names.size() == 1);
	CHECK(DeleteFileW((folder.path / names[0]).c_str()));
}

TEST_CASE("On Windows, the trust file stays held from a timed-out attempt into the next one", "[odbc]") {
	TemporaryFolder folder;
	redshift_test::FakeDriverFixture fixture;
	ScopedEnvironmentVariable unset("SSL_CERT_FILE", nullptr);
	fixture.fake.SetTrustDirectory(folder.Utf8());
	// A resume timeout, so the connect frees the handle and tries again.
	fixture.fake.FailCall("SQLDriverConnect", 1, SQL_ERROR, {FakeDiag {"HYT00", 0, "Timeout expired", {}}});
	const auto connect = ConnectWhileHeld(fixture, folder, 2);
	CHECK(connect.held);
	CHECK(connect.delete_refused);
	CHECK(connect.opened);
	CHECK(fixture.CallsTo("SQLDriverConnect") == 2);
}

TEST_CASE("On Windows, a driver source that fails leaves the trust folder empty", "[odbc]") {
	TemporaryFolder folder;
	redshift_test::FakeDriverFixture fixture;
	ScopedEnvironmentVariable unset("SSL_CERT_FILE", nullptr);
	fixture.fake.SetTrustDirectory(folder.Utf8());
	fixture.fake.FailLoad(DriverErrorKind::NOT_BUILT, "no driver");
	CHECK(Capture([&] { fixture.OpenConnection(); }) ==
	      Thrown {ExceptionType::IO, "the Redshift driver could not be loaded: no driver"});
	CHECK(folder.Names().empty());
}

TEST_CASE("On Windows, the endpoints' root CAs are in the store and survive the export", "[odbc]") {
	// The SHA-1 thumbprints, in hex and comma-separated, of the root CAs the Redshift endpoints chain to.
	const char *recorded = std::getenv("REDSHIFT_TEST_ENDPOINT_ROOTS");
	if (recorded == nullptr) {
		SKIP_TEST("skipped: REDSHIFT_TEST_ENDPOINT_ROOTS is unset");
		return;
	}
	redshift_test::CapturingLogSink log_sink;
	auto exported = Resolved(nullptr, log_sink);
	exported.erase(std::remove(exported.begin(), exported.end(), '\n'), exported.end());
	const HCERTSTORE store = CertOpenStore(CERT_STORE_PROV_SYSTEM_W, 0, 0,
	                                       CERT_SYSTEM_STORE_CURRENT_USER | CERT_STORE_READONLY_FLAG, L"ROOT");
	REQUIRE(store != nullptr);
	for (const auto &thumbprint : StringUtil::Split(recorded, ',')) {
		INFO(thumbprint);
		BYTE hash[20];
		DWORD size = sizeof(hash);
		REQUIRE(CryptStringToBinaryA(thumbprint.c_str(), 0, CRYPT_STRING_HEX, hash, &size, nullptr, nullptr));
		CRYPT_HASH_BLOB blob {size, hash};
		const auto root = CertFindCertificateInStore(store, X509_ASN_ENCODING, 0, CERT_FIND_SHA1_HASH, &blob, nullptr);
		REQUIRE(root != nullptr);
		std::string properties;
		for (DWORD id = CertEnumCertificateContextProperties(root, 0); id != 0;
		     id = CertEnumCertificateContextProperties(root, id)) {
			properties += std::to_string(id) + " ";
		}
		WARN("properties " + properties);
		CHECK(exported.find(Base64(root)) != std::string::npos);
		CertFreeCertificateContext(root);
	}
	CertCloseStore(store, 0);
}
