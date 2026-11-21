// TLS verification on the real driver, against the test servers tls-verification.sh starts on
// 127.0.0.1: a certificate the trust source does not vouch for, or one for
// another host, fails the connect once as a certificate failure, and the trusted one gets through.
// On Windows the control plane verifies against the Windows store, which does not
// trust the run's CA, so the database cases go through the plain-HTTP control plane there. Two
// cases record what the client sends a server that asks for SSPI or GSS authentication instead.
// Driver 2.3.1.0 keeps each connection's trust to that connection (its own SSL context), so a later
// connect does not inherit an earlier one's CA file; 2.3.0.0 kept every CA file a connect gave it for
// the rest of the process. The case that needs a database hop without the run's CA, one per platform,
// is still tagged [tls_first], which the script runs in a process of its own; that is now only
// cautious.
// Skipped unless REDSHIFT_TEST_TLS_DIR names that script's directory.

// catch.hpp comes first, as in this directory's other tests.
#include "catch.hpp"

#include "duckdb/common/exception.hpp"
#include "duckdb/common/string_util.hpp"
#include "fake_driver_fixture.hpp"
#include "odbc/odbc_diagnostics.hpp"
#include "odbc/odbc_errors.hpp"
#include "thrown.hpp"

#include <chrono>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <map>
#include <mutex>
#include <regex>
#include <sstream>
#include <string>
#include <thread>
#include <vector>

using namespace duckdb;
using redshift_driver::testing::ScopedEnvironmentVariable;

namespace {

const char *const TLS_DIR_VARIABLE = "REDSHIFT_TEST_TLS_DIR";

//! What each SQLDriverConnect the client makes returns, with the category ClassifyConnect gives a
//! failure's records and their messages, read before the client reads them.
class ConnectObserver {
public:
	//! Clears what was observed.
	static void Start() {
		std::lock_guard<std::mutex> guard(lock);
		connects = 0;
		category = ConnectCategory::OTHER;
		messages.clear();
		ca_file.clear();
	}

	//! A DriverSource: the embedded driver, its SQLDriverConnect observed.
	static redshift_driver::DriverLoadResult Source() {
		auto result = redshift_driver::LoadEmbeddedDriver();
		if (result.ok()) {
			std::lock_guard<std::mutex> guard(lock);
			target = result.driver->api;
			observed = *result.driver;
			observed.api.DriverConnect = &DriverConnect;
			result.driver = &observed;
		}
		return result;
	}

	//! How many SQLDriverConnect calls were made since Start.
	static idx_t Connects() {
		std::lock_guard<std::mutex> guard(lock);
		return connects;
	}
	//! How many SQLDriverConnect calls this observer has seen in the process; Start does not reset it.
	static idx_t ProcessConnects() {
		std::lock_guard<std::mutex> guard(lock);
		return process_connects;
	}
	//! ClassifyConnect's category for the last failure's records.
	static ConnectCategory Category() {
		std::lock_guard<std::mutex> guard(lock);
		return category;
	}
	//! The last failure's messages, redacted for the connect phase, one per line.
	static std::string Messages() {
		std::lock_guard<std::mutex> guard(lock);
		return messages;
	}
	//! The loaded driver's trust directory, where the Windows export is written; empty elsewhere.
	static std::string TrustDirectory() {
		std::lock_guard<std::mutex> guard(lock);
		return observed.trust_directory;
	}
	//! The CaFile the last connect string named.
	static std::string CaFile() {
		std::lock_guard<std::mutex> guard(lock);
		return ca_file;
	}

private:
	template <class... Args>
	static SQLRETURN DriverConnect(SQLHDBC dbc, SQLHWND window, SQLCHAR *in, SQLSMALLINT in_length, Args... args) {
		const SQLRETURN rc = target.DriverConnect(dbc, window, in, in_length, args...);
		std::lock_guard<std::mutex> guard(lock);
		connects++;
		process_connects++;
		const auto text = reinterpret_cast<const char *>(in);
		ca_file = BracedValue(in_length == SQL_NTS ? std::string(text) : std::string(text, in_length), "CaFile");
		if (!SQL_SUCCEEDED(rc)) {
			// Diagnostic reads leave the records for the client's own read after this returns.
			const auto records = ReadConnectDiagnostics(target, {SQL_HANDLE_DBC, dbc});
			category = ClassifyConnect(records).category;
			messages.clear();
			for (const auto &record : records.records) {
				messages += record.SqlState() + " " + record.Message() + "\n";
			}
		}
		return rc;
	}

