//===----------------------------------------------------------------------===//
// The rules a `redshift` ATTACH's configuration passes before anything is read
// (redshift_config_options.hpp). The AWS credentials come from exactly one stated source:
//
//   SECRET <aws or s3 secret>                      the keys a DuckDB aws or s3 secret holds, which the
//                                                  aws or httpfs extension stored or resolved
//   CREDENTIAL_PROVIDER aws_default_chain          the client's default credential provider chain
//   CREDENTIAL_PROVIDER profile, PROFILE '<name>'  a named AWS profile
//
// never implied by an omitted option, except where the aws extension's cluster identifier is translated
// (redshift_attach_aws_forms.hpp). There is no redshift secret type: a configuration is
// given as options or a path on each ATTACH, and a SECRET that names a secret of any type but aws
// or s3 is refused for its type.
//===----------------------------------------------------------------------===//

#include "redshift_config_options.hpp"

#include <algorithm>
#include <iterator>

#include "duckdb/common/exception.hpp"
#include "duckdb/common/string_util.hpp"
#include "redshift_target_hook.hpp"

namespace duckdb {

namespace {

//! Every CREDENTIAL_PROVIDER value, in the order errors list them. A new credential provider is
//! added here, and to the parameter rules in ValidateRedshiftConfig if it takes parameters.
const char *const CREDENTIAL_PROVIDERS[] = {REDSHIFT_PARAM_CREDENTIAL_PROVIDER_AWS_DEFAULT_CHAIN,
                                            REDSHIFT_PARAM_CREDENTIAL_PROVIDER_PROFILE};

//! Words that ask for a sign-in other than IAM, found anywhere in a CREDENTIAL_PROVIDER value or an
//! ATTACH option name, in any letter case: the Redshift driver's own sign-in plugins, for IAM Identity
//! Center or an identity provider, and a database user and password. No supported value or option
//! holds one.
const char *const OTHER_SIGN_IN_WORDS[] = {"idc",    "idp",    "identity", "sso",    "browser", "token", "auth",
                                           "issuer", "plugin", "password", "passwd", "pwd",     "user",  "uid"};

//! The longest AWS secret name, as the client takes it.
constexpr idx_t AWS_SECRET_NAME_MAX_BYTES = 255;

//! Whether `name` is empty or an AWS secret name the client accepts and a message can repeat: at most
//! AWS_SECRET_NAME_MAX_BYTES, no ASCII control character or '}', no leading or trailing space.
bool IsUsableAwsSecretName(const string &name) {
	if (name.size() > AWS_SECRET_NAME_MAX_BYTES) {
		return false;
	}
	for (auto character : name) {
		const auto byte = static_cast<unsigned char>(character);
		if (byte < 0x20 || byte == 0x7F || byte == '}') {
			return false;
		}
	}
	return name.empty() || (name.front() != ' ' && name.back() != ' ');
}

//! The hint for a missing or invalid CREDENTIAL_PROVIDER: "supported values {a, b}".
string SupportedCredentialProviders() {
	string values;
	for (auto value : CREDENTIAL_PROVIDERS) {
		values += values.empty() ? value : string(", ") + value;
	}
	return "supported values {" + values + "}";
}

//! Every configuration error has one of three forms, each naming the parameter and, where it
//! helps, ending in what to do instead:
//!   Missing required parameter '<KEY>': <hint>
//!   Invalid value '<value>' for parameter '<KEY>': <hint>
//!   Invalid parameter '<KEY>'[: <hint>]
//! None names its source: the ATTACH that failed already does.
[[noreturn]] void ThrowMissing(const char *key, const string &hint) {
	throw InvalidConfigurationException("Missing required parameter '%s': %s", key, hint);
}

[[noreturn]] void ThrowInvalidValue(const char *key, const string &value, const string &hint) {
	throw InvalidConfigurationException("Invalid value '%s' for parameter '%s': %s", value, key, hint);
}

[[noreturn]] void ThrowInvalidParameter(const char *key, const string &hint = "") {
	if (hint.empty()) {
		throw InvalidConfigurationException("Invalid parameter '%s'", key);
	}
	throw InvalidConfigurationException("Invalid parameter '%s': %s", key, hint);
}

//! Refuses an ACCOUNT_ID that is not empty and not exactly 12 ASCII digits (ValidateRedshiftOptions).
void ValidateRedshiftAccountId(const string &account_id) {
	const bool twelve_digits = account_id.size() == 12 && std::all_of(account_id.begin(), account_id.end(),
	                                                                  [](char c) { return c >= '0' && c <= '9'; });
	if (!account_id.empty() && !twelve_digits) {
		// The value is not repeated, as the client's refusal repeats none: an ATTACH option can hold
		// anything, a misplaced key or control bytes among it.
		throw InvalidConfigurationException(
		    "Invalid value for parameter 'ACCOUNT_ID': it must be exactly 12 decimal digits");
	}
}

//! ValidateRedshiftOptions' credential and region rules (redshift_config_options.hpp). Lowercases
//! `credential_provider` in place, so a valid result's `profile` is already the one RedshiftConfig
//! takes: empty for the default chain and for SECRET.
void ValidateRedshiftConfig(RedshiftConfigOptions &options) {
	// REGION has no default and is never inferred from the caller's AWS credentials, which may belong
	// to another region. An AWS secret's own region setting is the fallback an ATTACH takes
	// (ResolveRedshiftAttachConfig), so with SECRET it may be left out here; beside the aws extension's
	// cluster identifier, its translation (redshift_attach_aws_forms.hpp) has already filled the region
	// from s3_region or the environment when no AWS secret is read.
	const bool from_secret = !options.aws_secret.empty();
	if (options.region.empty() && !from_secret) {
		ThrowMissing("REGION", "the AWS region of the target Redshift");
	}

	if (from_secret) {
		// The secret gives the credentials, so a second source beside it is refused, not ignored.
		if (!options.credential_provider.empty()) {
			ThrowInvalidParameter("CREDENTIAL_PROVIDER",
			                      "it is not valid with SECRET, whose aws or s3 secret gives the AWS credentials");
		}
		if (!options.profile.empty()) {
			throw InvalidConfigurationException(REDSHIFT_PROFILE_WITHOUT_PROFILE_PROVIDER);
		}
		// Messages and Describe() repeat the name, so it is checked before anything reads it, by the rule
		// the client applies at connect. The value is not repeated: it may be a
		// misplaced key.
		if (!IsUsableAwsSecretName(options.aws_secret)) {
			ThrowInvalidParameter("SECRET", "it must be the name of a DuckDB secret, of at most 255 bytes, with no "
			                                "control character or '}' and no leading or trailing space");
		}
		return;
	}

	// Without SECRET, CREDENTIAL_PROVIDER has no default, so the credential source is always explicit, except
	// beside the aws extension's cluster identifier, whose translation has already chosen one.
	// Its value is a keyword, matched case-insensitively and kept lowercase.
	if (options.credential_provider.empty()) {
		ThrowMissing("CREDENTIAL_PROVIDER",
		             SupportedCredentialProviders() + ", or name a DuckDB aws or s3 secret in SECRET");
	}
	const auto credential_provider = StringUtil::Lower(options.credential_provider);
	const auto supported =
	    std::find(std::begin(CREDENTIAL_PROVIDERS), std::end(CREDENTIAL_PROVIDERS), credential_provider);
	if (supported == std::end(CREDENTIAL_PROVIDERS)) {
		const auto iam_only = IamOnlyHint(credential_provider);
		// The value as written, not lowercased, so the user can find it.
		ThrowInvalidValue("CREDENTIAL_PROVIDER", options.credential_provider,
		                  SupportedCredentialProviders() + (iam_only.empty() ? "" : "; " + iam_only));
	}
	options.credential_provider = credential_provider;
	const bool uses_profile = credential_provider == REDSHIFT_PARAM_CREDENTIAL_PROVIDER_PROFILE;

	// PROFILE goes with CREDENTIAL_PROVIDER profile and nothing else. Refused, not ignored,
	// alongside aws_default_chain: the default chain would never read it.
	if (uses_profile && options.profile.empty()) {
		ThrowMissing("PROFILE", "CREDENTIAL_PROVIDER profile needs the name of an AWS profile");
	}
	if (!uses_profile && !options.profile.empty()) {
		throw InvalidConfigurationException(REDSHIFT_PROFILE_WITHOUT_PROFILE_PROVIDER);
	}
}

//! ValidateRedshiftOptions' target rules. Returns the target the options name, UNSPECIFIED for none.
RedshiftTarget ValidateRedshiftTarget(const RedshiftConfigOptions &options) {
	const bool has_workgroup = !options.workgroup.empty();
	if (has_workgroup && !options.cluster.empty()) {
		ThrowInvalidParameter("CLUSTER", "it is not valid with WORKGROUP. Give one target: a Serverless WORKGROUP "
		                                 "with its ACCOUNT_ID, or a provisioned CLUSTER");
	}
	if (!has_workgroup && !options.account_id.empty()) {
		ThrowInvalidParameter("ACCOUNT_ID", "it is only valid with WORKGROUP");
	}
	if (!has_workgroup) {
		return options.cluster.empty() ? RedshiftTarget::UNSPECIFIED : RedshiftTarget::PROVISIONED;
	}
	// An ARN carries an account and a region, which ACCOUNT_ID and REGION already give; taking either
	// from it would give one target two sources.
	if (StringUtil::StartsWith(StringUtil::Lower(options.workgroup), "arn:")) {
		ThrowInvalidValue("WORKGROUP", options.workgroup,
		                  "give the workgroup's name, not its ARN, and the account that owns it in ACCOUNT_ID");
	}
	if (!options.endpoint_label.empty()) {
		// A workgroup endpoint that carries no account: the path named the host, and never an account
		// with it, so its database is the caller's to choose, as for any workgroup.
		return RedshiftTarget::SERVERLESS;
	}
	if (options.account_id.empty()) {
		if (!AcceptsAccountlessWorkgroup(options.workgroup)) {
			ThrowMissing("ACCOUNT_ID", "WORKGROUP needs the 12-digit ID of the AWS account that owns the workgroup");
		}
		if (!options.database.empty()) {
			ThrowInvalidParameter("DATABASE", "it is not valid with a WORKGROUP given without ACCOUNT_ID, whose "
			                                  "database is fixed");
		}
	}
	return RedshiftTarget::SERVERLESS;
}

} // namespace

string IamOnlyHint(const string &text) {
	const auto lower = StringUtil::Lower(text);
	const bool other_sign_in = std::any_of(std::begin(OTHER_SIGN_IN_WORDS), std::end(OTHER_SIGN_IN_WORDS),
	                                       [&](const char *word) { return StringUtil::Contains(lower, word); });
	if (!other_sign_in) {
		return string();
	}
	return "only IAM authentication is supported, with AWS credentials from CREDENTIAL_PROVIDER or from a DuckDB "
	       "aws or s3 secret in SECRET, which can hold those of an IAM Identity Center sign-in; the Redshift "
	       "driver's own sign-in plugins, for IAM Identity Center or an identity provider such as Okta, and a "
	       "database user and password are not";
}

RedshiftTarget ValidateRedshiftOptions(RedshiftConfigOptions &options) {
	ValidateRedshiftConfig(options);
	const auto target = ValidateRedshiftTarget(options);
	ValidateRedshiftAccountId(options.account_id);
	return target;
}

} // namespace duckdb
