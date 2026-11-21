// The Windows root store's export, its placement in the trust folder and the rule for the trust
// file's descriptor, over certificates, file calls and descriptors the cases give, so Linux runs
// them as Windows does; and the SSL_CERT_FILE step both share.

// catch.hpp comes first, as in this directory's other tests.
#include "catch.hpp"

#include "duckdb/common/numeric_utils.hpp"
#include "odbc/trust_store_cert_file.hpp"
#include "odbc/trust_store_export.hpp"
#include "thrown.hpp"

#include <functional>
#include <map>
#include <string>
#include <vector>

using namespace duckdb;
using redshift_test::Capture;
using redshift_test::Thrown;

namespace {

//! Properties that limit trust, and one on neither list, by wincrypt.h value: disallowed key usage,
//! a not-before cut-off and its usage with no usage list read, name constraints, distrust date, the
//! root program's chain policies, the CA distrust date, and CERT_FIRST_USER_PROP_ID.
const std::vector<uint32_t> LIMITING = {122, 126, 127, 84, 104, 105, 128, 0x8000};

StoreCertificate Certificate(const std::string &der, std::vector<uint32_t> properties = {}) {
	StoreCertificate copy;
	copy.der = der;
	copy.thumbprint = "sha1:" + der;
	copy.public_key = "key:" + der;
	copy.properties = std::move(properties);
	return copy;
}

StoreCertificate WithUsages(const std::string &der, std::vector<std::string> usages) {
	auto copy = Certificate(der, {store_property::ENHANCED_KEY_USAGE});
	copy.enhanced_key_usage = std::move(usages);
	return copy;
}

StoreCertificate WithNotBefore(const std::string &der, std::vector<uint32_t> properties,
                               std::vector<std::string> usages) {
	auto copy = Certificate(der, std::move(properties));
	copy.not_before_usage = std::move(usages);
	return copy;
}

//! The PEM entry of one DER encoding, base64 written out by hand.
std::string Entry(const std::string &base64) {
	return "-----BEGIN CERTIFICATE-----\n" + base64 + "\n-----END CERTIFICATE-----\n";
}

const std::string NOTHING_KEPT =
    "The Windows root store holds no CA certificate that can verify the Redshift connection: none is left once "
    "those the Disallowed store holds, and those with a store property that limits their trust, are left out. Set "
    "SSL_CERT_FILE to a readable file of PEM CA certificates, or add the roots Redshift's certificates chain to into "
    "the Windows root store";

const std::string FOLDER_FIX =
    "Set REDSHIFT_DRIVER_DIR to a folder you can write whose path is ASCII, has no leading or trailing space, holds "
    "no ASCII control character or '}', and leaves the trust file's path within 255 bytes, or set SSL_CERT_FILE to a "
    "readable file of PEM CA certificates";

//! The end of a failed file call's refusal: the folder is made when the driver loads.
const std::string CALL_FIX =
    "If its drive is full or you cannot write the folder, fix that and try again; if the folder was removed, restart "
    "the program, which makes it again. Otherwise set REDSHIFT_DRIVER_DIR to a "
    "folder you can write whose path is ASCII, has no leading or trailing space, holds no ASCII control character or "
    "'}', and leaves the trust file's path within 255 bytes, then restart the program, or set SSL_CERT_FILE to a "
    "readable file of PEM CA certificates";

const std::string KEPT_BUSY = "Another process kept holding or removing the Redshift trust file, so it could not be "
                              "checked before connecting. Try again once that process, such as a virus scanner, has "
                              "finished with it, or set SSL_CERT_FILE to a readable file of PEM CA certificates";

const std::string DIRECTORY = "C:\\Users\\tester\\trust";
const std::string NAME = std::string(64, 'a') + ".pem";
const std::string NOT_OURS = "The Redshift trust folder already holds " + NAME +
                             ", but it is not the file this extension writes: its content, owner or access list "
                             "differs, so it is not used. Delete it to let the extension write it again, or set "
                             "SSL_CERT_FILE to a readable file of PEM CA certificates";
const std::string PATH = DIRECTORY + "\\" + NAME;
const std::string CONTENT = Entry("AAEC");

//! The file calls as a map of paths, each scripted outcome used once, in order, before the map's.
class ScriptedTrustFiles : public TrustFiles {
public:
	struct File {
		std::string content;
		bool owner_only;
	};