	static inline std::mutex lock;
	static inline redshift_driver::OdbcApi target;
	static inline redshift_driver::LoadedDriver observed;
	static inline idx_t connects = 0;
	static inline idx_t process_connects = 0;
	static inline ConnectCategory category = ConnectCategory::OTHER;
	static inline std::string messages;
	static inline std::string ca_file;

	//! The value of `keyword={value}` in `text`, the form the client's encoder writes; empty if absent.
	static std::string BracedValue(const std::string &text, const std::string &keyword) {
		const auto opening = keyword + "={";
		const auto start = text.find(opening);
		if (start == std::string::npos) {
			return std::string();
		}
		const auto value = start + opening.size();
		return text.substr(value, text.find('}', value) - value);
	}
};

//! Fails unless no connect came before in this process, whose CA file the driver would still trust
//! (fe-secure.c, initialize_SSL).
void RequireFirstConnectInProcess() {
	INFO("an earlier connect in this process gave the driver a CA file it still trusts; run [tls_first] on its own");
	REQUIRE(ConnectObserver::ProcessConnects() == 0);
}

//! The directory tls-verification.sh made, and the ports its servers listen on, by case name.
struct TestServers {
	std::string directory;
	std::map<std::string, int> ports;

	//! The path of `name` in the directory.
	std::string File(const std::string &name) const {
		return directory + "/" + name;
	}
	//! How many lines the servers have written to `name`: one per event it counts.
	idx_t Lines(const std::string &name) const {
		std::ifstream file(File(name));
		idx_t lines = 0;
		for (std::string line; std::getline(file, line);) {
			lines++;
		}
		return lines;
	}
};

//! REDSHIFT_TEST_TLS_DIR, or null after skipping the case when it is unset.
const char *TestServersDirectoryOrSkip() {
	const auto directory = std::getenv(TLS_DIR_VARIABLE);
	if (!directory) {
		SKIP_TEST("skipped: unset: REDSHIFT_TEST_TLS_DIR (test/client/odbc/tls-verification.sh sets it)");
	}
	return directory;
}

//! Fails the case in a build that cannot run it: REDSHIFT_TEST_TLS_DIR is set, so the script was
//! started against the wrong build.
void RequireTlsTestBuild() {
	REQUIRE(BuildHasTestEndpoints());
	REQUIRE(redshift_driver::LoadEmbeddedDriver().ok());
}

//! The servers in `directory`, from the ports file the script waited for.
TestServers ReadTestServers(const char *directory) {
	TestServers servers {directory, {}};
	std::ifstream file(servers.File("ports"));
	std::string name;
	int port;
	while (file >> name >> port) {
		servers.ports[name] = port;
	}
	return servers;
}

//! What one connect to a database case showed.
struct ConnectOutcome {
	redshift_test::Thrown thrown;
	idx_t driver_connects;
	ConnectCategory category;
	std::string messages;
	//! The connections the database case accepted, and the startup messages it got, during the connect.
	idx_t server_connections;
	idx_t startups;
	//! The loaded driver's trust directory, read before the process state is reset.
	std::string trust_directory;
	//! The CaFile the connect string named.
	std::string ca_file;
};

//! Which control plane a connect calls: over HTTPS, its certificate signed by the run's trusted CA,
//! or over plain HTTP, for Windows' database cases.
enum class ControlPlane { HTTPS, HTTP };

//! The control plane a database case calls on this platform: on Windows the driver verifies it
//! against the Windows store, which does not trust the run's CA.
constexpr ControlPlane DATABASE_CASE_CONTROL_PLANE =
#ifdef _WIN32
    ControlPlane::HTTP;
#else
    ControlPlane::HTTPS;
#endif

//! Appends the case's name and `messages` to `records` in the servers' directory, which
//! tls-verification.sh prints, so a run records the driver's text where a case does not pin it.
void RecordMessages(const TestServers &servers, const std::string &messages) {
	std::ofstream(servers.File("records"), std::ios::app)
	    << "== " << Catch::getResultCapture().getCurrentTestName() << "\n"
	    << messages;
}

//! Opens a connection to the database case `name` with `ca_file` as SSL_CERT_FILE, or with it unset
//! when `ca_file` is empty, through `control`. Fake AWS keys: the control plane is the test's own.
ConnectOutcome ConnectTo(const TestServers &servers, const std::string &name, const std::string &ca_file,
                         ControlPlane control = DATABASE_CASE_CONTROL_PLANE) {
	const auto url = control == ControlPlane::HTTPS
	                     ? "https://localhost:" + std::to_string(servers.ports.at("control"))
	                     : "http://localhost:" + std::to_string(servers.ports.at("control_http"));
	ScopedEnvironmentVariable trust("SSL_CERT_FILE", ca_file.empty() ? nullptr : servers.File(ca_file).c_str());
	ScopedEnvironmentVariable host(TEST_HOST_VARIABLE, "localhost");
	ScopedEnvironmentVariable endpoint(TEST_URL_VARIABLE, url.c_str());
	ScopedEnvironmentVariable test_case(TEST_CASE_VARIABLE, nullptr);
	ScopedEnvironmentVariable key_id("AWS_ACCESS_KEY_ID", "AKIAIOSFODNN7EXAMPLE");
	ScopedEnvironmentVariable secret("AWS_SECRET_ACCESS_KEY", "wJalrXUtnFEMI/K7MDENG/bPxRfiCYEXAMPLEKEY");
	ScopedEnvironmentVariable token("AWS_SESSION_TOKEN", nullptr);
	ScopedEnvironmentVariable profile("AWS_PROFILE", nullptr);
	auto config = redshift_test::UsEast1Config();
	config.endpoint_port = servers.ports.at(name);

	const auto connections_before = servers.Lines(name + ".connections");
	const auto startups_before = servers.Lines(name + ".startups");
	SetDriverSourceForTesting(&ConnectObserver::Source);
	ConnectObserver::Start();
	auto sink = make_shared_ptr<redshift_test::CapturingLogSink>();
	ConnectOutcome outcome;
	outcome.thrown = redshift_test::Capture([&] { OpenRedshiftConnection(config, sink); });
	outcome.trust_directory = ConnectObserver::TrustDirectory();
	outcome.ca_file = ConnectObserver::CaFile();
	ResetProcessStateForTesting();
	outcome.driver_connects = ConnectObserver::Connects();
	outcome.category = ConnectObserver::Category();
	outcome.messages = ConnectObserver::Messages();
	outcome.server_connections = servers.Lines(name + ".connections") - connections_before;
	outcome.startups = servers.Lines(name + ".startups") - startups_before;
	RecordMessages(servers, outcome.messages);
	return outcome;
}

//! A trust source that vouches for none of the servers' certificates: on Windows the store's export,
//! with SSL_CERT_FILE unset; elsewhere a CA that signs none of them.
const char *const NO_SERVER_CA =
#ifdef _WIN32
    "";
#else
    "unused-ca.pem";
#endif

//! How the client names the test workgroup at `port`, ahead of a connect failure's explanation.
std::string FailureLead(int port) {
	return "Could not connect to Redshift at localhost (region us-east-1, workgroup analytics, account 012345678901, "
	       "port " +
	       std::to_string(port) + "): ";
}

//! The client's message for a certificate failure on the test workgroup at `port`.
std::string CertificateFailure(int port) {
#ifdef _WIN32
	return FailureLead(port) + "the server's TLS certificate could not be verified.\nCheck the trusted CA "
	                           "certificates, from SSL_CERT_FILE or the Windows root store, and the network path to "
	                           "the Redshift host. A host whose automatic root update is off, or has not run yet, may "
	                           "lack the roots Redshift's certificates chain to: turn it on, or let it run, then "
	                           "connect again; or set SSL_CERT_FILE. Verification cannot be turned off.\nSQLSTATE: "
	                           "HY000";
#else
	return FailureLead(port) + "the server's TLS certificate could not be verified.\nCheck the trusted CA "
	                           "certificates, from SSL_CERT_FILE or the system store, and the network path to the "
	                           "Redshift host. Verification cannot be turned off.\nSQLSTATE: HY000";
#endif
}

//! Whether `thrown` is the client's message for a connect failure of no category on the test workgroup
//! at `port`: the general cause and hint, with the driver's reason in its own sentence between them when
//! the record has text, which differs by case and platform, or without it.
bool IsOtherFailure(const redshift_test::Thrown &thrown, int port) {
	const auto lead = FailureLead(port) + "the connection attempt failed.";
	const std::string tail =
	    "\nCheck the region and that this host can reach the workgroup, then retry.\nSQLSTATE: HY000";
	const auto &message = thrown.message;
	if (thrown.type != ExceptionType::IO || message.size() < lead.size() + tail.size() ||
	    message.compare(0, lead.size(), lead) != 0 ||
	    message.compare(message.size() - tail.size(), tail.size(), tail) != 0) {
		return false;
	}
	const auto reason = message.substr(lead.size(), message.size() - lead.size() - tail.size());
	const std::string reason_lead = " The driver reported \"";
	return reason.empty() ||
	       (reason.size() > reason_lead.size() + 2 && reason.compare(0, reason_lead.size(), reason_lead) == 0 &&
	        reason.compare(reason.size() - 2, 2, "\".") == 0);
}

//! How long a probe waits for an authentication case's line: longer than the servers' wait for a
//! reply (PEER_TIMEOUT_SECONDS), as the line comes after it.
constexpr std::chrono::seconds PROBE_LINE_WAIT {30};

//! The forms of an authentication case's line: no reply, or a reply's type and length and, for a
//! token, its mechanism, size and, for NTLM and Kerberos, its SHA-256. Never the token's bytes.
const std::regex
    PROBE_LINE_FORM("request=[a-z]+ code=[0-9]+ reply=(none (closed|reset|timeout)|([A-Za-z]|0x[0-9a-f]{2}) "
                    "length=-?[0-9]+( body=unread| mechanism=(unknown bytes=[0-9]+( truncated)?|"
                    "(ntlm-[a-z]+|kerberos) bytes=[0-9]+( truncated)? sha256=[0-9a-f]{64}))?)");

//! What a connect to an authentication case showed, with the lines the case wrote to <case>.auth
//! during it, one per startup message: the method it asked for and the client's reply.
struct ProbeOutcome {
	ConnectOutcome connect;
	idx_t server_line_count;
	std::string server_lines;
};

//! Connects to the authentication case `name`, which asks for its method in place of the password
//! exchange and then refuses, and appends the client's failure and the case's lines to `records`.
//! The lines are waited for: the connect can return before the case writes them, as when the driver
//! gives up but holds the connection open until the case stops waiting for a reply.
ProbeOutcome ProbeAuthentication(const TestServers &servers, const std::string &name) {
	const auto file = name + ".auth";
	const auto before = servers.Lines(file);
	ProbeOutcome outcome {ConnectTo(servers, name, "trusted-ca.pem"), 0, std::string()};
	const auto deadline = std::chrono::steady_clock::now() + PROBE_LINE_WAIT;
	while (servers.Lines(file) < before + outcome.connect.startups && std::chrono::steady_clock::now() < deadline) {
		std::this_thread::sleep_for(std::chrono::milliseconds(50));
	}
	std::ifstream lines(servers.File(file));
	idx_t index = 0;
	for (std::string line; std::getline(lines, line); index++) {
		if (index >= before) {
			outcome.server_line_count++;
			outcome.server_lines += line + "\n";
		}
	}
	std::ofstream(servers.File("records"), std::ios::app)
	    << "client: " << outcome.connect.thrown.message
	    << "\nserver: " << (outcome.server_lines.empty() ? "(no line)\n" : outcome.server_lines);
	return outcome;
}

//! Probes the authentication case `name`, which asks for the method `code`, and checks what holds
//! whatever the client sends: the connect fails after the startup message, and the case recorded
//! the client's reply to each one.
void CheckProbe(const TestServers &servers, const std::string &name, int code) {
	const auto outcome = ProbeAuthentication(servers, name);
	INFO(outcome.connect.messages << outcome.server_lines);
	CHECK(outcome.connect.thrown.type == ExceptionType::IO);
	CHECK(outcome.connect.startups >= 1);
	CHECK(outcome.server_line_count == outcome.connect.startups);
	CHECK(
	    StringUtil::StartsWith(outcome.server_lines, "request=" + name + " code=" + std::to_string(code) + " reply="));
	std::istringstream lines(outcome.server_lines);
	for (std::string line; std::getline(lines, line);) {
		INFO(line);
		CHECK(std::regex_match(line, PROBE_LINE_FORM));
	}
#ifdef __linux__ // Measured on Linux, where both go to GSSAPI, on a host with no Kerberos ticket.
	INFO(outcome.connect.thrown.message);
	CHECK(IsOtherFailure(outcome.connect.thrown, servers.ports.at(name)));
	CHECK(outcome.connect.category == ConnectCategory::OTHER);
	CHECK(StringUtil::StartsWith(outcome.connect.messages,
	                             "HY000 [Redshift][ODBC Driver][Server]GSSAPI continuation error: No credentials were "
	                             "supplied, or the credentials were unavailable or inaccessible\n"));
#endif
}

} // namespace

