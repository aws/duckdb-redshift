// Connect failures: the category, its fixed hint, the SQLSTATEs and the driver's identity, and no
// word of the driver's text except, for a failure no category names, its redacted, sanitized reason. Every expected
// message is written out here, so a change to the client's wording fails a case.

// catch.hpp comes first, as the extension's C++ tests include it: DuckDB's copy declares the
// duckdb_base_std aliases it needs only while no DuckDB header has opened that namespace.
#include "catch.hpp"

#include "diagnostic_fixtures.hpp"
#include "duckdb/common/exception.hpp"
#include "odbc/odbc_errors.hpp"
#include "thrown.hpp"

#include <string>
#include <type_traits>
#include <utility>
#include <vector>

using duckdb::ConnectCategory;
using duckdb::ExceptionType;
using duckdb::redshift_driver::DriverIdentity;
using duckdb::redshift_driver::DriverOrigin;
using duckdb::redshift_driver::testing::FakeDiag;
using redshift_test::Capture;
using redshift_test::CaptureError;
using redshift_test::ConnectRecords;
using redshift_test::Thrown;

namespace {

const std::string TARGET = "wg.example (region us-east-1)";

DriverIdentity Identity() {
	DriverIdentity identity;
	identity.origin = DriverOrigin::TEST_FAKE;
	identity.version = "2.3.0.0";
	identity.platform = "linux_amd64";
	return identity;
}

//! A permission hint written here, so a denied connect is shown to carry its target's own.
const std::string PERMISSION_HINT = "Grant it the target's actions on its resource.";

//! One timed-out attempt of 61 seconds, on a workgroup.
duckdb::ConnectFailureContext Context() {
	return {TARGET, {"workgroup", true, PERMISSION_HINT, ""}, Identity(), 1, 61};
}

//! The same on a cluster, which does not resume from idle.
duckdb::ConnectFailureContext ClusterContext() {
	return {TARGET, {"cluster", false, PERMISSION_HINT, ""}, Identity(), 1, 61};
}

//! What ThrowConnectFailure throws for a connect that failed with `diags`, on a workgroup unless
//! `context` says otherwise.
Thrown ConnectFailure(std::vector<FakeDiag> diags, const duckdb::ConnectFailureContext &context = Context()) {
	const auto records = ConnectRecords(SQL_ERROR, std::move(diags));
	return Capture([&]() { duckdb::ThrowConnectFailure(records, context); });
}

ConnectCategory CategoryOf(std::vector<FakeDiag> diags) {
	const auto records = ConnectRecords(SQL_ERROR, std::move(diags));
	return duckdb::ClassifyConnect(records).category;
}

//! Whether ClassifyConnect can be called with an argument of type `T`.
template <class T, class = void>
struct Classifies : std::false_type {};
template <class T>
struct Classifies<T, decltype(void(duckdb::ClassifyConnect(std::declval<T>())))> : std::true_type {};

//! The driver's prefix on every IAM failure measured on driver 2.3.0.0.
const std::string IAM_PREFIX = "[Redshift][ODBC Driver][Server][860:8:IAMConnectionError]: ";

//! The credentials message up to the SQLSTATE it ends with.
const std::string CREDENTIALS_LEAD =
    "Could not connect to Redshift at wg.example (region us-east-1): no usable AWS credentials were found through "
    "the AWS credential chain.\nCheck the credentials this process can reach: the default AWS credential chain, or "
    "the profile PROFILE names.\nSQLSTATE: ";

const std::string CREDENTIALS_MESSAGE = CREDENTIALS_LEAD + "HY000";

//! The credentials message for credentials the chain found and the service refused.
const std::string REFUSED_MESSAGE =
    "Could not connect to Redshift at wg.example (region us-east-1): the AWS credentials found through the AWS "
    "credential chain were refused or have expired.\nCheck the credentials this process can reach: the default AWS "
    "credential chain, or the profile PROFILE names.\nSQLSTATE: HY000";

//! Driver 2.3.0.0's record for credentials the service does not recognise.
const std::string UNRECOGNIZED_CLIENT =
    IAM_PREFIX + "UnrecognizedClientException: The security token included in the request is invalid";

//! The credentials message for credentials the service does not recognise, which also names the region.
const std::string UNRECOGNIZED_MESSAGE =
    "Could not connect to Redshift at wg.example (region us-east-1): the AWS credentials found through the AWS "
    "credential chain were refused or have expired.\nCheck the credentials this process can reach: the default AWS "
    "credential chain, or "
    "the profile PROFILE names. If these credentials work in other regions, the region may be an opt-in region the "
    "account has not enabled or their session token does not cover; check it.\nSQLSTATE: HY000";

} // namespace

TEST_CASE("Each credential failure measured on driver 2.3.0.0 gives the credentials message with its hint", "[odbc]") {
	CHECK(ConnectFailure({{"HY000", 0, IAM_PREFIX + "No AWS profile found: no-such-profile"}}) ==
	      Thrown {ExceptionType::IO, CREDENTIALS_MESSAGE});
	CHECK(ConnectFailure(
	          {{"HY000", 0, IAM_PREFIX + "MissingAuthenticationTokenException: Missing Authentication Token"}}) ==
	      Thrown {ExceptionType::IO, CREDENTIALS_MESSAGE});
}

TEST_CASE("Credentials the service does not recognise get the credentials hint and the region's", "[odbc]") {
	// As driver 2.3.0.0 gave it for an opt-in region, and for credentials it does not know.
	CHECK(ConnectFailure({{"HY000", 0, UNRECOGNIZED_CLIENT}}) == Thrown {ExceptionType::IO, UNRECOGNIZED_MESSAGE});
	// Only the code adds it: the phrase alone, or the code inside a name, gives the plain hint.
	CHECK(ConnectFailure({{"HY000", 0, "The security token included in the request is invalid"}}) ==
	      Thrown {ExceptionType::IO, REFUSED_MESSAGE});
	CHECK(ConnectFailure({{"HY000", 0, IAM_PREFIX + "No AWS profile found: unrecognizedclientexception-lab"}}) ==
	      Thrown {ExceptionType::IO, CREDENTIALS_MESSAGE});
	// The record the category acted on decides: an earlier credentials record keeps the plain hint.
	CHECK(ConnectFailure({{"HY000", 0, IAM_PREFIX + "No AWS profile found: p"}, {"HY000", 0, UNRECOGNIZED_CLIENT}}) ==
	      Thrown {ExceptionType::IO, CREDENTIALS_MESSAGE});
	// Another category's record keeps its own hint.
	auto context = Context();
	context.hints.availability = "Check the target's name.";
	CHECK(ConnectFailure({{"HY000", 0, IAM_PREFIX + "ResourceNotFoundException: UnrecognizedClientException"}},
	                     context) ==
	      Thrown {ExceptionType::IO, "Could not connect to Redshift at wg.example (region us-east-1): the Redshift "
	                                 "workgroup was not found.\nCheck the target's name.\nSQLSTATE: HY000"});
	// An AWS secret's credentials get the secret's hint, then the region's.
	context.hints.aws_secret = "creds";
	CHECK(ConnectFailure({{"HY000", 0, UNRECOGNIZED_CLIENT}}, context) ==
	      Thrown {ExceptionType::IO,
	              "Could not connect to Redshift at wg.example (region us-east-1): the AWS credentials from AWS secret "
	              "'creds' were refused or have expired.\nThis extension does not refresh an AWS secret's credentials: "
	              "check the secret's keys, or recreate the secret to get new temporary credentials (for the aws "
	              "extension's default secret, reopen the database), then retry. If these credentials work in other "
	              "regions, the region may be an opt-in region the account has not enabled or their session token does "
	              "not cover; check it.\nSQLSTATE: HY000"});
}

