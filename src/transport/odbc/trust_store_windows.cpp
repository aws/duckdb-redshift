//===----------------------------------------------------------------------===//
//                         DuckDB - Redshift
//
// src/transport/odbc/trust_store_windows.cpp
//
// ResolveTrustSource on Windows: SSL_CERT_FILE, else the current user's ROOT store exported to a PEM
// file, as the database hop takes its trust only as a file.
// The Windows calls only; trust_store_export.cpp holds the rules, which Linux tests too.
//===----------------------------------------------------------------------===//

#include "odbc/trust_store_export.hpp"

#include "duckdb/common/exception.hpp"
#include "odbc/process_environment.hpp"
#include "odbc/trust_store_cert_file.hpp"
#include "odbc/windows_text.hpp"

#include <aclapi.h>
#include <algorithm>
#include <atomic>
#include <cstddef>
#include <memory>
#include <sys/stat.h>
#include <sys/types.h>
#include <wincrypt.h>

#pragma comment(lib, "crypt32.lib")

namespace duckdb {

static_assert(store_property::SHA1_HASH == CERT_SHA1_HASH_PROP_ID, "store property values are wincrypt.h's");
static_assert(store_property::MD5_HASH == CERT_MD5_HASH_PROP_ID, "store property values are wincrypt.h's");
static_assert(store_property::ENHANCED_KEY_USAGE == CERT_ENHKEY_USAGE_PROP_ID,
              "store property values are wincrypt.h's");
static_assert(store_property::FRIENDLY_NAME == CERT_FRIENDLY_NAME_PROP_ID, "store property values are wincrypt.h's");
static_assert(store_property::SIGNATURE_HASH == CERT_SIGNATURE_HASH_PROP_ID, "store property values are wincrypt.h's");
static_assert(store_property::KEY_IDENTIFIER == CERT_KEY_IDENTIFIER_PROP_ID, "store property values are wincrypt.h's");
static_assert(store_property::ISSUER_PUBLIC_KEY_MD5_HASH == CERT_ISSUER_PUBLIC_KEY_MD5_HASH_PROP_ID,
              "store property values are wincrypt.h's");
static_assert(store_property::SUBJECT_PUBLIC_KEY_MD5_HASH == CERT_SUBJECT_PUBLIC_KEY_MD5_HASH_PROP_ID,
              "store property values are wincrypt.h's");
static_assert(store_property::ISSUER_SERIAL_NUMBER_MD5_HASH == CERT_ISSUER_SERIAL_NUMBER_MD5_HASH_PROP_ID,
              "store property values are wincrypt.h's");
static_assert(store_property::SUBJECT_NAME_MD5_HASH == CERT_SUBJECT_NAME_MD5_HASH_PROP_ID,
              "store property values are wincrypt.h's");
static_assert(store_property::AUTH_ROOT_SHA256_HASH == CERT_AUTH_ROOT_SHA256_HASH_PROP_ID,
              "store property values are wincrypt.h's");
static_assert(store_property::SHA256_HASH == CERT_SHA256_HASH_PROP_ID, "store property values are wincrypt.h's");
static_assert(store_property::ROOT_PROGRAM_CERT_POLICIES == CERT_ROOT_PROGRAM_CERT_POLICIES_PROP_ID,
              "store property values are wincrypt.h's");
static_assert(store_property::SIGN_HASH_CNG_ALGORITHM == CERT_SIGN_HASH_CNG_ALG_PROP_ID,
              "store property values are wincrypt.h's");
static_assert(store_property::SUBJECT_PUBLIC_KEY_BIT_LENGTH == CERT_SUBJECT_PUB_KEY_BIT_LENGTH_PROP_ID,
              "store property values are wincrypt.h's");
static_assert(store_property::ISSUER_PUBLIC_KEY_BIT_LENGTH == CERT_ISSUER_PUB_KEY_BIT_LENGTH_PROP_ID,
              "store property values are wincrypt.h's");
static_assert(store_property::NOT_BEFORE_TIME == CERT_NOT_BEFORE_FILETIME_PROP_ID,
              "store property values are wincrypt.h's");
static_assert(store_property::NOT_BEFORE_USAGE == CERT_NOT_BEFORE_ENHKEY_USAGE_PROP_ID,
              "store property values are wincrypt.h's");

static_assert(trust_file_entry::ACCESS_ALLOWED == ACCESS_ALLOWED_ACE_TYPE, "winnt.h's value");
static_assert(trust_file_entry::FULL_CONTROL == FILE_ALL_ACCESS, "winnt.h's value");

namespace {

//! The largest existing trust file read for the comparison; a larger one is not the export.
constexpr LONGLONG MAX_TRUST_FILE_BYTES = 64LL * 1024 * 1024;

//! _wstat64 first, because CreateFileW reports a directory as denied access, which would pass it
//! over. The open is the readability test, as _waccess reads only the read-only attribute.
CertFileState ClassifyCertFile(const string &path) {
	const auto wide = Utf8ToWide(path);
	if (wide.empty()) {
		return CertFileState::UNREADABLE;
	}
	struct _stat64 info;
	const bool found = _wstat64(wide.c_str(), &info) == 0;
	if (found && ((info.st_mode & _S_IFMT) != _S_IFREG || info.st_size <= 0)) {
		return CertFileState::NOT_A_BUNDLE;
	}
	const HANDLE file = CreateFileW(wide.c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
	                                nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
	if (file == INVALID_HANDLE_VALUE) {
		const auto error = GetLastError();
		const bool unreadable =
		    error == ERROR_FILE_NOT_FOUND || error == ERROR_PATH_NOT_FOUND || error == ERROR_ACCESS_DENIED;
		return unreadable ? CertFileState::UNREADABLE : CertFileState::OPEN_FAILED;
	}
	CloseHandle(file);
	// Opened although _wstat64 found nothing, so it changed in between: refused, not guessed at.
	return found ? CertFileState::BUNDLE : CertFileState::OPEN_FAILED;
}

IOException StoreUnreadable(const char *store, DWORD error) {
	return IOException("The Windows %s store could not be read (Windows error %d), so the CA certificates to verify "
	                   "the Redshift connection against are not known. Set %s to a readable file of PEM CA "
	                   "certificates",
	                   store, error, TRUST_VARIABLE);
}

//! The usages of `certificate`'s enhanced key usage property; none when they cannot be read, which
//! leaves it out.
vector<string> ReadUsages(PCCERT_CONTEXT certificate) {
	DWORD size = 0;
	if (!CertGetEnhancedKeyUsage(certificate, CERT_FIND_PROP_ONLY_ENHKEY_USAGE_FLAG, nullptr, &size) || size == 0) {
		return {};
	}
	vector<BYTE> buffer(size);
	auto usage = reinterpret_cast<PCERT_ENHKEY_USAGE>(buffer.data());
	if (!CertGetEnhancedKeyUsage(certificate, CERT_FIND_PROP_ONLY_ENHKEY_USAGE_FLAG, usage, &size)) {
		return {};
	}
	vector<string> usages;
	for (DWORD index = 0; index < usage->cUsageIdentifier; index++) {
		usages.emplace_back(usage->rgpszUsageIdentifier[index]);
	}
	return usages;
}

//! The usages the not-before cut-off withdraws; none when they cannot be read, which leaves it out.
vector<string> ReadNotBeforeUsages(PCCERT_CONTEXT certificate) {
	DWORD size = 0;
	if (!CertGetCertificateContextProperty(certificate, CERT_NOT_BEFORE_ENHKEY_USAGE_PROP_ID, nullptr, &size) ||
	    size == 0) {
		return {};
	}
	vector<BYTE> encoded(size);
	if (!CertGetCertificateContextProperty(certificate, CERT_NOT_BEFORE_ENHKEY_USAGE_PROP_ID, encoded.data(), &size)) {
		return {};
	}
	PCERT_ENHKEY_USAGE decoded = nullptr;
	DWORD decoded_size = 0;
	if (!CryptDecodeObjectEx(X509_ASN_ENCODING, X509_ENHANCED_KEY_USAGE, encoded.data(), size, CRYPT_DECODE_ALLOC_FLAG,
	                         nullptr, &decoded, &decoded_size)) {
		return {};
	}
	const std::unique_ptr<CERT_ENHKEY_USAGE, decltype(&LocalFree)> usage(decoded, &LocalFree);
	vector<string> usages;
	for (DWORD index = 0; index < usage->cUsageIdentifier; index++) {
		usages.emplace_back(usage->rgpszUsageIdentifier[index]);
	}
	return usages;
}

bool ReadCopy(PCCERT_CONTEXT certificate, StoreCertificate &copy) {
	copy.der.assign(reinterpret_cast<const char *>(certificate->pbCertEncoded), certificate->cbCertEncoded);
	for (DWORD property = CertEnumCertificateContextProperties(certificate, 0); property != 0;
	     property = CertEnumCertificateContextProperties(certificate, property)) {
		copy.properties.push_back(property);
	}
	// Read after the properties are listed, since reading the hash can add it to the copy.
	BYTE thumbprint[20];
	DWORD size = sizeof(thumbprint);
	if (!CertGetCertificateContextProperty(certificate, CERT_SHA1_HASH_PROP_ID, thumbprint, &size)) {
		return false;
	}
	copy.thumbprint.assign(reinterpret_cast<const char *>(thumbprint), size);
	const auto &key = certificate->pCertInfo->SubjectPublicKeyInfo.PublicKey;
	copy.public_key.assign(reinterpret_cast<const char *>(key.pbData), key.cbData);
	const auto &properties = copy.properties;
	if (std::find(properties.begin(), properties.end(), store_property::ENHANCED_KEY_USAGE) != properties.end()) {
		copy.enhanced_key_usage = ReadUsages(certificate);
	}
	if (std::find(properties.begin(), properties.end(), store_property::NOT_BEFORE_USAGE) != properties.end()) {
		copy.not_before_usage = ReadNotBeforeUsages(certificate);
	}
	return true;
}

//! Every copy the current user's `store` holds. A read that stops early refuses, since a partial
//! Disallowed store would trust what it left out.
vector<StoreCertificate> ReadStore(const wchar_t *store, const char *name) {
	const HCERTSTORE opened =
	    CertOpenStore(CERT_STORE_PROV_SYSTEM_W, 0, 0, CERT_SYSTEM_STORE_CURRENT_USER | CERT_STORE_READONLY_FLAG, store);
	if (!opened) {
		throw StoreUnreadable(name, GetLastError());
	}
	vector<StoreCertificate> copies;
	PCCERT_CONTEXT certificate = nullptr;
	while ((certificate = CertEnumCertificatesInStore(opened, certificate)) != nullptr) {
		copies.emplace_back();
		if (!ReadCopy(certificate, copies.back())) {
			const auto error = GetLastError();
			CertFreeCertificateContext(certificate);
			CertCloseStore(opened, 0);
			throw StoreUnreadable(name, error);
		}
	}
	const auto error = GetLastError();
	CertCloseStore(opened, 0);
	if (error != static_cast<DWORD>(CRYPT_E_NOT_FOUND) && error != ERROR_NO_MORE_FILES) {
		throw StoreUnreadable(name, error);
	}
	return copies;
}

DistrustedCertificates ReadDistrusted() {
	DistrustedCertificates distrusted;
	for (auto &copy : ReadStore(L"Disallowed", "Disallowed")) {
		distrusted.thumbprints.insert(copy.thumbprint);
		distrusted.public_keys.insert(copy.public_key);
	}
	return distrusted;
}

//! `<SHA-256 of content>.pem`, so equal content always has the one name and is never rewritten.
string ContentName(const string &content) {
	BYTE hash[32];
	DWORD size = sizeof(hash);
	if (!CryptHashCertificate2(L"SHA256", 0, nullptr, reinterpret_cast<const BYTE *>(content.data()),
	                           static_cast<DWORD>(content.size()), hash, &size) ||
	    size != sizeof(hash)) {
		throw IOException("The name of the Redshift trust file could not be computed (Windows error %d)",
		                  GetLastError());
	}
	static const char HEX[] = "0123456789abcdef";
	string name;
	for (const auto value : hash) {
		name += HEX[value >> 4];
		name += HEX[value & 15];
	}
	return name + ".pem";
}

TrustFileStatus StatusOf(DWORD error) {
	switch (error) {
	case ERROR_SUCCESS:
		return {TrustFileOutcome::DONE, error};
	case ERROR_FILE_EXISTS:
	case ERROR_ALREADY_EXISTS:
		return {TrustFileOutcome::EXISTS, error};
	case ERROR_FILE_NOT_FOUND:
		return {TrustFileOutcome::NOT_FOUND, error};
	case ERROR_SHARING_VIOLATION:
		return {TrustFileOutcome::SHARING_VIOLATION, error};
	default:
		return {TrustFileOutcome::FAILED, error};
	}
}

class HeldHandle : public HeldTrustFile {
public:
	explicit HeldHandle(HANDLE handle_p) : handle(handle_p) {
	}
	~HeldHandle() override {
		CloseHandle(handle);
	}
	HeldHandle(const HeldHandle &) = delete;
	HeldHandle &operator=(const HeldHandle &) = delete;

private:
	HANDLE handle;
};

string SidBytes(PSID sid) {
	return string(static_cast<const char *>(sid), GetLengthSid(sid));
}

//! One access list entry. Allowed and denied entries share ACCESS_ALLOWED_ACE's layout; any other
//! type keeps no rights or SID, which refuses it.
TrustFileEntry ReadEntry(ACE_HEADER *header) {
	TrustFileEntry entry;
	entry.type = header->AceType;
	entry.flags = header->AceFlags;
	if (header->AceType != ACCESS_ALLOWED_ACE_TYPE && header->AceType != ACCESS_DENIED_ACE_TYPE) {
		return entry;
	}
	const auto allowed = reinterpret_cast<ACCESS_ALLOWED_ACE *>(header);
	const PSID sid = &allowed->SidStart;
	const DWORD size = header->AceSize;
	const auto start = static_cast<DWORD>(offsetof(ACCESS_ALLOWED_ACE, SidStart));
	// Each part is read only once the entry's size covers it: the mask, the SID's fixed 8 bytes
	// (revision, sub-authority count, authority), then its sub-authorities.
	if (size < start) {
		return entry;
	}
	entry.rights = allowed->Mask;
	if (size - start < 8) {
		return entry;
	}
	const DWORD sub_authorities = static_cast<const BYTE *>(sid)[1];
	if (8 + 4 * sub_authorities <= size - start && IsValidSid(sid)) {
		entry.sid = SidBytes(sid);
	}
	return entry;
}

//! `file`'s owner and access list as IsWrittenDescriptor judges them; false when either cannot be
//! read.
bool ReadDescriptor(HANDLE file, TrustFileDescriptor &found) {
	PSID owner = nullptr;
	PACL acl = nullptr;
	PSECURITY_DESCRIPTOR info = nullptr;
	const SECURITY_INFORMATION what = OWNER_SECURITY_INFORMATION | DACL_SECURITY_INFORMATION;
	// The result is a Windows error code, so any value but ERROR_SUCCESS (0) is a failed read.
	if (GetSecurityInfo(file, SE_FILE_OBJECT, what, &owner, nullptr, &acl, nullptr, &info)) {
		return false;
	}
	SECURITY_DESCRIPTOR_CONTROL control = 0;
	DWORD revision = 0;
	bool read = GetSecurityDescriptorControl(info, &control, &revision) != FALSE;
	if (owner && IsValidSid(owner)) {
		found.owner = SidBytes(owner);
	}
	found.dacl_protected = (control & SE_DACL_PROTECTED) != 0;
	for (DWORD index = 0; read && acl && index < acl->AceCount; index++) {
		void *entry = nullptr;
		read = GetAce(acl, index, &entry) != FALSE;
		if (read) {
			found.entries.push_back(ReadEntry(static_cast<ACE_HEADER *>(entry)));
		}
	}
	LocalFree(info);
	return read;
}

//! The descriptor every trust file is written with: owned by the token user's SID, which an elevated
//! process sets too, and a protected access list, so the folder's inheritable entries are not merged
//! in, with one entry, full control for that SID.
class OwnerOnlyDescriptor {
public:
	OwnerOnlyDescriptor() {
		HANDLE token = nullptr;
		DWORD size = 0;
		vector<BYTE> user;
		if (OpenProcessToken(GetCurrentProcess(), TOKEN_QUERY, &token)) {
			GetTokenInformation(token, TokenUser, nullptr, 0, &size);
			user.resize(size);
			if (size == 0 || !GetTokenInformation(token, TokenUser, user.data(), size, &size)) {
				user.clear();
			}
			CloseHandle(token);
		}
		if (user.empty()) {
			throw Failure();
		}
		const PSID user_sid = reinterpret_cast<TOKEN_USER *>(user.data())->User.Sid;
		sid.resize(GetLengthSid(user_sid));
		acl.resize(sizeof(ACL) + sizeof(ACCESS_ALLOWED_ACE) + sid.size());
		const bool built = CopySid(static_cast<DWORD>(sid.size()), Sid(), user_sid) &&
		                   InitializeAcl(Acl(), static_cast<DWORD>(acl.size()), ACL_REVISION) &&
		                   AddAccessAllowedAce(Acl(), ACL_REVISION, FILE_ALL_ACCESS, Sid()) &&
		                   InitializeSecurityDescriptor(&descriptor, SECURITY_DESCRIPTOR_REVISION) &&
		                   SetSecurityDescriptorOwner(&descriptor, Sid(), FALSE) &&
		                   SetSecurityDescriptorDacl(&descriptor, TRUE, Acl(), FALSE) &&
		                   SetSecurityDescriptorControl(&descriptor, SE_DACL_PROTECTED, SE_DACL_PROTECTED);
		if (!built) {
			throw Failure();
		}
	}
	OwnerOnlyDescriptor(const OwnerOnlyDescriptor &) = delete;
	OwnerOnlyDescriptor &operator=(const OwnerOnlyDescriptor &) = delete;

	SECURITY_ATTRIBUTES Attributes() {
		return {sizeof(SECURITY_ATTRIBUTES), &descriptor, FALSE};
	}
	//! Whether `file`'s owner and access list are exactly this descriptor's (IsWrittenDescriptor).
	bool Matches(HANDLE file) {
		TrustFileDescriptor found;
		return ReadDescriptor(file, found) && IsWrittenDescriptor(found, SidBytes(Sid()));
	}

private:
	static IOException Failure() {
		return IOException("The access list of the Redshift trust file could not be built (Windows error %d)",
		                   GetLastError());
	}
	PSID Sid() {
		return sid.data();
	}
	PACL Acl() {
		return reinterpret_cast<PACL>(acl.data());
	}

	vector<BYTE> sid;
	vector<BYTE> acl;
	//! Absolute, so it points into `sid` and `acl`, which is why this is not copyable.
	SECURITY_DESCRIPTOR descriptor;
};

class WindowsTrustFiles : public TrustFiles {
public:
	TrustFileStatus WriteNew(const string &path, const string &content) override {
		auto attributes = descriptor.Attributes();
		const HANDLE file = CreateFileW(Utf8ToWide(path).c_str(), GENERIC_WRITE, 0, &attributes, CREATE_NEW,
		                                FILE_ATTRIBUTE_NORMAL, nullptr);
		if (file == INVALID_HANDLE_VALUE) {
			return StatusOf(GetLastError());
		}
		DWORD written = 0;
		const bool whole = WriteFile(file, content.data(), static_cast<DWORD>(content.size()), &written, nullptr) &&
		                   written == content.size() && FlushFileBuffers(file);
		const DWORD error = whole ? ERROR_SUCCESS : GetLastError();
		CloseHandle(file);
		return whole ? StatusOf(ERROR_SUCCESS) : TrustFileStatus {TrustFileOutcome::FAILED, error};
	}
	TrustFileStatus Move(const string &from, const string &to) override {
		if (MoveFileExW(Utf8ToWide(from).c_str(), Utf8ToWide(to).c_str(), MOVEFILE_WRITE_THROUGH)) {
			return StatusOf(ERROR_SUCCESS);
		}
		return StatusOf(GetLastError());
	}
	OpenedTrustFile Open(const string &path) override {
		OpenedTrustFile opened;
		const HANDLE file = CreateFileW(Utf8ToWide(path).c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING,
		                                FILE_ATTRIBUTE_NORMAL, nullptr);
		if (file == INVALID_HANDLE_VALUE) {
			opened.status = StatusOf(GetLastError());
			return opened;
		}
		opened.held = make_uniq<HeldHandle>(file);
		opened.owner_only = descriptor.Matches(file);
		LARGE_INTEGER size;
		if (!GetFileSizeEx(file, &size)) {
			opened.status = {TrustFileOutcome::FAILED, GetLastError()};
			return opened;
		}
		opened.status = StatusOf(ERROR_SUCCESS);
		if (size.QuadPart > MAX_TRUST_FILE_BYTES) {
			opened.owner_only = false;
			return opened;
		}
		opened.content.resize(static_cast<size_t>(size.QuadPart));
		DWORD bytes_read = 0;
		if (!opened.content.empty() &&
		    (!ReadFile(file, &opened.content[0], static_cast<DWORD>(opened.content.size()), &bytes_read, nullptr) ||
		     bytes_read != opened.content.size())) {
			opened.status = {TrustFileOutcome::FAILED, GetLastError()};
		}
		return opened;
	}
	void Delete(const string &path) override {
		DeleteFileW(Utf8ToWide(path).c_str());
	}
	vector<string> StaleNames(const string &directory) override {
		vector<string> names;
		WIN32_FIND_DATAW found;
		const HANDLE search = FindFirstFileW(Utf8ToWide(directory + "\\*").c_str(), &found);
		if (search == INVALID_HANDLE_VALUE) {
			return names;
		}
		FILETIME now;
		GetSystemTimeAsFileTime(&now);
		// FILETIME counts 100-nanosecond intervals.
		const ULONGLONG cutoff =
		    ((ULONGLONG(now.dwHighDateTime) << 32) | now.dwLowDateTime) - 7ULL * 24 * 3600 * 10000000;
		do {
			const ULONGLONG written =
			    (ULONGLONG(found.ftLastWriteTime.dwHighDateTime) << 32) | found.ftLastWriteTime.dwLowDateTime;
			const auto name = WideToUtf8(found.cFileName);
			if (!(found.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) && written < cutoff && name) {
				names.push_back(*name);
			}
		} while (FindNextFileW(search, &found));
		FindClose(search);
		return names;
	}
	void Pause() override {
		Sleep(100);
	}
	string UniqueText() override {
		// The tick count too: a process given an earlier one's ID starts its count again, and would
		// meet that process's leftover temporary file.
		static std::atomic<uint64_t> calls {0};
		return std::to_string(GetCurrentProcessId()) + "-" + std::to_string(GetTickCount64()) + "-" +
		       std::to_string(++calls);
	}

private:
	OwnerOnlyDescriptor descriptor;
};

} // namespace

TrustSource ResolveTrustSource(RedshiftLogSink &log_sink) {
	const auto ssl_cert_file = ReadEnvironmentVariable(TRUST_VARIABLE);
	if (ssl_cert_file) {
		auto chosen = UseCertFile(*ssl_cert_file, ClassifyCertFile(*ssl_cert_file));
		if (chosen) {
			return *chosen;
		}
		log_sink.Debug(string(TRUST_VARIABLE) +
		               " is set but does not name a readable file; using the Windows root store");
	}
	TrustSource source;
	source.export_content = ExportRootStore(ReadStore(L"ROOT", "ROOT"), ReadDistrusted());
	return source;
}

PlacedTrust PlaceTrustSource(const TrustSource &resolved, const string &trust_directory) {
	if (resolved.export_content.empty()) {
		return PlacedTrust {resolved, nullptr};
	}
	WindowsTrustFiles files;
	return PlaceTrustExport(files, trust_directory, ContentName(resolved.export_content), resolved.export_content);
}

unique_ptr<TrustFiles> MakeWindowsTrustFiles() {
	return make_uniq<WindowsTrustFiles>();
}

} // namespace duckdb