TEST_CASE("A database certificate from a CA outside the trust source fails the connect once, unretried",
          "[odbc][tls]") {
	const auto directory = TestServersDirectoryOrSkip();
	if (!directory) {
		return;
	}
	RequireTlsTestBuild();
	const auto servers = ReadTestServers(directory);
	const auto outcome = ConnectTo(servers, "untrusted", "trusted-ca.pem");
	INFO(outcome.messages);
	CHECK(outcome.thrown ==
	      redshift_test::Thrown {ExceptionType::IO, CertificateFailure(servers.ports.at("untrusted"))});
	CHECK(outcome.category == ConnectCategory::CERTIFICATE);
#ifndef _WIN32 // Measured on Linux; a Windows run records its text (RecordMessages).
	CHECK(outcome.messages == "HY000 [Redshift][ODBC Driver][Server]SSL error: CERTIFICATE_VERIFY_FAILED\n");
#endif
	// One connect and one connection: no retry, and no second connection without TLS.
	CHECK(outcome.driver_connects == 1);
	CHECK(outcome.server_connections == 1);
	CHECK(outcome.startups == 0);
}

TEST_CASE("A trusted CA's database certificate for another host name fails the connect once, unretried",
          "[odbc][tls]") {
	const auto directory = TestServersDirectoryOrSkip();
	if (!directory) {
		return;
	}
	RequireTlsTestBuild();
	const auto servers = ReadTestServers(directory);
	const auto outcome = ConnectTo(servers, "wrong_host", "trusted-ca.pem");
	INFO(outcome.messages);
	CHECK(outcome.thrown ==
	      redshift_test::Thrown {ExceptionType::IO, CertificateFailure(servers.ports.at("wrong_host"))});
	CHECK(outcome.category == ConnectCategory::CERTIFICATE);
#ifndef _WIN32
	CHECK(outcome.messages ==
	      "HY000 [Redshift][ODBC Driver][Server]server certificate does not match host name \"localhost\"\n");
#endif
	CHECK(outcome.driver_connects == 1);
	CHECK(outcome.server_connections == 1);
	CHECK(outcome.startups == 0);
}