TEST_CASE("An expired AWS token gives the credentials message with its hint", "[odbc]") {
	// As measured on driver 2.3.0.0, from a session token past its expiry.
	const std::string expired = "[860:8:IAMConnectionError]: ExpiredTokenException: Unable to parse ExceptionName: "
	                            "ExpiredTokenException Message: The security token included in the request is expired";
	CHECK(ConnectFailure({{"HY000", 0, expired}}) == Thrown {ExceptionType::IO, REFUSED_MESSAGE});
	// Either phrase alone is enough.
	CHECK(ConnectFailure({{"HY000", 0, "ExpiredTokenException"}}) == Thrown {ExceptionType::IO, REFUSED_MESSAGE});
	CHECK(ConnectFailure({{"HY000", 0, "The security token included in the request is expired"}}) ==
	      Thrown {ExceptionType::IO, REFUSED_MESSAGE});
}

TEST_CASE("A diagnostic naming no credential provider gives the credentials message", "[odbc]") {
	CHECK(ConnectFailure({{"HY000", 0, "No credential providers found in the chain"}}) ==
	      Thrown {ExceptionType::IO, CREDENTIALS_MESSAGE});
}

TEST_CASE("A wrong secret key gives the credentials message, from either signature error", "[odbc]") {
	// Driver 2.3.0.0's records for a wrong secret key, as measured: on a serverless workgroup it names
	// InvalidSignatureException, as Redshift Serverless's JSON API does, and on a provisioned cluster
	// SignatureDoesNotMatch, from the cluster's credentials request, a query API.
	const std::string text = "The request signature we calculated does not match the signature you provided. Check "
	                         "your AWS Secret Access Key and signing method. Consult the service documentation for "
	                         "details.";
	CHECK(ConnectFailure({{"HY000", 0, IAM_PREFIX + "InvalidSignatureException: " + text}}) ==
	      Thrown {ExceptionType::IO, REFUSED_MESSAGE});
	CHECK(ConnectFailure({{"HY000", 0, IAM_PREFIX + "SignatureDoesNotMatch: " + text}}) ==
	      Thrown {ExceptionType::IO, REFUSED_MESSAGE});
	// Each name, and the message, alone is enough.
	CHECK(CategoryOf({{"HY000", 0, "InvalidSignatureException"}}) == ConnectCategory::CREDENTIALS);
	CHECK(CategoryOf({{"HY000", 0, "SignatureDoesNotMatch"}}) == ConnectCategory::CREDENTIALS);
	CHECK(CategoryOf({{"HY000", 0, text}}) == ConnectCategory::CREDENTIALS);
}

TEST_CASE("A profile chain that loops or runs too long gives the credentials message", "[odbc]") {
	// The driver's own texts for a source_profile chain (IAMProfileCredentialsProvider.cpp LoadProfile).
	// Driver 2.3.0.0 gave the loop's for a PROFILE that loops; through AWS_PROFILE the default chain
	// sends no credentials instead, MissingAuthenticationTokenException.
	CHECK(ConnectFailure({{"HY000", 0, IAM_PREFIX + "Chained profiles loop, please update profile settings."}}) ==
	      Thrown {ExceptionType::IO, CREDENTIALS_MESSAGE});
	CHECK(ConnectFailure(
	          {{"HY000", 0, IAM_PREFIX + "Maximum chained profiles reached, please update profile settings."}}) ==
	      Thrown {ExceptionType::IO, CREDENTIALS_MESSAGE});
}

TEST_CASE("A certificate failure gives the certificate message with its hint", "[odbc]") {
#ifdef _WIN32
	// Windows' hint names its root store and the missing-roots case.
	const Thrown expected {ExceptionType::IO,
	                       "Could not connect to Redshift at wg.example (region us-east-1): the server's TLS "
	                       "certificate could not be verified.\nCheck the trusted CA certificates, from SSL_CERT_FILE "
	                       "or the Windows root store, and the network path to the Redshift host. A host whose "
	                       "automatic root update is off, or has not run yet, may lack the roots Redshift's "
	                       "certificates chain to: turn it on, or let it run, then connect again; or set "
	                       "SSL_CERT_FILE. Verification cannot be turned off.\nSQLSTATE: HY000"};
#else
	const Thrown expected {ExceptionType::IO,
	                       "Could not connect to Redshift at wg.example (region us-east-1): the server's TLS "
	                       "certificate could not be verified.\nCheck the trusted CA certificates, from SSL_CERT_FILE "
	                       "or the system store, and the network path to the Redshift host. Verification cannot be "
	                       "turned off.\nSQLSTATE: HY000"};
#endif
	CHECK(ConnectFailure({{"HY000", 0, "curlCode: 77, Problem with the SSL CA cert (path? access rights?)"}}) ==
	      expected);
	CHECK(ConnectFailure({{"HY000", 0, "curlCode: 60, SSL peer certificate or SSH remote key was not OK"}}) ==
	      expected);
	CHECK(ConnectFailure({{"HY000", 0, "SSL error: certificate verify failed"}}) == expected);
	// As tls_verification_test.cpp recorded them on driver 2.3.0.0: the database hop's chain and
	// host-name failures, and the control plane's chain failure.
	CHECK(ConnectFailure({{"HY000", 0, "[Redshift][ODBC Driver][Server]SSL error: CERTIFICATE_VERIFY_FAILED"}}) ==
	      expected);
	CHECK(ConnectFailure({{"HY000", 0,
	                       "[Redshift][ODBC Driver][Server]server certificate does not match host name "
	                       "\"localhost\""}}) == expected);
	CHECK(ConnectFailure({{"HY000", 0,
	                       IAM_PREFIX + ": curlCode: 60, SSL peer certificate or SSH remote key was not OK; Details: "
	                                    "SSL certificate OpenSSL verify result: unable to get local issuer certificate "
	                                    "(20)"}}) == expected);
	CHECK(ConnectFailure({{"HY000", 0, "server certificate does not match host name \"x\""}}) == expected);
	CHECK(ConnectFailure({{"HY000", 0, "root certificate file \"/x\" does not exist"}}) == expected);
}

TEST_CASE("The Windows control plane's text for a request with no response gets the general message", "[odbc]") {
	// As tls_verification_test.cpp recorded it on Windows on driver 2.3.0.0 for a certificate the
	// store does not trust. The SDK gives it to every request that fails before a response, so it
	// cannot tell a certificate failure from a host it cannot reach.
	const std::string failed_request = IAM_PREFIX + ": Encountered network error when sending http request";
	CHECK(CategoryOf({{"HY000", 0, failed_request}}) == ConnectCategory::OTHER);
	CHECK(ConnectFailure({{"HY000", 0, failed_request}}) ==
	      Thrown {ExceptionType::IO, "Could not connect to Redshift at wg.example (region us-east-1): the connection "
	                                 "attempt failed. The driver reported \"Encountered network error when sending "
	                                 "http request\".\nCheck the region and that this host can reach the workgroup, "
	                                 "then retry.\nSQLSTATE: HY000"});
}

