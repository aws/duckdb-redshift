// ATTACH configuration: the options and the path, the credential-source and target rules, and the
// refusals. The resolver is called directly, inside a transaction as the ATTACH callback is. No
// connection is involved.

#include "catch.hpp"

#include <cstdlib>
#include <initializer_list>
#include <tuple>
#include <utility>

#include "duckdb.hpp"
#include "duckdb/main/config.hpp"
#include "redshift_attach.hpp"
#include "redshift_aws_secret.hpp"
#include "duckdb/common/sql_identifier.hpp"
#include "duckdb/common/string_util.hpp"
#include "redshift_extension.hpp"
#include "redshift_target_hook.hpp"
#include "aws_secret_stand_ins.hpp"
#include "test_log_storage.hpp"
#include "test_util.hpp"

using namespace duckdb;

namespace {

//! The handle every resolution here is for, which each refusal names.
const char *const HANDLE = "rs";

//! ATTACH options as DuckDB delivers them to the callback: lowercase names, typed values.
using AttachOptionMap = unordered_map<string, Value>;

AttachOptionMap Options(std::initializer_list<std::pair<const char *, const char *>> entries) {
	AttachOptionMap options;
	for (auto &entry : entries) {
		options[StringUtil::Lower(entry.first)] = Value(entry.second);
	}
	return options;
}

//! The configuration options of an ATTACH of `path`, as the ATTACH callback reads them.
RedshiftConfigOptions ReadOptions(const AttachOptionMap &options, const string &path = "") {
	AwsFormOptions aws_forms;
	return ReadRedshiftAttachOptions(HANDLE, path, options, aws_forms);
}

//! An in-memory database with the extension loaded, and a connection to create secrets on.
//! Persistent secrets are off, so a secret stored in ~/.duckdb/stored_secrets never reaches a test.
struct Fixture {
	DuckDB db {nullptr, redshift_test::NoPersistentSecrets().get()};
	Connection con {db};

	Fixture() {
		db.LoadStaticExtension<RedshiftExtension>();
	}

	void Run(const string &sql) {
		auto result = con.Query(sql);
		INFO(sql << ": " << redshift_test::ErrorIn(*result));
		REQUIRE_FALSE(result->HasError());
	}

	//! `secret_name` is SECRET's value, null for an ATTACH without SECRET.
	RedshiftConfig Resolve(const AttachOptionMap &options, const char *secret_name = nullptr, const string &path = "") {
		const string name = secret_name ? secret_name : "";
		optional_ptr<const string> secret = secret_name ? &name : nullptr;
		RedshiftConfig config;
		con.context->RunFunctionInTransaction([&] {
			AwsFormOptions aws_forms;
			const auto attach_options = ReadRedshiftAttachOptions(HANDLE, path, options, aws_forms);
			config = ResolveRedshiftAttachConfig(*con.context, HANDLE, path, attach_options, secret, aws_forms);
		});
		return config;
	}

	//! The message of the refusal resolving throws, without the "Cannot attach rs: " every one
	//! starts with. Fails the test unless it is an InvalidConfigurationException that names the
	//! handle.
	string Refusal(const AttachOptionMap &options, const char *secret_name = nullptr, const string &path = "") {
		auto thrown = redshift_test::Capture([&] { Resolve(options, secret_name, path); });
		INFO("secret " << (secret_name ? secret_name : "(none)") << ": " << thrown.message);
		REQUIRE(thrown.type == ExceptionType::INVALID_CONFIGURATION);
		const string prefix = "Cannot attach rs: ";
		REQUIRE(thrown.message.compare(0, prefix.size(), prefix) == 0);
		return thrown.message.substr(prefix.size());
	}
};

//! Options for a complete configuration with the default credential chain and `target`.
AttachOptionMap Chain(std::initializer_list<std::pair<const char *, const char *>> target) {
	auto options = Options({{"REGION", "us-east-1"}, {"CREDENTIAL_PROVIDER", "aws_default_chain"}});
	for (auto &entry : Options(target)) {
		options[entry.first] = entry.second;
	}
	return options;
}

//! Whether `config` holds the given region, profile, target and database fields. It leaves out
//! endpoint_label, aws_secret and port, which each test that sets one checks itself, and
//! CheckDefaults checks unset.
void CheckConfig(const RedshiftConfig &config, const string &region, const string &profile, RedshiftTarget target,
                 const string &workgroup, const string &account_id, const string &cluster, const string &database) {
	CHECK(config.region == region);
	CHECK(config.profile == profile);
	CHECK(config.target == target);
	CHECK(config.workgroup == workgroup);
	CHECK(config.account_id == account_id);
	CHECK(config.cluster == cluster);
	CHECK(config.database == database);
}

//! Whether `config` leaves endpoint_label, aws_secret and port, which CheckConfig does not check, unset.
void CheckDefaults(const RedshiftConfig &config) {
	CHECK(config.endpoint_label.empty());
	CHECK(config.aws_secret.empty());
	CHECK(config.endpoint_port == 0);
}

const char *const ACCOUNT = "012345678901";

const char *const BAD_ACCOUNT = "Invalid value for parameter 'ACCOUNT_ID': it must be exactly 12 decimal digits";
const char *const MISSING_REGION = "Missing required parameter 'REGION': the AWS region of the target Redshift";
const char *const MISSING_CREDENTIAL_PROVIDER =
    "Missing required parameter 'CREDENTIAL_PROVIDER': supported values {aws_default_chain, profile}, or name a "
    "DuckDB aws or s3 secret in SECRET";
const char *const MISSING_PROFILE =
    "Missing required parameter 'PROFILE': CREDENTIAL_PROVIDER profile needs the name of an AWS profile";
const char *const PROFILE_WITHOUT_PROFILE_PROVIDER =
    "Invalid parameter 'PROFILE': it is only valid with CREDENTIAL_PROVIDER profile";
const char *const MISSING_TARGET =
    "Missing target: give WORKGROUP with ACCOUNT_ID for a Serverless workgroup or CLUSTER "
    "for a provisioned cluster, or the endpoint as a redshift:// path";
const char *const MISSING_ACCOUNT =
    "Missing required parameter 'ACCOUNT_ID': WORKGROUP needs the 12-digit ID of the AWS account that owns the "
    "workgroup";
const char *const ACCOUNT_WITHOUT_WORKGROUP = "Invalid parameter 'ACCOUNT_ID': it is only valid with WORKGROUP";
//! The refusal of a SECRET that names no secret, which repeats no value: it may be a pasted key.
const char *const SECRET_NOT_FOUND =
    "the aws or s3 secret the ATTACH named in SECRET, or chose for a cluster identifier path, does not exist: "
    "SECRET must name a DuckDB secret of type aws or s3";
//! The hint that ends a refusal asking for a sign-in other than IAM.
const char *const IAM_ONLY =
    "only IAM authentication is supported, with AWS credentials from CREDENTIAL_PROVIDER or from a DuckDB aws or s3 "
    "secret in SECRET, which can hold those of an IAM Identity Center sign-in; the Redshift driver's own sign-in "
    "plugins, for IAM Identity Center or an identity provider such as Okta, and a database user and password are not";
const char *const BOTH_TARGETS = "Invalid parameter 'CLUSTER': it is not valid with WORKGROUP. Give one target: a "
                                 "Serverless WORKGROUP with its ACCOUNT_ID, or a provisioned CLUSTER";

} // namespace

TEST_CASE("options alone are the whole configuration, field by field", "[attach][config]") {
	Fixture f;
	CheckConfig(f.Resolve(Chain({{"WORKGROUP", "wg"}, {"ACCOUNT_ID", ACCOUNT}})), "us-east-1", "",
	            RedshiftTarget::SERVERLESS, "wg", ACCOUNT, "", "");
	CheckConfig(f.Resolve(Chain({{"WORKGROUP", "wg"}, {"ACCOUNT_ID", ACCOUNT}, {"DATABASE", "sales"}})), "us-east-1",
	            "", RedshiftTarget::SERVERLESS, "wg", ACCOUNT, "", "sales");
	CheckConfig(f.Resolve(Chain({{"CLUSTER", "etl"}})), "us-east-1", "", RedshiftTarget::PROVISIONED, "", "", "etl",
	            "");
	CheckConfig(f.Resolve(Options({{"REGION", "eu-west-1"},
	                               {"CREDENTIAL_PROVIDER", "PROFILE"},
	                               {"PROFILE", "My.Profile"},
	                               {"CLUSTER", "etl"},
	                               {"DATABASE", "Sales"}})),
	            "eu-west-1", "My.Profile", RedshiftTarget::PROVISIONED, "", "", "etl", "Sales");
	// Values are verbatim: encoding them is the client's job, and so is checking what each may hold, except
	// ACCOUNT_ID's 12 digits, which ATTACH checks itself.
	CheckConfig(f.Resolve(Chain({{"WORKGROUP", "W;x={y}"}, {"ACCOUNT_ID", ACCOUNT}})), "us-east-1", "",
	            RedshiftTarget::SERVERLESS, "W;x={y}", ACCOUNT, "", "");
	CHECK(f.Refusal(Chain({{"WORKGROUP", "wg"}, {"ACCOUNT_ID", "12345678901"}})) == BAD_ACCOUNT);
	// The fields no option sets stay unset.
	CheckDefaults(f.Resolve(Chain({{"WORKGROUP", "wg"}, {"ACCOUNT_ID", ACCOUNT}})));
	CheckDefaults(f.Resolve(Chain({{"CLUSTER", "etl"}})));
}

TEST_CASE("every target combination", "[attach][config]") {
	Fixture f;
	CHECK(f.Refusal(Chain({})) == MISSING_TARGET);
	CHECK(f.Refusal(Chain({{"DATABASE", "d"}})) == MISSING_TARGET);
	CHECK(f.Refusal(Chain({{"WORKGROUP", "wg"}, {"ACCOUNT_ID", ACCOUNT}, {"CLUSTER", "c"}})) == BOTH_TARGETS);
	CHECK(f.Refusal(Chain({{"WORKGROUP", "wg"}, {"CLUSTER", "c"}})) == BOTH_TARGETS);
	CHECK(f.Refusal(Chain({{"ACCOUNT_ID", ACCOUNT}})) == ACCOUNT_WITHOUT_WORKGROUP);
	CHECK(f.Refusal(Chain({{"ACCOUNT_ID", ACCOUNT}, {"CLUSTER", "c"}})) == ACCOUNT_WITHOUT_WORKGROUP);
	// This build's hook accepts no workgroup without ACCOUNT_ID, with or without DATABASE.
	CHECK(f.Refusal(Chain({{"WORKGROUP", "wg"}})) == MISSING_ACCOUNT);
	CHECK(f.Refusal(Chain({{"WORKGROUP", "wg"}, {"DATABASE", "d"}})) == MISSING_ACCOUNT);
	for (auto arn : {"arn:aws:redshift-serverless:us-east-1:012345678901:workgroup/abc", "ARN:x"}) {
		CHECK(f.Refusal(Chain({{"WORKGROUP", arn}, {"ACCOUNT_ID", ACCOUNT}})) ==
		      StringUtil::Format("Invalid value '%s' for parameter 'WORKGROUP': give the workgroup's name, not its "
		                         "ARN, and the account that owns it in ACCOUNT_ID",
		                         arn));
	}
	// The ARN is named even without ACCOUNT_ID, the likeliest way to paste one: the ARN check comes
	// first, so the user is not only told that ACCOUNT_ID is missing.
	CHECK(f.Refusal(Chain({{"WORKGROUP", "arn:aws:redshift-serverless:us-east-1:012345678901:workgroup/abc"}}))
	          .find("for parameter 'WORKGROUP': give the workgroup's name, not its ARN") != string::npos);
	// The prefix is "arn:", so a name that merely starts with "arn" is an ordinary workgroup.
	CheckConfig(f.Resolve(Chain({{"WORKGROUP", "arnold-wg"}, {"ACCOUNT_ID", ACCOUNT}})), "us-east-1", "",
	            RedshiftTarget::SERVERLESS, "arnold-wg", ACCOUNT, "", "");
}

TEST_CASE("options with no SECRET must be complete", "[attach][config]") {
	Fixture f;
	CHECK(f.Refusal({}) == MISSING_REGION);
	CHECK(f.Refusal(Options({{"CREDENTIAL_PROVIDER", "aws_default_chain"}, {"CLUSTER", "c"}})) == MISSING_REGION);
	CHECK(f.Refusal(Options({{"REGION", "us-east-1"}, {"CLUSTER", "c"}})) == MISSING_CREDENTIAL_PROVIDER);
	CHECK(f.Refusal(Options({{"REGION", "us-east-1"}, {"CREDENTIAL_PROVIDER", "profile"}, {"CLUSTER", "c"}})) ==
	      MISSING_PROFILE);
	// PROFILE alone names no credential provider; aws_default_chain takes none.
	CHECK(f.Refusal(Options({{"REGION", "us-east-1"}, {"PROFILE", "p2"}, {"CLUSTER", "c"}})) ==
	      MISSING_CREDENTIAL_PROVIDER);
	CHECK(f.Refusal(Chain({{"PROFILE", "p2"}, {"CLUSTER", "c"}})) == PROFILE_WITHOUT_PROFILE_PROVIDER);
	// aws_secret is not a credential provider: SECRET names the aws or s3 secret instead.
	CHECK(f.Refusal(Options({{"REGION", "us-east-1"}, {"CREDENTIAL_PROVIDER", "aws_secret"}, {"CLUSTER", "c"}})) ==
	      "Invalid value 'aws_secret' for parameter 'CREDENTIAL_PROVIDER': supported values {aws_default_chain, "
	      "profile}");
}

TEST_CASE("a credential provider or option that asks for a sign-in other than IAM says only IAM is supported",
          "[attach][config]") {
	// One name for each word the hint looks for, so that each word is needed.
	for (auto text : {"idc", "azure_idp", "web_identity", "sso", "browser", "token", "auth_type", "issuer_url",
	                  "plugin_name", "password", "passwd", "pwd", "db_user", "uid"}) {
		INFO(text);
		CHECK(IamOnlyHint(text) == IAM_ONLY);
	}
	CHECK(IamOnlyHint("Plugin_Name") == IAM_ONLY);
	// A word matches anywhere in a name, so these get the hint too, harmlessly: they are refused anyway.
	for (auto text : {"user_agent", "guid", "custom_domain_association"}) {
		INFO(text);
		CHECK(IamOnlyHint(text) == IAM_ONLY);
	}
	// The refusal keeps the value as written, and the hint repeats none of it.
	Fixture f;
	CHECK(f.Refusal(Options({{"REGION", "us-east-1"}, {"CREDENTIAL_PROVIDER", "Browser_IdC"}, {"CLUSTER", "c"}})) ==
	      string("Invalid value 'Browser_IdC' for parameter 'CREDENTIAL_PROVIDER': supported values "
	             "{aws_default_chain, profile}; ") +
	          IAM_ONLY);
}

