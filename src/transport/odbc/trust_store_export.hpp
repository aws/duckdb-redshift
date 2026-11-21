//===----------------------------------------------------------------------===//
//                         DuckDB - Redshift
//
// src/transport/odbc/trust_store_export.hpp
//
// The Windows root store's export: which certificates it keeps, the PEM it writes, how that file
// is placed in the trust directory and held, and the descriptor an existing one must have. Plain
// C++ over certificates, file calls and descriptors given as input, so Linux runs its tests too;
// trust_store_windows.cpp reads the real stores, files and descriptors.
//===----------------------------------------------------------------------===//

#pragma once

#include "duckdb/common/string.hpp"
#include "duckdb/common/unique_ptr.hpp"
#include "duckdb/common/vector.hpp"
#include "odbc/trust_store.hpp"

#include <cstdint>
#include <set>

namespace duckdb {

//! The store properties that do not limit trust, by their
//! wincrypt.h values, which trust_store_windows.cpp asserts against the SDK's. A PEM file cannot
//! carry a limit, so a certificate with any other property is left out.
namespace store_property {
constexpr uint32_t SHA1_HASH = 3;
constexpr uint32_t MD5_HASH = 4;
constexpr uint32_t ENHANCED_KEY_USAGE = 9;
constexpr uint32_t FRIENDLY_NAME = 11;
constexpr uint32_t SIGNATURE_HASH = 15;
constexpr uint32_t KEY_IDENTIFIER = 20;
constexpr uint32_t ISSUER_PUBLIC_KEY_MD5_HASH = 24;
constexpr uint32_t SUBJECT_PUBLIC_KEY_MD5_HASH = 25;
constexpr uint32_t ISSUER_SERIAL_NUMBER_MD5_HASH = 28;
constexpr uint32_t SUBJECT_NAME_MD5_HASH = 29;
constexpr uint32_t ROOT_PROGRAM_CERT_POLICIES = 83;
constexpr uint32_t SIGN_HASH_CNG_ALGORITHM = 89;
constexpr uint32_t SUBJECT_PUBLIC_KEY_BIT_LENGTH = 92;
constexpr uint32_t ISSUER_PUBLIC_KEY_BIT_LENGTH = 94;
constexpr uint32_t AUTH_ROOT_SHA256_HASH = 98;
constexpr uint32_t SHA256_HASH = 107;
constexpr uint32_t NOT_BEFORE_TIME = 126;
constexpr uint32_t NOT_BEFORE_USAGE = 127;
} // namespace store_property

//! The usage an enhanced key usage property must include for its certificate to be kept.
extern const char *const SERVER_AUTHENTICATION_USAGE;
//! anyExtendedKeyUsage, which a not-before cut-off's list may name to withdraw every usage.
extern const char *const ANY_USAGE;

//! One copy of a certificate, as one of the ROOT store's physical stores holds it.
struct StoreCertificate {
	//! The DER encoding, which is the same in every copy.
	string der;
	//! The SHA-1 of `der`, by which the Disallowed store is matched.
	string thumbprint;
	//! The subject public key's bits, so a reissued certificate for a distrusted key is matched too.
	string public_key;
	//! The store properties this copy carries.
	vector<uint32_t> properties;
	//! The usages of its enhanced key usage property, when it carries one.
	vector<string> enhanced_key_usage;
	//! The usages its not-before cut-off withdraws from later-issued certificates, when it carries one.
	vector<string> not_before_usage;
};

//! What the Disallowed store holds, by thumbprint and by public key.
struct DistrustedCertificates {
	std::set<string> thumbprints;
	std::set<string> public_keys;
};

//! The PEM file of each certificate in `copies` that `distrusted` does not hold and no copy of which
//! carries a property that limits it, once each and sorted by DER bytes, so the content does not
//! depend on the store's order. Throws IOException naming the Windows root store when none is kept.
string ExportRootStore(const vector<StoreCertificate> &copies, const DistrustedCertificates &distrusted);

//! How one file call ended, as the Windows calls report it.
enum class TrustFileOutcome : uint8_t { DONE, EXISTS, NOT_FOUND, SHARING_VIOLATION, FAILED };

struct TrustFileStatus {
	TrustFileOutcome outcome;
	//! The system's error code when the call did not succeed, for the message.
	uint32_t error = 0;
};

//! A file held open sharing reads only, so no process can delete or replace it while this lives.
class HeldTrustFile {
public:
	virtual ~HeldTrustFile() = default;
};

//! The one access entry WriteNew writes, by winnt.h's values, which trust_store_windows.cpp asserts
//! against the SDK's: allowed, with no inheritance flag, full control.
namespace trust_file_entry {
constexpr uint8_t ACCESS_ALLOWED = 0;
constexpr uint32_t FULL_CONTROL = 0x001F01FF;
} // namespace trust_file_entry

//! One entry of a file's access list. `sid` holds the SID's bytes, empty for another entry type.
struct TrustFileEntry {
	uint8_t type = 0;
	uint8_t flags = 0;
	uint32_t rights = 0;
	string sid;
};

//! A file's owner and access list, as the Windows adapter reads them with GetSecurityInfo.
struct TrustFileDescriptor {
	//! The owner SID's bytes, empty when the file has none.
	string owner;
	//! Whether the access list is protected, so the folder's inheritable entries are not merged in.
	bool dacl_protected = false;
	//! None for no access list or a NULL one, which grants every user everything.
	vector<TrustFileEntry> entries;
};

//! Whether `found` is exactly the descriptor WriteNew writes for the token user `user` (SID bytes):
//! owner `user`, protected, one full-control entry for `user`. An elevated process sets that owner
//! too. Any other entry, SYSTEM's or Administrators' included, refuses.
bool IsWrittenDescriptor(const TrustFileDescriptor &found, const string &user);

//! An existing file, opened sharing reads only.
struct OpenedTrustFile {
	TrustFileStatus status;
	//! Set when the open succeeded.
	unique_ptr<HeldTrustFile> held;
	//! Whether IsWrittenDescriptor accepts its owner and access list.
	bool owner_only = false;
	string content;
};

//! The file calls the placement makes, given so tests can script each outcome.
class TrustFiles {
public:
	virtual ~TrustFiles() = default;
	//! Creates `path`, which must not exist, owned by the token user with a protected access list
	//! granting only that user, then writes `content`, flushes and closes it.
	virtual TrustFileStatus WriteNew(const string &path, const string &content) = 0;
	//! Renames `from` to `to`, never replacing a file there.
	virtual TrustFileStatus Move(const string &from, const string &to) = 0;
	virtual OpenedTrustFile Open(const string &path) = 0;
	//! Deletes `path`, ignoring a failure such as another process holding it.
	virtual void Delete(const string &path) = 0;
	//! The names of the files in `directory` last written more than seven days ago.
	virtual vector<string> StaleNames(const string &directory) = 0;
	//! Waits before a call that met a sharing violation is tried again.
	virtual void Pause() = 0;
	//! Text unique to this process and call, for a temporary file's name.
	virtual string UniqueText() = 0;
};

//! The most times the placement writes the file or waits for a held one, or retries the move,
//! before the connect fails.
constexpr int TRUST_FILE_ATTEMPTS = 10;

//! The source the connection string names, and the file held while the driver connects.
struct PlacedTrust {
	TrustSource source;
	unique_ptr<HeldTrustFile> held;
};

//! `content`'s file, `name`, in `directory`: the file already there when it is the one this would
//! write, else written to a temporary file and moved there without replacing. Returned open sharing
//! reads only. Throws IOException naming REDSHIFT_DRIVER_DIR and then SSL_CERT_FILE, never the path,
//! when `directory` is empty or the file's path breaks a trust path rule; and when a file of the
//! name is another one, or another process keeps holding or removing it.
PlacedTrust PlaceTrustExport(TrustFiles &files, const string &directory, const string &name, const string &content);

#ifdef _WIN32
//! `resolved` as it is when SSL_CERT_FILE chose it; otherwise its export, named by the SHA-256 of
//! its content, placed in `trust_directory` (trust_store_windows.cpp).
PlacedTrust PlaceTrustSource(const TrustSource &resolved, const string &trust_directory);
//! The real file calls, for the Windows tests.
unique_ptr<TrustFiles> MakeWindowsTrustFiles();
#endif

} // namespace duckdb