TEST_CASE("A denied connect gives the permission message with its target's hint", "[odbc]") {
	const std::string message = "Could not connect to Redshift at wg.example (region us-east-1): the AWS identity is "
	                            "not allowed to connect to the workgroup.\nGrant it the target's actions on its "
	                            "resource.\nSQLSTATE: ";
	CHECK(ConnectFailure({{"42501", 0, "permission denied"}}) == Thrown {ExceptionType::IO, message + "42501"});
	CHECK(ConnectFailure({{"HY000", 0, IAM_PREFIX + "AccessDeniedException: not authorized"}}) ==
	      Thrown {ExceptionType::IO, message + "HY000"});
	CHECK(ConnectFailure({{"HY000", 0, "insufficient privileges"}}) == Thrown {ExceptionType::IO, message + "HY000"});
}

TEST_CASE("A cluster's denied DescribeClusters gives the permission message with its target's hint", "[odbc]") {
	// The query APIs' AccessDenied, with no "Exception"; modelled, not measured.
	CHECK(ConnectFailure({{"HY000", 0,
	                       IAM_PREFIX + "AccessDenied: User: arn:aws:iam::123456789012:user/u is not authorized to "
	                                    "perform: redshift:DescribeClusters"}},
	                     ClusterContext()) ==
	      Thrown {ExceptionType::IO, "Could not connect to Redshift at wg.example (region us-east-1): the AWS identity "
	                                 "is not allowed to connect to the cluster.\n" +
	                                     PERMISSION_HINT + "\nSQLSTATE: HY000"});
}

//! GetCredentials' not-found answer: the name and phrase driver 2.3.0.0 gave (workgroup renamed), in the
//! driver's IAM error layout.
const std::string NOT_FOUND = IAM_PREFIX + "ResourceNotFoundException: Serverless workgroup analytics not found.";
//! A cluster's not-found answer, as GetClusterCredentialsWithIAM gave it on driver 2.3.0.0 (cluster renamed).
const std::string CLUSTER_NOT_FOUND = IAM_PREFIX + "ClusterNotFound: Cluster warehouse-1 not found.";

TEST_CASE("A workgroup the region and account do not have gives the not-found message with its target's hint",
          "[odbc]") {
	auto context = Context();
	context.hints.availability = "Check the target's name.";
	CHECK(ConnectFailure({{"HY000", 0, NOT_FOUND}}, context) ==
	      Thrown {ExceptionType::IO, "Could not connect to Redshift at wg.example (region us-east-1): the Redshift "
	                                 "workgroup was not found.\nCheck the target's name.\nSQLSTATE: HY000"});
	context = ClusterContext();
	context.hints.availability = "Check the cluster's name.";
	CHECK(ConnectFailure({{"HY000", 0, CLUSTER_NOT_FOUND}}, context) ==
	      Thrown {ExceptionType::IO, "Could not connect to Redshift at wg.example (region us-east-1): the Redshift "
	                                 "cluster was not found.\nCheck the cluster's name.\nSQLSTATE: HY000"});
}

//! libpq's text for a database the server does not have, as the driver stamps it. Its name, which the
//! user chose, must never reach the message.
const std::string NO_DATABASE =
    "[Redshift][ODBC Driver][Server]3D000:FATAL:  database \"secret_sales_db\" does not exist";

TEST_CASE("A host name that does not resolve gives the not-found category, the resolver's cause, and the target's hint",
          "[odbc]") {
	auto context = Context();
	context.hints.availability = "Check the target's name.";
	// As driver 2.3.0.0 gave it, the host renamed.
	CHECK(ConnectFailure({{"HY000", 0,
	                       "[Redshift][ODBC Driver][Server]could not translate host name \"wg.example\" to address: "
	                       "Name or service not known"}},
	                     context) ==
	      Thrown {ExceptionType::IO,
	              "Could not connect to Redshift at wg.example (region us-east-1): the Redshift "
	              "workgroup's host name could not be resolved.\nCheck the target's name.\nSQLSTATE: "
	              "HY000"});
	// macOS's text, the system's, and Windows', from the driver's own libpq getaddrinfo (pg_gai_strerror),
	// both read from the driver's source; unmeasured.
	for (const auto *answer : {"nodename nor servname provided, or not known", "Unknown host"}) {
		INFO(answer);
		CHECK(ConnectFailure({{"HY000", 0,
		                       std::string("[Redshift][ODBC Driver][Server]could not translate host name "
		                                   "\"wg.example\" to address: ") +
		                           answer}},
		                     context)
		          .message.find("could not be resolved.\nCheck the target's name.\nSQLSTATE") != std::string::npos);
	}
	// GetCredentials' answer for a workgroup that does not exist says it was not found, and nothing of the
	// resolver.
	const auto missing = ConnectFailure({{"HY000", 0, "ResourceNotFoundException: Workgroup not found"}}, context);
	CHECK(missing.message.find("the Redshift workgroup was not found.\nCheck the target's name.\n") !=
	      std::string::npos);
	CHECK(missing.message.find("could not be resolved") == std::string::npos);
	// A resolver that failed for now says nothing of the name, so it keeps the general message.
	CHECK(CategoryOf({{"HY000", 0,
	                   "[Redshift][ODBC Driver][Server]could not translate host name \"wg.example\" to address: "
	                   "Temporary failure in name resolution"}}) == ConnectCategory::OTHER);
}

TEST_CASE("A database the target does not have gives the database message with its hint, never the name", "[odbc]") {
	CHECK(ConnectFailure({{"3D000", 0, NO_DATABASE}}) ==
	      Thrown {ExceptionType::IO,
	              "Could not connect to Redshift at wg.example (region us-east-1): the database was not found on the "
	              "Redshift workgroup.\nCheck DATABASE, or the database in the path, against the databases the "
	              "workgroup has.\nSQLSTATE: 3D000"});
	const auto cluster = ConnectFailure({{"3D000", 0, NO_DATABASE}}, ClusterContext());
	CHECK(cluster.message.find("the database was not found on the Redshift cluster.") != std::string::npos);
	CHECK(cluster.message.find("the databases the cluster has") != std::string::npos);
	for (const auto &thrown : {ConnectFailure({{"3D000", 0, NO_DATABASE}}), cluster}) {
		CHECK(thrown.message.find("secret_sales_db") == std::string::npos);
		CHECK(thrown.message.find("does not exist") == std::string::npos);
		CHECK(thrown.message.find("reach the") == std::string::npos);
	}
	// Decided by the state alone: the text without it is not a database failure.
	CHECK(CategoryOf({{"3D000", 0, "anything"}}) == ConnectCategory::DATABASE_NOT_FOUND);
	// The record's text repeats the name, which may spell another rule's code or phrase; the state still
	// decides, and the name never reaches the message.
	for (const auto &name :
	     {"accessdenied", "accessdeniedexception", "expiredtokenexception", "resourcenotfoundexception",
	      "clusternotfound", "insufficient privilege", "no credential provider"}) {
		INFO(name);
		const auto record =
		    std::string("[Redshift][ODBC Driver][Server]3D000:FATAL:  database \"") + name + "\" does not exist";
		CHECK(CategoryOf({{"3D000", 0, record}}) == ConnectCategory::DATABASE_NOT_FOUND);
		const auto thrown = ConnectFailure({{"3D000", 0, record}});
		CHECK(thrown.message.find("the database was not found on the Redshift workgroup.") != std::string::npos);
		CHECK(thrown.message.find(name) == std::string::npos);
	}
	// Another record of the same failure still decides by the rules' order: a credential failure wins.
	CHECK(CategoryOf({{"3D000", 0, NO_DATABASE}, {"28000", 0, "ExpiredTokenException: the token expired"}}) ==
	      ConnectCategory::CREDENTIALS);
	CHECK(CategoryOf({{"HY000", 0, NO_DATABASE}}) == ConnectCategory::OTHER);
	// A 3D000 libpq carried over from an earlier error, which no server severity opens, does not decide it.
	CHECK(CategoryOf({{"3D000", 0, "[Redshift][ODBC Driver][Server]3D000:could not receive data from server"}}) ==
	      ConnectCategory::OTHER);
}