TEST_CASE("A trusted CA's database certificate for the host passes verification, so the startup message is sent",
          "[odbc][tls]") {
	const auto directory = TestServersDirectoryOrSkip();
	if (!directory) {
		return;
	}
	RequireTlsTestBuild();
	const auto servers = ReadTestServers(directory);
	const auto outcome = ConnectTo(servers, "trusted", "trusted-ca.pem");
	INFO(outcome.messages);
	CHECK(outcome.startups == 1);
	// The server then drops the connection without TLS's closing alert, which fails the connect.
	INFO(outcome.thrown.message);
	CHECK(IsOtherFailure(outcome.thrown, servers.ports.at("trusted")));
	CHECK(outcome.category == ConnectCategory::OTHER);
#ifndef _WIN32
	CHECK(outcome.messages == "HY000 [Redshift][ODBC Driver][Server]SSL error: UNEXPECTED_EOF_WHILE_READING\n");
#endif
	CHECK(outcome.driver_connects == 1);
	CHECK(outcome.server_connections == 1);
}

TEST_CASE("A connect to a closed port fails once, with the client's message for no category", "[odbc][tls]") {
	const auto directory = TestServersDirectoryOrSkip();
	if (!directory) {
		return;
	}
	RequireTlsTestBuild();
	const auto servers = ReadTestServers(directory);
	// The driver's refusal text, Winsock's on Windows, is not in the thrown message under any locale:
	// connect-phase driver text is not shown. 2.3.0.0's refusal text is fixed
	// English on Windows (libpq's win32.c table) and the C locale's on Linux.
	const auto outcome = ConnectTo(servers, "closed", "trusted-ca.pem");
	INFO(outcome.messages);
	INFO(outcome.thrown.message);
	CHECK(IsOtherFailure(outcome.thrown, servers.ports.at("closed")));
	CHECK(outcome.category == ConnectCategory::OTHER);
	// A substring, as the record carries the address and the port, and on Windows Winsock's code.
	CHECK(outcome.messages.find("Connection refused") != std::string::npos);
	CHECK(outcome.driver_connects == 1);
}

