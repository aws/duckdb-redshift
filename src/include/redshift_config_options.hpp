//===----------------------------------------------------------------------===//
//                         DuckDB - Redshift
//
// redshift_config_options.hpp
//
// The configuration options a `redshift` ATTACH takes, and the rules they pass. There is no
// `redshift` secret type: an ATTACH's SECRET names a DuckDB `aws` or `s3` secret, which gives
// the AWS credentials (redshift_aws_secret.hpp).
//===----------------------------------------------------------------------===//

#pragma once

#include "duckdb/common/common.hpp"
#include "redshift_transport.hpp"

namespace duckdb {

//! The configuration options of an ATTACH, each followed by its values where it has a fixed set. A
//! value carries its option's name as its prefix. Lowercase, as DuckDB's parser lowercases ATTACH
//! option names.
constexpr const char *REDSHIFT_PARAM_REGION = "region";

constexpr const char *REDSHIFT_PARAM_CREDENTIAL_PROVIDER = "credential_provider";
//! The client's default AWS credential provider chain. Values are stored lowercase whatever
//! case they were written in.
constexpr const char *REDSHIFT_PARAM_CREDENTIAL_PROVIDER_AWS_DEFAULT_CHAIN = "aws_default_chain";
//! A named AWS profile, given by PROFILE.
constexpr const char *REDSHIFT_PARAM_CREDENTIAL_PROVIDER_PROFILE = "profile";

constexpr const char *REDSHIFT_PARAM_PROFILE = "profile";

//! The target: a Serverless WORKGROUP with the ACCOUNT_ID that owns it, or a provisioned CLUSTER.
//! DATABASE goes with either.
constexpr const char *REDSHIFT_PARAM_WORKGROUP = "workgroup";
constexpr const char *REDSHIFT_PARAM_ACCOUNT_ID = "account_id";
constexpr const char *REDSHIFT_PARAM_CLUSTER = "cluster";
constexpr const char *REDSHIFT_PARAM_DATABASE = "database";

//! The refusal of PROFILE beside any credential source but CREDENTIAL_PROVIDER profile: ValidateRedshiftOptions'
//! and, before it chooses a secret, the aws extension's cluster identifier form's (redshift_attach_aws_forms.cpp).
constexpr const char *REDSHIFT_PROFILE_WITHOUT_PROFILE_PROVIDER =
    "Invalid parameter 'PROFILE': it is only valid with CREDENTIAL_PROVIDER profile";

//! An ATTACH's configuration, from its options and its path, before ValidateRedshiftOptions. An empty
//! field means the option was not given; the ATTACH refuses an explicitly empty value itself, before
//! filling this in.
struct RedshiftConfigOptions {
	string region;
	string credential_provider;
	string profile;
	//! Not a configuration option: the name SECRET gives, of the DuckDB `aws` or `s3` secret whose AWS
	//! credentials the connection uses. Never its contents.
	string aws_secret;
	string workgroup;
	string account_id;
	string cluster;
	string database;
	//! Not parameters: set only from an ATTACH path (redshift_attach_path.hpp) that names a workgroup
	//! endpoint with no account (the label) or a cluster endpoint (the label and the port), and a
	//! workgroup endpoint's port when its URI gives one. Empty and 0 from the options.
	string endpoint_label;
	int endpoint_port = 0;

	//! Whether the options name a target: a workgroup or a cluster. ACCOUNT_ID alone names none;
	//! ValidateRedshiftOptions refuses it.
	bool NamesTarget() const {
		return !workgroup.empty() || !cluster.empty();
	}
};

//! A configuration option: its ATTACH option name, and the field of RedshiftConfigOptions it fills.
struct RedshiftConfigParameter {
	const char *name;
	string RedshiftConfigOptions::*field;
};

//! Every configuration option, the list ATTACH reads as strings. It accepts these, SECRET, and the
//! options of the aws extension's forms (IsAwsFormOption, redshift_attach_aws_forms.hpp).
constexpr RedshiftConfigParameter REDSHIFT_CONFIG_PARAMETERS[] = {
    {REDSHIFT_PARAM_REGION, &RedshiftConfigOptions::region},
    {REDSHIFT_PARAM_CREDENTIAL_PROVIDER, &RedshiftConfigOptions::credential_provider},
    {REDSHIFT_PARAM_PROFILE, &RedshiftConfigOptions::profile},
    {REDSHIFT_PARAM_WORKGROUP, &RedshiftConfigOptions::workgroup},
    {REDSHIFT_PARAM_ACCOUNT_ID, &RedshiftConfigOptions::account_id},
    {REDSHIFT_PARAM_CLUSTER, &RedshiftConfigOptions::cluster},
    {REDSHIFT_PARAM_DATABASE, &RedshiftConfigOptions::database}};

//! The rules an ATTACH's configuration passes before anything is read. The AWS credentials come from
//! exactly one source: SECRET (`aws_secret`), the name of a DuckDB `aws` or `s3` secret that a message
//! can repeat (at most 255 bytes, no control byte or '}', no leading or trailing space), or
//! CREDENTIAL_PROVIDER, aws_default_chain or profile (ASCII case-insensitive, lowercased in place),
//! with PROFILE exactly when it is profile; beside SECRET, both are refused. REGION is given, except
//! with SECRET, where the ATTACH then takes the AWS secret's. Then the target rules, which say which
//! settings go together, not what each value may hold: WORKGROUP and CLUSTER are not both given,
//! ACCOUNT_ID goes only with WORKGROUP, WORKGROUP is a name rather than an ARN, and a WORKGROUP without
//! ACCOUNT_ID needs an endpoint label from the path or the accountless-workgroup hook, which then
//! refuses DATABASE. Last, an ACCOUNT_ID of exactly 12 ASCII digits, refused without repeating the
//! value (the client checks it again at connect). Returns the target, UNSPECIFIED when the options
//! name none, which ResolveRedshiftAttachConfig refuses; otherwise throws an
//! InvalidConfigurationException that names the option. Reads nothing, the AWS secret included.
RedshiftTarget ValidateRedshiftOptions(RedshiftConfigOptions &options);

//! The hint that ends the refusal of a CREDENTIAL_PROVIDER value or an ATTACH option name asking for a
//! sign-in other than IAM, such as one of the Redshift driver's own sign-in plugins or a database
//! password: that only IAM is supported. Empty for any other text. It never repeats `text`.
string IamOnlyHint(const string &text);

} // namespace duckdb
