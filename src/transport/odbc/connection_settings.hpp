//===----------------------------------------------------------------------===//
//                         DuckDB - Redshift
//
// src/transport/odbc/connection_settings.hpp
//
// Everything the driver is told about a connection is encoded here, each value checked first: a
// customer cannot pass a connection string, DSN or driver option. The trust
// source and the refused environment are settled elsewhere; only the test endpoints are read from
// the environment here. No ODBC header: tested offline.
//===----------------------------------------------------------------------===//

#pragma once

#include "duckdb/common/string.hpp"
#include "duckdb/common/typedefs.hpp"
#include "duckdb/common/vector.hpp"
#include "odbc/build_identity.hpp"
#include "odbc/target_hints.hpp"
#include "odbc/trust_store.hpp"
#include "redshift_transport.hpp"

namespace duckdb {

//! Throws InvalidInputException, before any driver call, unless `region` is exactly, case included,
//! an `aws`-partition name in the generated allowlist: it goes into the host name,
//! where quoting cannot help. A listed region need not have the target a configuration names.
void ValidateRegion(const string &region);

//! Throws InvalidInputException, before any driver call, for a profile the driver would not use as
//! written: one holding NUL, an ASCII control character or `}`, starting or
//! ending with a space, or over 255 bytes. An empty profile is valid: the default credential chain.
void ValidateProfile(const string &profile);

//! What the connection settings and a connect failure take from one target.
struct TargetSettings {
	//! A workgroup's derived host, which Target() and Host name unless a test endpoint replaces it
	//! (ConnectHost), or a cluster endpoint's, which Host names the same way. Empty for a cluster named
	//! by its identifier alone, whose host and port the driver looks up.
	string host;
	//! A cluster endpoint's port, which Port names with `host`. 0 otherwise: a workgroup's is 5439.
	int port = 0;
	//! The Workgroup or the ClusterId setting: one is empty.
	string workgroup;
	string cluster;
	string database;
	TargetHints hints;
};

//! Throws InvalidInputException, before any driver call, for a refused region (ValidateRegion), a
//! configuration with no target or one outside the enum, fields that do not fit its target, a
//! workgroup, account, cluster, endpoint label, port or database value outside its rules, or a
//! target this build has no settings for. Otherwise the target's settings, built only from values
//! that passed.
TargetSettings ResolveTarget(const RedshiftConfig &config);

//! ResolveTarget's settings once ValidateRegion and then ValidateProfile have passed: the order in
//! which a connect refuses its settings. Throws what the three throw.
TargetSettings ValidateAndResolveTarget(const RedshiftConfig &config);

//! The settings of a Serverless workgroup with no account and no endpoint label, which ResolveTarget
//! asks for once the region, the fields and the workgroup name have passed. The public build's
//! refuses every one with InvalidInputException; a build with a preview implementation reaches the
//! ones the target hook accepts. A workgroup with an endpoint label never reaches it: ResolveTarget builds its
//! host from the label, in every build.
TargetSettings AccountlessWorkgroupSettings(const RedshiftConfig &config);

//! The longest profile the driver uses as written: rsconnect.cpp parseConnectString copies Profile
//! into a 256-byte buffer with rs_strncpy, so a longer name arrives cut to 255 bytes, not the
//! unchanged profile.
constexpr idx_t PROFILE_MAX_BYTES = 255;
//! The longest AWS secret name the client accepts. It only appears in messages, never reaches the
//! driver, and passes the encoder's value rules so a message cannot carry control bytes.
constexpr idx_t AWS_SECRET_NAME_MAX_BYTES = 255;
//! What driver 2.3.0.0 keeps of an AccessKeyID or SecretAccessKey, and of a SessionToken
//! (rsiam.h MAX_IAM_BUF_VAL and MAX_IAM_SESSION_TOKEN_LEN, less the terminating NUL).
constexpr idx_t AWS_KEY_MAX_BYTES = 255;
constexpr idx_t AWS_SESSION_TOKEN_MAX_BYTES = 2047;

//! The longest trust path both hops get whole. Driver 2.3.0.0's rsconnect.cpp
//! copyCommonConnectionProperties cut CaFile to 255 bytes for the control plane while
//! parseConnectString kept 259 of TrustStore; 2.3.1.0 keeps up to 1,023 bytes of each and drops a
//! longer value (storeCertPathOrRecordOverflow). The limit stays at 2.3.0.0's, which both take whole,
//! so the two hops are never verified against different files.
constexpr idx_t TRUST_PATH_MAX_BYTES = 255;

//! Why the driver could not be given `path` as its trust source as written, as a clause after "it",
//! or empty: an empty path, then Add's value rule, then TRUST_PATH_MAX_BYTES, then, on Windows, a
//! byte outside ASCII. Public so the trust search can refuse a customer's path with its own fix
//! before the path reaches Add.
string FindUnusableTrustPathReason(const string &path);

//! Whether `keyword`, compared in any case as the driver compares keywords, is one of the driver's
//! or its libpq's Kerberos or GSS options, aliases included. Add refuses them.
bool IsKerberosOrGssKeyword(const string &keyword);

//! The driver's connection string, built one setting at a time. `Add` is the only way to append,
//! so every value in it has been checked, whether the client derived it or a customer gave it.
class ConnectionStringEncoder {
public:
	//! Appends `keyword={value};`, the brace-quoted form the driver reads, checking no length: each
	//! setting has its own limit. Throws InvalidInputException for a value holding NUL, an ASCII
	//! control character or `}`, or starting or ending with a space, and InternalException for a
	//! repeated, malformed, Kerberos or GSS `keyword`, which is a fixed name and never customer text.
	void Add(const string &keyword, const string &value);