	TrustFileStatus WriteNew(const std::string &path, const std::string &content) override {
		writes++;
		// A failed write or flush still leaves the created file, for the caller to delete.
		files[path] = File {content, true};
		return Next(write_outcomes);
	}
	TrustFileStatus Move(const std::string &from, const std::string &to) override {
		before_move();
		auto status = Next(move_outcomes);
		if (status.outcome == TrustFileOutcome::DONE && files.count(to)) {
			return {TrustFileOutcome::EXISTS, 80};
		}
		if (status.outcome == TrustFileOutcome::DONE) {
			files[to] = files[from];
			files.erase(from);
		}
		return status;
	}
	OpenedTrustFile Open(const std::string &path) override {
		OpenedTrustFile opened;
		opened.status = Next(open_outcomes);
		auto found = files.find(path);
		if (opened.status.outcome == TrustFileOutcome::DONE && found == files.end()) {
			opened.status = {TrustFileOutcome::NOT_FOUND, 2};
		}
		if (opened.status.outcome == TrustFileOutcome::DONE) {
			opened.held = make_uniq<HeldTrustFile>();
			opened.owner_only = found->second.owner_only;
			opened.content = found->second.content;
		}
		return opened;
	}
	void Delete(const std::string &path) override {
		deleted.push_back(path);
		files.erase(path);
	}
	vector<std::string> StaleNames(const std::string &) override {
		return stale;
	}
	void Pause() override {
		pauses++;
	}
	std::string UniqueText() override {
		return "7-" + std::to_string(writes + 1);
	}

	std::map<std::string, File> files;
	std::vector<TrustFileStatus> write_outcomes, move_outcomes, open_outcomes;
	std::function<void()> before_move = [] {
	};
	vector<std::string> stale;
	std::vector<std::string> deleted;
	int writes = 0;
	int pauses = 0;

private:
	static TrustFileStatus Next(std::vector<TrustFileStatus> &outcomes) {
		if (outcomes.empty()) {
			return {TrustFileOutcome::DONE, 0};
		}
		auto next = outcomes.front();
		outcomes.erase(outcomes.begin());
		return next;
	}
};

std::vector<TrustFileStatus> Repeated(TrustFileOutcome outcome, int times) {
	return std::vector<TrustFileStatus>(static_cast<size_t>(times), TrustFileStatus {outcome, 32});
}

} // namespace

TEST_CASE("The export writes each kept certificate as PEM, 64 base64 columns to a line", "[odbc]") {
	// 49 bytes are 68 base64 characters, the last byte padded: one full line and one of 4.
	const std::string long_der(49, '\0');
	CHECK(ExportRootStore({Certificate(long_der)}, {}) ==
	      "-----BEGIN CERTIFICATE-----\n" + std::string(64, 'A') + "\nAA==\n-----END CERTIFICATE-----\n");
	CHECK(ExportRootStore({Certificate("\xff")}, {}) == Entry("/w=="));
	CHECK(ExportRootStore({Certificate("\xfb\xff")}, {}) == Entry("+/8="));
	CHECK(ExportRootStore({Certificate("\x01\x02\x03")}, {}) == Entry("AQID"));
}

TEST_CASE("The export leaves out a certificate carrying a property that limits trust or is on neither list", "[odbc]") {
	for (auto property : LIMITING) {
		INFO(property);
		CHECK(ExportRootStore({Certificate("\x01", {property}), Certificate("\x02")}, {}) == Entry("Ag=="));
	}
}

TEST_CASE("The export keeps a certificate with every kept property, its usages including server authentication",
          "[odbc]") {
	auto copy = WithUsages("\x01", {"1.3.6.1.5.5.7.3.2", SERVER_AUTHENTICATION_USAGE});
	copy.properties.insert(copy.properties.end(),
	                       {store_property::SHA1_HASH, store_property::MD5_HASH, store_property::FRIENDLY_NAME,
	                        store_property::SIGNATURE_HASH, store_property::KEY_IDENTIFIER,
	                        store_property::ISSUER_PUBLIC_KEY_MD5_HASH, store_property::SUBJECT_PUBLIC_KEY_MD5_HASH,
	                        store_property::ISSUER_SERIAL_NUMBER_MD5_HASH, store_property::SUBJECT_NAME_MD5_HASH,
	                        store_property::AUTH_ROOT_SHA256_HASH, store_property::SHA256_HASH,
	                        store_property::SUBJECT_PUBLIC_KEY_BIT_LENGTH, store_property::ROOT_PROGRAM_CERT_POLICIES});
	CHECK(ExportRootStore({copy}, {}) == Entry("AQ=="));
}

