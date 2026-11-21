#include "odbc/trust_store_export.hpp"

#include "duckdb/common/exception.hpp"
#include "duckdb/common/string_util.hpp"
#include "odbc/connection_settings.hpp"
#include "odbc/trust_store_cert_file.hpp"

#include <algorithm>
#include <map>

namespace duckdb {

const char *const SERVER_AUTHENTICATION_USAGE = "1.3.6.1.5.5.7.3.1";
const char *const ANY_USAGE = "2.5.29.37.0";

namespace {

bool HasServerAuthentication(const vector<string> &usages) {
	return std::find(usages.begin(), usages.end(), SERVER_AUTHENTICATION_USAGE) != usages.end();
}

bool Carries(const StoreCertificate &copy, uint32_t property) {
	return std::find(copy.properties.begin(), copy.properties.end(), property) != copy.properties.end();
}

bool DoesNotLimitTrust(const StoreCertificate &copy, uint32_t property) {
	switch (property) {
	case store_property::SHA1_HASH:
	case store_property::MD5_HASH:
	case store_property::FRIENDLY_NAME:
	case store_property::SIGNATURE_HASH:
	case store_property::KEY_IDENTIFIER:
	case store_property::ISSUER_PUBLIC_KEY_MD5_HASH:
	case store_property::SUBJECT_PUBLIC_KEY_MD5_HASH:
	case store_property::ISSUER_SERIAL_NUMBER_MD5_HASH:
	case store_property::SUBJECT_NAME_MD5_HASH:
	case store_property::AUTH_ROOT_SHA256_HASH:
	case store_property::SHA256_HASH:
	// Caches CryptoAPI writes onto a root while it builds a chain through it.
	case store_property::SIGN_HASH_CNG_ALGORITHM:
	case store_property::SUBJECT_PUBLIC_KEY_BIT_LENGTH:
	case store_property::ISSUER_PUBLIC_KEY_BIT_LENGTH:
	// The root program's policies only mark extended validation, which the file never grants.
	case store_property::ROOT_PROGRAM_CERT_POLICIES:
		return true;
	case store_property::ENHANCED_KEY_USAGE:
		// An empty usage list allows nothing, and the file would drop the limit of any other list.
		return HasServerAuthentication(copy.enhanced_key_usage);
	case store_property::NOT_BEFORE_TIME:
	case store_property::NOT_BEFORE_USAGE: {
		// The cut-off withdraws every usage without its list, so the pair is kept only whole, with a
		// read list that names neither server authentication nor any usage.
		const auto &usages = copy.not_before_usage;
		return Carries(copy, store_property::NOT_BEFORE_TIME) && Carries(copy, store_property::NOT_BEFORE_USAGE) &&
		       !usages.empty() && !HasServerAuthentication(usages) &&
		       std::find(usages.begin(), usages.end(), ANY_USAGE) == usages.end();
	}
	default:
		return false;
	}
}

bool IsKept(const vector<const StoreCertificate *> &copies, const DistrustedCertificates &distrusted) {
	for (auto copy : copies) {
		if (distrusted.thumbprints.count(copy->thumbprint) || distrusted.public_keys.count(copy->public_key)) {
			return false;
		}
		for (auto property : copy->properties) {
			if (!DoesNotLimitTrust(*copy, property)) {
				return false;
			}
		}
	}
	return true;
}

string Base64(const string &bytes) {
	static const char ALPHABET[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
	const auto data = reinterpret_cast<const uint8_t *>(bytes.data());
	string text;
	for (idx_t start = 0; start < bytes.size(); start += 3) {
		const auto remaining = bytes.size() - start;
		uint32_t group = uint32_t(data[start]) << 16;
		if (remaining > 1) {
			group |= uint32_t(data[start + 1]) << 8;
		}
		if (remaining > 2) {
			group |= data[start + 2];
		}
		text += ALPHABET[(group >> 18) & 63];
		text += ALPHABET[(group >> 12) & 63];
		text += remaining > 1 ? ALPHABET[(group >> 6) & 63] : '=';
		text += remaining > 2 ? ALPHABET[group & 63] : '=';
	}
	return text;
}

string Pem(const string &der) {
	const auto text = Base64(der);
	string pem = "-----BEGIN CERTIFICATE-----\n";
	for (idx_t start = 0; start < text.size(); start += 64) {
		pem += text.substr(start, 64) + "\n";
	}
	return pem + "-----END CERTIFICATE-----\n";
}

//! The folder REDSHIFT_DRIVER_DIR must name to hold the trust file.
string TrustDirectoryRule() {
	return StringUtil::Format("a folder you can write whose path is ASCII, has no leading or trailing space, holds "
	                          "no ASCII control character or '}', and leaves the trust file's path within %llu bytes",
	                          TRUST_PATH_MAX_BYTES);
}

//! What each of the folder-path refusals ends with: the setting that moves the driver and the
//! trust folder together, then the bundle that needs no folder.
string TrustDirectoryFix() {
	return StringUtil::Format("Set REDSHIFT_DRIVER_DIR to %s, or set %s to a readable file of PEM CA certificates",
	                          TrustDirectoryRule(), TRUST_VARIABLE);
}

IOException TrustFileCallFailed(const char *action, const TrustFileStatus &status) {
	// The folder is made, and REDSHIFT_DRIVER_DIR read, once, when the driver loads, so a removed
	// folder or a new setting needs a restart; SSL_CERT_FILE is read at every connect.
	return IOException("The Redshift trust file, the CA certificates exported from the Windows root store, could "
	                   "not be %s in its folder (Windows error %d). If its drive is full or you cannot write the "
	                   "folder, fix that and try again; if the folder was removed, restart the program, which makes "
	                   "it again. Otherwise set REDSHIFT_DRIVER_DIR to %s, then restart the program, or set %s to a "
	                   "readable file of PEM CA certificates",
	                   action, status.error, TrustDirectoryRule(), TRUST_VARIABLE);
}

IOException TrustFileKeptBusy() {
	return IOException("Another process kept holding or removing the Redshift trust file, so it could not be checked "
	                   "before connecting. Try again once that process, such as a virus scanner, has finished with "
	                   "it, or set %s to a readable file of PEM CA certificates",
	                   TRUST_VARIABLE);
}

//! Whether this call's file was moved into place; false when another one took the name first.
bool WriteAndMove(TrustFiles &files, const string &directory, const string &path, const string &content) {
	// No longer than the file's own name, so it keeps within the length the path was checked for.
	const auto temporary = directory + "\\tmp-" + files.UniqueText() + ".tmp";
	const auto written = files.WriteNew(temporary, content);
	if (written.outcome != TrustFileOutcome::DONE) {
		files.Delete(temporary);
		throw TrustFileCallFailed("written", written);
	}
	for (int attempt = 1;; attempt++) {
		const auto moved = files.Move(temporary, path);
		if (moved.outcome == TrustFileOutcome::DONE) {
			return true;
		}
		// A virus scan of the new file is the usual sharing violation, and it ends on its own.
		if (moved.outcome == TrustFileOutcome::SHARING_VIOLATION && attempt < TRUST_FILE_ATTEMPTS) {
			files.Pause();
			continue;
		}
		files.Delete(temporary);
		switch (moved.outcome) {
		case TrustFileOutcome::EXISTS:
			return false;
		case TrustFileOutcome::SHARING_VIOLATION:
			throw TrustFileKeptBusy();
		default:
			throw TrustFileCallFailed("moved into place", moved);
		}
	}
}

//! Store changes and killed processes would otherwise grow the folder without bound.
void PruneStaleFiles(TrustFiles &files, const string &directory, const string &kept) {
	for (auto &name : files.StaleNames(directory)) {
		if (name != kept && (StringUtil::EndsWith(name, ".pem") || StringUtil::EndsWith(name, ".tmp"))) {
			files.Delete(directory + "\\" + name);
		}
	}
}

} // namespace

string ExportRootStore(const vector<StoreCertificate> &copies, const DistrustedCertificates &distrusted) {
	// Grouped by DER, so each certificate is judged by every copy; the map's order is the DER bytes'.
	std::map<string, vector<const StoreCertificate *>> certificates;
	for (auto &copy : copies) {
		certificates[copy.der].push_back(&copy);
	}
	string pem;
	for (auto &certificate : certificates) {
		if (IsKept(certificate.second, distrusted)) {
			pem += Pem(certificate.first);
		}
	}
	if (pem.empty()) {
		throw IOException("The Windows root store holds no CA certificate that can verify the Redshift connection: "
		                  "none is left once those the Disallowed store holds, and those with a store property that "
		                  "limits their trust, are left out. Set %s to a readable file of PEM CA certificates, or add "
		                  "the roots Redshift's certificates chain to into the Windows root store",
		                  TRUST_VARIABLE);
	}
	return pem;
}

bool IsWrittenDescriptor(const TrustFileDescriptor &found, const string &user) {
	if (user.empty() || found.owner != user || !found.dacl_protected || found.entries.size() != 1) {
		return false;
	}
	const auto &entry = found.entries[0];
	return entry.type == trust_file_entry::ACCESS_ALLOWED && entry.flags == 0 &&
	       entry.rights == trust_file_entry::FULL_CONTROL && entry.sid == user;
}

PlacedTrust PlaceTrustExport(TrustFiles &files, const string &directory, const string &name, const string &content) {
	if (directory.empty()) {
		throw IOException("No folder could hold the Redshift trust file, the CA certificates exported from the Windows "
		                  "root store: neither the driver's folder nor the per-user cache could have one. %s",
		                  TrustDirectoryFix());
	}
	const auto path = directory + "\\" + name;
	const auto reason = FindUnusableTrustPathReason(path);
	if (!reason.empty()) {
		throw IOException("The path of the Redshift trust file, the CA certificates exported from the Windows root "
		                  "store, %s, so the driver cannot be given it. %s",
		                  reason, TrustDirectoryFix());
	}
	for (int attempt = 1;; attempt++) {
		auto opened = files.Open(path);
		const auto outcome = opened.status.outcome;
		// Each write or pause is followed by an open, so a file written on the last attempt is used.
		if (attempt > TRUST_FILE_ATTEMPTS &&
		    (outcome == TrustFileOutcome::NOT_FOUND || outcome == TrustFileOutcome::SHARING_VIOLATION)) {
			throw TrustFileKeptBusy();
		}
		switch (outcome) {
		case TrustFileOutcome::DONE:
			// The bytes checked are the bytes the driver reads, as the handle keeps the file in place.
			if (!opened.owner_only || opened.content != content) {
				throw IOException("The Redshift trust folder already holds %s, but it is not the file this extension "
				                  "writes: its content, owner or access list differs, so it is not used. Delete it to "
				                  "let the extension write it again, or set %s to a readable file of PEM CA "
				                  "certificates",
				                  name, TRUST_VARIABLE);
			}
			return PlacedTrust {TrustSource {path}, std::move(opened.held)};
		case TrustFileOutcome::NOT_FOUND:
			if (WriteAndMove(files, directory, path, content)) {
				PruneStaleFiles(files, directory, name);
			}
			break;
		case TrustFileOutcome::SHARING_VIOLATION:
			files.Pause();
			break;
		default:
			throw TrustFileCallFailed("opened", opened.status);
		}
	}
}

} // namespace duckdb
