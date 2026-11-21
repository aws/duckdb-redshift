// Stand-ins for what httpfs and the aws extension register, which this build links neither of: the
// `aws` and `s3` secret types, whose `config` provider stores key_id, secret, session_token and region
// as given, and the aws extension's `aws_default` storage, whose GetSecretByName returns
// `__aws_default` without keys until a LookupSecret for type `aws` resolves it, and whose lookups
// answer nothing while aws_default_credentials or enable_external_access is false
// (duckdb-aws src/aws_default_secret_storage.cpp). They copy that contract, not the extensions'
// credential chains, so the extension's tests check its reading of an AWS secret.

#pragma once

#include <atomic>

#include "duckdb.hpp"
#include "duckdb/main/config.hpp"
#include "duckdb/main/secret/secret.hpp"
#include "duckdb/main/secret/secret_manager.hpp"
#include "duckdb/main/secret/secret_storage.hpp"
#include "duckdb/main/settings.hpp"

namespace redshift_test {

using duckdb::BaseSecret;
using duckdb::CatalogTransaction;
using duckdb::ClientContext;
using duckdb::CreateSecretFunction;
using duckdb::CreateSecretInput;
using duckdb::DatabaseInstance;
using duckdb::DBConfig;
using duckdb::EnableExternalAccessSetting;
using duckdb::Identifier;
using duckdb::idx_t;
using duckdb::InvalidInputException;
using duckdb::KeyValueSecret;
using duckdb::LogicalType;
using duckdb::make_uniq;
using duckdb::OnCreateConflict;
using duckdb::OnEntryNotFound;
using duckdb::optional_ptr;
using duckdb::SecretEntry;
using duckdb::SecretManager;
using duckdb::SecretMatch;
using duckdb::SecretPersistType;
using duckdb::SecretStorage;
using duckdb::SecretType;
using duckdb::Settings;
using duckdb::string;
using duckdb::unique_ptr;
using duckdb::Value;
using duckdb::vector;

//! The `config` provider of the stand-in `aws` and `s3` types.
inline unique_ptr<BaseSecret> CreateStandInAwsSecret(ClientContext &, CreateSecretInput &input) {
	auto scope = input.scope;
	if (scope.empty()) {
		scope.push_back(input.type == "s3" ? "s3://" : "");
	}
	auto secret = make_uniq<KeyValueSecret>(scope, input.type, input.provider, input.name);
	for (auto key : {"key_id", "secret", "session_token", "region"}) {
		auto entry = input.options.find(key);
		if (entry != input.options.end()) {
			secret->secret_map[key] = entry->second;
		}
	}
	secret->redact_keys = {"secret", "session_token"};
	return std::move(secret);
}

//! The aws extension's setting that switches its default secret off, with that extension's name and default.
constexpr const char *AWS_DEFAULT_CREDENTIALS_SETTING = "aws_default_credentials";

//! The aws extension's default storage, reduced to what the extension relies on. `key_id` and `secret` are what
//! its credential chain would find; the first lookup for type aws or s3 resolves them, as the aws
//! extension's does. As there, a lookup answers nothing while aws_default_credentials or
//! enable_external_access is false, and GetSecretByName still returns the secrets. `lookups` counts
//! every lookup that answers, which proves the extension read the secret through the resolving lookup and how
//! often.
class StandInAwsDefaultStorage : public SecretStorage {
public:
	static constexpr const char *STORAGE_NAME = "aws_default";
	static constexpr const char *AWS_SECRET_NAME = "__aws_default";
	static constexpr const char *S3_SECRET_NAME = "__aws_default_s3";

	StandInAwsDefaultStorage(string key_id_p, string secret_p, std::atomic<idx_t> &lookups_p)
	    : SecretStorage(STORAGE_NAME, 1000000000000LL), key_id(std::move(key_id_p)), secret(std::move(secret_p)),
	      lookups(lookups_p) {
	}