// A server may ask for SSPI or GSS in place of the password exchange. These cases record what the
// client sends it, a token or nothing, and do not judge it, so no case fails on it.
TEST_CASE("A server that asks for SSPI authentication in place of the password exchange opens no session",
          "[odbc][tls]") {
	const auto directory = TestServersDirectoryOrSkip();
	if (!directory) {
		return;
	}
	RequireTlsTestBuild();
	CheckProbe(ReadTestServers(directory), "sspi", 12);
}

TEST_CASE("A server that asks for GSS authentication in place of the password exchange opens no session",
          "[odbc][tls]") {
	const auto directory = TestServersDirectoryOrSkip();
	if (!directory) {
		return;
	}
	RequireTlsTestBuild();
	CheckProbe(ReadTestServers(directory), "gss", 10);
}

TEST_CASE("A later connect does not trust an earlier connect's CA file and fails as a certificate failure",
          "[odbc][tls]") {
	const auto directory = TestServersDirectoryOrSkip();
	if (!directory) {
		return;
	}
	RequireTlsTestBuild();
	const auto servers = ReadTestServers(directory);
	const auto earlier = ConnectTo(servers, "trusted", "trusted-ca.pem", ControlPlane::HTTP);
	INFO(earlier.messages);
	REQUIRE(earlier.startups == 1);
	// The plain-HTTP control plane, so that only the database hop's trust is in question. Driver
	// 2.3.1.0's trust is per connection: the later connect, whose CA file signs no server, fails at the
	// certificate, though an earlier connect in the same process trusted the run's CA.
	const auto later = ConnectTo(servers, "trusted", NO_SERVER_CA, ControlPlane::HTTP);
	INFO(later.messages);
#ifdef _WIN32
	// With SSL_CERT_FILE unset the client gives the driver the store's export, in the loader's trust
	// directory.
	REQUIRE(!later.trust_directory.empty());
	REQUIRE(!later.ca_file.empty());
	CHECK(std::filesystem::equivalent(std::filesystem::u8path(later.ca_file).parent_path(),
	                                  std::filesystem::u8path(later.trust_directory)));
#else
	CHECK(later.ca_file == servers.File(NO_SERVER_CA));
#endif
	CHECK(later.ca_file != earlier.ca_file);
	CHECK(later.thrown == redshift_test::Thrown {ExceptionType::IO, CertificateFailure(servers.ports.at("trusted"))});
	CHECK(later.category == ConnectCategory::CERTIFICATE);
	CHECK(later.driver_connects == 1);
	CHECK(later.server_connections == 1);
	CHECK(later.startups == 0);
}