TEST_CASE("The export keeps a root that chain building has given its cached properties", "[odbc]") {
	// The signature hash's CNG algorithm and the subject's and issuer's public key bit lengths.
	for (auto property : {store_property::SIGN_HASH_CNG_ALGORITHM, store_property::SUBJECT_PUBLIC_KEY_BIT_LENGTH,
	                      store_property::ISSUER_PUBLIC_KEY_BIT_LENGTH}) {
		INFO(property);
		CHECK(ExportRootStore({Certificate("\x01", {property})}, {}) == Entry("AQ=="));
	}
}

TEST_CASE("The export leaves out a certificate whose usages lack server authentication, or are empty", "[odbc]") {
	CHECK(ExportRootStore({WithUsages("\x01", {"1.3.6.1.5.5.7.3.4"}), Certificate("\x02")}, {}) == Entry("Ag=="));
	CHECK(ExportRootStore({WithUsages("\x01", {}), Certificate("\x02")}, {}) == Entry("Ag=="));
}

TEST_CASE("The export keeps a certificate whose not-before cut-off withdraws only other usages", "[odbc]") {
	const std::vector<uint32_t> cut_off = {store_property::NOT_BEFORE_TIME, store_property::NOT_BEFORE_USAGE};
	const std::string code_signing = "1.3.6.1.5.5.7.3.3";
	CHECK(ExportRootStore({WithNotBefore("\x01", cut_off, {code_signing})}, {}) == Entry("AQ=="));
}

TEST_CASE("The export leaves out a not-before cut-off that withdraws server authentication or every usage", "[odbc]") {
	const std::vector<uint32_t> cut_off = {store_property::NOT_BEFORE_TIME, store_property::NOT_BEFORE_USAGE};
	const std::vector<std::string> with_server = {"1.3.6.1.5.5.7.3.3", SERVER_AUTHENTICATION_USAGE};
	CHECK(ExportRootStore({WithNotBefore("\x01", cut_off, with_server), Certificate("\x02")}, {}) == Entry("Ag=="));
	// A time with no usage list read withdraws every usage.
	CHECK(ExportRootStore(
	          {WithNotBefore("\x01", {store_property::NOT_BEFORE_TIME}, {"1.3.6.1.5.5.7.3.3"}), Certificate("\x02")},
	          {}) == Entry("Ag=="));
	CHECK(ExportRootStore({WithNotBefore("\x01", cut_off, {}), Certificate("\x02")}, {}) == Entry("Ag=="));
	CHECK(ExportRootStore({WithNotBefore("\x01", cut_off, {"1.3.6.1.5.5.7.3.3", ANY_USAGE}), Certificate("\x02")},
	                      {}) == Entry("Ag=="));
	// A usage list with no time is refused too, so nothing rests on how Windows reads it alone.
	CHECK(ExportRootStore(
	          {WithNotBefore("\x01", {store_property::NOT_BEFORE_USAGE}, {"1.3.6.1.5.5.7.3.3"}), Certificate("\x02")},
	          {}) == Entry("Ag=="));
}

TEST_CASE("The export leaves out what the Disallowed store holds, by thumbprint or by public key", "[odbc]") {
	DistrustedCertificates by_thumbprint;
	by_thumbprint.thumbprints.insert("sha1:\x01");
	CHECK(ExportRootStore({Certificate("\x01"), Certificate("\x02")}, by_thumbprint) == Entry("Ag=="));
	// Reissued: another encoding and thumbprint for the distrusted key.
	auto reissued = Certificate("\x01");
	reissued.public_key = "key:distrusted";
	DistrustedCertificates by_key;
	by_key.public_keys.insert("key:distrusted");
	CHECK(ExportRootStore({reissued, Certificate("\x02")}, by_key) == Entry("Ag=="));
}