TEST_CASE("a supported option or credential provider, or other text without one of the words, gets no IAM-only hint",
          "[attach][config]") {
	CHECK(IamOnlyHint("secret").empty());
	for (auto &parameter : REDSHIFT_CONFIG_PARAMETERS) {
		INFO(parameter.name);
		CHECK(IamOnlyHint(parameter.name).empty());
	}
	for (auto text : {REDSHIFT_PARAM_CREDENTIAL_PROVIDER_AWS_DEFAULT_CHAIN, REDSHIFT_PARAM_CREDENTIAL_PROVIDER_PROFILE,
	                  "aws_secret", "host", "port", ""}) {
		INFO(text);
		CHECK(IamOnlyHint(text).empty());
	}
	// Near misses: each cuts a word short, or, as ssl, changes a letter of one.
	for (auto text : {"id", "ssl", "identit", "browse", "tok", "aut", "issue", "plug", "pass", "pw", "use", "ui"}) {
		INFO(text);
		CHECK(IamOnlyHint(text).empty());
	}
	// So an unknown option without one of the words ends its refusal at its name.
	Fixture f;
	auto result = f.con.Query("ATTACH '' AS rs (TYPE redshift, REGION 'us-east-1', CREDENTIAL_PROVIDER "
	                          "aws_default_chain, CLUSTER 'c', HOSTNAME 'h')");
	REQUIRE(result->HasError());
	CHECK(StringUtil::EndsWith(result->GetError(), "Cannot attach rs: Invalid ATTACH option 'HOSTNAME'"));
}

TEST_CASE("an option value is a non-empty string, never cast", "[attach][config]") {
	for (auto name : {"region", "credential_provider", "profile", "workgroup", "account_id", "cluster", "database",
	                  "host", "dbname"}) {
		const auto upper = StringUtil::Upper(name);
		INFO(upper);
		for (auto &value : {Value::BIGINT(12345678901), Value::INTEGER(1), Value::BOOLEAN(true), Value::DOUBLE(1.5)}) {
			auto thrown = redshift_test::Capture([&] { ReadOptions({{name, value}}); });
			CHECK(thrown.type == ExceptionType::INVALID_CONFIGURATION);
			CHECK(thrown.message ==
			      StringUtil::Format("Cannot attach rs: Invalid value for parameter '%s': it must be a string; quote "
			                         "it, like %s '<value>'",
			                         upper, upper));
		}
		auto thrown = redshift_test::Capture([&] { ReadOptions({{name, Value("")}}); });
		CHECK(thrown.message ==
		      StringUtil::Format("Cannot attach rs: Invalid value '' for parameter '%s': it cannot be empty", upper));
		// Beside a cluster identifier path, an empty DATABASE or DBNAME is none, as the aws extension reads it
		// ("beside a cluster identifier, DBNAME is DATABASE, and an empty one is none").
		if (string(name) == "database" || string(name) == "dbname") {
			CHECK(ReadOptions({{name, Value("")}}, "my-cluster").database.empty());
			// Only an empty string is none there: any other value is still checked.
			auto not_string = redshift_test::Capture([&] { ReadOptions({{name, Value::INTEGER(1)}}, "my-cluster"); });
			CHECK(not_string.message ==
			      StringUtil::Format("Cannot attach rs: Invalid value for parameter '%s': it must be a string; quote "
			                         "it, like %s '<value>'",
			                         upper, upper));
		}
	}
	// Other options are not read here: the callback refuses unknown ones by name.
	auto options = ReadOptions({{"secret", Value::INTEGER(1)}, {"endpoint", Value("x")}});
	CHECK(options.region.empty());
}

TEST_CASE("a path other than an endpoint URI, a workgroup endpoint's ARN or a cluster identifier is refused",
          "[attach][config]") {
	Fixture f;
	const string refusal = "the path must be empty, a redshift:// endpoint or a workgroup endpoint's ARN. Give the "
	                       "configuration as options: ATTACH '' AS rs (TYPE redshift, REGION '<region>', ...), "
	                       "or the endpoint: ATTACH 'redshift://<endpoint>[:<port>][/<database>]' AS rs (TYPE "
	                       "redshift, ...)";
	// Every remedy names only this extension's own forms, never the aws extension's cluster identifier path,
	// which this extension takes for compatibility.
	CHECK(refusal.find("ATTACH '<cluster>'") == string::npos);
	// Whatever else the ATTACH holds, and without repeating the path. A path that is not a cluster
	// identifier by its rule, an upper-case one included, is not taken as one.
	for (auto path : {"region=us-east-1 credential_provider=aws_default_chain", " ", "PATH_SENTINEL", "My-Cluster",
	                  "1cluster", "my--cluster", "my-cluster-", "my_cluster", "my.cluster",
	                  "c234567890123456789012345678901234567890123456789012345678901234"}) {
		CHECK(f.Refusal(Chain({{"CLUSTER", "c"}}), nullptr, path) == refusal);
		CHECK(f.Refusal({}, "nope", path) == refusal);
	}
}

TEST_CASE("SECRET must name an existing aws or s3 secret", "[attach][config]") {
	Fixture f;
	redshift_test::RegisterAwsSecretStandIns(*f.db.instance);
	f.Run("CREATE SECRET web (TYPE http)");
	const auto complete = Options({{"REGION", "us-east-1"}, {"CLUSTER", "c"}});

	CHECK(f.Refusal(complete, "nope") == SECRET_NOT_FOUND);
	// An empty SECRET is not the same as no SECRET, even when the options alone are complete.
	CHECK(f.Refusal(Chain({{"CLUSTER", "c"}}), "") == "Invalid value '' for parameter 'SECRET': it cannot be empty");
	CHECK(f.Refusal(complete, "web") == "secret 'web' has type 'http': SECRET must name a secret of type aws or s3");
}

TEST_CASE("without SECRET, no aws or s3 secret is ever used", "[attach][config]") {
	Fixture f;
	redshift_test::RegisterAwsSecretStandIns(*f.db.instance);
	// Unnamed: DuckDB names it __default_aws, the secret a path-scoped lookup would pick.
	f.Run("CREATE SECRET (TYPE aws, KEY_ID 'k', SECRET 's', REGION 'us-east-1')");

	CHECK(f.Refusal({}) == MISSING_REGION);
	CHECK(f.Refusal(Options({{"REGION", "us-east-1"}, {"CLUSTER", "c"}})) == MISSING_CREDENTIAL_PROVIDER);

	// Named explicitly, it is an ordinary secret.
	CHECK(f.Resolve(Options({{"CLUSTER", "c"}}), "__default_aws").aws_secret == "__default_aws");
}

//===--------------------------------------------------------------------===//
// The ATTACH path forms that name a target: a redshift:// endpoint URI and a workgroup endpoint's
// long-form ARN (redshift_attach_path.cpp).
//===--------------------------------------------------------------------===//

namespace {

//! The default credential chain and nothing else: the path gives the target and region.
AttachOptionMap ChainOnly() {
	return Options({{"CREDENTIAL_PROVIDER", "aws_default_chain"}});
}

const char *const LABELLED_URI = "redshift://wg-a.lbl.us-east-1.redshift-serverless.amazonaws.com";
const char *const LONG_ARN = "arn:aws:redshift-serverless:us-east-1:012345678901:workgroup/lbl/wg-a";

} // namespace

TEST_CASE("a redshift:// endpoint names a workgroup with its account", "[attach][config][path]") {
	Fixture f;
	auto config =
	    f.Resolve(ChainOnly(), nullptr, "redshift://wg1.012345678901.us-east-1.redshift-serverless.amazonaws.com");
	CheckConfig(config, "us-east-1", "", RedshiftTarget::SERVERLESS, "wg1", ACCOUNT, "", "");
	CHECK(config.endpoint_label.empty());
	// The host in any letter case, with the workgroup port and a database.
	config = f.Resolve(ChainOnly(), nullptr,
	                   "REDSHIFT://WG1.012345678901.US-EAST-1.Redshift-Serverless.AmazonAWS.com.:5439/Sales_1");
	CheckConfig(config, "us-east-1", "", RedshiftTarget::SERVERLESS, "wg1", ACCOUNT, "", "Sales_1");
	// The database is carried verbatim, as DATABASE is; the client checks it.
	config =
	    f.Resolve(ChainOnly(), nullptr, "redshift://wg1.012345678901.us-east-1.redshift-serverless.amazonaws.com/de-v");
	CHECK(config.database == "de-v");
	// A trailing slash names no database.
	config =
	    f.Resolve(ChainOnly(), nullptr, "redshift://wg1.012345678901.us-east-1.redshift-serverless.amazonaws.com/");
	CHECK(config.database.empty());
}

TEST_CASE("a redshift:// endpoint names a workgroup endpoint that carries no account", "[attach][config][path]") {
	Fixture f;
	auto config = f.Resolve(ChainOnly(), nullptr, LABELLED_URI);
	CheckConfig(config, "us-east-1", "", RedshiftTarget::SERVERLESS, "wg-a", "", "", "");
	CHECK(config.endpoint_label == "lbl");
	// Its database is the caller's: the endpoint names the host, so nothing fixes it.
	config = f.Resolve(ChainOnly(), nullptr, string(LABELLED_URI) + ":5439/dev");
	CHECK(config.database == "dev");
	config = f.Resolve(Options({{"CREDENTIAL_PROVIDER", "aws_default_chain"}, {"DATABASE", "analytics"}}), nullptr,
	                   LABELLED_URI);
	CHECK(config.database == "analytics");
	CHECK(config.Describe() == "region us-east-1, workgroup wg-a, endpoint label lbl, database analytics");
	// The second label is an account when it is digits alone, and a label otherwise, digits and a letter too.
	config =
	    f.Resolve(ChainOnly(), nullptr, "redshift://wg-a.01234567890a.us-east-1.redshift-serverless.amazonaws.com");
	CheckConfig(config, "us-east-1", "", RedshiftTarget::SERVERLESS, "wg-a", "", "", "");
	CHECK(config.endpoint_label == "01234567890a");
	// The host in any letter case, with one root dot: the label is kept lower-cased, as the client builds the host.
	config =
	    f.Resolve(ChainOnly(), nullptr, "REDSHIFT://WG-A.LBL.US-EAST-1.Redshift-Serverless.AmazonAWS.com.:5440/dev");
	CheckConfig(config, "us-east-1", "", RedshiftTarget::SERVERLESS, "wg-a", "", "", "dev");
	CHECK(config.endpoint_label == "lbl");
	CHECK(config.endpoint_port == 5440);
}

TEST_CASE("a redshift:// endpoint names a cluster endpoint: its cluster, label, region and port",
          "[attach][config][path]") {
	Fixture f;
	// A cluster may listen on any port, and the client connects to exactly the one given.
	auto config = f.Resolve(ChainOnly(), nullptr,
	                        "redshift://my-cluster.abc123xyz.us-east-1.redshift.amazonaws.com:5440/analytics");
	CheckConfig(config, "us-east-1", "", RedshiftTarget::PROVISIONED, "", "", "my-cluster", "analytics");
	CHECK(config.endpoint_label == "abc123xyz");
	CHECK(config.endpoint_port == 5440);
	CHECK(config.Describe() ==
	      "region us-east-1, cluster my-cluster, endpoint label abc123xyz, port 5440, database analytics");
	// The highest and lowest ports are ports.
	config = f.Resolve(ChainOnly(), nullptr, "redshift://my-cluster.abc123xyz.us-east-1.redshift.amazonaws.com:65535");
	CheckConfig(config, "us-east-1", "", RedshiftTarget::PROVISIONED, "", "", "my-cluster", "");
	CHECK(config.endpoint_port == 65535);
	config = f.Resolve(ChainOnly(), nullptr, "redshift://my-cluster.abc123xyz.us-east-1.redshift.amazonaws.com:1");
	CHECK(config.endpoint_port == 1);
	// No port: Redshift's default, 5439, which an explicit :5439 also gives.
	for (auto path : {"redshift://my-cluster.abc123xyz.us-east-1.redshift.amazonaws.com",
	                  "redshift://my-cluster.abc123xyz.us-east-1.redshift.amazonaws.com/dev",
	                  "redshift://my-cluster.abc123xyz.us-east-1.redshift.amazonaws.com:5439"}) {
		INFO(path);
		config = f.Resolve(ChainOnly(), nullptr, path);
		CHECK(config.endpoint_label == "abc123xyz");
		CHECK(config.endpoint_port == 5439);
	}
	// The host in any letter case: the label is kept lower-cased, as the client builds the host.
	config = f.Resolve(ChainOnly(), nullptr, "redshift://My-Cluster.ABC123xyz.US-EAST-1.Redshift.AmazonAWS.com.");
	CHECK(config.cluster == "my-cluster");
	CHECK(config.endpoint_label == "abc123xyz");
}

TEST_CASE("CLUSTER alone names no endpoint, so the client looks the cluster up as before", "[attach][config][path]") {
	Fixture f;
	auto config = f.Resolve(Chain({{"CLUSTER", "my-cluster"}, {"DATABASE", "analytics"}}));
	CheckConfig(config, "us-east-1", "", RedshiftTarget::PROVISIONED, "", "", "my-cluster", "analytics");
	CHECK(config.endpoint_label.empty());
	CHECK(config.endpoint_port == 0);
	CHECK(config.Describe() == "region us-east-1, cluster my-cluster, database analytics");
	// A workgroup's path without a port keeps none, which the client sends as 5439; with one, its port.
	for (auto path : {"redshift://wg1.012345678901.us-east-1.redshift-serverless.amazonaws.com",
	                  "redshift://wg-a.lbl.us-east-1.redshift-serverless.amazonaws.com", LONG_ARN}) {
		INFO(path);
		CHECK(f.Resolve(ChainOnly(), nullptr, path).endpoint_port == 0);
	}
	CHECK(
	    f.Resolve(ChainOnly(), nullptr, "redshift://wg1.012345678901.us-east-1.redshift-serverless.amazonaws.com:5439")
	        .endpoint_port == 5439);
	CHECK(f.Resolve(ChainOnly(), nullptr, "redshift://wg-a.lbl.us-east-1.redshift-serverless.amazonaws.com:5440")
	          .endpoint_port == 5440);
}

TEST_CASE("a workgroup endpoint's long-form ARN names the workgroup and its endpoint label", "[attach][config][path]") {
	Fixture f;
	auto config = f.Resolve(ChainOnly(), nullptr, LONG_ARN);
	CheckConfig(config, "us-east-1", "", RedshiftTarget::SERVERLESS, "wg-a", "", "", "");
	CHECK(config.endpoint_label == "lbl");
	// REGION may repeat the ARN's region.
	config = f.Resolve(Chain({}), nullptr, LONG_ARN);
	CHECK(config.region == "us-east-1");
	// SECRET gives the credentials, and its AWS secret's region gives way to the path's.
	redshift_test::RegisterAwsSecretStandIns(*f.db.instance);
	f.Run("CREATE SECRET creds (TYPE aws, KEY_ID 'k', SECRET 's', REGION 'eu-west-1')");
	config = f.Resolve({}, "creds", LONG_ARN);
	CheckConfig(config, "us-east-1", "", RedshiftTarget::SERVERLESS, "wg-a", "", "", "");
	CHECK(config.aws_secret == "creds");
}