TEST_CASE("An error code inside a name the user chose does not decide the category", "[odbc]") {
	for (const auto &name :
	     {"accessdenied-lab", "lab-accessdenied", "accessdenied_lab", "accessdenied2", "accessdeniedlab",
	      "labaccessdenied", "xclusternotfound", "missingauthenticationtoken-lab", "signaturedoesnotmatch-lab"}) {
		INFO(name);
		CHECK(CategoryOf({{"HY000", 0,
		                   IAM_PREFIX + "ResourceNotFoundException: Serverless workgroup " + name + " not found."}}) ==
		      ConnectCategory::TARGET_NOT_FOUND);
		// With no code before the name, only the whole-word match keeps it out.
		CHECK(CategoryOf({{"HY000", 0, std::string("workgroup ") + name + ": timeout expired"}}) ==
		      ConnectCategory::RESUME_TIMEOUT);
	}
	CHECK(CategoryOf({{"HY000", 0, IAM_PREFIX + "AccessDeniedException: not authorized on clusternotfound-lab"}}) ==
	      ConnectCategory::PERMISSION);
}

TEST_CASE("A database named like the TLS verify-failure code does not make a denial a certificate failure", "[odbc]") {
	CHECK(CategoryOf({{"42501", 0,
	                   "[Redshift][ODBC Driver][Server]42501:FATAL: permission denied for database "
	                   "\"certificate_verify_failed\""}}) == ConnectCategory::PERMISSION);
}

TEST_CASE("An exact target name does not override an earlier error code", "[odbc]") {
	CHECK(CategoryOf({{"HY000", 0, "ResourceNotFoundException: Serverless workgroup accessdenied not found."}}) ==
	      ConnectCategory::TARGET_NOT_FOUND);
	CHECK(CategoryOf({{"HY000", 0, IAM_PREFIX + "AccessDeniedException: not authorized on clusternotfound"}}) ==
	      ConnectCategory::PERMISSION);
}

TEST_CASE("A host name with an error code as one of its labels does not decide the category", "[odbc]") {
	const std::vector<std::string> hosts = {"accessdenied.012345678901.us-east-1.redshift-serverless.amazonaws.com",
	                                        "clusternotfound.abc123xyz789.us-east-1.redshift.amazonaws.com"};
	for (const auto &host : hosts) {
		INFO(host);
		// libpq's texts for a host that does not resolve and for one that does not answer (fe-connect.c).
		const FakeDiag unresolved {
		    "08001", 0, "could not translate host name \"" + host + "\" to address: Name or service not known"};
		const FakeDiag unanswered {
		    "08001", 0,
		    "could not connect to server: Connection timed out\n\tIs the server running on host \"" + host +
		        "\" (10.0.0.1) and accepting\n\tTCP/IP connections on port 5439?"};
		// glibc's phrase decides the first alone, so the second, a resolver failing for now, shows the
		// label does not.
		const FakeDiag unresolved_for_now {"08001", 0,
		                                   "could not translate host name \"" + host +
		                                       "\" to address: Temporary failure in name resolution"};
		CHECK(CategoryOf({unresolved}) == ConnectCategory::TARGET_NOT_FOUND);
		CHECK(CategoryOf({unresolved_for_now}) == ConnectCategory::OTHER);
		CHECK(CategoryOf({unanswered}) == ConnectCategory::OTHER);
		CHECK(CategoryOf({unanswered, {"HYT00", 0, "Timeout"}}) == ConnectCategory::RESUME_TIMEOUT);
	}
	// The last label of a dotted name, too; a '.' that ends a sentence still ends the code.
	CHECK(CategoryOf({{"08001", 0, "could not translate host name \"wg.accessdenied\" to address"}}) ==
	      ConnectCategory::OTHER);
	CHECK(CategoryOf({{"HY000", 0, "AccessDenied. Check the policy."}}) == ConnectCategory::PERMISSION);
}

TEST_CASE("Each one-word error code still decides its category as a whole word", "[odbc]") {
	CHECK(CategoryOf({{"HY000", 0, "AccessDenied"}}) == ConnectCategory::PERMISSION);
	CHECK(CategoryOf({{"HY000", 0, "(AccessDeniedException)"}}) == ConnectCategory::PERMISSION);
	CHECK(CategoryOf({{"HY000", 0, CLUSTER_NOT_FOUND}}) == ConnectCategory::TARGET_NOT_FOUND);
	CHECK(CategoryOf({{"HY000", 0, "Error: MissingAuthenticationToken."}}) == ConnectCategory::CREDENTIALS);
	// A phrase of several words before the code does not count as the first code.
	CHECK(CategoryOf({{"HY000", 0, "timeout expired: AccessDenied"}}) == ConnectCategory::PERMISSION);
	// A code first held inside a longer name still counts where it later stands alone.
	CHECK(CategoryOf({{"HY000", 0, "workgroup accessdenied-lab: AccessDenied"}}) == ConnectCategory::PERMISSION);
	CHECK(CategoryOf({{"HY000", 0, "cluster clusternotfound-1: ClusterNotFound"}}) ==
	      ConnectCategory::TARGET_NOT_FOUND);
}

TEST_CASE("Only a target not found takes the target's availability hint", "[odbc]") {
	auto context = Context();
	context.hints.availability = "The target says more.";
	for (const auto &text : {"No AWS profile found: p", "certificate verify failed", "AccessDeniedException: no",
	                         "timeout expired", "generic"}) {
		INFO(text);
		CHECK(ConnectFailure({{"HY000", 0, text}}, context).message.find("The target says more.") == std::string::npos);
	}
}

TEST_CASE("A 42501 the driver carried over from an earlier server message does not decide the category", "[odbc]") {
	CHECK(CategoryOf({{"42501", 0, "[Redshift][ODBC Driver][Server]42501:could not receive data from server"}}) ==
	      ConnectCategory::OTHER);
}

