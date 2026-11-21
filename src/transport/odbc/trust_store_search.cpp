#include "odbc/trust_store_search.hpp"

#include "duckdb/common/exception.hpp"
#include "duckdb/common/string_util.hpp"
#include "odbc/process_environment.hpp"
#include "odbc/trust_store_cert_file.hpp"

#include <sys/stat.h>
#include <unistd.h>

namespace duckdb {

namespace {

//! access(2) follows symbolic links and judges by the real, not the effective, user and group IDs.
bool IsReadable(const string &path) {
	return access(path.c_str(), R_OK) == 0;
}

//! stat rather than lstat: a bundle path, the system's or SSL_CERT_FILE's, can be a symbolic link.
bool IsNonEmptyRegularFile(const string &path) {
	struct stat info;
	return stat(path.c_str(), &info) == 0 && S_ISREG(info.st_mode) && info.st_size > 0;
}

//! Readability first, so a file this process may not read is passed over whatever it is.
CertFileState ClassifyCertFile(const string &path) {
	if (!IsReadable(path)) {
		return CertFileState::UNREADABLE;
	}
	return IsNonEmptyRegularFile(path) ? CertFileState::BUNDLE : CertFileState::NOT_A_BUNDLE;
}

//! The missing-trust error: the mechanisms, every location tried, and the fix.
IOException NoTrustSourceError(bool variable_set, const vector<string> &system_bundles) {
	auto tried = variable_set ? string("the file ") + TRUST_VARIABLE + " names, then the system locations "
	                          : string("the system locations ");
	return IOException("No CA certificate bundle was found to verify the Redshift connection against. Tried %s%s. "
	                   "Set %s to a readable file of PEM CA certificates, or install the operating system's CA "
	                   "certificates",
	                   tried, StringUtil::Join(system_bundles, ", "), TRUST_VARIABLE);
}

} // namespace

bool IsReadableBundle(const string &path) {
	return IsNonEmptyRegularFile(path) && IsReadable(path);
}

TrustSource ResolveTrustSourceFrom(const char *ssl_cert_file, const vector<string> &system_bundles,
                                   RedshiftLogSink &log_sink) {
	if (ssl_cert_file) {
		const string path(ssl_cert_file);
		auto chosen = UseCertFile(path, ClassifyCertFile(path));
		if (chosen) {
			return *chosen;
		}
		log_sink.Debug(string(TRUST_VARIABLE) +
		               " is set but does not name a readable file; trying the system CA bundle locations");
	}
	for (auto &path : system_bundles) {
		if (IsReadableBundle(path)) {
			return TrustSource {path};
		}
	}
	throw NoTrustSourceError(ssl_cert_file != nullptr, system_bundles);
}

TrustSource ResolveTrustSource(RedshiftLogSink &log_sink) {
	const auto ssl_cert_file = ReadEnvironmentVariable(TRUST_VARIABLE);
	return ResolveTrustSourceFrom(ssl_cert_file ? ssl_cert_file->c_str() : nullptr, SystemCaBundles(), log_sink);
}

} // namespace duckdb