TEST_CASE("an ARN of another form is told the form that names the same target", "[attach][config][path]") {
	Fixture f;
	// An ARN that names its target by ID needs an AWS lookup, which this extension does not make, so it
	// is told the form that names the target by name; none sends the user to the aws extension, which
	// hands every Redshift ATTACH to this one.
	const string by_id = ", which names its target by ID, and finding the target it means needs an AWS lookup that "
	                     "this extension does not make. ";
	const string workgroup_remedy = "Give the workgroup's name with its account: ATTACH '' AS <handle> (TYPE "
	                                "redshift, WORKGROUP '<workgroup>', ACCOUNT_ID '<account>', REGION "
	                                "'<region>', ...), or its endpoint: ATTACH "
	                                "'redshift://<workgroup>.<account>.<region>.redshift-serverless.amazonaws.com' AS "
	                                "<handle> (TYPE redshift, ...)";
	const string cluster_remedy = "Give the cluster as CLUSTER: ATTACH '' AS <handle> (TYPE redshift, CLUSTER "
	                              "'<cluster>', REGION '<region>', ...), or its endpoint as the path: ATTACH "
	                              "'redshift://<cluster>.<label>.<region>.redshift.amazonaws.com[:<port>]' AS <handle> "
	                              "(TYPE redshift, ...)";
	const std::pair<const char *, string> by_id_forms[] = {
	    {"arn:aws:redshift-serverless:us-east-1:012345678901:workgroup/0b1c2d3e-4f5a-6b7c-8d9e-0f1a2b3c4d5e",
	     "The path is a Serverless workgroup ARN" + by_id + workgroup_remedy},
	    {"arn:aws:redshift-serverless:us-east-1:012345678901:namespace/0b1c2d3e-4f5a-6b7c-8d9e-0f1a2b3c4d5e",
	     "The path is a Serverless namespace ARN" + by_id + workgroup_remedy},
	    {"arn:aws:redshift-serverless:us-east-1:012345678901:workgroup/a:b",
	     "The path is a Serverless workgroup ARN" + by_id + workgroup_remedy},
	    {"arn:aws:redshift:us-east-1:012345678901:namespace:0b1c2d3e-4f5a-6b7c-8d9e-0f1a2b3c4d5e",
	     "The path is a provisioned namespace ARN" + by_id + cluster_remedy},
	    // duckdb-aws takes the resource after the fifth ':', so it may hold a further ':'.
	    {"arn:aws:redshift:us-east-1:012345678901:namespace:a:b",
	     "The path is a provisioned namespace ARN" + by_id + cluster_remedy},
	    // Without a region or an account, a namespace ARN still names its cluster by ID.
	    {"arn:aws:redshift::012345678901:namespace:0b1c2d3e",
	     "The path is a provisioned namespace ARN" + by_id + cluster_remedy},
	    {"arn:aws:redshift:::namespace:a:b", "The path is a provisioned namespace ARN" + by_id + cluster_remedy},
	    // A Serverless ARN by ID, too, whether or not it has a region and an account.
	    {"arn:aws:redshift-serverless::012345678901:workgroup/0b1c2d3e",
	     "The path is a Serverless workgroup ARN" + by_id + workgroup_remedy},
	    {"arn:aws:redshift-serverless:us-east-1::namespace/0b1c2d3e",
	     "The path is a Serverless namespace ARN" + by_id + workgroup_remedy},
	    {"arn:aws:redshift:us-east-1:012345678901:cluster:my-cluster", "The path is a cluster ARN. " + cluster_remedy},
	};
	for (auto &entry : by_id_forms) {
		INFO(entry.first);
		CHECK(f.Refusal(ChainOnly(), nullptr, entry.first) == entry.second);
	}
	// Any other ARN gets every form TYPE takes.
	const string forms =
	    "TYPE redshift takes a redshift://<endpoint>[:<port>][/<database>] path; a workgroup endpoint's "
	    "long-form ARN, arn:aws:redshift-serverless:<region>:<account>:workgroup/<label>/<workgroup>; or the empty "
	    "path with "
	    "WORKGROUP and ACCOUNT_ID, or CLUSTER";
	const std::pair<const char *, string> other[] = {
	    {"arn:aws:redshift:us-east-1:012345678901:dbuser:my-cluster/u1",
	     "The path is a Redshift ARN of a resource other than a cluster or a namespace. " + forms},
	    {"arn:aws:redshift:us-east-1:012345678901:namespace:",
	     "The path is a Redshift ARN of a resource other than a cluster or a namespace. " + forms},
	    {"arn:aws:redshift-serverless:us-east-1:012345678901:workgroup/",
	     "The path is not a workgroup endpoint's long-form ARN. " + forms},
	    {"arn:aws:redshift-serverless:us-east-1:012345678901:snapshot/0b1c2d3e",
	     "The path is not a workgroup endpoint's long-form ARN. " + forms},
	};
	for (auto &entry : other) {
		INFO(entry.first);
		CHECK(f.Refusal(ChainOnly(), nullptr, entry.first) == entry.second);
	}
	for (auto &entry : by_id_forms) {
		auto refusal = f.Refusal(ChainOnly(), nullptr, entry.first);
		CHECK(refusal.find("aws extension") == string::npos);
		CHECK(refusal.find("without TYPE") == string::npos);
		// Nor to the aws extension's cluster identifier path, which this extension takes for compatibility only.
		CHECK(refusal.find("ATTACH '<cluster>'") == string::npos);
		CHECK(refusal.find("012345678901") == string::npos);
	}
}

TEST_CASE("every malformed ARN is refused with its reason", "[attach][config][path]") {
	Fixture f;
	const std::pair<const char *, const char *> cases[] = {
	    {"arn:aws-cn:redshift-serverless:cn-north-1:012345678901:workgroup/lbl/wg-a", "outside the aws partition"},
	    {"arn:aws:s3:us-east-1:012345678901:workgroup/lbl/wg-a", "is not a Redshift Serverless workgroup ARN"},
	    {"arn:aws:redshift-serverless:useast1:012345678901:workgroup/lbl/wg-a", "no valid AWS region"},
	    {"arn:aws:redshift-serverless:us-east-1:12345678901:workgroup/lbl/wg-a", "no 12-digit account"},
	    {"arn:aws:redshift-serverless:us-east-1:01234567890a:workgroup/lbl/wg-a", "no 12-digit account"},
	    {"arn:aws:redshift-serverless:us-east-1:012345678901:workgroup/-lbl/wg-a", "endpoint label"},
	    {"arn:aws:redshift-serverless:us-east-1:012345678901:workgroup/LBL/wg-a", "endpoint label"},
	    {"arn:aws:redshift-serverless:us-east-1:012345678901:workgroup//wg-a", "endpoint label"},
	    {"arn:aws:redshift-serverless:us-east-1:012345678901:workgroup/lbl/wg", "workgroup name"},
	    {"arn:aws:redshift-serverless:us-east-1:012345678901:workgroup/lbl/wg_a", "workgroup name"},
	    {"ARN:aws:redshift-serverless:us-east-1:012345678901:workgroup/lbl/wg-a", "is not a Redshift Serverless"},
	    {"arn:aws:redshift-serverless", "is not a Redshift Serverless workgroup ARN"},
	    {"arn:aws", "is not a Redshift Serverless workgroup ARN. TYPE redshift takes a redshift://"},
	    {"arn:", "is not a Redshift Serverless workgroup ARN. TYPE redshift takes a redshift://"},
	    // A trailing separator is a field of its own, not ignored.
	    {"arn:aws:redshift-serverless:us-east-1:012345678901:workgroup/lbl/wg-a/",
	     "is not a workgroup endpoint's long-form ARN. TYPE"},
	    {"arn:aws:redshift-serverless:us-east-1:012345678901:workgroup/lbl/wg-a:",
	     "is not a workgroup endpoint's long-form ARN. TYPE"},
	    {"arn:aws:redshift-serverless:us-east-1:012345678901:workgroup/a/b/c",
	     "is not a workgroup endpoint's long-form ARN. TYPE"},
	    {"arn:aws:redshift-serverless:us-east-1:012345678901:snapshot/lbl/wg-a",
	     "is not a workgroup endpoint's long-form ARN. TYPE"},
	    {"arn:aws:redshift-serverless:us-east-1:012345678901:namespace/lbl/wg-a",
	     "is not a workgroup endpoint's long-form ARN. TYPE"},
	    {"arn:aws:redshift-serverless:u-east-1:012345678901:workgroup/lbl/wg-a", "no valid AWS region"},
	    {"arn:aws:redshift-serverless:us-east-x:012345678901:workgroup/lbl/wg-a", "no valid AWS region"},
	    {"arn:aws:redshift-serverless:us-e4st-1:012345678901:workgroup/lbl/wg-a", "no valid AWS region"},
	    {"arn:aws:redshift-serverless:us-1:012345678901:workgroup/lbl/wg-a", "no valid AWS region"},
	    {"arn:aws:redshift-serverless:us--1:012345678901:workgroup/lbl/wg-a", "no valid AWS region"},
	    {"arn:aws:redshift-serverless:us-east-1:012345678901:workgroup/lbl-/wg-a", "endpoint label"},
	    // Digits alone would build an account's workgroup host.
	    {"arn:aws:redshift-serverless:us-east-1:012345678901:workgroup/111122223333/wg-a",
	     "is a workgroup ARN whose endpoint label is not 1 to 63 lower-case letters, digits and '-', not starting or "
	     "ending with '-' and not digits alone"},
	    {"arn:aws:redshift-serverless:us-east-1:012345678901:workgroup/1/wg-a", "not digits alone"},
	};
	for (auto &entry : cases) {
		INFO(entry.first);
		auto refusal = f.Refusal(ChainOnly(), nullptr, entry.first);
		CHECK(refusal.find(entry.second) != string::npos);
		// No refusal repeats the path, so none holds its account or its label.
		CHECK(refusal.find("012345678901") == string::npos);
		CHECK(refusal.find("lbl") == string::npos);
		CHECK(refusal.find("111122223333") == string::npos);
	}
	// The partition refusal gives the forms TYPE takes, as the other refusals of a wrong ARN do.
	CHECK(f.Refusal(ChainOnly(), nullptr, cases[0].first)
	          .find("outside the aws partition, which this extension does not support. TYPE redshift takes a "
	                "redshift://<endpoint>") != string::npos);
	// The workgroup name's rule reads the same in the ARN's refusal as in an endpoint's.
	CHECK(f.Refusal(ChainOnly(), nullptr, "arn:aws:redshift-serverless:us-east-1:012345678901:workgroup/lbl/wg")
	          .find("is a workgroup ARN whose workgroup name is not 3 to 64 lower-case letters, digits and '-'") !=
	      string::npos);
	CHECK(f.Refusal(ChainOnly(), nullptr, "redshift://w.012345678901.us-east-1.redshift-serverless.amazonaws.com")
	          .find("has a workgroup name that is not 3 to 64 lower-case letters, digits and '-'") != string::npos);
	CHECK(f.Refusal(ChainOnly(), nullptr, "redshift://w.lbl.us-east-1.redshift-serverless.amazonaws.com")
	          .find("has a workgroup name that is not 3 to 64 lower-case letters, digits and '-'") != string::npos);
}

TEST_CASE("every malformed redshift:// endpoint is refused with its reason, never repeating it",
          "[attach][config][path]") {
	Fixture f;
	const char *const CLUSTER_LABEL =
	    "has a cluster endpoint label, after the cluster identifier, that is not 1 to 63 lower-case letters and digits";
	// The whole text, the forms hint included, so the hint's labelled form is pinned too.
	const char *const WORKGROUP_LABEL =
	    "has an endpoint label, after the workgroup name, that is not 1 to 63 lower-case "
	    "letters, digits and '-', not starting or ending with '-' and not digits alone. "
	    "Give a Serverless workgroup endpoint (<workgroup>.<account>.<region>.redshift-serverless.amazonaws.com, or "
	    "<workgroup>.<label>.<region>.redshift-serverless.amazonaws.com for one without an account) or a cluster "
	    "endpoint (<cluster>.<label>.<region>.redshift.amazonaws.com)";
	const std::pair<string, const char *> cases[] = {
	    {"redshift://u:p@wg1.012345678901.us-east-1.redshift-serverless.amazonaws.com", "user information"},
	    {"redshift://wg1.012345678901.us-east-1.redshift-serverless.amazonaws.com?x=1", "query or a fragment"},
	    {"redshift://wg1.012345678901.us-east-1.redshift-serverless.amazonaws.com#x", "query or a fragment"},
	    {"redshift://wg1%2e012345678901.us-east-1.redshift-serverless.amazonaws.com", "percent-encoding"},
	    {"redshift://wg1.012345678901.us-east-1.redshift-serverless.amazonaws.com\\x", "backslash"},
	    {"redshift://wg1.012345678901.us-east-1.redshift-serverless .amazonaws.com", "space"},
	    {"redshift://[::1]:5439", "IP address"},
	    {"redshift://", "has no host"},
	    {"redshift://10.0.0.1:5439", "is not a Redshift endpoint"},
	    {"redshift://wg1.012345678901.us-east-1.redshift-serverless.example.com", "is not a Redshift endpoint"},
	    {"redshift://wg1.012345678901.us-east-1.amazonaws.com", "is not a Redshift endpoint"},
	    {"redshift://a.wg1.012345678901.us-east-1.redshift-serverless.amazonaws.com", "is not a Redshift endpoint"},
	    {"redshift://wg1..us-east-1.redshift-serverless.amazonaws.com", "is not a Redshift endpoint"},
	    {"redshift://wg1.012345678901.us-east-1.redshift-serverless.amazonaws.com..", "is not a Redshift endpoint"},
	    {"redshift://wg-a.lbl.us-east-1-.redshift-serverless.amazonaws.com", "no AWS region"},
	    {"redshift://w.012345678901.us-east-1.redshift-serverless.amazonaws.com", "workgroup name"},
	    {"redshift://wg1.12345.us-east-1.redshift-serverless.amazonaws.com", "12-digit account"},
	    {"redshift://wg1.0123456789012.us-east-1.redshift-serverless.amazonaws.com", "12-digit account"},
	    {"redshift://wg1.012345678901.useast1.redshift-serverless.amazonaws.com", "no AWS region"},
	    {"redshift://wg-a.-lbl.us-east-1.redshift-serverless.amazonaws.com", WORKGROUP_LABEL},
	    {"redshift://1cluster.abc.us-east-1.redshift.amazonaws.com", "cluster identifier"},
	    {"redshift://my--cluster.abc.us-east-1.redshift.amazonaws.com", "cluster identifier"},
	    {"redshift://my-cluster-.abc.us-east-1.redshift.amazonaws.com", "cluster identifier"},
	    {"redshift://c" + string(63, 'x') + ".abc.us-east-1.redshift.amazonaws.com", "cluster identifier"},
	    {"redshift://my-cluster.ab_c.us-east-1.redshift.amazonaws.com", CLUSTER_LABEL},
	    {"redshift://my-cluster.ab-c.us-east-1.redshift.amazonaws.com", CLUSTER_LABEL},
	    {"redshift://my-cluster." + string(64, 'l') + ".us-east-1.redshift.amazonaws.com", CLUSTER_LABEL},
	    {"redshift://w" + string(64, 'g') + ".012345678901.us-east-1.redshift-serverless.amazonaws.com",
	     "workgroup name"},
	    {"redshift://wg-a." + string(64, 'l') + ".us-east-1.redshift-serverless.amazonaws.com", WORKGROUP_LABEL},
	    {"redshift://a.b.c.d.amazonaws.com.x", "is not a Redshift endpoint"},
	    {"redshift://a.b.c.dd.amazonaws.com", "is not a Redshift endpoint"},
	    {"redshift://wg-a.lbl.redshift-serverless.us-east-1.amazonaws.com", "is not a Redshift endpoint"},
	    {"redshift://wg-a.lbl-.us-east-1.redshift-serverless.amazonaws.com", WORKGROUP_LABEL},
	    {"redshift://wg-a.l_b.us-east-1.redshift-serverless.amazonaws.com", WORKGROUP_LABEL},
	    {"redshift://wg1.012345678901.us-east-1.redshift-serverless.amazonaws.com/d\xc3\xa9v", "non-ASCII"},
	    {"redshift://wg1.012345678901.us-east-1.redshift-serverless.amazonaws.com/de\x7fv", "a control character"},
	    {"redshift://wg1.012345678901.us-east-1.redshift-serverless.amazonaws.com/de\tv", "a control character"},
	    {"redshift://wg1.012345678901.us-east-1.redshift-serverless.amazonaws.com:0", "from 1 to 65535"},
	    {"redshift://wg1.012345678901.us-east-1.redshift-serverless.amazonaws.com:65536", "from 1 to 65535"},
	    {"redshift://wg1.012345678901.us-east-1.redshift-serverless.amazonaws.com:54x", "from 1 to 65535"},
	    {"redshift://wg1.012345678901.us-east-1.redshift-serverless.amazonaws.com:", "from 1 to 65535"},
	    {"redshift://wg1.012345678901.us-east-1.redshift-serverless.amazonaws.com:005439", "from 1 to 65535"},
	    {"redshift://my-cluster.abc123xyz.us-east-1.redshift.amazonaws.com:65536", "from 1 to 65535"},
	    {"redshift://w.lbl.us-east-1.redshift-serverless.amazonaws.com", "workgroup name"},
	    {"redshift://my-cluster.-abc.us-east-1.redshift.amazonaws.com", CLUSTER_LABEL},
	    {"redshift://my-cluster.abc-.us-east-1.redshift.amazonaws.com", CLUSTER_LABEL},
	    {"redshift://my-cluster.abc123xyz.us-east-1.redshift.amazonaws.com:0", "from 1 to 65535"},
	    {"redshift://wg1.012345678901.us-east-1.redshift-serverless.amazonaws.org", "is not a Redshift endpoint"},
	    {"redshift://wg1.012345678901.us-east-1.redshift-serverless.amazonaws.com/dev/x", "below the database"},
	};
	for (auto &entry : cases) {
		INFO(entry.first);
		auto refusal = f.Refusal(ChainOnly(), nullptr, entry.first);
		CHECK(refusal.find(entry.second) != string::npos);
		CHECK(refusal.find("012345678901") == string::npos);
		// Nor a label: no refusal text holds "lbl" or "l_b".
		CHECK(refusal.find("lbl") == string::npos);
		CHECK(refusal.find("l_b") == string::npos);
	}
	// A workgroup's port is any from 1 to 65535, in both workgroup forms, read as a number.
	CHECK(f.Resolve(ChainOnly(), nullptr, string(LABELLED_URI) + ":05440").endpoint_port == 5440);
	for (auto port : {1, 5439, 5440, 15439, 65535}) {
		INFO(port);
		auto config = f.Resolve(ChainOnly(), nullptr, string(LABELLED_URI) + ":" + std::to_string(port));
		CHECK(config.endpoint_port == port);
		config = f.Resolve(
		    ChainOnly(), nullptr,
		    "redshift://wg1.012345678901.us-east-1.redshift-serverless.amazonaws.com:" + std::to_string(port) + "/dev");
		CHECK(config.endpoint_port == port);
		CHECK(config.database == "dev");
	}
}