TEST_CASE("A connect timeout gives the resume-timeout message with its hint and the network-access one", "[odbc]") {
	const std::string message =
	    "Could not connect to Redshift at wg.example (region us-east-1): the Redshift "
	    "Serverless workgroup did not answer within 61 seconds over 1 connect attempt.\nIt "
	    "may still be resuming from idle suspension, which can take a few minutes; retry. If it "
	    "keeps timing out, the workgroup may not be publicly reachable from this "
	    "host's network.\nSQLSTATE: ";
	CHECK(ConnectFailure({{"HY000", 0, "timeout expired"}}) == Thrown {ExceptionType::IO, message + "HY000"});
	CHECK(ConnectFailure({{"HYT00", 0, "Timeout"}}) == Thrown {ExceptionType::IO, message + "HYT00"});
	CHECK(ConnectFailure({{"HYT01", 0, "Connection timeout"}}) == Thrown {ExceptionType::IO, message + "HYT01"});
}

TEST_CASE("A connect timeout on a cluster gives the network-access hint and none of the resume text", "[odbc]") {
	const std::string message = "Could not connect to Redshift at wg.example (region us-east-1): the Redshift cluster "
	                            "did not answer.\nThe cluster may not be publicly reachable from this host's network; "
	                            "check its network access, then retry.\nSQLSTATE: ";
	for (const auto &diag : {FakeDiag {"HY000", 0, "timeout expired"}, FakeDiag {"HYT00", 0, "Timeout"},
	                         FakeDiag {"HYT01", 0, "Connection timeout"}}) {
		INFO(diag.sqlstate);
		const auto thrown = ConnectFailure({diag}, ClusterContext());
		CHECK(thrown == Thrown {ExceptionType::IO, message + diag.sqlstate});
		CHECK(thrown.message.find("resum") == std::string::npos);
		CHECK(thrown.message.find("attempt") == std::string::npos);
	}
}

TEST_CASE("A denied connect, and one the client cannot name, on a cluster name the cluster", "[odbc]") {
	CHECK(ConnectFailure({{"42501", 0, "permission denied"}}, ClusterContext()) ==
	      Thrown {ExceptionType::IO, "Could not connect to Redshift at wg.example (region us-east-1): the AWS "
	                                 "identity is not allowed to connect to the cluster.\n" +
	                                     PERMISSION_HINT + "\nSQLSTATE: 42501"});
	CHECK(ConnectFailure({{"08001", 0, "could not connect"}}, ClusterContext()) ==
	      Thrown {ExceptionType::IO, "Could not connect to Redshift at wg.example (region us-east-1): the connection "
	                                 "attempt failed. The driver reported \"could not connect\".\nCheck the region "
	                                 "and that this host can reach the cluster, then retry.\nSQLSTATE: 08001"});
}

TEST_CASE("A cluster endpoint's refused login says the credentials may be for another account", "[odbc]") {
	// Unmeasured: the server's class 28 refusal of another account's database credentials is the expected
	// record, from the driver's source (public source v2.3.0, rslibpq.c:535 and rsutil.c:516-520).
	auto context = ClusterContext();
	const std::string login_refused = "Use credentials for the account of the endpoint.";
	context.hints.login_refused = login_refused;
	const std::string lead =
	    "Could not connect to Redshift at wg.example (region us-east-1): the Redshift cluster refused the login.\n" +
	    login_refused + "\nSQLSTATE: ";
	CHECK(ConnectFailure({{"28000", 0, "FATAL: password authentication failed for user \"IAMR:u\""}}, context) ==
	      Thrown {ExceptionType::IO, lead + "28000"});
	CHECK(ConnectFailure({{"HY000", 0, "generic"}, {"28P01", 0, "password authentication failed"}}, context) ==
	      Thrown {ExceptionType::IO, lead + "28P01"});
	// The first class 28 record decides the SQLSTATE, not the first record that is not HY000.
	CHECK(ConnectFailure({{"08001", 0, "could not connect"}, {"28000", 0, "a"}, {"28P01", 0, "b"}}, context) ==
	      Thrown {ExceptionType::IO, lead + "28000"});
	// A class 28 state the driver stamped on a server error counts; one it carried over from an earlier message,
	// whose text opens with no server severity, is not this failure's state, but the server's refused-password
	// text still makes it a refused login, under any state, so the reason, which names the user, is not shown.
	CHECK(ConnectFailure({{"28000", 0, "[Redshift][ODBC Driver][Server]28000:FATAL:  password authentication failed"}},
	                     context) == Thrown {ExceptionType::IO, lead + "28000"});
	const auto carried = ConnectFailure(
	    {{"28000", 0, "[Redshift][ODBC Driver][Server]28000: password authentication failed for user \"IAMR:u\""}},
	    context);
	CHECK(carried.message == lead + "HY000");
	CHECK(carried.message.find("IAMR:u") == std::string::npos);
	const auto general =
	    ConnectFailure({{"HY000", 0, "FATAL: Password Authentication Failed for user \"IAMR:u\""}}, context);
	CHECK(general.message == lead + "HY000");
	// A class 28 record decides the SQLSTATE even after a record that holds only the text.
	CHECK(ConnectFailure(
	          {{"HY000", 0, "password authentication failed"}, {"28P01", 0, "FATAL: password authentication failed"}},
	          context) == Thrown {ExceptionType::IO, lead + "28P01"});
	// A carried-over class 28 state with another text is a refused login too, its SQLSTATE HY000.
	const auto carried_other =
	    ConnectFailure({{"28000", 0, "[Redshift][ODBC Driver][Server]28000: no entry for user \"IAMR:u\""}}, context);
	CHECK(carried_other.message == lead + "HY000");
	CHECK(carried_other.message.find("IAMR:u") == std::string::npos);
	// A cause the user can fix beats the text.
	CHECK(ConnectFailure({{"HY000", 0, "password authentication failed for user \"IAMR:u\""},
	                      {"HY000", 0, "No AWS profile found: p"}},
	                     context)
	          .message.find("no usable AWS credentials") != std::string::npos);
	CHECK(CategoryOf({{"28000", 0, "FATAL: password authentication failed"}}) == ConnectCategory::OTHER);
	// A cause the user can fix still wins, and a record outside class 28 without the refused-password text
	// gets the general message.
	CHECK(ConnectFailure({{"28000", 0, "No AWS profile found"}}, context).message.find("no usable AWS credentials") !=
	      std::string::npos);
	CHECK(ConnectFailure({{"08001", 0, "could not connect"}}, context).message.find("the connection attempt failed") !=
	      std::string::npos);
	// Without the target's hint, as for a cluster named by CLUSTER, a class 28 state is still a refused
	// login, with the general sign-in hint: its record, which names the database user, is never shown.
	CHECK(
	    ConnectFailure({{"28000", 0, "FATAL: password authentication failed for user \"IAMR:u\""}}, ClusterContext()) ==
	    Thrown {ExceptionType::IO, "Could not connect to Redshift at wg.example (region us-east-1): the Redshift "
	                               "cluster refused the login.\nCheck that this AWS identity may sign in to the "
	                               "cluster's database, then retry.\nSQLSTATE: 28000"});
}