// Not on Windows, where the database cases already call the plain-HTTP control plane,
// SSL_CERT_FILE does not reach the control plane, and the [tls_first] case is against the store's
// export.
#ifndef _WIN32
TEST_CASE("The plain-HTTP control plane also gets a trusted database case through to the startup message",
          "[odbc][tls]") {
	const auto directory = TestServersDirectoryOrSkip();
	if (!directory) {
		return;
	}
	RequireTlsTestBuild();
	const auto servers = ReadTestServers(directory);
	const auto outcome = ConnectTo(servers, "trusted", "trusted-ca.pem", ControlPlane::HTTP);
	INFO(outcome.messages);
	CHECK(outcome.ca_file == servers.File("trusted-ca.pem"));
	CHECK(outcome.startups == 1);
	INFO(outcome.thrown.message);
	CHECK(IsOtherFailure(outcome.thrown, servers.ports.at("trusted")));
	CHECK(outcome.category == ConnectCategory::OTHER);
	CHECK(outcome.messages == "HY000 [Redshift][ODBC Driver][Server]SSL error: UNEXPECTED_EOF_WHILE_READING\n");
	CHECK(outcome.driver_connects == 1);
	CHECK(outcome.server_connections == 1);
}

// On Windows SSL_CERT_FILE does not reach the control plane: the next case.
TEST_CASE("An unrelated CA as SSL_CERT_FILE fails the control-plane call before any database connection",
          "[odbc][tls]") {
	const auto directory = TestServersDirectoryOrSkip();
	if (!directory) {
		return;
	}
	RequireTlsTestBuild();
	const auto servers = ReadTestServers(directory);
	// The trusted case's server, so that only the control plane's certificate can fail.
	const auto outcome = ConnectTo(servers, "trusted", "unrelated-ca.pem");
	INFO(outcome.messages);
	CHECK(outcome.thrown == redshift_test::Thrown {ExceptionType::IO, CertificateFailure(servers.ports.at("trusted"))});
	CHECK(outcome.category == ConnectCategory::CERTIFICATE);
	CHECK(outcome.messages == "HY000 [Redshift][ODBC Driver][Server][860:8:IAMConnectionError]: : curlCode: 60, SSL "
	                          "peer certificate or SSH remote key was not OK; Details: SSL certificate OpenSSL verify "
	                          "result: unable to get local issuer certificate (20)\n");
	CHECK(outcome.driver_connects == 1);
	CHECK(outcome.server_connections == 0);
	CHECK(outcome.startups == 0);
}