TEST_CASE("names at their longest are accepted in a path", "[attach][config][path]") {
	Fixture f;
	const string workgroup = "w" + string(63, 'g');
	const string label = string(63, 'l');
	const string cluster = "c" + string(62, 'x');
	auto config = f.Resolve(ChainOnly(), nullptr,
	                        "redshift://" + workgroup + ".012345678901.us-east-1.redshift-serverless.amazonaws.com");
	CHECK(config.workgroup == workgroup);
	config = f.Resolve(ChainOnly(), nullptr,
	                   "redshift://" + workgroup + "." + label + ".us-east-1.redshift-serverless.amazonaws.com");
	CHECK(config.endpoint_label == label);
	config = f.Resolve(ChainOnly(), nullptr, "redshift://" + cluster + ".abc.us-east-1.redshift.amazonaws.com");
	CHECK(config.cluster == cluster);
	const string cluster_label = "a" + string(61, '0') + "z";
	config = f.Resolve(ChainOnly(), nullptr, "redshift://c1." + cluster_label + ".us-east-1.redshift.amazonaws.com");
	CHECK(config.endpoint_label == cluster_label);
	config = f.Resolve(ChainOnly(), nullptr,
	                   "arn:aws:redshift-serverless:us-gov-west-1:012345678901:workgroup/" + label + "/" + workgroup);
	CHECK(config.workgroup == workgroup);
	CHECK(config.region == "us-gov-west-1");
}

TEST_CASE("a path that names the target is its only source", "[attach][config][path]") {
	Fixture f;
	const string target_options = "the path names the target, so WORKGROUP, ACCOUNT_ID and CLUSTER cannot be given too";
	for (auto path : {LABELLED_URI, LONG_ARN}) {
		INFO(path);
		CHECK(f.Refusal(Options({{"CREDENTIAL_PROVIDER", "aws_default_chain"}, {"WORKGROUP", "wg-a"}}), nullptr,
		                path) == target_options);
		CHECK(f.Refusal(Options({{"CREDENTIAL_PROVIDER", "aws_default_chain"}, {"ACCOUNT_ID", ACCOUNT}}), nullptr,
		                path) == target_options);
		CHECK(f.Refusal(Options({{"CREDENTIAL_PROVIDER", "aws_default_chain"}, {"CLUSTER", "c"}}), nullptr, path) ==
		      target_options);
		CHECK(f.Refusal(Chain({}), nullptr, string(path).replace(string(path).find("us-east-1"), 9, "eu-west-1")) ==
		      "REGION names another region than the path; give one, or make them agree");
	}
	CHECK(f.Refusal(Options({{"CREDENTIAL_PROVIDER", "aws_default_chain"}, {"DATABASE", "d"}}), nullptr,
	                string(LABELLED_URI) + "/dev") == "the path names the database, so DATABASE cannot be given too");
	// A path still needs credentials, from CREDENTIAL_PROVIDER or SECRET.
	CHECK(f.Refusal({}, nullptr, LONG_ARN) == MISSING_CREDENTIAL_PROVIDER);
}

TEST_CASE("a workgroup named without an account still needs ACCOUNT_ID or the build's hook", "[attach][config][path]") {
	Fixture f;
	// The labelled endpoint comes only from a path; WORKGROUP alone has no label to give, and no
	// build's hook accepts this name.
	REQUIRE_FALSE(AcceptsAccountlessWorkgroup("wg-a"));
	CHECK(f.Refusal(Chain({{"WORKGROUP", "wg-a"}})).find("Missing required parameter 'ACCOUNT_ID'") == 0);
}

//===--------------------------------------------------------------------===//
// SECRET: the AWS credentials of a DuckDB aws or s3 secret, the one credential source beside
// CREDENTIAL_PROVIDER
//===--------------------------------------------------------------------===//

namespace {

const char *const PROVIDER_BESIDE_SECRET =
    "Invalid parameter 'CREDENTIAL_PROVIDER': it is not valid with SECRET, whose aws or s3 secret gives the AWS "
    "credentials";

//! Options for a configuration that SECRET completes: REGION and `target`, with no credential provider.
AttachOptionMap RegionAnd(std::initializer_list<std::pair<const char *, const char *>> target) {
	auto options = Options({{"REGION", "us-east-1"}});
	for (auto &entry : Options(target)) {
		options[entry.first] = entry.second;
	}
	return options;
}

//! What `sql` reported: empty when it succeeded.
string SecretError(Fixture &f, const string &sql) {
	return redshift_test::FailureOf(*f.con.Query(sql)).message;
}

} // namespace

TEST_CASE("SECRET names an aws or s3 secret, and gives the AWS credentials alone", "[attach][config][secret]") {
	Fixture f;
	// The resolver reads the AWS secret it names (the end-to-end tests below check that read).
	redshift_test::RegisterAwsSecretStandIns(*f.db.instance);
	f.Run("CREATE SECRET creds (TYPE aws, KEY_ID 'k', SECRET 's')");
	f.Run("CREATE SECRET bucket (TYPE s3, KEY_ID 'k', SECRET 's')");
	auto config = f.Resolve(RegionAnd({{"CLUSTER", "c"}}), "creds");
	CheckConfig(config, "us-east-1", "", RedshiftTarget::PROVISIONED, "", "", "c", "");
	CHECK(config.aws_secret == "creds");
	CHECK(config.endpoint_port == 0);
	CHECK(config.Describe() == "region us-east-1, credentials from AWS secret creds, cluster c");
	CHECK(f.Resolve(RegionAnd({{"CLUSTER", "c"}}), "bucket").aws_secret == "bucket");

	// One credential source: CREDENTIAL_PROVIDER, any value, and PROFILE are refused beside SECRET.
	for (auto provider : {"aws_default_chain", "profile", "aws_secret", "sso"}) {
		INFO(provider);
		CHECK(f.Refusal(Chain({{"CLUSTER", "c"}, {"CREDENTIAL_PROVIDER", provider}}), "creds") ==
		      PROVIDER_BESIDE_SECRET);
	}
	CHECK(f.Refusal(Chain({{"CLUSTER", "c"}, {"CREDENTIAL_PROVIDER", "profile"}, {"PROFILE", "p"}}), "creds") ==
	      PROVIDER_BESIDE_SECRET);
	CHECK(f.Refusal(RegionAnd({{"CLUSTER", "c"}, {"PROFILE", "p"}}), "creds") == PROFILE_WITHOUT_PROFILE_PROVIDER);
	// Neither is refused too.
	CHECK(f.Refusal(RegionAnd({{"CLUSTER", "c"}})) == MISSING_CREDENTIAL_PROVIDER);
	CHECK(f.Refusal(RegionAnd({{"CLUSTER", "c"}}), "") ==
	      "Invalid value '' for parameter 'SECRET': it cannot be empty");
	// The rules every configuration shares are checked before the AWS secret is read: these name one
	// that does not exist, and are refused for their own reason.
	CHECK(f.Refusal(RegionAnd({{"CLUSTER", "c"}, {"PROFILE", "p"}}), "nope") == PROFILE_WITHOUT_PROFILE_PROVIDER);
	CHECK(f.Refusal(RegionAnd({}), "nope") == MISSING_TARGET);
	CHECK(f.Refusal(RegionAnd({{"CLUSTER", "c"}}), "nope") == SECRET_NOT_FOUND);
}

TEST_CASE("an ACCOUNT_ID refusal repeats no value", "[attach][config]") {
	Fixture f;
	// The value may be a misplaced key, or hold control bytes.
	for (const auto &account : {string("AKIAIOSFODNN7EXAMPLE"), string("0123456789\x1b[2J")}) {
		INFO(account);
		CHECK(f.Refusal(Chain({{"WORKGROUP", "wg"}, {"ACCOUNT_ID", account.c_str()}})) == BAD_ACCOUNT);
	}
}

TEST_CASE("there is no redshift secret type", "[attach][config][secret]") {
	Fixture f;
	// CREATE SECRET of the type fails as an unknown type, persistent or not; a configuration is given as
	// ATTACH options or a path.
	for (auto create : {"CREATE SECRET cfg", "CREATE PERSISTENT SECRET cfg"}) {
		INFO(create);
		const auto refused = SecretError(f, string(create) + " (TYPE redshift, REGION 'us-east-1', CREDENTIAL_PROVIDER "
		                                                     "aws_default_chain, CLUSTER 'c')");
		CHECK(refused.find("redshift") != string::npos);
		CHECK_FALSE(refused.empty());
	}
	auto types = f.con.Query("SELECT count(*) FROM duckdb_secret_types() WHERE type = 'redshift'");
	CHECK(types->Fetch()->GetValue(0, 0).GetValue<int64_t>() == 0);
}

TEST_CASE("SECRET must be a name a message can repeat", "[attach][config][secret]") {
	Fixture f;
	redshift_test::RegisterAwsSecretStandIns(*f.db.instance);
	const string unusable = "Invalid parameter 'SECRET': it must be the name of a DuckDB secret, of at most 255 "
	                        "bytes, with no control character or '}' and no leading or trailing space";
	const string canary = "wJalrXUtnFEMIK7MDENGbPxRfiCYCANARYKEY";
	for (const auto &name : {string(256, 'a'), canary + "\x1b[2J", canary + "\n", canary + "}", " " + canary,
	                         canary + " ", canary + string(1, '\x7f')}) {
		INFO(name);
		const auto refused = f.Refusal(RegionAnd({{"CLUSTER", "c"}}), name.c_str());
		CHECK(refused == unusable);
		CHECK(refused.find(canary) == string::npos);
	}
	// At the limit, and with inner spaces, the name is accepted and then looked up.
	const string longest(255, 'a');
	CHECK(f.Refusal(RegionAnd({{"CLUSTER", "c"}}), longest.c_str()) == SECRET_NOT_FOUND);
	// A key pasted into SECRET, as the aws and s3 secrets' own SECRET parameter takes one, is a usable
	// name that names no secret, and its refusal does not repeat it.
	const string pasted_key = "wJalrXUtnFEMI/K7MDENG/bPxRfiCYEXAMPLEKEY";
	CHECK(f.Refusal(RegionAnd({{"CLUSTER", "c"}}), pasted_key.c_str()) == SECRET_NOT_FOUND);
	f.Run("CREATE SECRET \"my creds\" (TYPE aws, KEY_ID 'k', SECRET 's')");
	CHECK(f.Resolve(RegionAnd({{"CLUSTER", "c"}}), "my creds").aws_secret == "my creds");
}

//===--------------------------------------------------------------------===//
// Reading the AWS secret, end to end through the mock
//===--------------------------------------------------------------------===//

namespace {

const char *const CANARY_KEY_ID = "AKIDCANARYKEYIDVALUE";
const char *const CANARY_SECRET = "canary-secret-access-key-value";
const char *const CANARY_TOKEN = "canary-session-token-value";

//! A database with the stand-in aws and s3 types, debug logging on, and, when asked, the stand-in default
//! storage holding canary keys.
//! Why the aws extension's default secret cannot be read while its default credentials are off.
const char *const DEFAULT_SECRET_SWITCHED_OFF =
    "AWS secret '__aws_default' could not be read: the aws extension resolves its default secret only while "
    "aws_default_credentials and enable_external_access are true";

struct AwsSecretFixture {
	DuckDB db {nullptr, redshift_test::NoPersistentSecrets().get()};
	Connection con {db};
	std::atomic<idx_t> lookups {0};
	//! The stand-in default storage, null without one.
	redshift_test::StandInAwsDefaultStorage *default_storage = nullptr;