TEST_CASE("Only the client's own words get the target's noun, not the target it names", "[odbc]") {
	auto context = ClusterContext();
	context.target = "{target} (region us-east-1, profile {target})";
	CHECK(ConnectFailure({{"HYT00", 0, "Timeout"}}, context).message ==
	      "Could not connect to Redshift at {target} (region us-east-1, profile {target}): the Redshift cluster did "
	      "not answer.\nThe cluster may not be publicly reachable from this host's network; check its network "
	      "access, then retry.\nSQLSTATE: HYT00");
}

TEST_CASE("A connect failure the client cannot name gives the general message, not a credentials one", "[odbc]") {
	const std::string lead = "Could not connect to Redshift at wg.example (region us-east-1): the connection "
	                         "attempt failed. The driver reported \"";
	const std::string hint =
	    "\".\nCheck the region and that this host can reach the workgroup, then retry.\nSQLSTATE: ";
	// The reason starts after the driver's IAM tag, and comes from the record the SQLSTATE comes from.
	CHECK(ConnectFailure({{"HY000", 0, IAM_PREFIX + "Credential should be scoped to a valid region."}}) ==
	      Thrown {ExceptionType::IO, lead + "Credential should be scoped to a valid region." + hint + "HY000"});
	CHECK(ConnectFailure({{"HY000", 0, "generic"}, {"08001", 0, "could not connect to server: Connection refused"}}) ==
	      Thrown {ExceptionType::IO, lead + "could not connect to server: Connection refused" + hint + "08001"});
}

TEST_CASE("Connect categories are tried in order across every record", "[odbc]") {
	CHECK(CategoryOf({{"HY000", 0, "timeout expired"}, {"HY000", 0, "No AWS profile found: p"}}) ==
	      ConnectCategory::CREDENTIALS);
	CHECK(CategoryOf({{"HY000", 0, "certificate verify failed"}, {"HY000", 0, "No AWS profile found: p"}}) ==
	      ConnectCategory::CREDENTIALS);
	CHECK(CategoryOf({{"42501", 0, "denied"}, {"HY000", 0, "certificate verify failed"}}) ==
	      ConnectCategory::CERTIFICATE);
	CHECK(CategoryOf({{"HYT00", 0, "Timeout"}, {"42501", 0, "denied"}}) == ConnectCategory::PERMISSION);
	CHECK(CategoryOf({{"HY000", 0, "ResourceNotFoundException: x"}, {"42501", 0, "denied"}}) ==
	      ConnectCategory::PERMISSION);
	CHECK(CategoryOf({{"HYT00", 0, "Timeout"}, {"HY000", 0, "ResourceNotFoundException: x"}}) ==
	      ConnectCategory::TARGET_NOT_FOUND);
	// A database not found comes after the target, and before a timeout and class 08.
	CHECK(CategoryOf({{"3D000", 0, "x"}, {"HY000", 0, "ResourceNotFoundException: x"}}) ==
	      ConnectCategory::TARGET_NOT_FOUND);
	CHECK(CategoryOf({{"3D000", 0, "x"}, {"HY000", 0, "No AWS profile found: p"}}) == ConnectCategory::CREDENTIALS);
	CHECK(CategoryOf({{"HYT00", 0, "Timeout"}, {"3D000", 0, "x"}}) == ConnectCategory::DATABASE_NOT_FOUND);
	CHECK(CategoryOf({{"08001", 0, "x"}, {"3D000", 0, "x"}}) == ConnectCategory::DATABASE_NOT_FOUND);
}

TEST_CASE("A resume timeout wins over a class 08 record in the same failure", "[odbc]") {
	const auto records = ConnectRecords(SQL_ERROR, {{"08001", 0, "could not connect"}, {"HYT00", 0, "Timeout"}});
	const auto classification = duckdb::ClassifyConnect(records);
	CHECK(classification.category == ConnectCategory::RESUME_TIMEOUT);
	REQUIRE(classification.acted_on);
	CHECK(classification.acted_on->SqlState() == "HYT00");
}

TEST_CASE("A connect exception carries SQLSTATEs plus the driver's identity, never driver text", "[odbc]") {
	const auto records = ConnectRecords(SQL_ERROR, {{"HY000", 0, "secret PWD=hunter2 at relay-1.internal pid 42"},
	                                                {"28000", 0, "No AWS profile found: AKIAIOSFODNN7EXAMPLE"}});
	const auto error = CaptureError([&]() { duckdb::ThrowConnectFailure(records, Context()); });
	CHECK(error.Type() == ExceptionType::IO);
	CHECK(error.RawMessage() == CREDENTIALS_LEAD + "28000");
	const duckdb::unordered_map<duckdb::string, duckdb::string> expected {
	    {"sqlstate", "28000"}, {"sqlstates", "HY000,28000"}, {"redshift_driver", "test-fake 2.3.0.0 linux_amd64"}};
	CHECK(error.ExtraInfo() == expected);
}

TEST_CASE("A connect failure whose records could not be read says so, with no SQLSTATE", "[odbc]") {
	duckdb::redshift_driver::testing::FakeOdbcDriver fake;
	fake.FailCall("SQLGetDiagRec", 1, SQL_ERROR, {});
	const auto records = ConnectRecords(fake, SQL_ERROR, {{"08001", 0, "could not connect"}});
	const auto error = CaptureError([&]() { duckdb::ThrowConnectFailure(records, Context()); });
	CHECK(error.RawMessage() == "Could not connect to Redshift at wg.example (region us-east-1): the connection "
	                            "attempt failed, and the driver's diagnostic records could not be read.\nCheck the "
	                            "region and that this host can reach the workgroup, then retry.");
	const duckdb::unordered_map<duckdb::string, duckdb::string> expected {
	    {"redshift_driver", "test-fake 2.3.0.0 linux_amd64"}};
	CHECK(error.ExtraInfo() == expected);
}

TEST_CASE("A connect failure with a record to act on does not say its records could not be read", "[odbc]") {
	const auto check = [](const duckdb::DiagnosticSet &records) {
		REQUIRE(records.is_incomplete);
		const auto error = CaptureError([&]() { duckdb::ThrowConnectFailure(records, Context()); });
		CHECK(error.RawMessage() == "Could not connect to Redshift at wg.example (region us-east-1): the connection "
		                            "attempt failed. The driver reported \"could not connect\".\nCheck the region and "
		                            "that this host can reach the workgroup, then retry.\nSQLSTATE: 08001");
		CHECK(error.ExtraInfo().at("sqlstate") == "08001");
	};
	{
		INFO("the second record's read fails");
		duckdb::redshift_driver::testing::FakeOdbcDriver fake;
		fake.FailCall("SQLGetDiagRec", 2, SQL_ERROR, {});
		check(ConnectRecords(fake, SQL_ERROR, {{"08001", 0, "could not connect"}, {"HY000", 0, "second"}}));
	}
	INFO("more records than the reader takes");
	check(ConnectRecords(SQL_ERROR, std::vector<FakeDiag>(65, FakeDiag {"08001", 0, "could not connect"})));
}

TEST_CASE("ClassifyConnect takes no temporary set, which would leave the record it acted on dangling", "[odbc]") {
	CHECK(Classifies<const duckdb::DiagnosticSet &>::value);
	CHECK_FALSE(Classifies<duckdb::DiagnosticSet>::value);
	CHECK_FALSE(Classifies<const duckdb::DiagnosticSet>::value);
}