TEST_CASE("The export judges a certificate by every copy, writing two plain copies once", "[odbc]") {
	CHECK(ExportRootStore({Certificate("\x01"), Certificate("\x01", {LIMITING[0]}), Certificate("\x02")}, {}) ==
	      Entry("Ag=="));
	// The limited copy first too, so judging only the last copy fails.
	CHECK(ExportRootStore({Certificate("\x01", {LIMITING[0]}), Certificate("\x01"), Certificate("\x02")}, {}) ==
	      Entry("Ag=="));
	CHECK(ExportRootStore({Certificate("\x02"), Certificate("\x02")}, {}) == Entry("Ag=="));
}

TEST_CASE("The export's bytes do not depend on the order the store lists its certificates", "[odbc]") {
	const auto sorted = ExportRootStore({Certificate("\x01"), Certificate("\x02"), Certificate("\xff")}, {});
	CHECK(sorted == Entry("AQ==") + Entry("Ag==") + Entry("/w=="));
	CHECK(ExportRootStore({Certificate("\xff"), Certificate("\x02"), Certificate("\x01")}, {}) == sorted);
}

TEST_CASE("A store whose every certificate is filtered out refuses, naming the Windows root store", "[odbc]") {
	DistrustedCertificates distrusted;
	distrusted.thumbprints.insert("sha1:\x02");
	CHECK(Capture([&] {
		      ExportRootStore({Certificate("\x01", {LIMITING[0]}), Certificate("\x02")}, distrusted);
	      }) == Thrown {ExceptionType::IO, NOTHING_KEPT});
	CHECK(Capture([&] { ExportRootStore({}, {}); }) == Thrown {ExceptionType::IO, NOTHING_KEPT});
}

TEST_CASE("A first placement moves its written temporary file to the name, holding it", "[odbc]") {
	ScriptedTrustFiles files;
	auto placed = PlaceTrustExport(files, DIRECTORY, NAME, CONTENT);
	CHECK(placed.source.path == PATH);
	CHECK(placed.held != nullptr);
	CHECK(files.writes == 1);
	REQUIRE(files.files.size() == 1);
	CHECK(files.files.count(PATH) == 1);
	CHECK(files.deleted.empty());
}

TEST_CASE("A second placement of the same content uses the file there, writing nothing", "[odbc]") {
	ScriptedTrustFiles files;
	files.files[PATH] = {CONTENT, true};
	files.stale = {std::string(64, 'b') + ".pem"};
	CHECK(PlaceTrustExport(files, DIRECTORY, NAME, CONTENT).source.path == PATH);
	CHECK(files.writes == 0);
	// Nothing was moved into place, so nothing is pruned.
	CHECK(files.deleted.empty());
}

TEST_CASE("A file of the name with other bytes, or another descriptor, is refused", "[odbc]") {
	const Thrown not_ours {ExceptionType::IO, NOT_OURS};
	ScriptedTrustFiles other_bytes;
	other_bytes.files[PATH] = {Entry("AQ=="), true};
	CHECK(Capture([&] { PlaceTrustExport(other_bytes, DIRECTORY, NAME, CONTENT); }) == not_ours);
	ScriptedTrustFiles other_descriptor;
	other_descriptor.files[PATH] = {CONTENT, false};
	CHECK(Capture([&] { PlaceTrustExport(other_descriptor, DIRECTORY, NAME, CONTENT); }) == not_ours);
	CHECK(other_bytes.writes + other_descriptor.writes == 0);
}

namespace {

//! A SID's bytes as winnt.h lays them out: revision 1, the count, the NT authority, then each
//! sub-authority little-endian.
std::string NtSid(const std::vector<uint32_t> &sub_authorities) {
	std::vector<uint8_t> bytes {1, NumericCast<uint8_t>(sub_authorities.size()), 0, 0, 0, 0, 0, 5};
	for (auto value : sub_authorities) {
		for (int shift = 0; shift < 32; shift += 8) {
			bytes.push_back(NumericCast<uint8_t>((value >> shift) & 0xff));
		}
	}
	return std::string(bytes.begin(), bytes.end());
}

const std::string USER = NtSid({21, 1, 2, 3, 1001});
const std::string SYSTEM = NtSid({18});
const std::string USERS = NtSid({32, 545});
const std::string ADMINISTRATORS = NtSid({32, 544});

//! winnt.h's ACCESS_DENIED_ACE_TYPE, ACCESS_ALLOWED_OBJECT_ACE_TYPE, INHERIT_ONLY_ACE,
//! INHERITED_ACE and FILE_GENERIC_READ.
constexpr uint8_t DENIED = 1;
constexpr uint8_t ALLOWED_OBJECT = 5;
constexpr uint8_t INHERIT_ONLY = 0x08;
constexpr uint8_t INHERITED = 0x10;
constexpr uint32_t GENERIC_READ_RIGHTS = 0x00120089;

TrustFileEntry FullControl(const std::string &sid) {
	TrustFileEntry entry;
	entry.type = trust_file_entry::ACCESS_ALLOWED;
	entry.rights = trust_file_entry::FULL_CONTROL;
	entry.sid = sid;
	return entry;
}

//! What the writer gives a file, in a process of USER, elevated or not.
TrustFileDescriptor Written() {
	TrustFileDescriptor written;
	written.owner = USER;
	written.dacl_protected = true;
	written.entries = {FullControl(USER)};
	return written;
}

} // namespace

