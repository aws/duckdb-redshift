//===----------------------------------------------------------------------===//
// The Redshift storage type, and the configuration of its ATTACH.
//
//   ATTACH '' AS <handle> (TYPE <REDSHIFT_ATTACH_TYPE> [, REGION '<region>']
//                          [, SECRET <aws or s3 secret> | , CREDENTIAL_PROVIDER <provider>
//                          [, PROFILE '<profile>']]
//                          [, WORKGROUP '<workgroup>'] [, ACCOUNT_ID '<12 digits>']
//                          [, CLUSTER '<cluster>'] [, DATABASE '<database>']);
//   ATTACH '<redshift:// URI or ARN>' [AS <handle>] (TYPE <REDSHIFT_ATTACH_TYPE> [, REGION '<region>']
//          [, SECRET <aws or s3 secret> | , CREDENTIAL_PROVIDER <provider> [, PROFILE '<profile>']]
//          [, DATABASE '<database>']);
//
// ATTACH statements written for the aws extension's Redshift ATTACH, its cluster identifier as the path
// and its HOST, PORT and DBNAME, are translated into these forms first (redshift_attach_aws_forms.hpp).
//
// The configuration comes from the ATTACH's options and its path alone. SECRET names a DuckDB aws or
// s3 secret, whose AWS credentials the connection uses (redshift_aws_secret.hpp), and is the one
// credential source beside CREDENTIAL_PROVIDER. The options are strings. A non-empty path names the
// target instead of the target options (redshift_attach_path.hpp); without AS, a path's
// handle takes the name DuckDB derives from it, while the empty path needs AS. DuckDB has typed each
// option's value before the callback runs, so a value that is not a string is refused rather than
// cast: an unquoted ACCOUNT_ID would already have lost a leading zero. DuckDB's parser refuses a
// repeated option, in any letter case, before the callback runs.
//
// Values are carried verbatim. Checking what each may hold, and encoding it for a connection, is
// the client's job below the interface.
//===----------------------------------------------------------------------===//

#include "redshift_attach.hpp"

#include <algorithm>
#include <iterator>

#include "duckdb/common/error_data.hpp"
#include "duckdb/common/exception.hpp"
#include "duckdb/common/file_system.hpp"
#include "duckdb/common/string_util.hpp"
#include "duckdb/main/attached_database.hpp"
#include "duckdb/main/config.hpp"
#include "duckdb/main/database_manager.hpp"
#include "duckdb/main/extension/extension_loader.hpp"
#include "duckdb/main/settings.hpp"
#include "duckdb/common/sql_identifier.hpp"
#include "duckdb/parser/parsed_data/attach_info.hpp"
#include "duckdb/storage/storage_extension.hpp"
#include "redshift_attach_aws_forms.hpp"
#include "redshift_attach_path.hpp"
#include "redshift_aws_secret.hpp"
#include "redshift_config_options.hpp"
#include "redshift_messages.hpp"
#include "storage/redshift_catalog.hpp"
#include "storage/redshift_client_state.hpp"
#include "storage/redshift_transaction.hpp"