	//! With `default_keys` false the stand-in default storage resolves to no keys, as the aws extension's does when
	//! its chain finds none.
	explicit AwsSecretFixture(bool with_default_storage = false, bool default_keys = true) {
		db.LoadStaticExtension<RedshiftExtension>();
		default_storage = redshift_test::RegisterAwsSecretStandIns(
		    *db.instance, with_default_storage ? &lookups : nullptr, default_keys ? CANARY_KEY_ID : "",
		    default_keys ? CANARY_SECRET : "");
		Run("CALL enable_logging(level = 'debug')");
	}

	void Run(const string &sql) {
		redshift_test::Run(con, sql);
	}

	//! The message of the refusal `sql` met. Fails the test unless it is an InvalidConfigurationException.
	string Error(const string &sql) {
		auto failure = redshift_test::FailureOf(*con.Query(sql));
		INFO(sql << ": " << failure.message);
		CHECK(failure.type == ExceptionType::INVALID_CONFIGURATION);
		return failure.message;
	}

	//! The message of the refusal the first query on `handle` met, from DuckDB connection `on`. Fails the
	//! test unless it is an InvalidConfigurationException.
	static string QueryError(Connection &on, const string &handle) {
		auto failure =
		    redshift_test::FailureOf(*on.Query("SELECT * FROM redshift_query('" + handle + "', 'rows=1 cols=1')"));
		INFO(handle << ": " << failure.message);
		CHECK(failure.type == ExceptionType::INVALID_CONFIGURATION);
		return failure.message;
	}

	//! The statement that attaches `handle` to a workgroup with SECRET `aws_secret`.
	static string Attach(const string &handle, const string &aws_secret, const string &region = "us-east-1") {
		return StringUtil::Format("ATTACH '' AS %s (TYPE redshift, %sSECRET %s, WORKGROUP 'wg1', ACCOUNT_ID "
		                          "'012345678901')",
		                          handle, region.empty() ? "" : "REGION '" + region + "', ",
		                          SQLIdentifier::ToString(aws_secret));
	}

	//! How many connections the mock opened, and how many of them with AWS credentials.
	std::pair<idx_t, idx_t> Opens() {
		auto result = con.Query("SELECT count(*), count(*) FILTER (WHERE message LIKE '% (with AWS credentials)') "
		                        "FROM duckdb_logs WHERE message LIKE 'redshift mock: opened a connection to %'");
		auto chunk = result->Fetch();
		return {chunk->GetValue(0, 0).GetValue<idx_t>(), chunk->GetValue(1, 0).GetValue<idx_t>()};
	}

	//! Fails the test if any canary value appears where this extension writes or reports.
	void CheckNoCanary(const string &also = "") {
		for (auto canary : {CANARY_KEY_ID, CANARY_SECRET, CANARY_TOKEN}) {
			INFO(canary);
			CHECK(also.find(canary) == string::npos);
			auto found = con.Query(StringUtil::Format(
			    // DuckDB's own query log records each statement's SQL, the CREATE SECRET that holds the
			    // canary among them; every other log line is checked.
			    "SELECT (SELECT count(*) FROM duckdb_logs WHERE type != 'QueryLog' AND message LIKE '%%%s%%') + "
			    "(SELECT count(*) FROM "
			    // The whole row, `options` and `tags` included: `options` lists the ATTACH options verbatim.
			    "duckdb_databases() d WHERE d::VARCHAR LIKE '%%%s%%')",
			    canary, canary));
			REQUIRE_FALSE(found->HasError());
			CHECK(found->Fetch()->GetValue(0, 0).GetValue<int64_t>() == 0);
		}
	}
};

} // namespace

TEST_CASE("an AWS secret's credentials are read at ATTACH and at each connection open", "[attach][aws_secret]") {
	AwsSecretFixture f;
	f.Run(StringUtil::Format("CREATE SECRET creds (TYPE aws, KEY_ID '%s', SECRET '%s', SESSION_TOKEN '%s', REGION "
	                         "'us-east-1')",
	                         CANARY_KEY_ID, CANARY_SECRET, CANARY_TOKEN));
	f.Run(AwsSecretFixture::Attach("rs", "creds"));
	// A path takes SECRET too.
	f.Run("ATTACH 'redshift://wg1.012345678901.us-east-1.redshift-serverless.amazonaws.com' AS rs_path (TYPE "
	      "redshift, SECRET creds)");
	// Each ATTACH opens its connection's session with the secret's credentials, which the calls reuse.
	CHECK(f.Opens() == std::make_pair(idx_t(2), idx_t(2)));
	f.Run("SELECT * FROM redshift_query('rs', 'rows=1 cols=1')");
	f.Run("SELECT * FROM redshift_query('rs_path', 'rows=1 cols=1')");
	CHECK(f.Opens() == std::make_pair(idx_t(2), idx_t(2)));
	auto opened =
	    f.con.Query("SELECT count(*) FROM duckdb_logs WHERE message = 'redshift mock: opened a connection to "
	                "region us-east-1, credentials from AWS secret creds, workgroup wg1, account 012345678901 "
	                "(with AWS credentials)'");
	CHECK(opened->Fetch()->GetValue(0, 0).GetValue<int64_t>() == 2);
	// An s3 secret serves too, and without a session token.
	f.Run(
	    StringUtil::Format("CREATE SECRET s3creds (TYPE s3, KEY_ID '%s', SECRET '%s')", CANARY_KEY_ID, CANARY_SECRET));
	f.Run(AwsSecretFixture::Attach("rs_s3", "s3creds"));
	f.Run("SELECT * FROM redshift_query('rs_s3', 'rows=1 cols=1')");
	CHECK(f.Opens() == std::make_pair(idx_t(3), idx_t(3)));
	// A handle with another credential provider is given none.
	f.Run(redshift_test::Attach("rs_chain"));
	f.Run("SELECT * FROM redshift_query('rs_chain', 'rows=1 cols=1')");
	CHECK(f.Opens() == std::make_pair(idx_t(4), idx_t(3)));
	f.CheckNoCanary();
}

TEST_CASE("ReadAwsSecret maps each field of the secret it reads", "[attach][aws_secret]") {
	AwsSecretFixture f(true);
	// Read in a transaction, as an ATTACH and an open read it.
	auto read = [&](const string &name) {
		RedshiftAwsSecretContents contents;
		f.con.context->RunFunctionInTransaction([&] { contents = ReadAwsSecret(*f.con.context, name); });
		return contents;
	};
	// The aws extension's default secret first: a user aws secret of the default scope would answer its
	// lookup instead. The stand-in resolves to the canary keys, with no token, in eu-north-1.
	auto by_default = read("__aws_default");
	CHECK(by_default.credentials.access_key_id == CANARY_KEY_ID);
	// The name in any letter case, as DuckDB identifiers compare.
	CHECK(read("__AWS_Default").credentials.access_key_id == CANARY_KEY_ID);
	CHECK(by_default.credentials.secret_access_key == CANARY_SECRET);
	CHECK(by_default.credentials.session_token.empty());
	CHECK(by_default.region == "eu-north-1");
	f.Run("CREATE SECRET with_token (TYPE aws, KEY_ID 'AKIDWITHTOKEN', SECRET 'secret-with-token', SESSION_TOKEN "
	      "'the-token', REGION 'us-west-2')");
	auto with_token = read("with_token");
	CHECK(with_token.credentials.access_key_id == "AKIDWITHTOKEN");
	CHECK(with_token.credentials.secret_access_key == "secret-with-token");
	CHECK(with_token.credentials.session_token == "the-token");
	CHECK(with_token.region == "us-west-2");
	f.Run("CREATE SECRET bucket (TYPE s3, KEY_ID 'AKIDBUCKET', SECRET 'secret-of-bucket', REGION 'eu-west-1')");
	auto bucket = read("bucket");
	CHECK(bucket.credentials.access_key_id == "AKIDBUCKET");
	CHECK(bucket.credentials.secret_access_key == "secret-of-bucket");
	CHECK(bucket.credentials.session_token.empty());
	CHECK(bucket.region == "eu-west-1");
}

TEST_CASE("an AWS secret that is missing, of another type or without keys is refused", "[attach][aws_secret]") {
	AwsSecretFixture f;
	f.Run("CREATE SECRET keyless (TYPE aws, REGION 'us-east-1')");
	f.Run("CREATE SECRET no_secret_key (TYPE aws, KEY_ID 'k')");
	f.Run("CREATE SECRET empty_key_id (TYPE aws, KEY_ID '', SECRET 's')");
	f.Run("CREATE SECRET empty_secret_key (TYPE s3, KEY_ID 'k', SECRET '')");
	f.Run("CREATE SECRET null_key_id (TYPE aws, KEY_ID NULL, SECRET 's')");
	f.Run("CREATE SECRET null_secret_key (TYPE aws, KEY_ID 'k', SECRET NULL)");
	// An empty session token is no token: the keys alone serve.
	f.Run("CREATE SECRET empty_token (TYPE aws, KEY_ID 'k', SECRET 's', SESSION_TOKEN '')");
	f.Run("CREATE SECRET other (TYPE http)");
	CHECK(f.Error(AwsSecretFixture::Attach("rs", "nope")) == string("Cannot attach rs: ") + SECRET_NOT_FOUND);
	CHECK(f.Error(AwsSecretFixture::Attach("rs", "other")) ==
	      "Cannot attach rs: secret 'other' has type 'http': SECRET must name a secret of type aws or s3");
	for (auto name :
	     {"keyless", "no_secret_key", "empty_key_id", "empty_secret_key", "null_key_id", "null_secret_key"}) {
		INFO(name);
		CHECK(f.Error(AwsSecretFixture::Attach("rs", name)) ==
		      StringUtil::Format("Cannot attach rs: AWS secret '%s' holds no AWS credentials (no key ID or secret "
		                         "key): create it with credentials the aws extension can resolve (for an sso "
		                         "chain, first sign in to IAM Identity Center with aws sso login --profile "
		                         "<profile>, where <profile> is the secret's PROFILE, otherwise AWS_DEFAULT_PROFILE, "
		                         "otherwise AWS_PROFILE, otherwise default)",
		                         name));
	}
	auto attached = f.con.Query("SELECT count(*) FROM duckdb_databases() WHERE database_name = 'rs'");
	CHECK(attached->Fetch()->GetValue(0, 0).GetValue<int64_t>() == 0);
	CHECK(f.Opens().first == 0);
	f.Run(AwsSecretFixture::Attach("rs_token", "empty_token"));
	f.Run("SELECT * FROM redshift_query('rs_token', 'rows=1 cols=1')");
	CHECK(f.Opens() == std::make_pair(idx_t(1), idx_t(1)));
}

TEST_CASE("while CONNECT-ed, a dropped AWS secret's refusal keeps its type, after CONNECT's words",
          "[attach][aws_secret][connect]") {
	AwsSecretFixture f;
	f.Run(StringUtil::Format("CREATE SECRET creds (TYPE aws, KEY_ID '%s', SECRET '%s')", CANARY_KEY_ID, CANARY_SECRET));
	// Attached from another DuckDB connection, whose session the ATTACH opens, so this one has none yet.
	Connection other(f.db);
	redshift_test::Run(other, AwsSecretFixture::Attach("rs", "creds"));
	f.Run("CONNECT rs");
	redshift_test::Run(other, "DROP SECRET creds");
	auto failure = redshift_test::FailureOf(*f.con.Query("SELECT 1"));
	CHECK(failure.type == ExceptionType::INVALID_CONFIGURATION);
	CHECK(StringUtil::StartsWith(failure.message, "CONNECT: attach handle \"rs\" could not open its Redshift "
	                                              "connection, so the statement was not run on Redshift. Run "
	                                              "DISCONNECT before"));
	CHECK(StringUtil::EndsWith(failure.message, string("The client reported: Redshift handle rs cannot open a "
	                                                   "connection: ") +
	                                                SECRET_NOT_FOUND));
	f.Run("DISCONNECT");
	CHECK(f.Opens().first == 1);
}

TEST_CASE("an AWS secret dropped or replaced after ATTACH takes effect at the next connection open",
          "[attach][aws_secret]") {
	AwsSecretFixture f;
	const string query = "SELECT * FROM redshift_query('rs', 'rows=1 cols=1')";
	f.Run(StringUtil::Format("CREATE SECRET creds (TYPE aws, KEY_ID '%s', SECRET '%s')", CANARY_KEY_ID, CANARY_SECRET));
	f.Run(AwsSecretFixture::Attach("rs", "creds"));
	CHECK(f.Opens() == std::make_pair(idx_t(1), idx_t(1)));
	f.Run("DROP SECRET creds");
	// The ATTACH's session keeps working, and another DuckDB connection's open is refused.
	f.Run(query);
	Connection other(f.db);
	auto failure = redshift_test::FailureOf(*other.Query(query));
	CHECK(failure.type == ExceptionType::INVALID_CONFIGURATION);
	CHECK(failure.message == string("Redshift handle rs cannot open a connection: ") + SECRET_NOT_FOUND);
	CHECK(failure.message.find("session") == string::npos);
	CHECK(f.Opens().first == 1);
	// Recreated, that connection opens.
	f.Run(StringUtil::Format("CREATE SECRET creds (TYPE aws, KEY_ID '%s', SECRET '%s')", CANARY_KEY_ID, CANARY_SECRET));
	redshift_test::Run(other, query);
	CHECK(f.Opens() == std::make_pair(idx_t(2), idx_t(2)));
	// Replaced without keys: the live sessions keep working, and a third DuckDB connection's open is refused.
	f.Run("CREATE OR REPLACE SECRET creds (TYPE aws, REGION 'us-east-1')");
	f.Run(query);
	redshift_test::Run(other, query);
	Connection third(f.db);
	auto refused = AwsSecretFixture::QueryError(third, "rs");
	CHECK(refused == "Redshift handle rs cannot open a connection: AWS secret 'creds' holds no AWS credentials (no key "
	                 "ID or secret key): create it with credentials the aws extension can resolve (for an sso chain, "
	                 "first sign in to IAM Identity Center with aws sso login --profile <profile>, where <profile> "
	                 "is the secret's PROFILE, otherwise AWS_DEFAULT_PROFILE, otherwise AWS_PROFILE, otherwise "
	                 "default)");
	CHECK(f.Opens().first == 2);
	// Replaced with keys again, that connection opens.
	f.Run(StringUtil::Format("CREATE OR REPLACE SECRET creds (TYPE aws, KEY_ID '%s', SECRET '%s')", CANARY_KEY_ID,
	                         CANARY_SECRET));
	redshift_test::Run(third, query);
	CHECK(f.Opens() == std::make_pair(idx_t(3), idx_t(3)));
	f.CheckNoCanary(refused + failure.message);
}

