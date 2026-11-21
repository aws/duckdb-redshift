//===----------------------------------------------------------------------===//
// Reading an AWS secret's credentials (redshift_aws_secret.hpp), the one place an ATTACH's
// SECRET is read: the ATTACH check and every connection open call it.
//===----------------------------------------------------------------------===//

#include "redshift_aws_secret.hpp"

#include <algorithm>
#include <iterator>

#include "duckdb/catalog/catalog_transaction.hpp"
#include "duckdb/common/exception.hpp"
#include "duckdb/main/client_context.hpp"
#include "duckdb/main/settings.hpp"
#include "duckdb/main/secret/secret.hpp"
#include "duckdb/main/secret/secret_manager.hpp"

namespace duckdb {

namespace {

//! The secret types whose credentials the aws extension stores: httpfs registers both, and the aws
//! extension's Redshift attach accepts both (duckdb-aws src/utils/utils.cpp AwsSecretTypes).
constexpr const char *AWS_SECRET_TYPES[] = {"aws", "s3"};

//! The aws extension's default secret storage (duckdb-aws AwsDefaultSecretStorage::STORAGE_NAME). Its
//! GetSecretByName returns `__aws_default` without resolving it; its LookupSecret resolves it.
constexpr const char *AWS_DEFAULT_STORAGE = "aws_default";

//! The aws extension's setting that switches its default secret off (duckdb-aws DEFAULT_CREDENTIALS_SETTING).
constexpr const char *AWS_DEFAULT_CREDENTIALS_SETTING = "aws_default_credentials";

} // namespace

bool AwsDefaultCredentialsEnabled(ClientContext &context) {
	Value enabled;
	if (context.TryGetCurrentSetting(AWS_DEFAULT_CREDENTIALS_SETTING, enabled) && !enabled.IsNull() &&
	    !enabled.GetValue<bool>()) {
		return false;
	}
	return Settings::Get<EnableExternalAccessSetting>(context);
}

namespace {

string ReadKey(const KeyValueSecret &secret, const char *key) {
	Value value;
	if (!secret.TryGetValue(key, value) || value.IsNull()) {
		return string();
	}
	return value.ToString();
}

//! The AWS secret `name`, resolved, of type aws or s3. Throws for one that does not exist, that
//! the default storage's lookup does not resolve (its default credentials are off, the lookup
//! answers nothing, or another aws secret answers it), or that has another type.
unique_ptr<SecretEntry> LookUpAwsSecret(ClientContext &context, const string &name) {
	auto &secret_manager = SecretManager::Get(context);
	auto transaction = CatalogTransaction::GetSystemCatalogTransaction(context);
	auto entry = secret_manager.GetSecretByName(transaction, name);
	if (!entry) {
		// The value is not repeated: it names no secret, so it may be a key pasted into SECRET, which the
		// aws and s3 secrets' own SECRET parameter takes. Every later refusal names a secret that exists.
		throw InvalidConfigurationException(
		    "the aws or s3 secret the ATTACH named in SECRET, or chose for a cluster identifier path, does not exist: "
		    "SECRET must name a DuckDB secret of type aws or s3");
	}
	if (entry->storage_mode == AWS_DEFAULT_STORAGE) {
		// The default storage answers no lookup while the aws extension's default credentials are off,
		// though it still lists its secrets by name (duckdb-aws AwsDefaultSecretStorage::LookupSecret,
		// DefaultCredentialsEnabled). Checked first, so a user's aws secret that would answer the lookup
		// instead never gives the reason.
		if (!AwsDefaultCredentialsEnabled(context)) {
			throw InvalidConfigurationException(
			    "AWS secret '%s' could not be read: the aws extension resolves its default secret only while "
			    "aws_default_credentials and enable_external_access are true",
			    name);
		}
		// The aws and s3 default secrets hold one identity (duckdb-aws AwsDefaultSecretStorage::SetSecrets),
		// so the aws lookup answers for either. A user's own aws secret with the default scope wins that
		// lookup over the default storage; it is never used in place of the secret SECRET named.
		auto match = secret_manager.LookupSecret(transaction, "", "aws");
		if (!match.HasMatch()) {
			// Not expected while the default credentials are on, short of a DROP SECRET between the two reads;
			// refused rather than read as the case below.
			throw InvalidConfigurationException(
			    "AWS secret '%s' could not be read: the aws extension's lookup for its default secret answered "
			    "nothing",
			    name);
		}
		if (match.secret_entry->storage_mode != AWS_DEFAULT_STORAGE) {
			throw InvalidConfigurationException(
			    "AWS secret '%s' could not be read: the aws extension resolves its default secret only through a "
			    "lookup, and another aws secret answers that lookup. Name that secret in SECRET instead",
			    name);
		}
		entry = std::move(match.secret_entry);
	}
	const auto &type = entry->secret->GetType().GetIdentifierName();
	if (std::find(std::begin(AWS_SECRET_TYPES), std::end(AWS_SECRET_TYPES), type) == std::end(AWS_SECRET_TYPES)) {
		// A secret of another type is refused here.
		throw InvalidConfigurationException("secret '%s' has type '%s': SECRET must name a secret of type aws or s3",
		                                    name, type);
	}
	return entry;
}

} // namespace

string FindOnlyUserAwsSecret(ClientContext &context) {
	auto &secret_manager = SecretManager::Get(context);
	auto transaction = CatalogTransaction::GetSystemCatalogTransaction(context);
	const auto all_secrets = secret_manager.AllSecrets(transaction);
	for (auto type : AWS_SECRET_TYPES) {
		vector<string> names;
		for (auto &entry : all_secrets) {
			// The aws extension's default secrets are its fallback, not a secret the user created.
			if (entry.storage_mode != AWS_DEFAULT_STORAGE && entry.secret->GetType().GetIdentifierName() == type) {
				names.push_back(entry.secret->GetName().GetIdentifierName());
			}
		}
		if (names.size() > 1) {
			throw InvalidConfigurationException("there are %llu secrets of type %s and no SECRET; name the one to "
			                                    "use with SECRET",
			                                    static_cast<uint64_t>(names.size()), type);
		}
		if (names.size() == 1) {
			return names[0];
		}
	}
	return string();
}

RedshiftAwsSecretContents ReadAwsSecret(ClientContext &context, const string &name) {
	auto entry = LookUpAwsSecret(context, name);
	// httpfs and the aws extension make only KeyValueSecrets of these types.
	auto key_value = dynamic_cast<const KeyValueSecret *>(entry->secret.get());
	RedshiftAwsSecretContents contents;
	auto &credentials = contents.credentials;
	if (key_value) {
		credentials.access_key_id = ReadKey(*key_value, "key_id");
		credentials.secret_access_key = ReadKey(*key_value, "secret");
		credentials.session_token = ReadKey(*key_value, "session_token");
		contents.region = ReadKey(*key_value, "region");
	}
	if (credentials.access_key_id.empty() || credentials.secret_access_key.empty()) {
		// With the default validation, the aws extension (duckdb-aws 1b96925) stores the keys an sso chain resolves
		// and refuses CREATE SECRET when it finds none, so a keyless sso secret was made while the sign-in was missing
		// or expired. Every keyless secret gets the hint, and it cannot name the profile: CHAIN and PROFILE are
		// recorded only under REFRESH 'auto'.
		if (entry->storage_mode == AWS_DEFAULT_STORAGE) {
			// The aws extension resolves its default secret's chain, which has no PROFILE, once in DuckDB's
			// database and keeps a keyless result (duckdb-aws AwsDefaultSecretStorage::Resolve; only expired
			// credentials are resolved again): only a restart after signing in, or another secret, helps.
			throw InvalidConfigurationException(
			    "AWS secret '%s' holds no AWS credentials (no key ID or secret key): the aws extension resolves its "
			    "default secret only once until DuckDB restarts, so sign in (for an sso chain, to IAM Identity Center "
			    "with aws sso login --profile <profile>, where <profile> is AWS_DEFAULT_PROFILE, otherwise "
			    "AWS_PROFILE, otherwise default), then restart DuckDB, or name in SECRET an aws secret you create",
			    name);
		}
		throw InvalidConfigurationException("AWS secret '%s' holds no AWS credentials (no key ID or secret key): "
		                                    "create it with credentials the aws extension can resolve (for an sso "
		                                    "chain, first sign in to IAM Identity Center with aws sso login "
		                                    "--profile <profile>, where <profile> is the secret's PROFILE, otherwise "
		                                    "AWS_DEFAULT_PROFILE, otherwise AWS_PROFILE, otherwise default)",
		                                    name);
	}
	return contents;
}

} // namespace duckdb