	unique_ptr<SecretEntry> StoreSecret(unique_ptr<const BaseSecret>, OnCreateConflict,
	                                    optional_ptr<CatalogTransaction>) override {
		throw InvalidInputException("Secrets cannot be created in the '%s' secret storage", STORAGE_NAME);
	}
	vector<SecretEntry> AllSecrets(optional_ptr<CatalogTransaction>) override {
		vector<SecretEntry> result;
		if (!dropped) {
			result.push_back(*Make(AWS_SECRET_NAME, "aws"));
			result.push_back(*Make(S3_SECRET_NAME, "s3"));
		}
		return result;
	}
	//! As duckdb-aws: dropping either name drops both, for the database's lifetime.
	void DropSecretByName(const Identifier &name, OnEntryNotFound on_entry_not_found,
	                      optional_ptr<CatalogTransaction>) override {
		if (dropped || (name != Identifier(AWS_SECRET_NAME) && name != Identifier(S3_SECRET_NAME))) {
			if (on_entry_not_found == OnEntryNotFound::THROW_EXCEPTION) {
				throw InvalidInputException("Failed to remove non-existent secret with name %s", name);
			}
			return;
		}
		dropped = true;
	}
	SecretMatch LookupSecret(const string &path, const string &type,
	                         optional_ptr<CatalogTransaction> transaction) override {
		SecretMatch best_match;
		if (type != "aws" && type != "s3") {
			return best_match;
		}
		if (!transaction || !transaction->context || !DefaultCredentialsEnabled(*transaction->context) || dropped ||
		    !answers_lookups) {
			return best_match;
		}
		resolved = true;
		lookups++;
		auto entry = Make(type == "aws" ? AWS_SECRET_NAME : S3_SECRET_NAME, type);
		return SelectBestMatch(*entry, path, tie_break_offset, best_match);
	}
	//! False makes every lookup answer nothing while the secrets are still listed by name, which the aws
	//! extension does only for a DROP SECRET between the two reads.
	bool answers_lookups = true;

	unique_ptr<SecretEntry> GetSecretByName(const string &name, optional_ptr<CatalogTransaction>) override {
		if (dropped) {
			return nullptr;
		}
		// Compared as DuckDB identifiers, in any letter case, as duckdb-aws does.
		if (Identifier(name) == Identifier(AWS_SECRET_NAME)) {
			return Make(AWS_SECRET_NAME, "aws");
		}
		if (Identifier(name) == Identifier(S3_SECRET_NAME)) {
			return Make(S3_SECRET_NAME, "s3");
		}
		return nullptr;
	}

private:
	//! duckdb-aws DefaultCredentialsEnabled.
	static bool DefaultCredentialsEnabled(ClientContext &context) {
		Value enabled;
		if (context.TryGetCurrentSetting(AWS_DEFAULT_CREDENTIALS_SETTING, enabled) && !enabled.IsNull() &&
		    !enabled.GetValue<bool>()) {
			return false;
		}
		return Settings::Get<EnableExternalAccessSetting>(context);
	}

	unique_ptr<SecretEntry> Make(const string &name, const string &type) const {
		vector<string> scope = {type == "s3" ? "s3://" : ""};
		auto kv = make_uniq<KeyValueSecret>(scope, Identifier(type), Identifier(resolved ? "credential_chain" : "lazy"),
		                                    Identifier(name));
		if (resolved) {
			kv->secret_map["key_id"] = Value(key_id);
			kv->secret_map["secret"] = Value(secret);
			kv->secret_map["region"] = Value("eu-north-1");
		}
		auto entry = make_uniq<SecretEntry>(std::move(kv));
		entry->persist_type = SecretPersistType::TEMPORARY;
		entry->storage_mode = STORAGE_NAME;
		return entry;
	}

	string key_id;
	string secret;
	std::atomic<idx_t> &lookups;
	bool resolved = false;
	bool dropped = false;
};

//! Registers the stand-in `aws` and `s3` secret types on `db`, and, when `default_storage_lookups`
//! is given, the stand-in default storage holding `default_key_id` and `default_secret`, which it returns
//! (null without one); the secret manager owns it.
inline StandInAwsDefaultStorage *RegisterAwsSecretStandIns(DatabaseInstance &db,
                                                           std::atomic<idx_t> *default_storage_lookups = nullptr,
                                                           const string &default_key_id = "",
                                                           const string &default_secret = "") {
	auto &secret_manager = SecretManager::Get(db);
	for (auto type : {"aws", "s3"}) {
		SecretType secret_type;
		secret_type.name = type;
		secret_type.deserializer = KeyValueSecret::Deserialize<KeyValueSecret>;
		secret_type.default_provider = "config";
		secret_manager.RegisterSecretType(secret_type);
		CreateSecretFunction function = {type, "config", CreateStandInAwsSecret};
		for (auto key : {"key_id", "secret", "session_token", "region"}) {
			function.named_parameters[key] = LogicalType::VARCHAR;
		}
		secret_manager.RegisterSecretFunction(function, OnCreateConflict::ERROR_ON_CONFLICT);
	}
	if (default_storage_lookups) {
		DBConfig::GetConfig(db).AddExtensionOption(AWS_DEFAULT_CREDENTIALS_SETTING,
		                                           "Use the credentials the default AWS credential chain finds when "
		                                           "no secret matches",
		                                           LogicalType::BOOLEAN, Value::BOOLEAN(true));
		auto storage = make_uniq<StandInAwsDefaultStorage>(default_key_id, default_secret, *default_storage_lookups);
		auto *registered = storage.get();
		secret_manager.LoadSecretStorage(std::move(storage));
		return registered;
	}
	return nullptr;
}

} // namespace redshift_test