TEST_CASE("The writer's descriptor, as an elevated process writes it too, is accepted", "[odbc]") {
	CHECK(IsWrittenDescriptor(Written(), USER));
}

TEST_CASE("A descriptor with another entry, SYSTEM's or Users', is refused", "[odbc]") {
	auto read = FullControl(USERS);
	read.rights = GENERIC_READ_RIGHTS;
	const auto administrators = FullControl(ADMINISTRATORS);
	for (const auto &other : {FullControl(SYSTEM), administrators, read, FullControl(USER)}) {
		auto found = Written();
		found.entries.push_back(other);
		CHECK_FALSE(IsWrittenDescriptor(found, USER));
	}
}

TEST_CASE("A descriptor with an unprotected access list, or no entry, is refused", "[odbc]") {
	auto unprotected = Written();
	unprotected.dacl_protected = false;
	CHECK_FALSE(IsWrittenDescriptor(unprotected, USER));
	// What GetSecurityInfo gives for a NULL access list, which grants everyone everything.
	auto none = Written();
	none.entries.clear();
	CHECK_FALSE(IsWrittenDescriptor(none, USER));
}

TEST_CASE("A descriptor with another owner, or none, is refused", "[odbc]") {
	for (const auto &owner : {ADMINISTRATORS, SYSTEM, std::string()}) {
		auto found = Written();
		found.owner = owner;
		CHECK_FALSE(IsWrittenDescriptor(found, USER));
	}
}

TEST_CASE("A descriptor whose one entry is for another SID is refused", "[odbc]") {
	auto found = Written();
	found.entries = {FullControl(USERS)};
	CHECK_FALSE(IsWrittenDescriptor(found, USER));
}

TEST_CASE("A deny entry, another entry type, or an inheritance flag is refused", "[odbc]") {
	auto deny = Written();
	deny.entries[0].type = DENIED;
	CHECK_FALSE(IsWrittenDescriptor(deny, USER));
	// The adapter keeps no SID for a type outside the allowed and denied layout.
	auto object = Written();
	object.entries[0].type = ALLOWED_OBJECT;
	object.entries[0].sid.clear();
	CHECK_FALSE(IsWrittenDescriptor(object, USER));
	for (const uint8_t flags : {INHERIT_ONLY, INHERITED}) {
		auto found = Written();
		found.entries[0].flags = flags;
		CHECK_FALSE(IsWrittenDescriptor(found, USER));
	}
}

TEST_CASE("An entry granting the user other rights than full control is refused", "[odbc]") {
	// Less: read only, and full control without WRITE_DAC; more: GENERIC_ALL added.
	const auto full = trust_file_entry::FULL_CONTROL;
	for (const uint32_t rights : {GENERIC_READ_RIGHTS, full & ~0x00040000U, full | 0x10000000U}) {
		auto found = Written();
		found.entries[0].rights = rights;
		CHECK_FALSE(IsWrittenDescriptor(found, USER));
	}
}

TEST_CASE("An empty token user SID refuses a descriptor whose every SID is empty", "[odbc]") {
	TrustFileDescriptor empty;
	empty.dacl_protected = true;
	empty.entries = {FullControl("")};
	CHECK_FALSE(IsWrittenDescriptor(empty, ""));
}

