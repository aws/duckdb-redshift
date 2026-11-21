//===----------------------------------------------------------------------===//
// The ATTACH statements written for the aws extension's Redshift ATTACH, translated into this
// extension's own forms (redshift_attach_aws_forms.hpp).
//===----------------------------------------------------------------------===//

#include "redshift_attach_aws_forms.hpp"

#include <algorithm>
#include <iterator>

#include "duckdb/common/exception.hpp"
#include "duckdb/common/file_system.hpp"
#include "duckdb/common/string_util.hpp"
#include "duckdb/main/client_context.hpp"
#include "redshift_attach.hpp"
#include "redshift_aws_secret.hpp"

namespace duckdb {

namespace {

//! Why a HOST that is not the endpoint of the target the ATTACH names is refused. HOST is not repeated:
//! it may be anything.
constexpr const char *HOST_NOT_THE_ENDPOINT_REASON =
    "the connection verifies the server's certificate against that name, so it cannot go through another address, "
    "such as a tunnel, a proxy or an IP address";

//! The refusal of a HOST beside a cluster identifier that is not that cluster's endpoint. The remedy names
//! this extension's own forms, as every refusal does: leaving HOST out would keep the aws extension's form.
[[noreturn]] void ThrowHostNotTheCluster() {
	throw InvalidConfigurationException("HOST must be the endpoint of the cluster the path names, "
	                                    "<cluster>.<label>.<region>.redshift.amazonaws.com: %s. %s",
	                                    HOST_NOT_THE_ENDPOINT_REASON, RedshiftClusterFormsHint());
}

//! The refusal of a HOST beside a redshift:// path that is not the path's host. HOST can only repeat the
//! path there, so leaving it out loses nothing.
[[noreturn]] void ThrowHostNotThePath() {
	throw InvalidConfigurationException("HOST must be the host the path or ARN names: %s. Leave HOST out",
	                                    HOST_NOT_THE_ENDPOINT_REASON);
}

//! Whether `value` is a TCP port, 1 to 65535, written as an integer or as a string of decimal digits,
//! and the port in `port`.
bool ReadPort(const Value &value, int &port) {
	if (value.type().id() == LogicalTypeId::VARCHAR) {
		const auto &digits = StringValue::Get(value);
		// At most five digits, as a redshift:// URI's port, so a zero-padded port such as '005439' is refused.
		if (digits.empty() || digits.size() > 5 || digits.find_first_not_of("0123456789") != string::npos) {
			return false;
		}
	} else if (!value.type().IsIntegral()) {
		return false;
	}
	auto as_bigint = value.DefaultTryCastAs(LogicalType::BIGINT);
	if (!as_bigint) {
		return false;
	}
	const auto number = as_bigint->GetValue<int64_t>();
	if (number < 1 || number > 65535) {
		return false;
	}
	port = static_cast<int>(number);
	return true;
}

//! The string value of option `name`, removed from `options`; false when it is not given. DuckDB refuses a
//! NULL option before the attach callback runs.
bool TakeString(unordered_map<string, Value> &options, const char *name, string &field) {
	auto entry = options.find(name);
	if (entry == options.end()) {
		return false;
	}
	const auto option_name = StringUtil::Upper(name);
	if (entry->second.type().id() != LogicalTypeId::VARCHAR) {
		throw InvalidConfigurationException(
		    "Invalid value for parameter '%s': it must be a string; quote it, like %s '<value>'", option_name,
		    option_name);
	}
	field = StringValue::Get(entry->second);
	options.erase(entry);
	return true;
}

//! The region the aws extension falls back to for a cluster identifier when neither REGION nor the AWS
//! secret gives one (duckdb-aws 1b96925 ResolveAwsRegion, src/utils/utils.cpp): the s3_region setting,
//! then AWS_REGION, then AWS_DEFAULT_REGION, the first that is set and not empty. Not the AWS profile's
//! region, which needs the AWS SDK's configuration reader. Empty when none is set.
string RegionFromSettingOrEnvironment(ClientContext &context) {
	Value s3_region;
	if (context.TryGetCurrentSetting("s3_region", s3_region) && !s3_region.IsNull() && !s3_region.ToString().empty()) {
		return s3_region.ToString();
	}
	// Read as DuckDB reads the environment, in UTF-8 on every platform. Like the aws extension's and
	// DuckDB's own reads, it does not take the client's environment lock, which only the client's
	// writes and reads share.
	for (auto variable : {"AWS_REGION", "AWS_DEFAULT_REGION"}) {
		auto value = FileSystem::GetEnvVariable(variable);
		if (!value.empty()) {
			return value;
		}
	}
	return string();
}

//! The endpoint HOST names, read as a `redshift://` path's host is, in any letter case and with one root
//! dot; false for any other host. Only letters, digits, '.' and '-' are read, so HOST cannot add a port,
//! a database or user information to the URI it is read in.
bool ParseHostAsEndpoint(const string &host, RedshiftPathTarget &target) {
	for (auto c : host) {
		if (!StringUtil::CharacterIsAlphaNumeric(c) && c != '.' && c != '-') {
			return false;
		}
	}
	try {
		target = ParseRedshiftEndpointUri(string("redshift://") + host);
	} catch (InvalidConfigurationException &) {
		return false;
	}
	return true;
}

//! A cluster identifier path: the cluster it names, pinned to HOST when HOST is its own endpoint, with
//! the aws extension's secret choice and region fallback.
void TranslateClusterIdentifier(ClientContext &context, const string &cluster, optional_ptr<const string> secret_name,
                                const AwsFormOptions &aws_forms, AwsFormTranslation &translation) {
	auto &options = translation.options;
	// A lower-case word meant as a database or handle name reads as a cluster identifier, so the refusal
	// also names the form that takes the target as options.
	if (!options.workgroup.empty() || !options.account_id.empty() || !options.cluster.empty()) {
		throw InvalidConfigurationException("the path names the target, so WORKGROUP, ACCOUNT_ID and CLUSTER cannot "
		                                    "be given too; to give the target as options, leave the path empty: "
		                                    "ATTACH '' AS <handle> (TYPE %s, ...)",
		                                    REDSHIFT_ATTACH_TYPE);
	}
	if (aws_forms.host.empty()) {
		// Without HOST the client looks the cluster up and connects at the port the lookup reports, so PORT
		// could not be honoured.
		if (aws_forms.port != 0) {
			throw InvalidConfigurationException(
			    "Invalid parameter 'PORT': without HOST the cluster is looked up and connected to at the port it "
			    "listens on, so PORT cannot be honoured. Give the cluster's endpoint and port as the path: ATTACH "
			    "'redshift://<cluster>.<label>.<region>.redshift.amazonaws.com:<port>' AS <handle> (TYPE %s, ...)",
			    REDSHIFT_ATTACH_TYPE);
		}
		translation.path.clear();
		options.cluster = cluster;
	} else {
		// HOST pins the connection to the cluster's own endpoint, as a redshift:// cluster endpoint does, and
		// gives the region; the client connects to exactly that host and port, 5439 without PORT.
		RedshiftPathTarget named;
		if (!ParseHostAsEndpoint(aws_forms.host, named) || named.cluster != cluster) {
			ThrowHostNotTheCluster();
		}
		if (!options.region.empty() && options.region != named.region) {
			throw InvalidConfigurationException("REGION names another region than HOST; give one, or make them agree");
		}
		translation.path =
		    "redshift://" + named.host + ":" + to_string(aws_forms.port != 0 ? aws_forms.port : named.endpoint_port);
	}

	// The credential source, as the aws extension chooses it: the only aws or s3 secret the user created
	// (FindOnlyUserAwsSecret), else the default credential chain, unless aws_default_credentials switches
	// that off.
	if (!secret_name && options.credential_provider.empty()) {
		// Refused before a secret is chosen: PROFILE asks for a profile, and choosing a secret would answer
		// with a refusal about SECRET instead (ValidateRedshiftOptions refuses it beside a secret too).
		if (!options.profile.empty()) {
			throw InvalidConfigurationException(REDSHIFT_PROFILE_WITHOUT_PROFILE_PROVIDER);
		}
		options.aws_secret = FindOnlyUserAwsSecret(context);
		if (options.aws_secret.empty()) {
			// The attach callback has already refused the ATTACH while external access is off, so this is
			// aws_default_credentials alone.
			if (!AwsDefaultCredentialsEnabled(context)) {
				throw InvalidConfigurationException(
				    "Missing required parameter 'SECRET': there is no aws or s3 secret to use, and "
				    "aws_default_credentials is false, so the default credential chain is not used either. Name a "
				    "DuckDB aws or s3 secret in SECRET, or set CREDENTIAL_PROVIDER");
			}
			options.credential_provider = REDSHIFT_PARAM_CREDENTIAL_PROVIDER_AWS_DEFAULT_CHAIN;
		}
	}

	// The region: REGION, then HOST's (the path's), then the AWS secret's, then s3_region and the
	// environment. With no AWS secret to read, the fallback comes now; with one, after its region setting.
	if (!options.region.empty() || !translation.path.empty()) {
		return;
	}
	const auto fallback = RegionFromSettingOrEnvironment(context);
	if (secret_name || !options.aws_secret.empty()) {
		translation.fallback_region = make_uniq<string>(fallback);
		return;
	}
	if (fallback.empty()) {
		throw InvalidConfigurationException("Missing required parameter 'REGION': set REGION, the s3_region setting, "
		                                    "or AWS_REGION or AWS_DEFAULT_REGION in the environment");
	}
	options.region = fallback;
}

} // namespace

bool IsAwsFormOption(const string &key) {
	if (key == AWS_FORM_HOST || key == AWS_FORM_PORT || key == AWS_FORM_DBNAME) {
		return true;
	}
	return std::any_of(std::begin(IGNORED_ATTACH_OPTIONS), std::end(IGNORED_ATTACH_OPTIONS),
	                   [&](const char *name) { return key == name; });
}

AwsFormOptions TakeAwsFormOptions(const string &path, unordered_map<string, Value> &options) {
	AwsFormOptions aws_forms;
	if (TakeString(options, AWS_FORM_HOST, aws_forms.host) && aws_forms.host.empty()) {
		throw InvalidConfigurationException("Invalid value '' for parameter 'HOST': it cannot be empty");
	}
	auto port = options.find(AWS_FORM_PORT);
	if (port != options.end()) {
		if (!ReadPort(port->second, aws_forms.port)) {
			throw InvalidConfigurationException(
			    "Invalid value for parameter 'PORT': it must be a number from 1 to 65535");
		}
		options.erase(port);
	}
	if (TakeString(options, AWS_FORM_DBNAME, aws_forms.dbname) && aws_forms.dbname.empty() &&
	    !IsClusterIdentifierPath(path)) {
		throw InvalidConfigurationException("Invalid value '' for parameter 'DBNAME': it cannot be empty");
	}
	// Beside a cluster identifier, the aws extension's form, an empty DATABASE means none, as that extension
	// reads it; beside any other path ReadRedshiftAttachOptions refuses it, as it refuses any empty option.
	auto database = options.find(REDSHIFT_PARAM_DATABASE);
	if (IsClusterIdentifierPath(path) && database != options.end() &&
	    database->second.type().id() == LogicalTypeId::VARCHAR && StringValue::Get(database->second).empty()) {
		options.erase(database);
	}
	for (auto ignored : IGNORED_ATTACH_OPTIONS) {
		options.erase(ignored);
	}
	return aws_forms;
}

AwsFormTranslation TranslateAwsForms(ClientContext &context, const string &path, const RedshiftPathTarget *uri_target,
                                     const RedshiftConfigOptions &options, optional_ptr<const string> secret_name,
                                     const AwsFormOptions &aws_forms) {
	AwsFormTranslation translation;
	translation.path = path;
	translation.options = options;
	const bool cluster_identifier = IsClusterIdentifierPath(path);

	// DBNAME is DATABASE under the aws extension's name, beside every form; an empty one is none, which
	// TakeAwsFormOptions allows only beside a cluster identifier.
	if (!aws_forms.dbname.empty()) {
		auto &database = translation.options.database;
		if (!database.empty() && database != aws_forms.dbname) {
			// Neither is echoed, as DATABASE is not.
			throw InvalidConfigurationException("DATABASE and DBNAME name different databases; give one");
		}
		if (uri_target && !uri_target->database.empty()) {
			throw InvalidConfigurationException("the path names the database, so DBNAME cannot be given too");
		}
		database = aws_forms.dbname;
	}

	if (cluster_identifier) {
		TranslateClusterIdentifier(context, path, secret_name, aws_forms, translation);
		return translation;
	}
	if (uri_target) {
		// Beside a redshift:// path, which names the endpoint and its port, HOST and PORT may only repeat it:
		// that is how duckdb/duckdb-aws#259 hands on an ARN it resolved, with the user's options as written.
		RedshiftPathTarget named;
		if (!aws_forms.host.empty() &&
		    (!ParseHostAsEndpoint(aws_forms.host, named) || named.host != uri_target->host)) {
			ThrowHostNotThePath();
		}
		if (aws_forms.port != 0 && (!uri_target->port_given || aws_forms.port != uri_target->endpoint_port)) {
			throw InvalidConfigurationException(
			    "PORT must be the port the path or ARN gives. Leave PORT out, or give the port in the path: ATTACH "
			    "'redshift://<endpoint>:<port>' AS <handle> (TYPE %s, ...)",
			    REDSHIFT_ATTACH_TYPE);
		}
		return translation;
	}
	// The empty path and the long-form workgroup ARN give no endpoint for HOST and PORT to repeat.
	if (!aws_forms.host.empty() || aws_forms.port != 0) {
		throw InvalidConfigurationException(
		    "Invalid parameter '%s': give the endpoint, and its port, as the path instead: ATTACH "
		    "'redshift://<endpoint>[:<port>]' AS <handle> (TYPE %s, ...); without one, WORKGROUP and ACCOUNT_ID or "
		    "CLUSTER name the endpoint",
		    aws_forms.host.empty() ? "PORT" : "HOST", REDSHIFT_ATTACH_TYPE);
	}
	return translation;
}

void ThrowMissingFallbackRegion(const string &aws_secret) {
	throw InvalidConfigurationException("Missing required parameter 'REGION': set REGION, include a region in the aws "
	                                    "or s3 secret '%s', or set the s3_region setting, or AWS_REGION or "
	                                    "AWS_DEFAULT_REGION in the environment",
	                                    aws_secret);
}

} // namespace duckdb