TEST_CASE("A database hop given only a CA that signs no server fails, and a later connect's CA then verifies",
          "[odbc][tls_first]") {
	const auto directory = TestServersDirectoryOrSkip();
	if (!directory) {
		return;
	}
	RequireTlsTestBuild();
	RequireFirstConnectInProcess();
	const auto servers = ReadTestServers(directory);
	const auto outcome = ConnectTo(servers, "trusted", NO_SERVER_CA, ControlPlane::HTTP);
	INFO("first connect: " << outcome.messages);
	CHECK(outcome.ca_file == servers.File(NO_SERVER_CA));
	CHECK(outcome.thrown == redshift_test::Thrown {ExceptionType::IO, CertificateFailure(servers.ports.at("trusted"))});
	CHECK(outcome.category == ConnectCategory::CERTIFICATE);
	CHECK(outcome.messages == "HY000 [Redshift][ODBC Driver][Server]SSL error: CERTIFICATE_VERIFY_FAILED\n");
	CHECK(outcome.driver_connects == 1);
	CHECK(outcome.server_connections == 1);
	CHECK(outcome.startups == 0);
	// A later connect's own CA file is the one its hop verifies against, so it gets through.
	const auto later = ConnectTo(servers, "trusted", "trusted-ca.pem", ControlPlane::HTTP);
	INFO("later connect: " << later.messages);
	CHECK(later.ca_file == servers.File("trusted-ca.pem"));
	CHECK(later.startups == 1);
	CHECK(later.driver_connects == 1);
	CHECK(later.server_connections == 1);
}
#endif