TEST_CASE("A placement that loses the move to another writer uses that writer's file", "[odbc]") {
	ScriptedTrustFiles files;
	files.before_move = [&] {
		files.files[PATH] = {CONTENT, true};
	};
	// The winner prunes; this placement moved nothing, so it does not.
	files.stale = {std::string(64, 'b') + ".pem"};
	CHECK(PlaceTrustExport(files, DIRECTORY, NAME, CONTENT).source.path == PATH);
	CHECK(files.files.size() == 1);
	CHECK(files.deleted == std::vector<std::string> {DIRECTORY + "\\tmp-7-1.tmp"});
}

TEST_CASE("A file moved into place is checked again at the open, and refused when it changed", "[odbc]") {
	const Thrown not_ours {ExceptionType::IO, NOT_OURS};
	const auto temporary = DIRECTORY + "\\tmp-7-1.tmp";
	ScriptedTrustFiles widened;
	widened.before_move = [&] {
		widened.files[temporary].owner_only = false;
	};
	CHECK(Capture([&] { PlaceTrustExport(widened, DIRECTORY, NAME, CONTENT); }) == not_ours);
	ScriptedTrustFiles changed;
	changed.before_move = [&] {
		changed.files[temporary].content = Entry("AQ==");
	};
	CHECK(Capture([&] { PlaceTrustExport(changed, DIRECTORY, NAME, CONTENT); }) == not_ours);
	CHECK(widened.writes + changed.writes == 2);
}

TEST_CASE("A file removed between a lost move and the open is written again", "[odbc]") {
	ScriptedTrustFiles files;
	files.move_outcomes = {{TrustFileOutcome::EXISTS, 80}};
	CHECK(PlaceTrustExport(files, DIRECTORY, NAME, CONTENT).source.path == PATH);
	CHECK(files.writes == 2);
	CHECK(files.files.size() == 1);
}

TEST_CASE("A sharing violation at the open or the move is retried, then fails the connect", "[odbc]") {
	ScriptedTrustFiles once;
	once.open_outcomes = {{TrustFileOutcome::SHARING_VIOLATION, 32}};
	once.move_outcomes = {{TrustFileOutcome::SHARING_VIOLATION, 32}};
	CHECK(PlaceTrustExport(once, DIRECTORY, NAME, CONTENT).source.path == PATH);
	CHECK(once.pauses == 2);

	ScriptedTrustFiles moves;
	moves.move_outcomes = Repeated(TrustFileOutcome::SHARING_VIOLATION, TRUST_FILE_ATTEMPTS);
	CHECK(Capture([&] { PlaceTrustExport(moves, DIRECTORY, NAME, CONTENT); }) == Thrown {ExceptionType::IO, KEPT_BUSY});
	CHECK(moves.pauses == TRUST_FILE_ATTEMPTS - 1);
	CHECK(moves.files.empty());

	ScriptedTrustFiles opens;
	opens.open_outcomes = Repeated(TrustFileOutcome::SHARING_VIOLATION, TRUST_FILE_ATTEMPTS);
	CHECK(Capture([&] { PlaceTrustExport(opens, DIRECTORY, NAME, CONTENT); }) == Thrown {ExceptionType::IO, KEPT_BUSY});
	CHECK(opens.pauses == TRUST_FILE_ATTEMPTS);
}

TEST_CASE("A file removed after every write fails the connect once the attempts run out", "[odbc]") {
	ScriptedTrustFiles files;
	files.open_outcomes = Repeated(TrustFileOutcome::NOT_FOUND, TRUST_FILE_ATTEMPTS + 1);
	CHECK(Capture([&] { PlaceTrustExport(files, DIRECTORY, NAME, CONTENT); }) == Thrown {ExceptionType::IO, KEPT_BUSY});
	CHECK(files.writes == TRUST_FILE_ATTEMPTS);
}

TEST_CASE("A file written on the last attempt is opened and used, not discarded", "[odbc]") {
	ScriptedTrustFiles files;
	files.open_outcomes = Repeated(TrustFileOutcome::NOT_FOUND, TRUST_FILE_ATTEMPTS);
	auto placed = PlaceTrustExport(files, DIRECTORY, NAME, CONTENT);
	CHECK(placed.source.path == PATH);
	CHECK(placed.held != nullptr);
	CHECK(files.writes == TRUST_FILE_ATTEMPTS);
}

