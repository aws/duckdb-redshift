#include "odbc/trust_store_cert_file.hpp"

#include "duckdb/common/exception.hpp"
#include "odbc/connection_settings.hpp"

namespace duckdb {

const char *const TRUST_VARIABLE = "SSL_CERT_FILE";

optional<TrustSource> UseCertFile(const string &path, CertFileState state) {
	switch (state) {
	case CertFileState::UNREADABLE:
		return nullopt;
	case CertFileState::NOT_A_BUNDLE:
		throw IOException("%s names an empty file, a directory or a special file such as a device, so it holds no CA "
		                  "certificates to verify the Redshift connection against. Set %s to a file of PEM CA "
		                  "certificates, or unset it to use the system CA certificates",
		                  TRUST_VARIABLE, TRUST_VARIABLE);
	case CertFileState::OPEN_FAILED:
		// Passing over a file that is there would widen a pinned bundle to the system roots.
		throw IOException("%s names a file that could not be checked or opened: it may be on an unreachable drive or "
		                  "share, have a name Windows refuses, or be held open by another program, such as a virus "
		                  "scanner. Try again once it can be opened, or unset %s to use the system CA certificates",
		                  TRUST_VARIABLE, TRUST_VARIABLE);
	case CertFileState::BUNDLE:
		break;
	}
	auto reason = FindUnusableTrustPathReason(path);
	if (!reason.empty()) {
		throw IOException("%s names a CA bundle whose path %s, so the Redshift driver cannot be given it. Move the "
		                  "bundle to a path of at most %llu bytes, with no leading or trailing space and without an "
		                  "ASCII control character or '}', or unset %s to use the system CA certificates",
		                  TRUST_VARIABLE, reason, TRUST_PATH_MAX_BYTES, TRUST_VARIABLE);
	}
	return TrustSource {path};
}

} // namespace duckdb