TEST_CASE("the aws extension's default secret is read through its lookup", "[attach][aws_secret]") {
	AwsSecretFixture f(true);
	// Once for the ATTACH's checks and once for its open; the call reuses that session.
	f.Run(AwsSecretFixture::Attach("rs", "__aws_default"));
	CHECK(f.lookups == 2);
	f.Run("SELECT * FROM redshift_query('rs', 'rows=1 cols=1')");
	CHECK(f.lookups == 2);
	CHECK(f.Opens() == std::make_pair(idx_t(1), idx_t(1)));
	// Its s3 sibling holds the same identity.
	f.Run(AwsSecretFixture::Attach("rs_s3", "__aws_default_s3"));
	f.Run("SELECT * FROM redshift_query('rs_s3', 'rows=1 cols=1')");
	CHECK(f.Opens() == std::make_pair(idx_t(2), idx_t(2)));
	// A scoped s3 secret does not answer the aws lookup, so it changes nothing.
	f.Run("CREATE SECRET scoped (TYPE s3, KEY_ID 'k', SECRET 's')");
	f.Run(AwsSecretFixture::Attach("rs_scoped", "__aws_default"));
	CHECK(f.Opens().first == 3);
	// A user's aws secret answers that lookup instead, and is never used in its place.
	f.Run("CREATE SECRET mine (TYPE aws, KEY_ID 'k2', SECRET 's2')");
	CHECK(f.Error(AwsSecretFixture::Attach("rs_mine", "__aws_default")) ==
	      "Cannot attach rs_mine: AWS secret '__aws_default' could not be read: the aws extension resolves its default "
	      "secret only through a lookup, and another aws secret answers that lookup. Name that secret in SECRET "
	      "instead");
	Connection other(f.db);
	CHECK(AwsSecretFixture::QueryError(other, "rs_scoped").find("could not be read") != string::npos);
	CHECK(f.Opens().first == 3);
	f.CheckNoCanary();
}

TEST_CASE("the aws extension's default secret, resolved without keys, is refused with a restart as the remedy",
          "[attach][aws_secret]") {
	// The aws extension keeps a keyless default secret until DuckDB restarts, so signing in alone does not help,
	// unlike a secret the user creates again; the refusal says so for either name.
	AwsSecretFixture f(true, false);
	for (auto name : {"__aws_default", "__aws_default_s3"}) {
		INFO(name);
		CHECK(f.Error(AwsSecretFixture::Attach("rs", name)) ==
		      StringUtil::Format("Cannot attach rs: AWS secret '%s' holds no AWS credentials (no key ID or secret "
		                         "key): the aws extension resolves its default secret only once until DuckDB "
		                         "restarts, so sign in (for an sso chain, to IAM Identity Center with aws sso login "
		                         "--profile <profile>, where <profile> is AWS_DEFAULT_PROFILE, otherwise AWS_PROFILE, "
		                         "otherwise default), then restart DuckDB, or name in SECRET an aws secret you create",
		                         name));
	}
	CHECK(f.lookups == 2);
	CHECK(f.Opens().first == 0);
}

TEST_CASE("the aws extension's default secret, switched off, is refused for that reason", "[attach][aws_secret]") {
	const string switched_off = DEFAULT_SECRET_SWITCHED_OFF;
	AwsSecretFixture f(true);
	f.Run(AwsSecretFixture::Attach("rs", "__aws_default"));
	f.Run("SET aws_default_credentials = false");
	// The storage still lists the secret by name, but its lookup answers nothing.
	CHECK(f.Error(AwsSecretFixture::Attach("rs_off", "__aws_default")) == "Cannot attach rs_off: " + switched_off);
	// The ATTACH's session keeps working; another DuckDB connection's open is refused while that
	// connection has the setting off too.
	f.Run("SELECT * FROM redshift_query('rs', 'rows=1 cols=1')");
	Connection other(f.db);
	redshift_test::Run(other, "SET aws_default_credentials = false");
	CHECK(AwsSecretFixture::QueryError(other, "rs") == "Redshift handle rs cannot open a connection: " + switched_off);
	f.Run("SET aws_default_credentials = true");
	redshift_test::Run(other, "SET aws_default_credentials = true");
	redshift_test::Run(other, "SELECT * FROM redshift_query('rs', 'rows=1 cols=1')");
	CHECK(f.Opens() == std::make_pair(idx_t(2), idx_t(2)));
	// With external access off, a handle attached before stays usable for its live sessions, but a new
	// session cannot read the default secret: the aws extension's lookup answers nothing then.
	f.Run("SET enable_external_access = false");
	f.Run("SELECT * FROM redshift_query('rs', 'rows=1 cols=1')");
	Connection third(f.db);
	CHECK(AwsSecretFixture::QueryError(third, "rs") == "Redshift handle rs cannot open a connection: " + switched_off);
	CHECK(f.Opens().first == 2);
	f.CheckNoCanary();
}

TEST_CASE("the aws extension's default secret, switched off, keeps that reason beside a user aws secret",
          "[attach][aws_secret]") {
	// A user's aws secret of the default scope would answer the lookup, but the reason stays that the
	// default credentials are off, never that another secret answered.
	AwsSecretFixture f(true);
	f.Run("CREATE SECRET mine (TYPE aws, KEY_ID 'k2', SECRET 's2')");
	f.Run("SET aws_default_credentials = false");
	CHECK(f.Error(AwsSecretFixture::Attach("rs", "__aws_default")) ==
	      string("Cannot attach rs: ") + DEFAULT_SECRET_SWITCHED_OFF);
	CHECK(f.lookups == 0);
	f.CheckNoCanary();
}

TEST_CASE("the aws extension's default secret, with external access off, keeps that reason beside a user aws secret",
          "[attach][aws_secret]") {
	// External access off refuses a new ATTACH first, so the handle is attached before, and
	// a new session from another DuckDB connection reads the default secret.
	AwsSecretFixture f(true);
	f.Run(AwsSecretFixture::Attach("rs", "__aws_default"));
	const auto lookups = f.lookups.load();
	f.Run("CREATE SECRET mine (TYPE aws, KEY_ID 'k2', SECRET 's2')");
	f.Run("SET enable_external_access = false");
	Connection other(f.db);
	CHECK(AwsSecretFixture::QueryError(other, "rs") ==
	      string("Redshift handle rs cannot open a connection: ") + DEFAULT_SECRET_SWITCHED_OFF);
	CHECK(f.lookups == lookups);
	// Only the ATTACH's own.
	CHECK(f.Opens().first == 1);
	f.CheckNoCanary();
}

TEST_CASE("the aws extension's default secret, listed but answering no lookup, is refused for that reason",
          "[attach][aws_secret]") {
	// Told apart from a lookup another aws secret answers, which would name the wrong cause.
	AwsSecretFixture f(true);
	f.default_storage->answers_lookups = false;
	CHECK(f.Error(AwsSecretFixture::Attach("rs", "__aws_default")) ==
	      "Cannot attach rs: AWS secret '__aws_default' could not be read: the aws extension's lookup for its default "
	      "secret answered nothing");
	CHECK(f.lookups == 0);
	f.CheckNoCanary();
}

TEST_CASE("the aws extension's default secret, dropped, is refused at the next open", "[attach][aws_secret]") {
	AwsSecretFixture f(true);
	f.Run(AwsSecretFixture::Attach("rs", "__aws_default"));
	f.Run("DROP SECRET __aws_default");
	// Both names go, as in duckdb-aws, for the database's lifetime.
	const string not_found = SECRET_NOT_FOUND;
	Connection other(f.db);
	CHECK(AwsSecretFixture::QueryError(other, "rs") == "Redshift handle rs cannot open a connection: " + not_found);
	CHECK(f.Error(AwsSecretFixture::Attach("rs_s3", "__aws_default_s3")) == "Cannot attach rs_s3: " + not_found);
	// Only the ATTACH's own, before the drop.
	CHECK(f.Opens().first == 1);
	f.CheckNoCanary();
}

TEST_CASE("the aws extension's default s3 secret, read first, is resolved through the aws lookup",
          "[attach][aws_secret]") {
	// Nothing has resolved the default storage yet, so a read by name alone would find no keys.
	AwsSecretFixture f(true);
	// Once for the ATTACH's checks and once for its open.
	f.Run(AwsSecretFixture::Attach("rs", "__aws_default_s3", ""));
	CHECK(f.lookups == 2);
	f.Run("SELECT * FROM redshift_query('rs', 'rows=1 cols=1')");
	CHECK(f.lookups == 2);
	// The region and keys are the aws match's (the stand-in resolves to eu-north-1).
	auto opened = f.con.Query("SELECT count(*) FROM duckdb_logs WHERE message = 'redshift mock: opened a connection to "
	                          "region eu-north-1, credentials from AWS secret __aws_default_s3, workgroup wg1, account "
	                          "012345678901 (with AWS credentials)'");
	CHECK(opened->Fetch()->GetValue(0, 0).GetValue<int64_t>() == 1);
	f.CheckNoCanary();
}

TEST_CASE("the default secret's region is taken from the one read an ATTACH makes", "[attach][aws_secret]") {
	AwsSecretFixture f(true);
	f.Run("ATTACH '' AS rs (TYPE redshift, SECRET __aws_default, CLUSTER 'c')");
	CHECK(f.lookups == 2);
	f.Run("SELECT * FROM redshift_query('rs', 'rows=1 cols=1')");
	CHECK(f.lookups == 2);
	auto opened = f.con.Query("SELECT count(*) FROM duckdb_logs WHERE message = 'redshift mock: opened a connection to "
	                          "region eu-north-1, credentials from AWS secret __aws_default, cluster c (with AWS "
	                          "credentials)'");
	CHECK(opened->Fetch()->GetValue(0, 0).GetValue<int64_t>() == 1);
}

TEST_CASE("an AWS secret name in two secret storages is refused at ATTACH and at a connection open",
          "[attach][aws_secret]") {
	AwsSecretFixture f(true);
	f.Run(AwsSecretFixture::Attach("rs", "__aws_default"));
	// A temporary secret of the default's name: the name now occurs in two storages.
	f.Run(StringUtil::Format("CREATE SECRET __aws_default (TYPE s3, SCOPE 's3://elsewhere', KEY_ID '%s', SECRET '%s')",
	                         CANARY_KEY_ID, CANARY_SECRET));
	const string ambiguity =
	    "Ambiguity detected for secret name '__aws_default', secret occurs in multiple storage backends.";
	CHECK(f.Error(AwsSecretFixture::Attach("rs_two", "__aws_default")) == "Cannot attach rs_two: " + ambiguity);
	Connection other(f.db);
	auto refused = AwsSecretFixture::QueryError(other, "rs");
	CHECK(refused == "Redshift handle rs cannot open a connection: " + ambiguity);
	// Only the ATTACH's own, before the second secret.
	CHECK(f.Opens().first == 1);
	f.CheckNoCanary(refused);
}

TEST_CASE("with SECRET, an ATTACH with no region takes the AWS secret's", "[attach][aws_secret]") {
	AwsSecretFixture f;
	f.Run("CREATE SECRET east (TYPE aws, KEY_ID 'k', SECRET 's', REGION 'us-east-1')");
	f.Run("CREATE SECRET regionless (TYPE aws, KEY_ID 'k', SECRET 's')");
	f.Run(AwsSecretFixture::Attach("rs", "east", ""));
	f.Run("SELECT * FROM redshift_query('rs', 'rows=1 cols=1')");
	auto opened = f.con.Query("SELECT count(*) FROM duckdb_logs WHERE message LIKE 'redshift mock: opened a connection "
	                          "to region us-east-1, credentials from AWS secret east,%'");
	CHECK(opened->Fetch()->GetValue(0, 0).GetValue<int64_t>() == 1);
	// REGION wins over it.
	f.Run(AwsSecretFixture::Attach("rs_west", "east", "eu-west-1"));
	f.Run("SELECT * FROM redshift_query('rs_west', 'rows=1 cols=1')");
	opened = f.con.Query("SELECT count(*) FROM duckdb_logs WHERE message LIKE 'redshift mock: opened a connection to "
	                     "region eu-west-1, credentials from AWS secret east,%'");
	CHECK(opened->Fetch()->GetValue(0, 0).GetValue<int64_t>() == 1);
	// With no region anywhere, REGION is missing, and the message names the AWS secret it looked in; an empty
	// region setting is none.
	CHECK(f.Error(AwsSecretFixture::Attach("rs_none", "regionless", "")) ==
	      "Cannot attach rs_none: Missing required parameter 'REGION': set REGION, or include a region in the aws or "
	      "s3 secret 'regionless'");
	f.Run("CREATE SECRET empty_region (TYPE aws, KEY_ID 'k', SECRET 's', REGION '')");
	CHECK(f.Error(AwsSecretFixture::Attach("rs_none", "empty_region", "")) ==
	      "Cannot attach rs_none: Missing required parameter 'REGION': set REGION, or include a region in the aws or "
	      "s3 secret 'empty_region'");
	// A secret that does not exist is refused as that, before any region rule.
	CHECK(f.Error(AwsSecretFixture::Attach("rs_none", "nope", "")) ==
	      string("Cannot attach rs_none: ") + SECRET_NOT_FOUND);
}

//===--------------------------------------------------------------------===//
// A cluster identifier as the path, the aws extension's form, which that extension hands to this one
// unchanged, and the aws extension's options and fallbacks it alone takes.
//===--------------------------------------------------------------------===//

namespace {

//! Sets or, for null, unsets the environment variable `name`.
void SetEnvironmentVariable(const char *name, const char *value) {
#ifdef _WIN32
	_putenv_s(name, value ? value : "");
#else
	if (value) {
		setenv(name, value, 1);
	} else {
		unsetenv(name);
	}
#endif
}

//! AWS_REGION and AWS_DEFAULT_REGION set to the given values (null: unset) for one scope, then put back.
class ScopedRegionEnvironment {
public:
	ScopedRegionEnvironment(const char *aws_region, const char *aws_default_region) {
		for (auto name : {"AWS_REGION", "AWS_DEFAULT_REGION"}) {
			const char *value = std::getenv(name);
			saved.emplace_back(name, value != nullptr, value ? value : "");
		}
		SetEnvironmentVariable("AWS_REGION", aws_region);
		SetEnvironmentVariable("AWS_DEFAULT_REGION", aws_default_region);
	}
	~ScopedRegionEnvironment() {
		for (auto &variable : saved) {
			SetEnvironmentVariable(std::get<0>(variable),
			                       std::get<1>(variable) ? std::get<2>(variable).c_str() : nullptr);
		}
	}

private:
	vector<std::tuple<const char *, bool, string>> saved;
};

const char *const CLUSTER_PATH = "my-cluster";
const char *const CLUSTER_HOST = "my-cluster.abc123xyz.us-east-1.redshift.amazonaws.com";

//! The refusal of a HOST beside a cluster identifier that is not that cluster's endpoint. Its remedy names
//! this extension's own forms, never "leave HOST out", which would keep the aws extension's form.
const char *const HOST_NOT_THE_CLUSTER =
    "HOST must be the endpoint of the cluster the path names, <cluster>.<label>.<region>.redshift.amazonaws.com: the "
    "connection verifies the server's certificate against that name, so it cannot go through another address, such "
    "as a tunnel, a proxy or an IP address. Give the cluster as CLUSTER: ATTACH '' AS <handle> (TYPE redshift, "
    "CLUSTER '<cluster>', REGION '<region>', ...), or its endpoint as the path: ATTACH "
    "'redshift://<cluster>.<label>.<region>.redshift.amazonaws.com[:<port>]' AS <handle> (TYPE redshift, ...)";
//! The refusal of a HOST beside a redshift:// path that is not the path's host, where leaving HOST out loses
//! nothing.
const char *const HOST_NOT_THE_PATH =
    "HOST must be the host the path or ARN names: the connection verifies the server's certificate against that name, "
    "so it cannot go through another address, such as a tunnel, a proxy or an IP address. Leave HOST out";

//! Options with the default credential chain and no region, beside `extra`, whose values may be typed.
AttachOptionMap ChainWith(std::initializer_list<std::pair<const char *, Value>> extra) {
	auto options = ChainOnly();
	for (auto &entry : extra) {
		options[StringUtil::Lower(entry.first)] = entry.second;
	}
	return options;
}

} // namespace