	//! The settings added so far, in the order they were added.
	string Build() const;

private:
	string text;
	//! Every keyword added, so a second one can be refused.
	vector<string> keywords;
};

//! The host and control-plane URL a test build's environment aims a connection at; both empty, the
//! usual case, when the connection goes where the target derives.
struct TestEndpoints {
	string host;
	string endpoint_url;
};

//! The end-to-end test variables: the first selects an end-to-end SQL test, the
//! other two aim its connection. The Makefile's SQL test runs unset them by these names, which a
//! test compares, so every reader and unsetter uses these.
constexpr const char *TEST_CASE_VARIABLE = "DUCKDB_REDSHIFT_TEST_E2E";
constexpr const char *TEST_URL_VARIABLE = "DUCKDB_REDSHIFT_TEST_ENDPOINT_URL";
constexpr const char *TEST_HOST_VARIABLE = "DUCKDB_REDSHIFT_TEST_HOST";
constexpr const char *TEST_VARIABLES[] = {TEST_CASE_VARIABLE, TEST_URL_VARIABLE, TEST_HOST_VARIABLE};

//! Whether this build has the test-endpoint override: only one configured with
//! REDSHIFT_TEST_ENDPOINTS, which sources.cmake allows in one test build directory alone.
bool BuildHasTestEndpoints();

//! The longest test host or URL a test-endpoint build takes: the driver copies EndPointUrl into its
//! 256-byte szEndpointUrl (rsiam.h), so a longer URL would reach it cut. The host takes the same
//! bound, which no DNS name exceeds.
constexpr idx_t TEST_ENDPOINT_MAX_BYTES = 255;

//! Refuses, with InvalidInputException naming variables and never a value, the end-to-end test
//! variables that `test_endpoints_build` cannot honour, or an empty, over-long or missing one in a
//! build that can; otherwise returns the endpoints they name. Reads only the
//! environment, through ReadEnvironmentVariable.
TestEndpoints ReadTestEndpoints(bool test_endpoints_build);

//! Throws InvalidInputException unless AWS credentials come exactly with `config.aws_secret`,
//! never beside a profile, the secret's name passes the encoder's value rules, and
//! the key ID, secret key and session token pass them within the driver's length limits.
void ValidateCredentialSource(const RedshiftConfig &config, optional_ptr<const RedshiftAwsCredentials> credentials);

//! The connection string for `config`: ResolveTarget's settings, `trust` as both TrustStore and
//! CaFile, Profile only when not empty, and AccessKeyID, SecretAccessKey and SessionToken from
//! `credentials`, which is given exactly when `config.aws_secret` is set. Throws
//! before encoding anything: InvalidInputException for a refused region, profile, target, port or
//! credential source, InternalException for an unusable `trust`.
string BuildConnectionString(const RedshiftConfig &config, const TrustSource &trust,
                             optional_ptr<const RedshiftAwsCredentials> credentials = nullptr);

//! The same, with `extension_version` in ApplicationName; the form above passes
//! the build's version.
string BuildConnectionString(const RedshiftConfig &config, const TrustSource &trust, const string &extension_version,
                             optional_ptr<const RedshiftAwsCredentials> credentials = nullptr);

//! The same, with the host and control-plane URL that `test_endpoints` names in place of the
//! derived ones when it names any, each value through the encoder.
//! A cluster named by its identifier alone takes the URL alone, as it is sent no Host; a cluster
//! endpoint takes both.
string BuildConnectionString(const RedshiftConfig &config, const TrustSource &trust,
                             const TestEndpoints &test_endpoints,
                             optional_ptr<const RedshiftAwsCredentials> credentials = nullptr);

//! The Host setting of a workgroup or a cluster endpoint: the test host when `test_endpoints` names
//! one, else the target's.
string ConnectHost(const TargetSettings &target, const TestEndpoints &test_endpoints);

//! What Target() names before the configuration: a cluster's identifier, whether or not a Host is
//! sent to it, else ConnectHost.
string TargetName(const TargetSettings &target, const TestEndpoints &test_endpoints);

//! The longest ApplicationName sent: NAMEDATALEN - 1, what a Postgres server keeps of
//! application_name. Redshift keeps more (the 255 bytes the driver passes on, 250 in its connection
//! log), so the name arrives whole wherever it is read.
constexpr idx_t APPLICATION_NAME_MAX_BYTES = 63;

//! The ApplicationName every BuildConnectionString sends, which Redshift's connection log shows:
//! `duckdb_redshift/<extension_version> duckdb/<duckdb_version> <platform>`. A part that is empty
//! or not one token of letters, digits, '.', '_' and '-' is left out. Past
//! APPLICATION_NAME_MAX_BYTES the platform goes first, then the DuckDB part, then the extension's
//! version, so `duckdb_redshift` always stays and nothing is cut inside a part.
string ApplicationName(const string &extension_version, const string &duckdb_version, const string &platform);

//! The same for `build`'s three parts.
string ApplicationName(const BuildIdentity &build);

//! Whether `connection_string`, as BuildConnectionString returns it, sets `keyword` in any case, as
//! the driver compares keywords; an alias does not count.
bool SetsKeyword(const string &connection_string, const string &keyword);

} // namespace duckdb