TEST_CASE("A connect failure with no record carries the driver's identity but no SQLSTATE", "[odbc]") {
	const auto records = ConnectRecords(SQL_ERROR, {});
	const auto error = CaptureError([&]() { duckdb::ThrowConnectFailure(records, Context()); });
	CHECK(error.RawMessage() == "Could not connect to Redshift at wg.example (region us-east-1): the connection "
	                            "attempt failed.\nCheck the region and that this host can reach the workgroup, then "
	                            "retry.");
	const duckdb::unordered_map<duckdb::string, duckdb::string> expected {
	    {"redshift_driver", "test-fake 2.3.0.0 linux_amd64"}};
	CHECK(error.ExtraInfo() == expected);
}

TEST_CASE("A credential failure with an AWS secret's credentials names the secret, not the chain", "[odbc]") {
	auto context = Context();
	context.hints.aws_secret = "creds";
	for (auto text :
	     {"ExpiredTokenException", "The security token included in the request is invalid.", "SignatureDoesNotMatch"}) {
		INFO(text);
		auto thrown = ConnectFailure({{"HY000", 0, IAM_PREFIX + text}}, context);
		CHECK(thrown.type == ExceptionType::IO);
		CHECK(thrown.message ==
		      "Could not connect to Redshift at wg.example (region us-east-1): the AWS credentials from AWS secret "
		      "'creds' were refused or have expired.\nThis extension does not refresh an AWS secret's credentials: "
		      "check the secret's keys, or recreate the secret to get new temporary credentials (for the aws "
		      "extension's default secret, reopen the database), then retry.\nSQLSTATE: HY000");
		CHECK(thrown.message.find("credential chain") == std::string::npos);
	}
	// Other categories keep their own text.
	auto denied = ConnectFailure({{"42501", 0, "permission denied"}}, context);
	CHECK(denied.message.find(PERMISSION_HINT) != std::string::npos);
	// A real open's target names the AWS secret (credentials_test.cpp), so only the credential text is
	// checked for.
	for (auto phrase : {"were refused or have expired", "does not refresh", "credential chain"}) {
		CHECK(denied.message.find(phrase) == std::string::npos);
	}
	// A category with no hint of the target's own, a missing database here, keeps its hint too: the
	// AWS-secret hint is the credential category's alone.
	auto missing_database = ConnectFailure({{"3D000", 0, NO_DATABASE}}, context);
	CHECK(missing_database.message.find("Check DATABASE") != std::string::npos);
	CHECK(missing_database.message.find("does not refresh") == std::string::npos);
}

TEST_CASE("A connect failure no category names shows the driver's reason, redacted and sanitized", "[odbc]") {
	// Without the reason such a failure says only that the attempt failed, and the user cannot tell a
	// refused login, a throttled credentials request, a TLS failure or a network failure apart.
	const auto thrown =
	    ConnectFailure({{"HY000", 0, "[Redshift][ODBC Driver]Rate exceeded at relay-7.example.internal"}});
	CHECK(thrown.type == ExceptionType::IO);
	CHECK(thrown.message.find("the connection attempt failed. The driver reported \"") != std::string::npos);
	CHECK(thrown.message.find("Rate exceeded") != std::string::npos);
	CHECK(thrown.message.find("relay-7") == std::string::npos);
	CHECK(thrown.message.find("\nCheck the region and that this host can reach the workgroup, then retry.\n"
	                          "SQLSTATE: HY000") != std::string::npos);
}

TEST_CASE("The driver's reason of a connect failure carries no credential form", "[odbc]") {
	// Placeholders in the forms connect-phase redaction replaces, each in a record of its own so each
	// rule is tested alone: an access-key ID, credential assignments, and a run of 40 or more token
	// characters such as a session token or password, whole or split by a character the sanitizer
	// leaves out (U+200B), which joins it again.
	const std::string long_run = "placeholdertokenrunplaceholdertokenrunplaceholder";
	const std::vector<std::pair<std::string, std::string>> cases {
	    {"login failed for key AKIAIOSFODNN7EXAMPLE here", "AKIAIOSFODNN7EXAMPLE"},
	    {"login failed SecretAccessKey=placeholderSecretValue", "placeholderSecretValue"},
	    {"login failed PWD=placeholderPwd", "placeholderPwd"},
	    {"login failed SessionToken=placeholderToken", "placeholderToken"},
	    {"login failed with token " + long_run + " here", long_run.substr(0, 20)},
	    {"login failed with token placeholdertokenrun\xE2\x80\x8Bplaceholdertokenrunplace here",
	     "placeholdertokenrunplace"}};
	for (const auto &entry : cases) {
		INFO(entry.first);
		const auto thrown = ConnectFailure({{"08001", 0, entry.first}});
		CHECK(thrown.message.find("The driver reported \"login failed") != std::string::npos);
		CHECK(thrown.message.find(entry.second) == std::string::npos);
	}
}

TEST_CASE("The driver's reason keeps a bracket that is no IAM tag, and the client's noun stays out of it", "[odbc]") {
	CHECK(ConnectFailure({{"HY000", 0, "[Throttling] Rate exceeded"}})
	          .message.find("The driver reported \"[Throttling] Rate exceeded\".") != std::string::npos);
	CHECK(ConnectFailure({{"HY000", 0, "[860:8:IAMConnectionError]: : Rate exceeded"}})
	          .message.find("The driver reported \"Rate exceeded\".") != std::string::npos);
	CHECK(ConnectFailure({{"HY000", 0, "the {target} said no"}})
	          .message.find("The driver reported \"the {target} said no\".") != std::string::npos);
	// A record whose text is a severity word alone adds no reason.
	CHECK(ConnectFailure({{"08001", 0, "FATAL:"}}).message.find("The driver reported") == std::string::npos);
}

TEST_CASE("Each refused-credentials phrase alone gives the refused cause", "[odbc]") {
	for (const auto *phrase :
	     {"InvalidSignatureException: placeholder", "The request signature we calculated does not match",
	      "SignatureDoesNotMatch: placeholder", "ExpiredTokenException: placeholder",
	      "The security token included in the request is invalid",
	      "The security token included in the request is expired"}) {
		INFO(phrase);
		CHECK(ConnectFailure({{"HY000", 0, IAM_PREFIX + phrase}})
		          .message.find("the AWS credentials found through the AWS credential chain were refused or have "
		                        "expired") != std::string::npos);
	}
}

TEST_CASE("A connect failure no category names, with no text, adds no reason", "[odbc]") {
	CHECK(ConnectFailure({{"08001", 0, ""}}) ==
	      Thrown {ExceptionType::IO, "Could not connect to Redshift at wg.example (region us-east-1): the connection "
	                                 "attempt failed.\nCheck the region and that this host can reach the workgroup, "
	                                 "then retry.\nSQLSTATE: 08001"});
}

TEST_CASE("A connect failure in a category never carries the driver's text", "[odbc]") {
	const auto denied = ConnectFailure({{"42501", 0, "permission denied for relay-7.example.internal pid 4242"}});
	CHECK(denied.message.find("The driver reported") == std::string::npos);
	CHECK(denied.message.find("relay-7") == std::string::npos);
	const auto credentials = ConnectFailure({{"HY000", 0, IAM_PREFIX + "No AWS profile found: placeholder-profile"}});
	CHECK(credentials.message.find("placeholder-profile") == std::string::npos);
}