TEST_CASE("a cluster identifier as the path names the cluster, which the client looks up",
          "[attach][config][cluster_path]") {
	Fixture f;
	auto config = f.Resolve(Chain({}), nullptr, CLUSTER_PATH);
	CheckConfig(config, "us-east-1", "", RedshiftTarget::PROVISIONED, "", "", "my-cluster", "");
	CheckDefaults(config);
	CHECK(config.Describe() == "region us-east-1, cluster my-cluster");
	// At its longest, 63 characters.
	const string longest = "c" + string(62, '1');
	CHECK(f.Resolve(Chain({}), nullptr, longest).cluster == longest);
	CHECK(f.Resolve(Chain({{"DATABASE", "analytics"}}), nullptr, CLUSTER_PATH).database == "analytics");
	// The path is the target's one source.
	for (auto option : {"CLUSTER", "WORKGROUP", "ACCOUNT_ID"}) {
		INFO(option);
		CHECK(f.Refusal(Chain({{option, option == string("ACCOUNT_ID") ? ACCOUNT : "other"}}), nullptr, CLUSTER_PATH) ==
		      "the path names the target, so WORKGROUP, ACCOUNT_ID and CLUSTER cannot be given too; to give the target "
		      "as options, leave the path empty: ATTACH '' AS <handle> (TYPE redshift, ...)");
	}
}

TEST_CASE("beside a cluster identifier, HOST must be that cluster's own endpoint", "[attach][config][cluster_path]") {
	Fixture f;
	// The region comes from HOST, and the client connects to exactly that endpoint, at 5439 or PORT.
	auto config = f.Resolve(ChainWith({{"HOST", Value(CLUSTER_HOST)}}), nullptr, CLUSTER_PATH);
	CheckConfig(config, "us-east-1", "", RedshiftTarget::PROVISIONED, "", "", "my-cluster", "");
	CHECK(config.endpoint_label == "abc123xyz");
	CHECK(config.endpoint_port == 5439);
	config = f.Resolve(ChainWith({{"HOST", Value("MY-CLUSTER.ABC123XYZ.US-EAST-1.REDSHIFT.AMAZONAWS.COM.")},
	                              {"PORT", Value("5440")},
	                              {"DATABASE", Value("dev")}}),
	                   nullptr, CLUSTER_PATH);
	CHECK(config.endpoint_label == "abc123xyz");
	CHECK(config.endpoint_port == 5440);
	CHECK(config.Describe() ==
	      "region us-east-1, cluster my-cluster, endpoint label abc123xyz, port 5440, database dev");
	CHECK(f.Resolve(ChainWith({{"HOST", Value(CLUSTER_HOST)}, {"PORT", Value::INTEGER(65535)}}), nullptr, CLUSTER_PATH)
	          .endpoint_port == 65535);
	// REGION may repeat HOST's region, and no other.
	CHECK(f.Resolve(ChainWith({{"HOST", Value(CLUSTER_HOST)}, {"REGION", Value("us-east-1")}}), nullptr, CLUSTER_PATH)
	          .region == "us-east-1");
	CHECK(f.Refusal(ChainWith({{"HOST", Value(CLUSTER_HOST)}, {"REGION", Value("eu-west-1")}}), nullptr,
	                CLUSTER_PATH) == "REGION names another region than HOST; give one, or make them agree");
	// Any other host is refused, a tunnel's included, without repeating it.
	for (auto host :
	     {"127.0.0.1", "localhost", "no-such-host.invalid", "[::1]", "other.abc123xyz.us-east-1.redshift.amazonaws.com",
	      "my-cluster.012345678901.us-east-1.redshift-serverless.amazonaws.com",
	      "my-cluster.lbl.us-east-1.redshift-serverless.amazonaws.com",
	      // HOST is read inside a redshift:// URI, so nothing in it may add a port, a database or user information.
	      "my-cluster.abc123xyz.us-east-1.redshift.amazonaws.com:5439",
	      "my-cluster.abc123xyz.us-east-1.redshift.amazonaws.com/dev",
	      "admin@my-cluster.abc123xyz.us-east-1.redshift.amazonaws.com",
	      "my-cluster.abc-123.us-east-1.redshift.amazonaws.com", "my-cluster.abc123xyz.useast1.redshift.amazonaws.com",
	      "my-cluster.abc123xyz.us-east-1.redshift.amazonaws.com.cn",
	      // Every other label right, so only the check of the fifth label refuses it.
	      "my-cluster.abc123xyz.us-east-1.redshift.example.com",
	      "x.my-cluster.abc123xyz.us-east-1.redshift.amazonaws.com"}) {
		INFO(host);
		auto refusal = f.Refusal(ChainWith({{"HOST", Value(host)}}), nullptr, CLUSTER_PATH);
		CHECK(refusal == HOST_NOT_THE_CLUSTER);
		CHECK(refusal.find(host) == string::npos);
	}
	CHECK(f.Refusal(ChainWith({{"HOST", Value("")}}), nullptr, CLUSTER_PATH) ==
	      "Invalid value '' for parameter 'HOST': it cannot be empty");
	CHECK(f.Refusal(ChainWith({{"HOST", Value::INTEGER(1)}}), nullptr, CLUSTER_PATH) ==
	      "Invalid value for parameter 'HOST': it must be a string; quote it, like HOST '<value>'");
}

TEST_CASE("beside a cluster identifier, PORT goes only with HOST", "[attach][config][cluster_path]") {
	Fixture f;
	// Without HOST the client looks the cluster up, and the driver connects to the port the lookup reports,
	// so a PORT there could not be honoured: it is refused, whatever its value.
	for (auto &port : {Value("5439"), Value("5441"), Value::INTEGER(1), Value::BIGINT(65535), Value::USMALLINT(5439)}) {
		INFO(port.ToString());
		CHECK(f.Refusal(ChainWith({{"REGION", Value("us-east-1")}, {"PORT", port}}), nullptr, CLUSTER_PATH) ==
		      "Invalid parameter 'PORT': without HOST the cluster is looked up and connected to at the port it "
		      "listens on, so PORT cannot be honoured. Give the cluster's endpoint and port as the path: ATTACH "
		      "'redshift://<cluster>.<label>.<region>.redshift.amazonaws.com:<port>' AS <handle> (TYPE redshift, ...)");
	}
	for (auto &port : {Value("abc"), Value("70000"), Value("0"), Value("-1"), Value(""), Value("5439 "), Value("+5439"),
	                   Value("005439"), Value::INTEGER(0), Value::INTEGER(65536), Value::DOUBLE(5439),
	                   Value::BOOLEAN(true), Value("99999999999999999999")}) {
		INFO(port.ToString());
		auto thrown = redshift_test::Capture([&] { ReadOptions({{"port", port}}); });
		CHECK(thrown.message == "Cannot attach rs: Invalid value for parameter 'PORT': it must be a number from 1 to "
		                        "65535");
	}
}

TEST_CASE("beside a cluster identifier, DBNAME is DATABASE, and an empty one is none",
          "[attach][config][cluster_path]") {
	Fixture f;
	auto with = [&](std::initializer_list<std::pair<const char *, Value>> extra) {
		auto options = ChainWith(extra);
		options["region"] = Value("us-east-1");
		return options;
	};
	CHECK(f.Resolve(with({{"DBNAME", Value("analytics")}}), nullptr, CLUSTER_PATH).database == "analytics");
	CHECK(f.Resolve(with({{"DBNAME", Value("analytics")}, {"DATABASE", Value("analytics")}}), nullptr, CLUSTER_PATH)
	          .database == "analytics");
	CHECK(f.Refusal(with({{"DBNAME", Value("analytics")}, {"DATABASE", Value("sales")}}), nullptr, CLUSTER_PATH) ==
	      "DATABASE and DBNAME name different databases; give one");
	CHECK(f.Resolve(with({{"DATABASE", Value("")}}), nullptr, CLUSTER_PATH).database.empty());
	CHECK(f.Resolve(with({{"DBNAME", Value("")}}), nullptr, CLUSTER_PATH).database.empty());
	CHECK(f.Resolve(with({{"DBNAME", Value("")}, {"DATABASE", Value("sales")}}), nullptr, CLUSTER_PATH).database ==
	      "sales");
	CHECK(f.Resolve(with({{"DBNAME", Value("sales")}, {"DATABASE", Value("")}}), nullptr, CLUSTER_PATH).database ==
	      "sales");
}

TEST_CASE("beside the empty path and a long-form ARN, HOST and PORT are refused, and DBNAME is DATABASE",
          "[attach][config][path]") {
	Fixture f;
	const char *const wg_label_host = "wg-a.lbl.us-east-1.redshift-serverless.amazonaws.com";
	const char *const wg_account_host = "wg1.012345678901.us-east-1.redshift-serverless.amazonaws.com";
	// Neither form has an endpoint for HOST and PORT to repeat, even when HOST is the endpoint the target
	// options or the ARN name: the remedy is the endpoint as the path.
	const auto refusal = [](const char *option) {
		return StringUtil::Format("Invalid parameter '%s': give the endpoint, and its port, as the path instead: "
		                          "ATTACH 'redshift://<endpoint>[:<port>]' AS <handle> (TYPE redshift, ...); without "
		                          "one, WORKGROUP and ACCOUNT_ID or CLUSTER name the endpoint",
		                          option);
	};
	for (auto option : {"HOST", "PORT"}) {
		INFO(option);
		const bool host = string(option) == "HOST";
		auto config = ChainOnly();
		config["region"] = Value("us-east-1");
		config[StringUtil::Lower(option)] = host ? Value(wg_account_host) : Value("5440");
		CHECK(f.Refusal(config) == refusal(option));
		CHECK(f.Refusal(Chain({{"CLUSTER", "my-cluster"}, {option, host ? CLUSTER_HOST : "5439"}})) == refusal(option));
		CHECK(f.Refusal(
		          Chain({{"WORKGROUP", "wg1"}, {"ACCOUNT_ID", ACCOUNT}, {option, host ? wg_account_host : "5440"}})) ==
		      refusal(option));
		CHECK(
		    f.Refusal(Options({{"CREDENTIAL_PROVIDER", "aws_default_chain"}, {option, host ? wg_label_host : "5440"}}),
		              nullptr, LONG_ARN) == refusal(option));
	}
	// Given both, the refusal names HOST.
	auto both = ChainOnly();
	both["host"] = Value(wg_account_host);
	both["port"] = Value("5440");
	CHECK(f.Refusal(both) == refusal("HOST"));
	// DBNAME is DATABASE beside every form; an empty one is refused as an empty DATABASE is, by its own name.
	for (auto path : {"", LONG_ARN}) {
		INFO(path);
		auto options = *path ? Options({{"CREDENTIAL_PROVIDER", "aws_default_chain"}}) : Chain({{"CLUSTER", "c"}});
		options["dbname"] = Value("dev");
		CHECK(f.Resolve(options, nullptr, path).database == "dev");
		options["dbname"] = Value("");
		CHECK(f.Refusal(options, nullptr, path) == "Invalid value '' for parameter 'DBNAME': it cannot be empty");
		options.erase("dbname");
		options["database"] = Value("");
		CHECK(f.Refusal(options, nullptr, path) == "Invalid value '' for parameter 'DATABASE': it cannot be empty");
	}
}

TEST_CASE("the aws extension's options beside a redshift:// path may repeat what the path gives",
          "[attach][config][path]") {
	// As they reach this extension when the aws extension has resolved an ARN that names its target by ID
	// to the target's redshift:// endpoint, with the user's options passed on as given.
	Fixture f;
	const string cluster_uri = "redshift://my-cluster.abc123xyz.us-east-1.redshift.amazonaws.com:5439";
	const string workgroup_uri = string(LABELLED_URI) + ":5440";
	auto with = [](std::initializer_list<std::pair<const char *, Value>> extra) {
		auto options = ChainOnly();
		for (auto &option : extra) {
			options[StringUtil::Lower(option.first)] = option.second;
		}
		return options;
	};
	// HOST: the path's host, in any letter case and with one root dot.
	auto config = f.Resolve(with({{"HOST", Value(string(CLUSTER_HOST) + ".")}}), nullptr, cluster_uri);
	CHECK(config.cluster == "my-cluster");
	CHECK(config.endpoint_port == 5439);
	config = f.Resolve(with({{"HOST", Value("WG-A.LBL.US-EAST-1.REDSHIFT-SERVERLESS.AMAZONAWS.COM")}}), nullptr,
	                   workgroup_uri);
	CHECK(config.workgroup == "wg-a");
	const string other_host = HOST_NOT_THE_PATH;
	// The same labels in another order are another host.
	CHECK(f.Refusal(with({{"HOST", Value("wg-a.lbl.redshift-serverless.us-east-1.amazonaws.com")}}), nullptr,
	                workgroup_uri) == other_host);
	// The URI's own host is read the same way: in any letter case, and with one root dot.
	config = f.Resolve(with({{"HOST", Value(CLUSTER_HOST)}}), nullptr,
	                   "redshift://MY-CLUSTER.ABC123XYZ.US-EAST-1.REDSHIFT.AMAZONAWS.COM.:5439");
	CHECK(config.cluster == "my-cluster");
	CHECK(f.Refusal(with({{"HOST", Value("127.0.0.1")}}), nullptr, cluster_uri) == other_host);
	// The whole host is compared: the same cluster and region under another endpoint label is another host.
	CHECK(f.Refusal(with({{"HOST", Value("my-cluster.other1.us-east-1.redshift.amazonaws.com")}}), nullptr,
	                cluster_uri) == other_host);
	// One root dot only.
	CHECK(f.Refusal(with({{"HOST", Value(string(CLUSTER_HOST) + "..")}}), nullptr, cluster_uri) == other_host);
	// HOST is judged before PORT, and, as the aws extension's options are translated first, before the
	// target options the path refuses.
	CHECK(f.Refusal(with({{"HOST", Value("127.0.0.1")}, {"PORT", Value("5441")}}), nullptr, cluster_uri) == other_host);
	CHECK(f.Refusal(with({{"HOST", Value("127.0.0.1")}, {"WORKGROUP", Value("wg-a")}}), nullptr, cluster_uri) ==
	      other_host);
	CHECK(f.Refusal(with({{"HOST", Value(CLUSTER_HOST)}, {"WORKGROUP", Value("wg-a")}}), nullptr, cluster_uri) ==
	      "the path names the target, so WORKGROUP, ACCOUNT_ID and CLUSTER cannot be given too");
	CHECK(StringUtil::StartsWith(
	    f.Refusal(with({{"HOST", Value("127.0.0.1")}, {"WORKGROUP", Value("wg-a")}}), nullptr, CLUSTER_PATH),
	    "the path names the target, so WORKGROUP, ACCOUNT_ID and CLUSTER cannot be given too"));
	CHECK(f.Refusal(with({{"HOST", Value("other.abc123xyz.us-east-1.redshift.amazonaws.com")}}), nullptr,
	                cluster_uri) == other_host);
	// PORT: the port the path gives, as a number or quoted; none, or another, is refused.
	CHECK(f.Resolve(with({{"PORT", Value("5440")}}), nullptr, workgroup_uri).endpoint_port == 5440);
	CHECK(f.Resolve(with({{"PORT", Value::INTEGER(5439)}}), nullptr, cluster_uri).endpoint_port == 5439);
	const string other_port =
	    "PORT must be the port the path or ARN gives. Leave PORT out, or give the port in the path: "
	    "ATTACH 'redshift://<endpoint>:<port>' AS <handle> (TYPE redshift, ...)";
	CHECK(f.Refusal(with({{"PORT", Value("5441")}}), nullptr, workgroup_uri) == other_port);
	CHECK(f.Refusal(with({{"PORT", Value("5439")}}), nullptr,
	                "redshift://my-cluster.abc123xyz.us-east-1.redshift.amazonaws.com") == other_port);
	// DBNAME is DATABASE, beside a path that names no database, as the aws extension's resolved ARN is;
	// beside one that does, it is refused, as DATABASE is.
	CHECK(f.Resolve(with({{"DBNAME", Value("dev")}}), nullptr, cluster_uri).database == "dev");
	CHECK(f.Resolve(with({{"DBNAME", Value("dev")}, {"DATABASE", Value("dev")}}), nullptr, cluster_uri).database ==
	      "dev");
	CHECK(f.Refusal(with({{"DBNAME", Value("dev")}}), nullptr, cluster_uri + "/dev") ==
	      "the path names the database, so DBNAME cannot be given too");
	CHECK(f.Refusal(with({{"DATABASE", Value("dev")}}), nullptr, cluster_uri + "/dev") ==
	      "the path names the database, so DATABASE cannot be given too");
	CHECK(f.Refusal(with({{"DBNAME", Value("sales")}, {"DATABASE", Value("dev")}}), nullptr, cluster_uri) ==
	      "DATABASE and DBNAME name different databases; give one");
	CHECK(f.Refusal(with({{"DBNAME", Value("")}}), nullptr, cluster_uri) ==
	      "Invalid value '' for parameter 'DBNAME': it cannot be empty");
	CHECK(f.Refusal(with({{"DATABASE", Value("")}}), nullptr, cluster_uri) ==
	      "Invalid value '' for parameter 'DATABASE': it cannot be empty");
	// All of them at once, as the aws extension's resolved ARN can carry them.
	config = f.Resolve(with({{"HOST", Value(CLUSTER_HOST)}, {"PORT", Value("5439")}, {"DBNAME", Value("dev")}}),
	                   nullptr, cluster_uri);
	CHECK(config.cluster == "my-cluster");
	CHECK(config.database == "dev");
}