namespace duckdb {

namespace {

//! The option DuckDB passes for SECRET, as the parser lowercases it.
constexpr const char *SECRET_OPTION = "secret";

//! The name DuckDB gives the empty path when an ATTACH has no AS, which is also the name of its
//! default in-memory database. Compared as DuckDB compares database names, case-insensitively.
constexpr const char *DEFAULT_HANDLE_NAME = "memory";

//! How a refusal names the handle. An empty `handle` stands for a name DuckDB derived from a path,
//! which a refusal does not repeat: the path may hold anything, a password or an
//! account included. Such a refusal says the handle is named after its path, and asks for an AS name
//! other than the path's, since the callback cannot tell an AS that repeats the derived name from none.
string HandleInMessage(const string &handle) {
	return handle.empty() ? "the handle named after its path (give it an AS name other than the path's to see it here)"
	                      : SQLIdentifier::ToString(handle);
}

//! The handle in a refusal's example: a placeholder for a derived name.
string HandleInExample(const string &handle) {
	return handle.empty() ? "<handle>" : SQLIdentifier::ToString(handle);
}

//! Every refusal of an ATTACH names its handle, ahead of what is wrong: "Cannot attach <handle>: ...",
//! or, for a name derived from the path, "Cannot attach the handle named after its path (...): ...".
//! The one exception is the refusal of the handle name memory, which DuckDB gives an ATTACH of the
//! empty path without AS: that name is not one the user chose, so the message asks for one instead.
template <typename... ARGS>
[[noreturn]] void ThrowForHandle(const string &handle, const string &message, ARGS... params) {
	throw InvalidConfigurationException("Cannot attach %s: %s", HandleInMessage(handle),
	                                    StringUtil::Format(message, params...));
}

//! Runs `fn`, and names `handle` in an InvalidConfigurationException it throws. ValidateRedshiftOptions
//! and the AWS secret's reader do not know the handle.
template <class FN>
void NamingHandle(const string &handle, FN &&fn) {
	try {
		fn();
	} catch (InvalidConfigurationException &ex) {
		ThrowForHandle(handle, "%s", ErrorData(ex).RawMessage());
	}
}

} // namespace

RedshiftConfigOptions ReadRedshiftAttachOptions(const string &handle, const string &path,
                                                const unordered_map<string, Value> &given, AwsFormOptions &aws_forms) {
	// The aws extension's options are taken first, so that only this extension's own are read.
	auto options = given;
	NamingHandle(handle, [&] { aws_forms = TakeAwsFormOptions(path, options); });
	RedshiftConfigOptions attach_options;
	for (auto &parameter : REDSHIFT_CONFIG_PARAMETERS) {
		auto entry = options.find(parameter.name);
		if (entry == options.end()) {
			continue;
		}
		// DuckDB refuses a NULL option before the attach callback runs. The value is never repeated:
		// it can be any expression, such as a STRUCT of credentials a user meant to pass inline.
		const auto option_name = StringUtil::Upper(parameter.name);
		if (entry->second.type().id() != LogicalTypeId::VARCHAR) {
			ThrowForHandle(handle, "Invalid value for parameter '%s': it must be a string; quote it, like %s '<value>'",
			               option_name, option_name);
		}
		auto &value = StringValue::Get(entry->second);
		if (value.empty()) {
			ThrowForHandle(handle, "Invalid value '' for parameter '%s': it cannot be empty", option_name);
		}
		attach_options.*parameter.field = value;
	}
	return attach_options;
}

namespace {

//! Whether the ATTACH's options set any part of the target, ACCOUNT_ID alone included.
bool SetsTargetPart(const RedshiftConfigOptions &options) {
	return !options.workgroup.empty() || !options.account_id.empty() || !options.cluster.empty();
}

//! The target the ATTACH path names: none for the empty path, else a `redshift://` endpoint URI or a
//! workgroup endpoint's long-form ARN. Any other path is refused. Parsed before the AWS secret is
//! read, so a path is judged the same whatever credentials SECRET names. It is not repeated: it may
//! hold anything.
unique_ptr<RedshiftPathTarget> ParseAttachPath(const string &handle, const string &path) {
	if (path.empty()) {
		return nullptr;
	}
	if (!IsRedshiftEndpointUri(path) && !IsArnPath(path)) {
		ThrowForHandle(handle,
		               "the path must be empty, a redshift:// endpoint or a workgroup endpoint's ARN. Give the "
		               "configuration as options: ATTACH '' AS %s (TYPE %s, REGION '<region>', ...), or the "
		               "endpoint: ATTACH 'redshift://<endpoint>[:<port>][/<database>]' AS %s (TYPE %s, ...)",
		               HandleInExample(handle), REDSHIFT_ATTACH_TYPE, HandleInExample(handle), REDSHIFT_ATTACH_TYPE);
	}
	unique_ptr<RedshiftPathTarget> target;
	NamingHandle(handle, [&] {
		target = make_uniq<RedshiftPathTarget>(IsArnPath(path) ? ParseLongWorkgroupArn(path)
		                                                       : ParseRedshiftEndpointUri(path));
	});
	return target;
}

//! Applies the target a path names. It is the target's one source: target options, a REGION other
//! than its own, and DATABASE beside a path that names one are refused.
void ApplyPathTarget(RedshiftConfigOptions &resolved, const RedshiftPathTarget &target) {
	if (SetsTargetPart(resolved)) {
		throw InvalidConfigurationException(
		    "the path names the target, so WORKGROUP, ACCOUNT_ID and CLUSTER cannot be given too");
	}
	if (!resolved.region.empty() && resolved.region != target.region) {
		// The region is not echoed, as ValidateRegion does not echo it: it may hold control bytes or a
		// misplaced secret.
		throw InvalidConfigurationException("REGION names another region than the path; give one, or make them agree");
	}
	if (!target.database.empty() && !resolved.database.empty()) {
		throw InvalidConfigurationException("the path names the database, so DATABASE cannot be given too");
	}
	resolved.region = target.region;
	resolved.workgroup = target.workgroup;
	resolved.account_id = target.account_id;
	resolved.cluster = target.cluster;
	resolved.endpoint_label = target.endpoint_label;
	resolved.endpoint_port = target.endpoint_port;
	if (!target.database.empty()) {
		resolved.database = target.database;
	}
}

//! The ATTACH's configuration, once its options and path pass ValidateRedshiftOptions and name a
//! target. With SECRET the AWS secret is read once, after those rules pass: its credentials are checked
//! and dropped, and its own region setting is the last of the region's sources,
//! after REGION and the path, used when not empty, unless `fallback_region`, when not null, comes after
//! it (AwsFormTranslation). This reads no environment or profile itself, but the aws extension may have
//! filled that setting from them when it created the secret. Each connection open reads the secret again.
RedshiftConfig ValidatedAttachConfig(ClientContext &context, RedshiftConfigOptions &resolved,
                                     optional_ptr<const string> fallback_region) {
	const auto target = ValidateRedshiftOptions(resolved);
	if (target == RedshiftTarget::UNSPECIFIED) {
		throw InvalidConfigurationException(
		    "Missing target: give WORKGROUP with ACCOUNT_ID for a Serverless workgroup or CLUSTER for a "
		    "provisioned cluster, or the endpoint as a redshift:// path");
	}
	if (!resolved.aws_secret.empty()) {
		auto contents = ReadAwsSecret(context, resolved.aws_secret);
		if (resolved.region.empty() && contents.region.empty() && fallback_region) {
			if (fallback_region->empty()) {
				ThrowMissingFallbackRegion(resolved.aws_secret);
			}
			resolved.region = *fallback_region;
		} else if (resolved.region.empty()) {
			// The AWS secret was the last source of the region; an empty region setting is none.
			if (contents.region.empty()) {
				throw InvalidConfigurationException(
				    "Missing required parameter 'REGION': set REGION, or include a region in the aws or s3 secret "
				    "'%s'",
				    resolved.aws_secret);
			}
			resolved.region = contents.region;
		}
	}

	RedshiftConfig config;
	config.region = resolved.region;
	config.profile = resolved.profile;
	config.aws_secret = resolved.aws_secret;
	config.endpoint_port = resolved.endpoint_port;
	config.target = target;
	config.workgroup = resolved.workgroup;
	config.account_id = resolved.account_id;
	config.cluster = resolved.cluster;
	config.database = resolved.database;
	config.endpoint_label = resolved.endpoint_label;
	return config;
}

} // namespace

RedshiftConfig ResolveRedshiftAttachConfig(ClientContext &context, const string &handle, const string &path,
                                           const RedshiftConfigOptions &attach_options,
                                           optional_ptr<const string> secret_name, const AwsFormOptions &aws_forms) {
	// The path is parsed before anything else, and names the handle in its own refusals; a cluster
	// identifier, the aws extension's form, is the translation's to read.
	auto path_target = IsClusterIdentifierPath(path) ? nullptr : ParseAttachPath(handle, path);
	// An empty SECRET is refused rather than read as no SECRET, like a name that matches no secret.
	if (secret_name && secret_name->empty()) {
		ThrowForHandle(handle, "Invalid value '' for parameter 'SECRET': it cannot be empty");
	}
	RedshiftConfig config;
	NamingHandle(handle, [&] {
		const auto translation =
		    TranslateAwsForms(context, path, IsRedshiftEndpointUri(path) ? path_target.get() : nullptr, attach_options,
		                      secret_name, aws_forms);
		auto resolved = translation.options;
		if (secret_name) {
			resolved.aws_secret = *secret_name;
		}
		if (translation.path != path) {
			// A cluster identifier, as the empty path or, with HOST, as a redshift:// path the translation
			// built from a HOST it checked.
			path_target = translation.path.empty()
			                  ? nullptr
			                  : make_uniq<RedshiftPathTarget>(ParseRedshiftEndpointUri(translation.path));
		}
		if (path_target) {
			ApplyPathTarget(resolved, *path_target);
		}
		config = ValidatedAttachConfig(context, resolved, translation.fallback_region.get());
	});
	return config;
}

namespace {

//! The ATTACH options DuckDB reads into AttachOptions::access_mode, as the parser lowercases them,
//! and the value of each that asks for a read-only database.
struct AccessModeOption {
	const char *name;
	bool read_only_value;
};

//! How to get read-only access instead: the only guard redshift_query cannot get around.
constexpr const char *READ_ONLY_ACCESS_HINT = "For read-only access, use Redshift credentials with only SELECT grants";

const AccessModeOption ACCESS_MODE_OPTIONS[] = {
    {"read_only", true}, {"readonly", true}, {"read_write", false}, {"readwrite", false}};

//! Creates the attached database's catalog. DuckDB has already consumed its own options, such as
//! TYPE and READ_ONLY; what is left in `options.options` is this extension's: SECRET, the
//! configuration parameters and the aws extension's options (IsAwsFormOption).
unique_ptr<Catalog> RedshiftAttach(optional_ptr<StorageExtensionInfo> storage_info, ClientContext &context,
                                   AttachedDatabase &db, const string &name, AttachInfo &info, AttachOptions &options) {
	// In order: external access, then an attach CONNECT makes, then the handle name, then unknown option names, then
	// that SECRET is a string, the aws extension's options' values and the configuration options' values, then
	// read-only options written on the ATTACH, then the configuration (the path, an empty SECRET, the aws extension's
	// forms, the configuration and target rules, then the AWS secret's read), then the read-only default of a
	// read-only DuckDB, then the open of the attaching connection's session. Option errors come first, then the
	// READ_WRITE hint, and a failed open last.

	// A name DuckDB derived from the path may hold anything from it, so a handle named after its path,
	// without AS or by an AS that repeats that name in any letter case, gets an empty `handle`, which
	// HandleInMessage does not repeat.
	const bool derived_name =
	    !info.path.empty() &&
	    StringUtil::CIEquals(
	        name,
	        AttachedDatabase::ExtractDatabaseName(info.path, FileSystem::GetFileSystem(context)).GetIdentifierName());
	const string handle = derived_name ? string() : name;

	// A host that has external access off gets no new handle, before anything else is checked.
	// A handle attached before stays usable, as DuckDB's other attached databases
	// do. A loadable build cannot be loaded while external access is off, so for it only a run-time
	// SET reaches here; a statically linked build also reaches here when the host started with it off.
	if (!Settings::Get<EnableExternalAccessSetting>(context)) {
		throw PermissionException("Cannot attach %s: the host disallows external access (enable_external_access is "
		                          "false), so no new Redshift handle can be attached. Handles attached before it "
		                          "was disabled keep working.",
		                          HandleInMessage(handle));
	}

	// DuckDB attaches an ephemeral database only for CONNECT '<string>' and CONNECT TO ... EXTERNAL
	// RESOURCE (PhysicalConnect), neither of which this release supports. A string whose
	// `redshift:` prefix DuckDB strips would otherwise arrive here with an empty path.
	if (options.ephemeral) {
		throw InvalidConfigurationException("Invalid CONNECT: CONNECT to a string or an external resource is not "
		                                    "supported for %s. Attach a handle, then CONNECT to it: ATTACH '' AS "
		                                    "<handle> (TYPE %s, ...), then CONNECT <handle>",
		                                    REDSHIFT_ATTACH_TYPE, REDSHIFT_ATTACH_TYPE);
	}

	// DuckDB names an ATTACH without AS from its path before this callback runs, in the field AS fills,
	// so the callback cannot tell whether AS was given. It names the empty path
	// `memory`, so refusing that name makes AS required for the empty path. A path ATTACH without AS
	// keeps the name DuckDB derives, the path's last `/` segment up to its first `.`, unless that is
	// `memory` too. Refusing the name keeps the main database too: ATTACH OR REPLACE '' would
	// otherwise replace it, and DuckDB swaps in the replacement only after this callback returns. The
	// remedy keeps a path's place with a placeholder, since the path itself is not repeated.
	if (StringUtil::CIEquals(name, DEFAULT_HANDLE_NAME)) {
		throw InvalidConfigurationException("Invalid ATTACH: a Redshift handle cannot be named %s, which is DuckDB's "
		                                    "default database; name the handle: ATTACH '%s' AS <handle> (TYPE %s, ...)",
		                                    SQLIdentifier::ToString(name), info.path.empty() ? "" : "<path>",
		                                    REDSHIFT_ATTACH_TYPE);
	}
	optional_ptr<const string> secret_name;

	// Names are checked before SECRET's value, and in sorted order, so that an ATTACH with several
	// problems always reports the same one: the options arrive in an unordered map, without the
	// order they were written in. An unknown name that asks for a sign-in other than IAM is reported
	// before any other, so an option whose name asks for one gets the hint whatever sorts before it.
	vector<string> option_keys;
	for (auto &option : options.options) {
		option_keys.push_back(option.first);
	}
	std::sort(option_keys.begin(), option_keys.end());
	optional_ptr<const string> first_unknown_key;
	for (auto &key : option_keys) {
		if (key == SECRET_OPTION ||
		    std::any_of(std::begin(REDSHIFT_CONFIG_PARAMETERS), std::end(REDSHIFT_CONFIG_PARAMETERS),
		                [&](const RedshiftConfigParameter &parameter) { return key == parameter.name; }) ||
		    IsAwsFormOption(key)) {
			continue;
		}
		const auto iam_only = IamOnlyHint(key);
		if (!iam_only.empty()) {
			ThrowForHandle(handle, "Invalid ATTACH option '%s': %s", StringUtil::Upper(key), iam_only);
		}
		if (!first_unknown_key) {
			first_unknown_key = &key;
		}
	}
	if (first_unknown_key) {
		ThrowForHandle(handle, "Invalid ATTACH option '%s'", StringUtil::Upper(*first_unknown_key));
	}
	auto secret = options.options.find(SECRET_OPTION);
	if (secret != options.options.end()) {
		// DuckDB refuses a NULL option before the attach callback runs. The value is never repeated:
		// it can be any expression, such as a STRUCT of credentials a user meant to pass inline.
		if (secret->second.type().id() != LogicalTypeId::VARCHAR) {
			ThrowForHandle(
			    handle,
			    "Invalid value for parameter 'SECRET': it must be the name of a DuckDB secret of type aws or s3");
		}
		secret_name = &StringValue::Get(secret->second);
	}
	AwsFormOptions aws_forms;
	const auto attach_options = ReadRedshiftAttachOptions(handle, info.path, options.options, aws_forms);

	// The handle cannot be read-only: redshift_query sends its SQL to Redshift unchanged, so
	// nothing could keep it from writing, and DuckDB would still report the database as read-only.
	// Each access-mode option written on this ATTACH is checked on its own value first, so the
	// refusal names the one that asks for read-only even when several are given. DuckDB has already
	// cast each value to BOOLEAN once, so the cast here cannot fail.
	for (auto &option : ACCESS_MODE_OPTIONS) {
		auto entry = info.options.find(option.name);
		if (entry == info.options.end()) {
			continue;
		}
		if (BooleanValue::Get(entry->second.DefaultCastAs(LogicalType::BOOLEAN)) == option.read_only_value) {
			ThrowForHandle(handle, "Invalid value %s for ATTACH option '%s': a Redshift handle cannot be read-only. %s",
			               entry->second.ToString(), StringUtil::Upper(option.name), READ_ONLY_ACCESS_HINT);
		}
	}
	// The configuration is resolved before the inherited mode is checked, so that a configuration
	// error is reported first: in a read-only DuckDB, the READ_WRITE hint then comes last.
	auto config = ResolveRedshiftAttachConfig(context, handle, info.path, attach_options, secret_name, aws_forms);
	// With no option asking for it, a read-only mode is the default of a DuckDB opened read-only.
	if (options.access_mode == AccessMode::READ_ONLY) {
		ThrowForHandle(handle,
		               "this DuckDB is read-only, so ATTACH defaults to read-only, and a Redshift handle cannot be "
		               "read-only. Add READ_WRITE to the ATTACH options. %s",
		               READ_ONLY_ACCESS_HINT);
	}

	// The database's map of Redshift connections, which every DuckDB connection's state holds. The
	// state also notes the handle, so a rollback of the transaction that attached it retires it
	// (RedshiftClientState::TransactionRollback): DuckDB calls no OnDetach then.
	auto &client_state = RedshiftClientState::Get(context);
	auto catalog = make_uniq<RedshiftCatalog>(db, std::move(config), client_state.ConnectionMap());
	// The attaching DuckDB connection's session opens now, so a target, database or credentials the
	// client cannot connect with fail the ATTACH rather than its first statement. When the open
	// fails, the catalog's destructor forgets the handle, which nothing has listed or noted yet.
	// DuckDB refuses a plain ATTACH of a name in use only after this callback returns, so that
	// ATTACH opens nothing, and DuckDB's refusal is the one reported.
	const bool name_in_use = info.on_conflict == OnCreateConflict::ERROR_ON_CONFLICT &&
	                         DatabaseManager::Get(context).GetDatabase(Identifier(name)) != nullptr;
	try {
		if (!name_in_use) {
			catalog->OpenAttachingConnection(context);
		}
	} catch (std::exception &ex) {
		ErrorData error(ex);
		const auto type = error.Type();
		if (ShouldRethrowErrorAsIs(type)) {
			throw;
		}
		// Named as every ATTACH refusal is, with the open's own type, extra_info and SQLSTATE line kept.
		throw Exception(error.ExtraInfo(), type,
		                StringUtil::Format("Cannot attach %s: %s", HandleInMessage(handle), error.RawMessage()));
	}
	client_state.NoteAttach(catalog->HandleId());
	return std::move(catalog);
}

unique_ptr<TransactionManager> RedshiftCreateTransactionManager(optional_ptr<StorageExtensionInfo> storage_info,
                                                                AttachedDatabase &db, Catalog &catalog) {
	return make_uniq<RedshiftTransactionManager>(db);
}

} // namespace

void RegisterRedshiftAttach(ExtensionLoader &loader) {
	auto storage_extension = make_shared_ptr<StorageExtension>();
	storage_extension->attach = RedshiftAttach;
	storage_extension->create_transaction_manager = RedshiftCreateTransactionManager;
	StorageExtension::Register(DBConfig::GetConfig(loader.GetDatabaseInstance()), REDSHIFT_ATTACH_TYPE,
	                           std::move(storage_extension));
}

} // namespace duckdb