TEST_CASE("A workgroup's refused login says GetCredentials' temporary credentials were refused", "[odbc]") {
	auto context = Context();
	context.hints.login_refused = "The workgroup refused the temporary credentials.";
	CHECK(ConnectFailure({{"28P01", 0, "FATAL: password authentication failed for user \"IAMR:u\""}}, context) ==
	      Thrown {ExceptionType::IO, "Could not connect to Redshift at wg.example (region us-east-1): the Redshift "
	                                 "workgroup refused the login.\nThe workgroup refused the temporary credentials."
	                                 "\nSQLSTATE: 28P01"});
}

TEST_CASE("Credentials the service refused say they were refused, not that none were found", "[odbc]") {
	for (auto text :
	     {"InvalidSignatureException: The request signature we calculated does not match",
	      "ExpiredTokenException: The security token included in the request is expired", "SignatureDoesNotMatch"}) {
		INFO(text);
		const auto thrown = ConnectFailure({{"HY000", 0, IAM_PREFIX + text}});
		CHECK(thrown.message.find("the AWS credentials found through the AWS credential chain were refused or have "
		                          "expired") != std::string::npos);
		CHECK(thrown.message.find("no usable AWS credentials were found") == std::string::npos);
	}
	// Credentials the chain did not find keep the message that says so.
	CHECK(ConnectFailure({{"HY000", 0, IAM_PREFIX + "No AWS profile found: p"}})
	          .message.find("no usable AWS credentials were found through the AWS credential chain") !=
	      std::string::npos);
}

TEST_CASE("No credentials under the default chain while AWS_PROFILE is set adds the role profile hint, and only then",
          "[odbc]") {
	const std::string role_hint =
	    " If AWS_PROFILE names a profile that assumes a role (role_arn with source_profile), name it with "
	    "CREDENTIAL_PROVIDER profile, PROFILE '<name>' instead: the default AWS credential chain does not read such a "
	    "profile.";
	auto context = Context();
	context.hints.aws_profile_variable = true;
	const auto with_hint =
	    "Could not connect to Redshift at wg.example (region us-east-1): no usable AWS credentials "
	    "were found through the AWS credential chain.\nCheck the credentials this process can reach: "
	    "the default AWS credential chain, or the profile PROFILE names." +
	    role_hint + "\nSQLSTATE: HY000";
	// What driver 2.3.0.0 gave live for a role_arn profile that AWS_PROFILE selects, and the chain's other
	// no-credentials answer.
	CHECK(ConnectFailure({{"HY000", 0, IAM_PREFIX + "No AWS profile found: analyst"}}, context) ==
	      Thrown {ExceptionType::IO, with_hint});
	CHECK(
	    ConnectFailure({{"HY000", 0, IAM_PREFIX + "MissingAuthenticationTokenException: Missing Authentication Token"}},
	                   context) == Thrown {ExceptionType::IO, with_hint});
	// Credentials the service refused were found, so the hint would mislead.
	CHECK(ConnectFailure({{"HY000", 0, "The security token included in the request is expired"}}, context) ==
	      Thrown {ExceptionType::IO, REFUSED_MESSAGE});
	// An AWS secret's credentials never come from the chain.
	auto secret = context;
	secret.hints.aws_secret = "creds";
	CHECK(ConnectFailure({{"HY000", 0, IAM_PREFIX + "No AWS profile found: analyst"}}, secret)
	          .message.find("AWS_PROFILE") == std::string::npos);
	// Without AWS_PROFILE, the message is the one every other test pins.
	CHECK(ConnectFailure({{"HY000", 0, IAM_PREFIX + "No AWS profile found: analyst"}}) ==
	      Thrown {ExceptionType::IO, CREDENTIALS_MESSAGE});
}

TEST_CASE("A connect failure that is not about credentials gives the same message whether AWS_PROFILE is set or not",
          "[odbc]") {
	auto with_variable = Context();
	with_variable.hints.aws_profile_variable = true;
	const std::vector<std::string> failures {
	    // A host name that does not resolve, as driver 2.3.0.0 gave it.
	    "[Redshift][ODBC Driver][Server]could not translate host name \"wg.example\" to address: Name or service not "
	    "known",
	    // One no category names, whose reason the message quotes.
	    IAM_PREFIX + ": Encountered network error when sending http request"};
	for (const auto &failure : failures) {
		INFO(failure);
		const auto without = ConnectFailure({{"HY000", 0, failure}});
		CHECK(without.message.find("AWS_PROFILE") == std::string::npos);
		CHECK(ConnectFailure({{"HY000", 0, failure}}, with_variable) == without);
	}
}

TEST_CASE("A failure to reach a target on 5439 for want of a port says how to give another, and no other failure "
          "does",
          "[odbc]") {
	const auto &port_hint = redshift_test::DEFAULT_PORT_HINT;
	auto context = Context();
	context.hints.default_port = true;
	// A cluster never sets the flag, but a connect timeout on one would say it too.
	auto cluster = ClusterContext();
	cluster.hints.default_port = true;
	// A refused or failed connect, and a timeout, on a workgroup.
	const auto refused = ConnectFailure({{"08001", 0, "could not connect"}}, context);
	CHECK(refused.message.find("then retry." + port_hint + "\nSQLSTATE: 08001") != std::string::npos);
	CHECK(ConnectFailure({{"HYT00", 0, "Timeout"}}, context).message.find(port_hint) != std::string::npos);
	CHECK(ConnectFailure({{"HYT00", 0, "Timeout"}}, cluster).message.find(port_hint) != std::string::npos);
	// An IAM call's failure, over HTTPS before any connect to the host and port, says nothing of the port.
	CHECK(ConnectFailure({{"HY000", 0, IAM_PREFIX + "Rate exceeded"}}, context).message.find(port_hint) ==
	      std::string::npos);
	CHECK(ConnectFailure({{"HY000", 0, IAM_PREFIX + "Encountered network error when sending http request"}}, context)
	          .message.find(port_hint) == std::string::npos);
	// A refused connect whose host merely starts with that word is no IAM call's failure.
	CHECK(ConnectFailure({{"08001", 0,
	                       "could not connect to server: Connection refused. Is the server running on host "
	                       "\"iamconnectionerror-lab.012345678901.us-east-1.redshift-serverless.amazonaws.com\"?"}},
	                     context)
	          .message.find(port_hint) != std::string::npos);
	// A failure that reached the target says nothing of the port.
	CHECK(ConnectFailure({{"3D000", 0, NO_DATABASE}}, context).message.find(port_hint) == std::string::npos);
	CHECK(ConnectFailure({{"HY000", 0, "The security token included in the request is invalid"}}, context)
	          .message.find(port_hint) == std::string::npos);
	// Without the hint flag, none does.
	CHECK(ConnectFailure({{"08001", 0, "could not connect"}}).message.find(port_hint) == std::string::npos);
	CHECK(ConnectFailure({{"HYT00", 0, "Timeout"}}, ClusterContext()).message.find(port_hint) == std::string::npos);
}