TEST_CASE("the postgres extension's options and DURATION_SECONDS are ignored beside any form, whatever their value",
          "[attach][config][cluster_path]") {
	Fixture f;
	// DURATION_SECONDS is the lifetime of database credentials the driver mints itself at every connect, so
	// no value of it, in range, out of range or not a number, is ever read.
	const string ignored = ", SCHEMA 'public', ISOLATION_LEVEL 'serializable', USE_TEXT_PROTOCOL true, "
	                       "SECRET_STORAGE_TABLE 'x', CONNECT_DISPLAY 42, DURATION_SECONDS 'abc'";
	for (auto duration : {"900", "60", "-1", "1.5", "'abc'", "true"}) {
		f.Run(string("ATTACH 'my-cluster' AS rs_duration (TYPE redshift, REGION 'us-east-1', "
		             "CREDENTIAL_PROVIDER aws_default_chain, DURATION_SECONDS ") +
		      duration + ")");
		f.Run("DETACH rs_duration");
	}
	f.Run("ATTACH 'my-cluster' AS rs_cluster (TYPE redshift, REGION 'us-east-1', CREDENTIAL_PROVIDER "
	      "aws_default_chain" +
	      ignored + ")");
	f.Run("ATTACH '' AS rs_options (TYPE redshift, REGION 'us-east-1', CREDENTIAL_PROVIDER aws_default_chain, "
	      "CLUSTER 'c'" +
	      ignored + ")");
	f.Run("ATTACH '" + string(LONG_ARN) + "' AS rs_arn (TYPE redshift, CREDENTIAL_PROVIDER aws_default_chain" +
	      ignored + ")");
}

TEST_CASE("a secret chosen for a cluster identifier path is held to the secret name rule",
          "[attach][config][cluster_path][secret]") {
	// The rule SECRET's value meets applies to the secret chosen for the user too: its name is repeated
	// in messages and Describe(), and the client refuses it at connect.
	Fixture f;
	redshift_test::RegisterAwsSecretStandIns(*f.db.instance, nullptr, "dk", "ds");
	f.Run("CREATE SECRET \"a}b\" (TYPE aws, KEY_ID 'k', SECRET 's')");
	CHECK(StringUtil::StartsWith(f.Refusal(Options({{"REGION", "us-east-1"}}), nullptr, CLUSTER_PATH),
	                             "Invalid parameter 'SECRET': it must be the name of a DuckDB secret"));
}

TEST_CASE("a cluster identifier with no SECRET or CREDENTIAL_PROVIDER takes the only user aws or s3 secret",
          "[attach][config][cluster_path][secret]") {
	Fixture f;
	const auto region = Options({{"REGION", "us-east-1"}});
	// Without one, the default credential chain: neither a secret nor a profile.
	auto config = f.Resolve(region, nullptr, CLUSTER_PATH);
	CHECK(config.aws_secret.empty());
	CHECK(config.profile.empty());

	std::atomic<idx_t> default_lookups {0};
	redshift_test::RegisterAwsSecretStandIns(*f.db.instance, &default_lookups, "dk", "ds");
	// The aws extension's default secrets are not the user's.
	CHECK(f.Resolve(region, nullptr, CLUSTER_PATH).aws_secret.empty());
	CHECK(default_lookups == 0);

	f.Run("CREATE SECRET bucket (TYPE s3, KEY_ID 'k', SECRET 's', SCOPE 's3://bucket')");
	CHECK(f.Resolve(region, nullptr, CLUSTER_PATH).aws_secret == "bucket");
	// An aws secret comes before an s3 secret, whatever its scope.
	f.Run("CREATE SECRET mine (TYPE aws, KEY_ID 'k', SECRET 's', SCOPE 's3://elsewhere')");
	config = f.Resolve(region, nullptr, CLUSTER_PATH);
	CHECK(config.aws_secret == "mine");
	CHECK(config.Describe().find("mine") != string::npos);
	// Two of the type it would take are refused, asking for SECRET.
	f.Run("CREATE SECRET other (TYPE aws, KEY_ID 'k', SECRET 's')");
	CHECK(f.Refusal(region, nullptr, CLUSTER_PATH) ==
	      "there are 2 secrets of type aws and no SECRET; name the one to use with SECRET");
	CHECK(f.Resolve(region, "other", CLUSTER_PATH).aws_secret == "other");
	// PROFILE without CREDENTIAL_PROVIDER is refused for itself before a secret is chosen, so two secrets do
	// not answer it with a refusal about SECRET.
	CHECK(f.Refusal(Options({{"REGION", "us-east-1"}, {"PROFILE", "p"}}), nullptr, CLUSTER_PATH) ==
	      PROFILE_WITHOUT_PROFILE_PROVIDER);
	// A stated credential source is never replaced.
	config = f.Resolve(Chain({}), nullptr, CLUSTER_PATH);
	CHECK(config.aws_secret.empty());
	CHECK(f.Resolve(Options({{"REGION", "us-east-1"}, {"CREDENTIAL_PROVIDER", "profile"}, {"PROFILE", "p"}}), nullptr,
	                CLUSTER_PATH)
	          .profile == "p");
	f.Run("DROP SECRET other");
	// PROFILE goes only with CREDENTIAL_PROVIDER profile, beside a secret picked or not.
	CHECK(f.Refusal(Options({{"REGION", "us-east-1"}, {"PROFILE", "p"}}), nullptr, CLUSTER_PATH) ==
	      PROFILE_WITHOUT_PROFILE_PROVIDER);
	// Two s3 secrets do not matter while there is one aws secret: the aws type is taken first.
	f.Run("CREATE SECRET bucket2 (TYPE s3, KEY_ID 'k', SECRET 's')");
	CHECK(f.Resolve(region, nullptr, CLUSTER_PATH).aws_secret == "mine");
	f.Run("DROP SECRET mine");
	CHECK(f.Refusal(region, nullptr, CLUSTER_PATH) ==
	      "there are 2 secrets of type s3 and no SECRET; name the one to use with SECRET");
	// Beside any other form, no secret is chosen for the user.
	f.Run("DROP SECRET bucket2");
	CHECK(f.Refusal(Options({{"REGION", "us-east-1"}, {"CLUSTER", "c"}})) == MISSING_CREDENTIAL_PROVIDER);
	CHECK(f.Refusal(Options({{"REGION", "us-east-1"}}), nullptr, LONG_ARN) == MISSING_CREDENTIAL_PROVIDER);
}

TEST_CASE("a cluster identifier with no secret to take is refused while aws_default_credentials is false",
          "[attach][config][cluster_path][secret]") {
	Fixture f;
	std::atomic<idx_t> default_lookups {0};
	redshift_test::RegisterAwsSecretStandIns(*f.db.instance, &default_lookups, "dk", "ds");
	f.Run("SET aws_default_credentials = false");
	const auto region = Options({{"REGION", "us-east-1"}});
	CHECK(f.Refusal(region, nullptr, CLUSTER_PATH) ==
	      "Missing required parameter 'SECRET': there is no aws or s3 secret to use, and aws_default_credentials is "
	      "false, so the default credential chain is not used either. Name a DuckDB aws or s3 secret in SECRET, or set "
	      "CREDENTIAL_PROVIDER");
	// A stated source, or a user secret, is still used.
	CHECK(f.Resolve(Chain({}), nullptr, CLUSTER_PATH).aws_secret.empty());
	f.Run("CREATE SECRET mine (TYPE aws, KEY_ID 'k', SECRET 's')");
	CHECK(f.Resolve(region, nullptr, CLUSTER_PATH).aws_secret == "mine");
}

TEST_CASE("a cluster identifier with no REGION falls back as the aws extension does",
          "[attach][config][cluster_path][region]") {
	Fixture f;
	redshift_test::RegisterAwsSecretStandIns(*f.db.instance);
	f.Run("CREATE SECRET west (TYPE aws, KEY_ID 'k', SECRET 's', REGION 'us-west-2')");
	f.Run("CREATE SECRET noregion (TYPE aws, KEY_ID 'k', SECRET 's')");
	const auto chain = ChainOnly();
	{
		ScopedRegionEnvironment environment("eu-west-1", "eu-west-2");
		// REGION first, then the AWS secret's region, then AWS_REGION.
		CHECK(f.Resolve(Chain({}), nullptr, CLUSTER_PATH).region == "us-east-1");
		CHECK(f.Resolve({}, "west", CLUSTER_PATH).region == "us-west-2");
		CHECK(f.Resolve({}, "noregion", CLUSTER_PATH).region == "eu-west-1");
		CHECK(f.Resolve(chain, nullptr, CLUSTER_PATH).region == "eu-west-1");
		// HOST's region comes before the secret's, which it need not match, and before the environment's.
		CHECK(f.Resolve(Options({{"HOST", CLUSTER_HOST}}), "west", CLUSTER_PATH).region == "us-east-1");
		CHECK(f.Resolve(Options({{"CREDENTIAL_PROVIDER", "aws_default_chain"}, {"HOST", CLUSTER_HOST}}), nullptr,
		                CLUSTER_PATH)
		          .region == "us-east-1");
		// No other form reads the environment.
		CHECK(f.Refusal(Options({{"CREDENTIAL_PROVIDER", "aws_default_chain"}, {"CLUSTER", "c"}})) == MISSING_REGION);
		CHECK(f.Refusal({{"cluster", Value("c")}}, "noregion") ==
		      "Missing required parameter 'REGION': set REGION, or include a region in the aws or s3 secret "
		      "'noregion'");
	}
	{
		ScopedRegionEnvironment environment(nullptr, "eu-west-2");
		CHECK(f.Resolve(chain, nullptr, CLUSTER_PATH).region == "eu-west-2");
		// An empty AWS_REGION is not set.
		ScopedRegionEnvironment empty("", "eu-west-2");
		CHECK(f.Resolve(chain, nullptr, CLUSTER_PATH).region == "eu-west-2");
	}
	{
		ScopedRegionEnvironment environment(nullptr, nullptr);
		// Without httpfs, the s3_region setting does not exist.
		CHECK(f.Refusal(chain, nullptr, CLUSTER_PATH) ==
		      "Missing required parameter 'REGION': set REGION, the s3_region setting, or AWS_REGION or "
		      "AWS_DEFAULT_REGION in the environment");
		CHECK(f.Refusal({}, "noregion", CLUSTER_PATH) ==
		      "Missing required parameter 'REGION': set REGION, include a region in the aws or s3 secret 'noregion', "
		      "or set the s3_region setting, or AWS_REGION or AWS_DEFAULT_REGION in the environment");
		// httpfs's s3_region, with no default, comes before the environment once set.
		DBConfig::GetConfig(*f.db.instance).AddExtensionOption("s3_region", "S3 Region", LogicalType::VARCHAR, Value());
		CHECK(f.Refusal(chain, nullptr, CLUSTER_PATH).find("Missing required parameter 'REGION'") == 0);
		// s3_region set to NULL is not set either.
		f.Run("SET s3_region = NULL");
		CHECK(f.Refusal(chain, nullptr, CLUSTER_PATH) ==
		      "Missing required parameter 'REGION': set REGION, the s3_region setting, or AWS_REGION or "
		      "AWS_DEFAULT_REGION in the environment");
		f.Run("SET s3_region = 'ap-south-1'");
		CHECK(f.Resolve(chain, nullptr, CLUSTER_PATH).region == "ap-south-1");
		CHECK(f.Resolve({}, "noregion", CLUSTER_PATH).region == "ap-south-1");
		ScopedRegionEnvironment set("eu-west-1", nullptr);
		CHECK(f.Resolve(chain, nullptr, CLUSTER_PATH).region == "ap-south-1");
		CHECK(f.Resolve({}, "west", CLUSTER_PATH).region == "us-west-2");
		// An empty s3_region is not set, so the environment comes next.
		f.Run("SET s3_region = ''");
		CHECK(f.Resolve(chain, nullptr, CLUSTER_PATH).region == "eu-west-1");
		// The aws extension's bare form: no REGION and no SECRET, so the only user aws secret is taken, and its
		// region comes before s3_region and the environment.
		f.Run("SET s3_region = 'ap-south-1'");
		f.Run("DROP SECRET noregion");
		const auto config = f.Resolve({}, nullptr, CLUSTER_PATH);
		CHECK(config.aws_secret == "west");
		CHECK(config.region == "us-west-2");
	}
}