TEST_CASE("No temporary file remains after a failed write, flush or move", "[odbc]") {
	const std::string written = "The Redshift trust file, the CA certificates exported from the Windows root store, "
	                            "could not be written in its folder (Windows error 112). " +
	                            CALL_FIX;
	ScriptedTrustFiles write;
	write.write_outcomes = {{TrustFileOutcome::FAILED, 112}};
	CHECK(Capture([&] { PlaceTrustExport(write, DIRECTORY, NAME, CONTENT); }) == Thrown {ExceptionType::IO, written});
	CHECK(write.files.empty());

	ScriptedTrustFiles move;
	move.move_outcomes = {{TrustFileOutcome::FAILED, 5}};
	CHECK(Capture([&] { PlaceTrustExport(move, DIRECTORY, NAME, CONTENT); }) ==
	      Thrown {ExceptionType::IO, "The Redshift trust file, the CA certificates exported from the Windows root "
	                                 "store, could not be moved into place in its folder (Windows error 5). " +
	                                     CALL_FIX});
	CHECK(move.files.empty());
}

TEST_CASE("An open that fails for another reason fails the connect, naming the error", "[odbc]") {
	ScriptedTrustFiles files;
	files.open_outcomes = {{TrustFileOutcome::FAILED, 5}};
	CHECK(Capture([&] { PlaceTrustExport(files, DIRECTORY, NAME, CONTENT); }) ==
	      Thrown {ExceptionType::IO, "The Redshift trust file, the CA certificates exported from the Windows root "
	                                 "store, could not be opened in its folder (Windows error 5). " +
	                                     CALL_FIX});
	CHECK(files.writes == 0);
}

TEST_CASE("A new file prunes the stale exports and temporary files, and nothing else", "[odbc]") {
	ScriptedTrustFiles files;
	const auto old_export = std::string(64, 'b') + ".pem";
	files.stale = {old_export, "tmp-9-9.tmp", "notes.txt", NAME};
	PlaceTrustExport(files, DIRECTORY, NAME, CONTENT);
	CHECK(files.deleted == std::vector<std::string> {DIRECTORY + "\\" + old_export, DIRECTORY + "\\tmp-9-9.tmp"});
}

TEST_CASE("An empty trust folder refuses, naming REDSHIFT_DRIVER_DIR and then SSL_CERT_FILE", "[odbc]") {
	ScriptedTrustFiles files;
	CHECK(Capture([&] { PlaceTrustExport(files, "", NAME, CONTENT); }) ==
	      Thrown {ExceptionType::IO, "No folder could hold the Redshift trust file, the CA certificates exported "
	                                 "from the Windows root store: neither the driver's folder nor the per-user cache "
	                                 "could have one. " +
	                                     FOLDER_FIX});
	CHECK(files.writes == 0);
}

TEST_CASE("A trust file path that breaks a trust path rule refuses, never naming the path", "[odbc]") {
	ScriptedTrustFiles files;
	CHECK(Capture([&] { PlaceTrustExport(files, "C:\\Users\\a}b\\trust", NAME, CONTENT); }) ==
	      Thrown {ExceptionType::IO, "The path of the Redshift trust file, the CA certificates exported from the "
	                                 "Windows root store, holds '}', which ends a brace-quoted value, so the driver "
	                                 "cannot be given it. " +
	                                     FOLDER_FIX});
	CHECK(Capture([&] { PlaceTrustExport(files, "C:\\" + std::string(190, 'x'), NAME, CONTENT); }) ==
	      Thrown {ExceptionType::IO, "The path of the Redshift trust file, the CA certificates exported from the "
	                                 "Windows root store, is longer than the 255 bytes the driver keeps, so the "
	                                 "driver cannot be given it. " +
	                                     FOLDER_FIX});
	CHECK(files.writes == 0);
}

TEST_CASE("An SSL_CERT_FILE that exists but cannot be opened is refused, not passed over", "[odbc]") {
	CHECK(
	    Capture([] { UseCertFile("C:\\certs\\held.pem", CertFileState::OPEN_FAILED); }) ==
	    Thrown {ExceptionType::IO,
	            "SSL_CERT_FILE names a file that could not be checked or opened: it may be on an unreachable drive "
	            "or share, have a name Windows refuses, or be held open by another program, such as a virus "
	            "scanner. Try again once it can be opened, or unset SSL_CERT_FILE to use the system CA certificates"});
}