#ifdef _WIN32
TEST_CASE("On Windows, with SSL_CERT_FILE naming its CA, the HTTPS control plane fails before the database hop",
          "[odbc][tls]") {
	const auto directory = TestServersDirectoryOrSkip();
	if (!directory) {
		return;
	}
	RequireTlsTestBuild();
	const auto servers = ReadTestServers(directory);
	// The run's trusted CA signed the control plane's certificate, but the store does not hold it.
	// The SDK's Windows client gives this one text to every request that fails before a response,
	// so it names no certificate and the failure has no category.
	const auto outcome = ConnectTo(servers, "trusted", "trusted-ca.pem", ControlPlane::HTTPS);
	INFO(outcome.messages);
	INFO(outcome.thrown.message);
	CHECK(IsOtherFailure(outcome.thrown, servers.ports.at("trusted")));
	CHECK(outcome.category == ConnectCategory::OTHER);
	CHECK(outcome.messages == "HY000 [Redshift][ODBC Driver][Server][860:8:IAMConnectionError]: : Encountered "
	                          "network error when sending http request\n");
	CHECK(outcome.driver_connects == 1);
	CHECK(outcome.server_connections == 0);
	CHECK(outcome.startups == 0);
}

TEST_CASE("On Windows, with SSL_CERT_FILE unset, the database hop trusts the store's export, which holds no test CA",
          "[odbc][tls_first]") {
	const auto directory = TestServersDirectoryOrSkip();
	if (!directory) {
		return;
	}
	RequireTlsTestBuild();
	RequireFirstConnectInProcess();
	const auto servers = ReadTestServers(directory);
	const auto outcome = ConnectTo(servers, "trusted", NO_SERVER_CA);
	INFO(outcome.messages);
	CHECK(outcome.thrown == redshift_test::Thrown {ExceptionType::IO, CertificateFailure(servers.ports.at("trusted"))});
	CHECK(outcome.category == ConnectCategory::CERTIFICATE);
	CHECK(outcome.driver_connects == 1);
	CHECK(outcome.server_connections == 1);
	CHECK(outcome.startups == 0);
	// The export the connect used: a content-named file in the loader's trust directory.
	REQUIRE(!outcome.trust_directory.empty());
	REQUIRE(!outcome.ca_file.empty());
	const auto export_path = std::filesystem::u8path(outcome.ca_file);
	CHECK(std::filesystem::is_regular_file(export_path));
	CHECK(std::filesystem::equivalent(export_path.parent_path(), std::filesystem::u8path(outcome.trust_directory)));
	const auto name = export_path.filename().u8string();
	CHECK(name.size() == 68);
	CHECK(name.find_first_not_of("0123456789abcdef") == 64);
	CHECK(name.substr(64) == ".pem");
}
#endif
