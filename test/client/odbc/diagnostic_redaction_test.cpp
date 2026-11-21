// Redaction of diagnostic text and the sanitizer a server message passes
// through before a user or a log sees it. Every expected value is a literal.

// catch.hpp comes first, as the extension's C++ tests include it: DuckDB's copy declares the
// duckdb_base_std aliases it needs only while no DuckDB header has opened that namespace.
#include "catch.hpp"

#ifdef _WIN32
// QueryThreadCycleTime; NOMINMAX, then windows.h, read before any other DuckDB header.
#include "duckdb/common/windows.hpp"
#endif

#include "odbc/diagnostic_redaction.hpp"
#include "utf8proc_wrapper.hpp"

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <ctime>
#include <string>
#include <thread>
#include <utility>
#include <vector>

using duckdb::MessageExtent;
using duckdb::RedactConnectDiagnostic;
using duckdb::RedactStatementDiagnostic;
using duckdb::SanitizeServerMessage;

namespace {

using BoundValues = duckdb::vector<duckdb::string>;

std::string RedactConnect(const std::string &message) {
	return RedactConnectDiagnostic(message, MessageExtent::WHOLE, 0).text;
}

std::string RedactCutConnect(const std::string &message) {
	return RedactConnectDiagnostic(message, MessageExtent::CUT, 0).text;
}

std::string RedactStatement(const std::string &message, const BoundValues &bound_values = {}) {
	return RedactStatementDiagnostic(message, bound_values, MessageExtent::WHOLE, 0).text;
}

std::string RedactCutStatement(const std::string &message, const BoundValues &bound_values = {}) {
	return RedactStatementDiagnostic(message, bound_values, MessageExtent::CUT, 0).text;
}

//! `unit` repeated to `bytes` bytes or more.
std::string Repeated(const std::string &unit, size_t bytes) {
	std::string text;
	while (text.size() < bytes) {
		text += unit;
	}
	return text;
}

//! This thread's CPU time so far: nanoseconds, or CPU cycles on Windows.
uint64_t ThreadCpuTime() {
#ifdef _WIN32
	ULONG64 cycles = 0;
	REQUIRE(::QueryThreadCycleTime(::GetCurrentThread(), &cycles) != 0);
	return cycles;
#else
	timespec now {};
	REQUIRE(::clock_gettime(CLOCK_THREAD_CPUTIME_ID, &now) == 0);
	return uint64_t(now.tv_sec) * 1000000000 + uint64_t(now.tv_nsec);
#endif
}

//! The CPU time one statement redaction of `message` with `bound_values` takes on this thread.
//! Other work on the host does not add to it, as it does to the wall clock. Windows has no
//! CLOCK_THREAD_CPUTIME_ID, and GetThreadTimes moves in clock ticks of about 15 ms, longer than a
//! redaction, so there it counts cycles. The tests compare only ratios of one unit.
double RedactionCpuTime(const std::string &message, const BoundValues &bound_values = {}) {
	const auto started = ThreadCpuTime();
	const auto redacted = RedactStatement(message, bound_values);
	const auto stopped = ThreadCpuTime();
	CHECK(!redacted.empty());
	return double(stopped - started);
}

//! Whether `part` appears anywhere in `text`.
bool Contains(const std::string &text, const std::string &part) {
	return text.find(part) != std::string::npos;
}

//! `codepoint` written as UTF-8.
std::string Utf8(int32_t codepoint) {
	char bytes[4];
	int length = 0;
	REQUIRE(duckdb::Utf8Proc::CodepointToUtf8(codepoint, length, bytes));
	return std::string(bytes, bytes + length);
}

} // namespace

TEST_CASE("An access-key ID of either prefix is redacted in both phases", "[odbc]") {
	CHECK(RedactConnect("key AKIAIOSFODNN7EXAMPLE used") == "key <redacted> used");
	CHECK(RedactStatement("key ASIAIOSFODNN7EXAMPLE used") == "key <redacted> used");
}

TEST_CASE("A secret-key or session-token assignment loses its value, not its keyword", "[odbc]") {
	CHECK(RedactStatement("aws_secret_access_key=abc/+=_-") == "aws_secret_access_key=<redacted>");
	CHECK(RedactConnect("aws_session_token = {tok;en}}x}") == "aws_session_token=<redacted>");
	CHECK(RedactStatement("AWS_SECRET_ACCESS_KEY='quo''ted' after;x") == "AWS_SECRET_ACCESS_KEY=<redacted>;x");
}

TEST_CASE("A PWD value is redacted whether plain, braced or quoted", "[odbc]") {
	CHECK(RedactConnect("PWD=secret") == "PWD=<redacted>");
	CHECK(RedactConnect("PWD={braced;sec}}ret}") == "PWD=<redacted>");
	CHECK(RedactStatement("pwd=\"quoted\"") == "pwd=<redacted>");
	CHECK(RedactStatement("pwd=\"quo\"\"ted\" after;x") == "pwd=<redacted>;x");
}

TEST_CASE("A driver SessionToken assignment loses its value in any case, as the driver reads its keys", "[odbc]") {
	CHECK(RedactStatement("SessionToken=IQoJb3JpZ2luX2VjEXAMPLETOKEN") == "SessionToken=<redacted>");
	CHECK(RedactConnect("sessiontoken: {tok;en}}x};UID=u") == "sessiontoken=<redacted>;UID=u");
	CHECK(SanitizeServerMessage(
	          RedactStatement("invalid input syntax for integer: \"SessionToken=IQoJb3JpZ2luX2VjEXAMPLETOKEN\"")) ==
	      "invalid input syntax for integer: \"SessionToken=<redacted>\"");
	CHECK(RedactStatement("mySessionToken=value") == "mySessionToken=value");
}

TEST_CASE("A session_token assignment loses its value, as a credentials file or JSON writes it", "[odbc]") {
	CHECK(RedactStatement("{\"session_token\": \"tok;en\"}") == "{session_token=<redacted>");
	CHECK(RedactConnect("SESSION_TOKEN=tok;UID=u") == "SESSION_TOKEN=<redacted>;UID=u");
	// The longer `aws_session_token` matches first and the scan goes on after its value, so the
	// `session_token` inside it is never read; `my_session_token` below covers the run-on rule.
	CHECK(RedactStatement("aws_session_token=tok;UID=u") == "aws_session_token=<redacted>;UID=u");
	CHECK(RedactStatement("my_session_token=value") == "my_session_token=value");
}

TEST_CASE("A driver Password value is redacted whether plain, braced or quoted, a doubled brace kept inside",
          "[odbc]") {
	CHECK(
	    SanitizeServerMessage(RedactStatement("invalid input syntax for integer: \"Password={hunter2};UID=admin\"")) ==
	    "invalid input syntax for integer: \"Password=<redacted>;UID=admin\"");
	CHECK(RedactConnect("PASSWORD={a}};b}}};UID=u") == "PASSWORD=<redacted>;UID=u");
	CHECK(RedactStatement("password: 'a;b' tail;x") == "password=<redacted>;x");
	CHECK(RedactStatement("password=hunter22") == "password=<redacted>");
	CHECK(RedactStatement("db_password=value") == "db_password=value");
	CHECK(RedactStatement("password authentication failed for user \"u\"") ==
	      "password authentication failed for user \"u\"");
}

TEST_CASE("A driver SecretAccessKey value is redacted, braced with a doubled brace inside", "[odbc]") {
	CHECK(RedactConnect("SecretAccessKey={wJal}};rXUtn};UID=u") == "SecretAccessKey=<redacted>;UID=u");
	CHECK(RedactStatement("secretaccesskey = wJalrXUtnFEMI/K7MDENG") == "secretaccesskey=<redacted>");
	CHECK(RedactStatement("MySecretAccessKey=value") == "MySecretAccessKey=value");
}

TEST_CASE("The driver's other sensitive keys lose their values whether plain, braced or quoted", "[odbc]") {
	CHECK(RedactConnect("web_identity_token=eyJ.e30.sig;UID=u") == "web_identity_token=<redacted>;UID=u");
	CHECK(RedactStatement("Token: {tok;en}}x};UID=u") == "Token=<redacted>;UID=u");
	CHECK(RedactConnect("CLIENT_SECRET = 'a;b' tail;UID=u") == "CLIENT_SECRET=<redacted>;UID=u");
	CHECK(RedactStatement("proxypwd={hun}};ter2};UID=u") == "proxypwd=<redacted>;UID=u");
	CHECK(RedactStatement("{\"https_proxy_password\": \"pw;x\"}") == "{https_proxy_password=<redacted>");
}

TEST_CASE("A keyword after a hyphen stands apart, as a hyphen joins no word", "[odbc]") {
	// So a token in a URL's query goes too, before the URL rule takes the rest.
	CHECK(RedactStatement("see ?X-Amz-Security-Token=IQoJ&x=1") == "see ?X-Amz-Security-Token=<redacted>");
}

TEST_CASE("A word that only holds one of the driver's other sensitive keys keeps its value", "[odbc]") {
	CHECK(RedactStatement("web_identity_token_file=/var/run/token") == "web_identity_token_file=/var/run/token");
	CHECK(RedactStatement("my_token=value") == "my_token=value");
	CHECK(RedactStatement("tokenizer: ready") == "tokenizer: ready");
	CHECK(RedactStatement("token is not valid") == "token is not valid");
	CHECK(RedactStatement("client_secret_arn=value") == "client_secret_arn=value");
	CHECK(RedactStatement("ProxyPwdHint=value") == "ProxyPwdHint=value");
	CHECK(RedactStatement("no_https_proxy_password=value") == "no_https_proxy_password=value");
}

TEST_CASE("A CREDENTIALS string loses its token and master_symmetric_key values", "[odbc]") {
	CHECK(
	    RedactStatement("CREDENTIALS 'aws_access_key_id=AKIAIOSFODNN7EXAMPLE;aws_secret_access_key=wJal;token=IQoJ'") ==
	    "CREDENTIALS 'aws_access_key_id=<redacted>;aws_secret_access_key=<redacted>;token=<redacted>");
	CHECK(RedactStatement("CREDENTIALS 'aws_iam_role=r;MASTER_SYMMETRIC_KEY=EXAMPLEroot+key='") ==
	      "CREDENTIALS 'aws_iam_role=r;MASTER_SYMMETRIC_KEY=<redacted>");
	CHECK(RedactStatement("master_symmetric_key_id=value") == "master_symmetric_key_id=value");
}

TEST_CASE("A COPY or UNLOAD credential literal loses its text, not its keyword", "[odbc]") {
	CHECK(RedactStatement("COPY t FROM 's3://b/k' ACCESS_KEY_ID 'AKIAIOSFODNN7EXAMPLE' SECRET_ACCESS_KEY 'wJal/K7+x' "
	                      "SESSION_TOKEN 'IQoJ;b' DELIMITER ','") ==
	      "COPY t FROM 's3://b/k' ACCESS_KEY_ID '<redacted>' SECRET_ACCESS_KEY '<redacted>' SESSION_TOKEN '<redacted>' "
	      "DELIMITER ','");
	CHECK(RedactConnect("UNLOAD ('SELECT 1') TO 's3://b/p' MASTER_SYMMETRIC_KEY 'EXAMPLEroot+key=' ENCRYPTED") ==
	      "UNLOAD ('SELECT 1') TO 's3://b/p' MASTER_SYMMETRIC_KEY '<redacted>' ENCRYPTED");
}

TEST_CASE("A CREATE USER or ALTER USER password literal loses its text", "[odbc]") {
	CHECK(RedactStatement("CREATE USER u PASSWORD 'Hunter2;x' VALID UNTIL '2027-01-01'") ==
	      "CREATE USER u PASSWORD '<redacted>' VALID UNTIL '2027-01-01'");
	CHECK(RedactConnect("ALTER USER u PASSWORD 'md5abc'") == "ALTER USER u PASSWORD '<redacted>'");
}

TEST_CASE("A credential literal's keyword matches in any case, with any SQL white space or none before it", "[odbc]") {
	CHECK(RedactStatement("secret_access_key\t'a' x") == "secret_access_key '<redacted>' x");
	CHECK(RedactStatement("Session_Token\r\n  'a' x") == "Session_Token '<redacted>'\n x");
	CHECK(RedactStatement("password\f\v'a' x") == "password '<redacted>' x");
	CHECK(RedactStatement("PASSWORD'a' x") == "PASSWORD '<redacted>' x");
}

TEST_CASE("A credential literal ends at its closing quote, a doubled or escaped quote kept inside", "[odbc]") {
	CHECK(RedactStatement("PASSWORD 'it''s;x' VALID") == "PASSWORD '<redacted>' VALID");
	CHECK(RedactStatement("SESSION_TOKEN 'a\\'b' x") == "SESSION_TOKEN '<redacted>' x");
	CHECK(RedactStatement("SECRET_ACCESS_KEY 'a\nb' x") == "SECRET_ACCESS_KEY '<redacted>'\n x");
	CHECK(RedactStatement("SESSION_TOKEN '' x") == "SESSION_TOKEN '<redacted>' x");
}

TEST_CASE("A credential literal that nothing closes, or a cut leaves open, is redacted to the end", "[odbc]") {
	CHECK(RedactStatement("bad SECRET_ACCESS_KEY 'wJal x;y") == "bad SECRET_ACCESS_KEY '<redacted>'");
	CHECK(RedactStatement("bad SESSION_TOKEN '") == "bad SESSION_TOKEN '<redacted>'");
	CHECK(RedactCutStatement("bad PASSWORD 'hunter2 more wo") == "bad PASSWORD '<redacted>'...");
	CHECK(RedactCutConnect("MASTER_SYMMETRIC_KEY 'root") == "MASTER_SYMMETRIC_KEY '<redacted>'...");
}

TEST_CASE("Text that only looks like a SQL credential literal is left as it is", "[odbc]") {
	CHECK(RedactStatement("ALTER USER u PASSWORD DISABLE") == "ALTER USER u PASSWORD DISABLE");
	CHECK(RedactConnect("ALTER USER u PASSWORD DISABLE") == "ALTER USER u PASSWORD DISABLE");
	CHECK(RedactStatement("SECRET_ACCESS_KEY is required") == "SECRET_ACCESS_KEY is required");
	CHECK(RedactStatement("bad SESSION_TOKEN") == "bad SESSION_TOKEN");
	CHECK(RedactStatement("user_password 'x'") == "user_password 'x'");
	CHECK(RedactStatement("PASSWORDS 'x'") == "PASSWORDS 'x'");
	CHECK(RedactStatement("master_symmetric_key_hint 'x'") == "master_symmetric_key_hint 'x'");
	CHECK(RedactStatement("PASSWORD \\x") == "PASSWORD \\x");
}

TEST_CASE("A credential literal whose quotes backslashes escape is redacted to the end", "[odbc]") {
	// SQL inside a string literal escapes the inner quotes; the escaped closer keeps the literal open.
	CHECK(RedactStatement("EXECUTE 'ALTER USER u PASSWORD \\'hunter2\\''") ==
	      "EXECUTE 'ALTER USER u PASSWORD '<redacted>'");
	CHECK(RedactStatement("x SESSION_TOKEN \\\\\\'tok\\\\\\' y") == "x SESSION_TOKEN '<redacted>'");
}

TEST_CASE("Where a credential literal and an assignment overlap, the one that starts first gives the form", "[odbc]") {
	CHECK(RedactStatement("{'password':'x'} tail") == "{password=<redacted>");
	CHECK(RedactStatement("PASSWORD 'pwd=x;' tail") == "PASSWORD '<redacted>' tail");
}

TEST_CASE("A credential keyword in quotes of its own takes the literal after them", "[odbc]") {
	CHECK(RedactStatement("bad 'PASSWORD' 'PLACEHOLDER_SECRET' x") == "bad PASSWORD '<redacted>' x");
	CHECK(RedactStatement("bad \"SESSION_TOKEN\"\n'PLACEHOLDER_SECRET' x") == "bad SESSION_TOKEN '<redacted>'\n x");
	CHECK(RedactConnect("'secret_access_key''PLACEHOLDER_SECRET' x") == "secret_access_key '<redacted>' x");
	CHECK(RedactStatement("'MASTER_SYMMETRIC_KEY' \\'PLACEHOLDER_SECRET\\' x") == "MASTER_SYMMETRIC_KEY '<redacted>'");
	CHECK(RedactCutStatement("bad 'PASSWORD' 'PLACEHOLDER_SEC") == "bad PASSWORD '<redacted>'...");
	// A word before the opening quote keeps it, so the keyword does not run on from that word.
	CHECK(RedactStatement("bad x'PASSWORD' 'PLACEHOLDER_SECRET' y") == "bad x'PASSWORD '<redacted>' y");
}

TEST_CASE("A single quote straight after a credential keyword is also read as the literal's opening quote", "[odbc]") {
	// A literal that starts with a doubled quote then closes later on that reading.
	CHECK(RedactStatement("x PASSWORD'''PLACEHOLDER_SECRET' tail") == "x PASSWORD '<redacted>' tail");
	CHECK(RedactStatement("x PASSWORD' ''PLACEHOLDER_SECRET' tail") == "x PASSWORD '<redacted>' tail");
}

TEST_CASE("The literal after a credential keyword's closing quote is redacted, whatever quote opened the keyword",
          "[odbc]") {
	// Or none.
	CHECK(RedactStatement("PASSWORD' 'PLACEHOLDER_SECRET' tail") == "PASSWORD '<redacted>' tail");
	CHECK(RedactStatement("x PASSWORD\" 'PLACEHOLDER_SECRET' tail") == "x PASSWORD '<redacted>' tail");
	CHECK(RedactStatement("x \"PASSWORD' 'PLACEHOLDER_SECRET' tail") == "x \"PASSWORD '<redacted>' tail");
	// With no literal after it, a closing single quote opens one, which runs to the end, as before, and a
	// double quote opens none.
	CHECK(RedactStatement("column 'password' does not exist") == "column 'password '<redacted>'");
	CHECK(RedactStatement("x PASSWORD\" DISABLE") == "x PASSWORD\" DISABLE");
}

TEST_CASE("A credential keyword in quotes of its own that anything but SQL white space parts from a literal keeps "
          "it, an accepted residual",
          "[odbc]") {
	// Its closing quote opens a literal that ends at the next one's opening quote.
	CHECK(SanitizeServerMessage(RedactStatement("bad ('password', 'PLACEHOLDER_SECRET') x")) ==
	      "bad ('password '<redacted>'PLACEHOLDER_SECRET') x");
	CHECK(SanitizeServerMessage(RedactStatement("WHERE k = 'session_token' AND v = 'PLACEHOLDER_SECRET'")) ==
	      "WHERE k = 'session_token '<redacted>'PLACEHOLDER_SECRET'");
	// A closing double quote opens none.
	CHECK(RedactStatement("bad (\"password\", 'PLACEHOLDER_SECRET') x") ==
	      "bad (\"password\", 'PLACEHOLDER_SECRET') x");
	// A keyword in single quotes that an invisible character parts from the literal keeps it too.
	CHECK(SanitizeServerMessage(RedactStatement("bad 'PASSWORD'\xE2\x80\x8B'PLACEHOLDER_SECRET' x")) ==
	      "bad 'PASSWORD '<redacted>'PLACEHOLDER_SECRET' x");
}

TEST_CASE("A credential literal after a closing quote and a literal holding a character the joined line leaves out "
          "shows, an accepted residual",
          "[odbc]") {
	// Redaction replaces through the quote after that character, so the sanitizer has no doubled quote left to
	// rejoin.
	for (const std::string invisible : {"\t", "\xE2\x80\x8B", "\xC2\xA0"}) {
		INFO(invisible);
		CHECK(SanitizeServerMessage(RedactStatement("x PASSWORD''" + invisible + "'PLACEHOLDER_SECRET' tail")) ==
		      "x PASSWORD '<redacted>'PLACEHOLDER_SECRET' tail");
	}
	CHECK(SanitizeServerMessage(RedactStatement("x PASSWORD' '\t'PLACEHOLDER_SECRET' tail")) ==
	      "x PASSWORD '<redacted>'PLACEHOLDER_SECRET' tail");
	// Other text beside that character in the literal changes nothing.
	for (const std::string literal : {"''''\t'", "''a\t'"}) {
		INFO(literal);
		CHECK(SanitizeServerMessage(RedactStatement("x PASSWORD" + literal + "PLACEHOLDER_SECRET' tail")) ==
		      "x PASSWORD '<redacted>'PLACEHOLDER_SECRET' tail");
	}
	// Nor does a second credential keyword as that text: the joined line no longer rejoins it with its literal.
	CHECK(SanitizeServerMessage(
	          RedactStatement("x 'PASSWORD' 'secret_access_key\xE2\x80\x8B'PLACEHOLDER_SECRET' tail")) ==
	      "x PASSWORD '<redacted>'PLACEHOLDER_SECRET' tail");
	// A closing double quote does the same.
	CHECK(SanitizeServerMessage(
	          RedactStatement("x \"PASSWORD\" 'secret_access_key\xE2\x80\x8B'PLACEHOLDER_SECRET' tail")) ==
	      "x PASSWORD '<redacted>'PLACEHOLDER_SECRET' tail");
}

TEST_CASE("A literal that starts with a doubled quote after a double quote after a credential keyword keeps its text, "
          "an accepted residual",
          "[odbc]") {
	// A double quote opens no SQL literal, so only the empty literal after it is read.
	CHECK(RedactStatement("x PASSWORD\" ''PLACEHOLDER_SECRET' tail") ==
	      "x PASSWORD '<redacted>'PLACEHOLDER_SECRET' tail");
}

TEST_CASE("A credential literal whose quotes are doubled keeps its text, an accepted residual", "[odbc]") {
	// SQL inside a SQL literal doubles them, which reads as an empty literal.
	CHECK(RedactStatement("'PASSWORD ''x'''") == "'PASSWORD '<redacted>'x'''");
}

TEST_CASE("A credential literal or driver key that a character left out rejoins is redacted", "[odbc]") {
	CHECK(SanitizeServerMessage(RedactStatement("bad SECRET_\x01"
	                                            "ACCESS_KEY 'wJal' x")) == "bad SECRET_ACCESS_KEY '<redacted>' x");
	CHECK(SanitizeServerMessage(RedactStatement("bad ProxyPwd\x01=hunter2")) == "bad ProxyPwd=<redacted>");
}

TEST_CASE("A redacted credential literal passes the sanitizer unchanged", "[odbc]") {
	CHECK(SanitizeServerMessage(RedactStatement("bad PASSWORD 'hunter2' x")) == "bad PASSWORD '<redacted>' x");
}

TEST_CASE("A redacted credential or value that held a line end keeps one, so no later line joins the first", "[odbc]") {
	// A user sees only the first line, so text after the line end stays unseen.
	CHECK(RedactStatement("x PASSWORD\n'placeholder' tail") == "x PASSWORD '<redacted>'\n tail");
	CHECK(SanitizeServerMessage(RedactStatement("x PASSWORD\n'placeholder' tail")) == "x PASSWORD '<redacted>'");
	// An empty literal on the next line, then a doubled-quote leftover, which a later line kept unseen.
	CHECK(SanitizeServerMessage(RedactStatement("x PASSWORD\n''placeholder''")) == "x PASSWORD '<redacted>'");
	CHECK(SanitizeServerMessage(RedactStatement("SECRET_ACCESS_KEY 'a\r\nb' x")) == "SECRET_ACCESS_KEY '<redacted>'");
	CHECK(SanitizeServerMessage(RedactConnect("PWD='a\nb';tail")) == "PWD=<redacted>");
	CHECK(SanitizeServerMessage(RedactStatement("PWD={a\rb};tail")) == "PWD=<redacted>");
	CHECK(SanitizeServerMessage(RedactStatement("x abcdefgh\nijkl tail", {"abcdefgh\nijkl"})) == "x <param 1>");
	// A line end that is a value's last byte, or its first, counts too, and so does one in a cut message.
	CHECK(SanitizeServerMessage(RedactStatement("x abcdefgh\ntail", {"abcdefgh\n"})) == "x <param 1>");
	CHECK(SanitizeServerMessage(RedactStatement("x\nabcdefgh tail", {"\nabcdefgh"})) == "x<param 1>");
	CHECK(SanitizeServerMessage(RedactCutStatement("x abcdefgh\nijkl more tail", {"abcdefgh\nijkl"})) == "x <param 1>");
}

TEST_CASE("No canary in a SQL credential literal or a driver key's value reaches a diagnostic, in either phase, whole "
          "or cut",
          "[odbc]") {
	const std::vector<std::string> messages = {
	    "COPY t FROM 's3://b/k' SECRET_ACCESS_KEY 'c4nary' tail",
	    "x SESSION_TOKEN\n'c4;nary' tail",
	    "x 'PASSWORD' 'c4nary' tail",
	    "UNLOAD ('q') TO 's3://b/p' MASTER_SYMMETRIC_KEY 'c4''nary' tail",
	    "CREATE USER u PASSWORD 'c4nary",
	    "CREDENTIALS 'aws_access_key_id=k;token=c4nary' tail",
	    "CREDENTIALS 'aws_iam_role=r;master_symmetric_key=c4nary'",
	    "web_identity_token: c4nary tail",
	    "x \"client_secret\": \"c4;nary\" tail",
	    "ProxyPwd={c4;na}}ry} tail",
	    "https_proxy_password = 'c4;nary' tail",
	    "Token=c4nary tail",
	};
	for (const auto &message : messages) {
		INFO(message);
		CHECK(!Contains(RedactConnect(message), "c4"));
		CHECK(!Contains(RedactStatement(message), "c4"));
		CHECK(!Contains(RedactCutConnect(message), "c4"));
		CHECK(!Contains(RedactCutStatement(message), "c4"));
	}
}

TEST_CASE("A value keeps the quote that opens just before its keyword outside the replacement", "[odbc]") {
	CHECK(RedactStatement("invalid input syntax for integer: \"PWD=hunter2\"") ==
	      "invalid input syntax for integer: \"PWD=<redacted>\"");
	CHECK(SanitizeServerMessage(RedactStatement("syntax: 'aws_secret_access_key=wJalrXUtnFEMI'")) ==
	      "syntax: 'aws_secret_access_key=<redacted>'");
	// A braced or quoted value leaves it outside too, after the value's closer.
	CHECK(RedactStatement("\"Password={hunter2}\"") == "\"Password=<redacted>\"");
	CHECK(RedactStatement("\"PWD='x'\"") == "\"PWD=<redacted>\"");
	CHECK(RedactStatement("'PWD=\"x\"'") == "'PWD=<redacted>'");
	// Only the quote that opened just before the keyword, as the value's last byte, stays outside.
	CHECK(RedactStatement("\"PWD=hunter2'") == "\"PWD=<redacted>");
	CHECK(RedactStatement("x PWD=hunter2\"") == "x PWD=<redacted>");
	CHECK(RedactStatement("\"PWD=x\" at 5") == "\"PWD=<redacted>");
	// A brace that nothing closes keeps the quote inside, where it may be the secret's last byte.
	CHECK(RedactStatement("\"Password={hunter2\"") == "\"Password=<redacted>");
	// A quoted keyword's own closing quote is not one before it, so its value keeps a last quote.
	CHECK(RedactStatement("x \"PWD\"=hunter2\";UID=u") == "x PWD=<redacted>;UID=u");
}

TEST_CASE("A plain value ends at a semicolon or a line end", "[odbc]") {
	CHECK(RedactConnect("PWD=secret\nnext line") == "PWD=<redacted>\nnext line");
	CHECK(RedactConnect("PWD=secret\rnext line") == "PWD=<redacted>\rnext line");
}

TEST_CASE("An assignment with nothing after its equals sign is left as it is", "[odbc]") {
	CHECK(RedactConnect("PWD=\nUID=u") == "PWD=\nUID=u");
	CHECK(RedactStatement("missing PWD=") == "missing PWD=");
}

TEST_CASE("A braced or quoted value runs past a line end until it closes", "[odbc]") {
	CHECK(RedactConnect("PWD={a\nsecret}") == "PWD=<redacted>\n");
	CHECK(RedactStatement("aws_session_token=\"a\r\nsecret\"") == "aws_session_token=<redacted>\n");
	CHECK(RedactStatement("PWD: 'a\nsecret' tail") == "PWD=<redacted>\n");
}

TEST_CASE("A braced or quoted value may start with its doubled closer or be empty", "[odbc]") {
	CHECK(RedactConnect("PWD={}};secret}") == "PWD=<redacted>");
	CHECK(RedactConnect("PWD={};UID=u") == "PWD=<redacted>;UID=u");
	CHECK(RedactStatement("PWD=''';secret'") == "PWD=<redacted>");
	CHECK(RedactStatement("PWD='';UID=u") == "PWD=<redacted>;UID=u");
	CHECK(RedactStatement("PWD: \"\"\";secret\"") == "PWD=<redacted>");
	CHECK(RedactStatement("PWD: \"\";UID=u") == "PWD=<redacted>;UID=u");
}

TEST_CASE("Text after a closing brace or quote is still the value, up to the next semicolon", "[odbc]") {
	CHECK(RedactConnect("PWD='abc'defsecret;UID=u") == "PWD=<redacted>;UID=u");
	CHECK(RedactConnect("PWD=\"abc\"defsecret;UID=u") == "PWD=<redacted>;UID=u");
	CHECK(RedactConnect("PWD={abc}defsecret;UID=u") == "PWD=<redacted>;UID=u");
	CHECK(RedactStatement("aws_session_token='t'rest\nnext") == "aws_session_token=<redacted>\nnext");
	CHECK(RedactConnect("PWD='t'rest\rnext") == "PWD=<redacted>\rnext");
	CHECK(RedactCutConnect("bad PWD='ab'cd ef") == "bad PWD=<redacted>...");
}

TEST_CASE("Spaces or tabs around an assignment's equals sign are part of it", "[odbc]") {
	CHECK(RedactConnect("PWD\t=\t{a;b}") == "PWD=<redacted>");
	CHECK(RedactConnect("PWD = {a;b}") == "PWD=<redacted>");
	CHECK(RedactStatement("aws_session_token =  \"x;TOKENcanary\"") == "aws_session_token=<redacted>");
}

TEST_CASE("A form feed or vertical tab around an assignment's equals sign is part of it, as the driver skips it",
          "[odbc]") {
	CHECK(RedactConnect("PWD=\v{a;b}") == "PWD=<redacted>");
	CHECK(RedactConnect("PWD=\f{a;b}") == "PWD=<redacted>");
	CHECK(RedactStatement("at PWD\v={a;b} ok") == "at PWD=<redacted>");
	CHECK(RedactStatement("at PWD\f={a;b} ok") == "at PWD=<redacted>");
}

TEST_CASE("A colon assignment of each keyword loses its value in both phases", "[odbc]") {
	CHECK(RedactStatement("aws_secret_access_key: SECRETcanary") == "aws_secret_access_key=<redacted>");
	CHECK(RedactConnect("aws_secret_access_key: SECRETcanary") == "aws_secret_access_key=<redacted>");
	CHECK(RedactStatement("bad aws_session_token: TOKENcanary") == "bad aws_session_token=<redacted>");
	CHECK(RedactConnect("bad aws_session_token: TOKENcanary") == "bad aws_session_token=<redacted>");
	CHECK(RedactStatement("PWD: PWDcanary;UID=u") == "PWD=<redacted>;UID=u");
	CHECK(RedactConnect("PWD: PWDcanary;UID=u") == "PWD=<redacted>;UID=u");
}

TEST_CASE("Spaces or tabs around an assignment's colon are part of it, and it needs none", "[odbc]") {
	CHECK(RedactStatement("aws_session_token:TOKENcanary") == "aws_session_token=<redacted>");
	CHECK(RedactStatement("aws_session_token \t:TOKENcanary") == "aws_session_token=<redacted>");
	CHECK(RedactConnect("aws_secret_access_key:\t SECRETcanary") == "aws_secret_access_key=<redacted>");
	CHECK(RedactStatement("PWD : {a;PWDcanary}") == "PWD=<redacted>");
	CHECK(RedactConnect("PWD\t:\t'a;PWDcanary'") == "PWD=<redacted>");
	CHECK(RedactStatement("PWD: \t{a;PWDcanary}") == "PWD=<redacted>");
	CHECK(RedactConnect("aws_secret_access_key:  'x;SECRETcanary'") == "aws_secret_access_key=<redacted>");
}

TEST_CASE("A colon assignment's value is redacted whether plain, braced or quoted", "[odbc]") {
	CHECK(RedactStatement("PWD: {PWD;can}}ary}") == "PWD=<redacted>");
	CHECK(RedactStatement("aws_secret_access_key: 'SECRET;can''ary'") == "aws_secret_access_key=<redacted>");
	CHECK(RedactStatement("aws_session_token: \"TOKEN;canary\" after;x") == "aws_session_token=<redacted>;x");
	CHECK(RedactConnect("aws_session_token: 'TOKEN;canary'\nnext") == "aws_session_token=<redacted>\nnext");
	CHECK(RedactConnect("aws_secret_access_key: {SECRET;canary}rest;x") == "aws_secret_access_key=<redacted>;x");
	// An escaped brace or quote before a `;` keeps the value open past it.
	CHECK(RedactStatement("PWD: {a}};PWDcanary}") == "PWD=<redacted>");
	CHECK(RedactStatement("aws_secret_access_key: 'a'';SECRETcanary'") == "aws_secret_access_key=<redacted>");
	CHECK(RedactConnect("aws_session_token: \"a\"\";TOKENcanary\"") == "aws_session_token=<redacted>");
}

TEST_CASE("A colon assignment that a cut reaches or leaves open is redacted to the end", "[odbc]") {
	CHECK(RedactCutStatement("bad aws_session_token: {TOKEN;canary wo") == "bad aws_session_token=<redacted>...");
	CHECK(RedactCutStatement("bad aws_secret_access_key: \"SECRET;canary wo") ==
	      "bad aws_secret_access_key=<redacted>...");
	CHECK(RedactCutConnect("bad PWD: 'PWD;canary wo") == "bad PWD=<redacted>...");
	CHECK(RedactCutStatement("bad PWD: PWDcanary more") == "bad PWD=<redacted>...");
}

TEST_CASE("A colon assignment's keyword matches in any case", "[odbc]") {
	CHECK(RedactStatement("AWS_Session_Token: TOKENcanary") == "AWS_Session_Token=<redacted>");
	CHECK(RedactConnect("Aws_Secret_Access_Key : SECRETcanary") == "Aws_Secret_Access_Key=<redacted>");
	CHECK(RedactStatement("pwd: PWDcanary") == "pwd=<redacted>");
}

TEST_CASE("A keyword in double or single quotes, as a JSON key, loses its value and its quotes", "[odbc]") {
	CHECK(RedactStatement("\"aws_session_token\": \"SECRETtok\"") == "aws_session_token=<redacted>");
	CHECK(RedactStatement("{\"aws_session_token\":\"SECRETtok\"}") == "{aws_session_token=<redacted>");
	CHECK(RedactStatement("\"aws_session_token\"=SECRETtok") == "aws_session_token=<redacted>");
	CHECK(RedactStatement("'aws_session_token': 'SECRETtok'") == "aws_session_token=<redacted>");
	CHECK(RedactStatement("{'aws_session_token':'SECRETtok'}") == "{aws_session_token=<redacted>");
	CHECK(RedactStatement("'aws_session_token'=SECRETtok") == "aws_session_token=<redacted>");
	CHECK(RedactConnect("{\"aws_secret_access_key\":\"SECRET;canary\"}") == "{aws_secret_access_key=<redacted>");
	CHECK(RedactConnect("\"PWD\" : {a;PWDcanary}") == "PWD=<redacted>");
	CHECK(RedactStatement("\"Aws_Session_Token\"\t:\tSECRETtok") == "Aws_Session_Token=<redacted>");
}

TEST_CASE("Text around a quoted keyword's assignment is kept up to the value's end", "[odbc]") {
	CHECK(RedactStatement("bad {\"aws_session_token\":\"SECRETtok\"};next") ==
	      "bad {aws_session_token=<redacted>;next");
	CHECK(RedactConnect("{\n  \"aws_session_token\": \"SECRETtok\",\n  \"region\": \"us-east-1\"\n}") ==
	      "{\n  aws_session_token=<redacted>\n  \"region\": \"us-east-1\"\n}");
	CHECK(RedactStatement("SET x = '{\"a\": 1}'; SELECT '{\"pwd\": \"SECRETtok\"}'") ==
	      "SET x = '{\"a\": 1}'; SELECT '{pwd=<redacted>");
}

TEST_CASE("A backslash before a quote keeps a quoted value open, as JSON escapes it", "[odbc]") {
	CHECK(RedactStatement("{\"pwd\": \"a\\\"b;SECRETtail\"}") == "{pwd=<redacted>");
	CHECK(RedactConnect("{\"aws_session_token\": \"tok\", \"pwd\": \"a\\\";SECRETtail\"}") ==
	      "{aws_session_token=<redacted>");
	// A value that ends in a backslash-escaped quote is redacted to the end, which over-redacts.
	CHECK(RedactStatement("PWD='pass\\';UID=u") == "PWD=<redacted>");
	// A doubled quote is read before the backslash, so the value runs on to its real closer.
	CHECK(RedactStatement("PWD='a\\'';UID=u") == "PWD=<redacted>");
	CHECK(RedactStatement("PWD='ab\\'';Xsecret'") == "PWD=<redacted>");
	CHECK(RedactStatement("PWD=\"ab\\\"\";Xsecret\"") == "PWD=<redacted>");
	CHECK(RedactStatement("aws_secret_access_key='ab\\'';Xsecret' tail") == "aws_secret_access_key=<redacted>");
	CHECK(RedactStatement("PWD='a\\''b;secret'") == "PWD=<redacted>");
	CHECK(RedactStatement("PWD=\"a\\\"\"b;secret\"") == "PWD=<redacted>");
}

TEST_CASE("A backslash before a closing brace does not escape it", "[odbc]") {
	CHECK(RedactConnect("PWD={a\\};UID=u") == "PWD=<redacted>;UID=u");
	CHECK(RedactStatement("PWD={ab\\}};Xsecret}") == "PWD=<redacted>");
	CHECK(RedactStatement("PWD={a\\}}b;secret}") == "PWD=<redacted>");
}

TEST_CASE("A key whose own quotes are escaped or doubled loses its value and those quotes", "[odbc]") {
	CHECK(RedactStatement("{\\\"pwd\\\": \\\"secret\\\"}") == "{pwd=<redacted>");
	CHECK(RedactConnect("{\\\"aws_session_token\\\":\\\"tok;SECRET\\\"}") == "{aws_session_token=<redacted>");
	CHECK(RedactStatement("{\"\"pwd\"\": \"\"secret\"\"}") == "{pwd=<redacted>");
	CHECK(RedactStatement("SELECT '{''aws_secret_access_key'': ''SECRET''}'") ==
	      "SELECT '{aws_secret_access_key=<redacted>");
	CHECK(RedactStatement("\\'pwd\\' = SECRET") == "pwd=<redacted>");
}

TEST_CASE("A key whose own quotes are escaped twice loses its value and those quotes", "[odbc]") {
	// As a JSON string inside a JSON string writes it.
	CHECK(RedactStatement("{\\\\\\\"pwd\\\\\\\": \\\\\\\"secret\\\\\\\"}") == "{pwd=<redacted>");
	CHECK(RedactConnect("\\\\\\\"aws_session_token\\\\\\\":\\\\\\\"tok;SECRET\\\\\\\"") ==
	      "aws_session_token=<redacted>");
	CHECK(RedactStatement("\\\\\\'pwd\\\\\\' = SECRET") == "pwd=<redacted>");
}

TEST_CASE("A key whose own quotes are escaped three times loses its value and those quotes", "[odbc]") {
	// As JSON nested three deep writes it, seven backslashes before each quote; no bound on the run.
	CHECK(RedactStatement("{\\\\\\\\\\\\\\\"pwd\\\\\\\\\\\\\\\": \\\\\\\\\\\\\\\"secret\\\\\\\\\\\\\\\"}") ==
	      "{pwd=<redacted>");
}

TEST_CASE("Any run of backslashes before a key's quote reads as an escape, so its value runs to the line end",
          "[odbc]") {
	// Over-redaction: the JSON key `pwd\`, an escaped backslash and then a real quote, reads so too.
	CHECK(RedactStatement("pwd\\\\\": a;b") == "pwd=<redacted>");
}

TEST_CASE("The value after an escaped or doubled key runs to the line end", "[odbc]") {
	// Its value is quoted the same way, so a `;` in it cannot be told from one after it.
	CHECK(RedactStatement("{\"\"pwd\"\": \"\"a;SECRET\"\"}") == "{pwd=<redacted>");
	CHECK(RedactConnect("{\\\"pwd\\\": {a;SECRET}}\nnext") == "{pwd=<redacted>\nnext");
	CHECK(RedactConnect("{\\\\\\\"pwd\\\\\\\": {a;SECRET}}\nnext") == "{pwd=<redacted>\nnext");
}

TEST_CASE("An escaped key with nothing after its separator is left as it is, as any key is", "[odbc]") {
	CHECK(RedactStatement("{\\\"pwd\\\":;UID=u") == "{\\\"pwd\\\":;UID=u");
	CHECK(RedactConnect("{\\\"pwd\\\":\nnext") == "{\\\"pwd\\\":\nnext");
}

TEST_CASE("Blanks alone after an escaped key are a value that runs to the line end", "[odbc]") {
	CHECK(RedactStatement("{\\\"pwd\\\": ;UID=u") == "{pwd=<redacted>");
}

TEST_CASE("A quote after a keyword loses its opening quote only where the two match", "[odbc]") {
	CHECK(RedactStatement("aws_session_token\": SECRETtok") == "aws_session_token=<redacted>");
	CHECK(RedactStatement("'aws_session_token\": SECRETtok") == "'aws_session_token=<redacted>");
	CHECK(RedactStatement("\"aws_session_token': SECRETtok") == "\"aws_session_token=<redacted>");
	// Escape or doubling included. A byte comes first, so the bytes before the keyword, not their
	// count, decide; the last opening is one backslash short of its four-byte closing quote.
	CHECK(RedactStatement("{\"pwd\\\": SECRET") == "{\"pwd=<redacted>");
	CHECK(RedactStatement("{\"pwd\"\": SECRET") == "{\"pwd=<redacted>");
	CHECK(RedactStatement("{\\\\\"pwd\\\\\\\": SECRET") == "{\\\\\"pwd=<redacted>");
}

TEST_CASE("A quoted keyword keeps its opening quote where a word comes before it", "[odbc]") {
	// So that the keyword does not run on from that word.
	CHECK(RedactStatement("bad x\"pwd\": SECRET") == "bad x\"pwd=<redacted>");
	CHECK(RedactStatement("bad 1\"pwd\": SECRET") == "bad 1\"pwd=<redacted>");
	CHECK(RedactStatement("bad _\"pwd\": SECRET") == "bad _\"pwd=<redacted>");
	CHECK(SanitizeServerMessage(RedactStatement("at h.amazonaws.com\"\"pwd\"\": SECRET")) ==
	      "at <host>\"\"pwd=<redacted>");
}

TEST_CASE("A quoted, escaped or doubled keyword followed by a cast's `::` is not an assignment", "[odbc]") {
	CHECK(RedactStatement("SELECT \"pwd\"::text FROM t") == "SELECT \"pwd\"::text FROM t");
	CHECK(RedactConnect("WHERE 'pwd'::varchar = k") == "WHERE 'pwd'::varchar = k");
	CHECK(RedactStatement("SELECT \"\"pwd\"\"::text") == "SELECT \"\"pwd\"\"::text");
	CHECK(RedactStatement("SELECT \\\"pwd\\\"::text") == "SELECT \\\"pwd\\\"::text");
}

TEST_CASE("A message holding both separators loses every value", "[odbc]") {
	CHECK(RedactStatement("PWD=PWDcanary; aws_session_token: TOKENcanary; aws_secret_access_key=SECRETcanary") ==
	      "PWD=<redacted>; aws_session_token=<redacted>; aws_secret_access_key=<redacted>");
	CHECK(RedactConnect("aws_secret_access_key: SECRETcanary;PWD = PWDcanary") ==
	      "aws_secret_access_key=<redacted>;PWD=<redacted>");
}

TEST_CASE("Ordinary text after a keyword and a colon is redacted to the line end, as a value", "[odbc]") {
	// Over-redaction, chosen over leaking: the rule cannot tell this text from a value.
	CHECK(RedactStatement("missing PWD: set it in the DSN\nnext") == "missing PWD=<redacted>\nnext");
}

TEST_CASE("A plain value that holds a URL runs past a semicolon inside the URL", "[odbc]") {
	const std::string message = "PWD: see https://h/doc;jsessionid=SESSIONcanary";
	CHECK(RedactStatement(message) == "PWD=<redacted>");
	CHECK(RedactConnect(message) == "PWD=<redacted>");
	CHECK(SanitizeServerMessage(RedactStatement(message)) == "PWD=<redacted>");
	CHECK(RedactStatement("ERROR: invalid aws_session_token: https://sts.example/?a=1;X-Amz-Security-Token=SECRET") ==
	      "ERROR: invalid aws_session_token=<redacted>");
	// Every URL the value holds, each with its `;`.
	CHECK(RedactStatement("PWD: a https://h/x;y=1;z=2 and s3://b/k;v=SECRET") == "PWD=<redacted>");
	CHECK(RedactCutStatement("bad PWD: see https://h/doc;jsessionid=SESSIONcanary wo") == "bad PWD=<redacted>...");
	// A URL holds no line end, which still ends the value.
	CHECK(RedactStatement("PWD: see https://h/doc;x\nnext") == "PWD=<redacted>\nnext");
	// After a closing quote or brace, the rest of the value reads a URL the same way.
	CHECK(RedactStatement("PWD: 'a' see https://h/doc;jsessionid=SESSIONcanary") == "PWD=<redacted>");
	CHECK(RedactStatement("PWD={a}https://h/doc;jsessionid=SESSIONcanary now; next") == "PWD=<redacted>; next");
}

TEST_CASE("After a URL, a plain value runs on to the next semicolon or line end", "[odbc]") {
	CHECK(RedactStatement("PWD: see https://h/doc;jsessionid=SESSIONcanary for help; next") == "PWD=<redacted>; next");
	CHECK(RedactConnect("PWD: see https://h/doc;jsessionid=SESSIONcanary now\nnext") == "PWD=<redacted>\nnext");
	CHECK(SanitizeServerMessage(RedactStatement("PWD: see https://h/doc;jsessionid=SESSIONcanary for help; next")) ==
	      "PWD=<redacted>; next");
	// A value whose `;` is outside every URL ends there, as before.
	CHECK(RedactStatement("PWD: see https://h/doc more; next") == "PWD=<redacted>; next");
}

TEST_CASE("A URL's scheme matches in any case", "[odbc]") {
	CHECK(RedactStatement("PWD: see HTTPS://H/DOC;JSESSIONID=SESSIONcanary") == "PWD=<redacted>");
	CHECK(RedactStatement("PWD: see Https://h/doc;jsessionid=SESSIONcanary") == "PWD=<redacted>");
}

TEST_CASE("A scheme with no `://` after it is no URL, so a plain value still ends at its semicolon", "[odbc]") {
	CHECK(RedactStatement("PWD: see https:/h/doc;jsessionid=x") == "PWD=<redacted>;jsessionid=x");
	CHECK(RedactStatement("PWD: see https//h/doc;jsessionid=x") == "PWD=<redacted>;jsessionid=x");
	CHECK(RedactStatement("PWD: see mailto:a@h.example;subject=x") == "PWD=<redacted>;subject=x");
	CHECK(RedactStatement("PWD: see ://h/doc;jsessionid=x") == "PWD=<redacted>;jsessionid=x");
	// A URL that ends before the `;`, at a blank or a quote, leaves the `;` to end the value.
	CHECK(RedactStatement("PWD: see https://h/doc';jsessionid=x") == "PWD=<redacted>;jsessionid=x");
	CHECK(RedactStatement("PWD: see https://h/doc ;jsessionid=x") == "PWD=<redacted>;jsessionid=x");
	CHECK(RedactStatement("PWD: see https://h/doc\";jsessionid=x") == "PWD=<redacted>;jsessionid=x");
}

TEST_CASE("Redaction reads a URL as the sanitizer's URL rule does, through a marker inside it", "[odbc]") {
	CHECK(RedactStatement("PWD: see https://h/<param 1>;jsessionid=SESSIONcanary") == "PWD=<redacted>");
	CHECK(RedactStatement("PWD: see https://h/<redacted>;jsessionid=SESSIONcanary") == "PWD=<redacted>");
	// A tab or form feed, which the sanitizer's joined line leaves out, does not end a URL either.
	CHECK(RedactStatement("PWD: see https://h/doc\t?a=1;jsessionid=SESSIONcanary") == "PWD=<redacted>");
	CHECK(SanitizeServerMessage(RedactStatement("PWD: see https://h/doc\f?a=1;jsessionid=SESSIONcanary")) ==
	      "PWD=<redacted>");
	CHECK(SanitizeServerMessage("see https://h/doc\t?a=1;jsessionid=SESSIONcanary") == "see <url>");
}

TEST_CASE("A value that starts inside a URL keeps its end, and the sanitizer's URL rule takes the rest", "[odbc]") {
	CHECK(RedactStatement("at https://h/a?pwd:SECRET;tail/b rest; next") ==
	      "at https://h/a?pwd=<redacted>;tail/b rest; next");
	CHECK(SanitizeServerMessage(RedactStatement("at https://h/a?pwd:SECRET;tail/b rest; next")) ==
	      "at <url> rest; next");
	CHECK(SanitizeServerMessage(RedactConnect("connect to jdbc:redshift://h:5439/dev;UID=u;PWD=x;ssl=true failed: "
	                                          "password authentication failed")) ==
	      "connect to jdbc:<url> failed: password authentication failed");
}

TEST_CASE("A braced value's rest runs past a semicolon inside any URL, one that holds its closing brace too",
          "[odbc]") {
	CHECK(RedactStatement("PWD: {see https://h/doc}x;jsessionid=SESSIONcanary} now") == "PWD=<redacted>");
	CHECK(RedactStatement("at https://h/?pwd={a}b;tail rest") == "at https://h/?pwd=<redacted>");
}

TEST_CASE("A braced value's rest still ends at a line end after a URL", "[odbc]") {
	CHECK(RedactStatement("PWD={a}https://h/doc;x\nnext") == "PWD=<redacted>\nnext");
}

TEST_CASE("A keyword whose colon starts a URL's `://` is the URL's scheme, not an assignment", "[odbc]") {
	CHECK(RedactStatement("bad pwd://h/doc;jsessionid=SESSIONcanary ok") ==
	      "bad pwd://h/doc;jsessionid=SESSIONcanary ok");
	CHECK(SanitizeServerMessage(RedactStatement("bad pwd://h/doc;jsessionid=SESSIONcanary ok")) == "bad <url> ok");
	CHECK(SanitizeServerMessage(RedactStatement("bad x.PWD://h/doc;jsessionid=SESSIONcanary ok")) == "bad <url> ok");
	// Only a keyword's own colon: no scheme ends in a quote, so a quoted key's colon still separates.
	CHECK(RedactStatement("\"pwd\"://SECRET;UID=u") == "pwd=<redacted>;UID=u");
	// Only right before `//`: with blanks between, the URL is a value.
	CHECK(RedactStatement("PWD: //h/doc;jsessionid=SESSIONcanary") == "PWD=<redacted>;jsessionid=SESSIONcanary");
	CHECK(RedactStatement("PWD=//h/doc;UID=u") == "PWD=<redacted>;UID=u");
}

TEST_CASE("A keyword's colon before one slash separates a value, as only `://` starts a URL", "[odbc]") {
	// An AWS secret key can start with `/`.
	CHECK(RedactStatement("PWD:/secret;UID=u") == "PWD=<redacted>;UID=u");
	CHECK(RedactStatement("aws_secret_access_key:/AbCd+efGh;UID=u") == "aws_secret_access_key=<redacted>;UID=u");
	CHECK(RedactConnect("aws_secret_access_key:/AbCd+efGh;UID=u") == "aws_secret_access_key=<redacted>;UID=u");
}

TEST_CASE("An equals-sign value that holds a URL runs past a semicolon inside it too", "[odbc]") {
	CHECK(RedactStatement("PWD=see https://h/doc;jsessionid=SESSIONcanary") == "PWD=<redacted>");
	CHECK(RedactConnect("aws_session_token = https://h/doc;jsessionid=SESSIONcanary") ==
	      "aws_session_token=<redacted>");
	CHECK(SanitizeServerMessage(RedactStatement("PWD=see https://h/doc;jsessionid=SESSIONcanary")) == "PWD=<redacted>");
}

TEST_CASE("An assignment that starts inside an earlier assignment's value is still redacted in full", "[odbc]") {
	CHECK(RedactStatement("missing PWD: set PWD={a;SECRET} in the DSN") == "missing PWD=<redacted>");
	CHECK(RedactConnect("missing PWD: set PWD={a;SECRET} in the DSN") == "missing PWD=<redacted>");
	CHECK(RedactStatement("aws_session_token: x PWD={a;SECRET}") == "aws_session_token=<redacted>");
	CHECK(RedactStatement("PWD: see aws_session_token='a;SECRET'") == "PWD=<redacted>");
	CHECK(RedactStatement("PWD=x aws_session_token='a;SECRET'") == "PWD=<redacted>");
	CHECK(RedactCutStatement("missing PWD: set PWD={a;SECRET} in the DSN") == "missing PWD=<redacted>...");
	// An enclosed earlier value, whose rest stops at the later value's `;`.
	CHECK(RedactStatement("PWD={a} aws_session_token='b;SECRET'") == "PWD=<redacted>");
	// A later quoted value that crosses a line end, where the earlier plain value stops.
	CHECK(RedactConnect("PWD: x aws_session_token='a\nSECRET' z") == "PWD=<redacted>\n");
}

TEST_CASE("A colon assignment's value ends at a semicolon or a line end", "[odbc]") {
	CHECK(RedactStatement("PWD: secret;UID=u") == "PWD=<redacted>;UID=u");
	CHECK(RedactConnect("PWD: secret\nnext line") == "PWD=<redacted>\nnext line");
	CHECK(RedactStatement("PWD: secret\rnext line") == "PWD=<redacted>\rnext line");
}

TEST_CASE("A colon assignment with nothing after its colon is left as it is", "[odbc]") {
	CHECK(RedactStatement("PWD:;UID=u") == "PWD:;UID=u");
	CHECK(RedactStatement("missing PWD:\nnext line") == "missing PWD:\nnext line");
	CHECK(RedactConnect("missing PWD:\r\nnext") == "missing PWD:\r\nnext");
}

TEST_CASE("Blanks alone are a value, after either separator", "[odbc]") {
	CHECK(RedactStatement("PWD: ;UID=u") == "PWD=<redacted>;UID=u");
	CHECK(RedactConnect("PWD= ;UID=u") == "PWD=<redacted>;UID=u");
}

TEST_CASE("A keyword followed by a cast's `::` is not an assignment", "[odbc]") {
	CHECK(RedactStatement("LINE 1: SELECT pwd::text, id FROM users WHERE id = 1") ==
	      "LINE 1: SELECT pwd::text, id FROM users WHERE id = 1");
	CHECK(RedactConnect("SELECT u.pwd::varchar FROM u") == "SELECT u.pwd::varchar FROM u");
	CHECK(RedactStatement("aws_session_token :: text") == "aws_session_token :: text");
	CHECK(RedactStatement("PWD: x") == "PWD=<redacted>");
	CHECK(RedactStatement("SELECT pwd::text FROM t; PWD: x") == "SELECT pwd::text FROM t; PWD=<redacted>");
	// Only after a colon: a value after `=` may start with colons.
	CHECK(RedactConnect("PWD=:secret;UID=u") == "PWD=<redacted>;UID=u");
	CHECK(RedactStatement("aws_session_token=::tok") == "aws_session_token=<redacted>");
	// Blanks between the colons make them a separator and a value.
	CHECK(RedactStatement("PWD: :secret") == "PWD=<redacted>");
}

TEST_CASE("No canary in a colon, quoted-key or escaped value reaches a diagnostic, in either phase, whole or cut",
          "[odbc]") {
	const std::vector<std::string> messages = {
	    "aws_secret_access_key: c4nary tail",
	    "aws_session_token:c4nary tail",
	    "PWD : c4nary tail",
	    "x aws_session_token:\t{c4;nary} tail",
	    "x Aws_Secret_Access_Key: 'c4;nary' tail",
	    "x pwd: \"c4;nary\" tail",
	    "PWD=a; aws_session_token: c4nary b",
	    "bad aws_secret_access_key: {c4nary;open wo",
	    "bad: aws_secret_access_key: c4nary; aws_session_token: c4nary tail",
	    "PWD: \t{a;c4nary} tail",
	    "missing PWD: set PWD={a;c4nary} in the DSN",
	    "aws_session_token: x PWD={a;c4nary} tail",
	    "PWD: see aws_session_token='a;c4nary' tail",
	    "PWD={a} aws_session_token='b;c4nary' tail",
	    "PWD: x aws_session_token='a\nc4nary' z",
	    "x {\"aws_session_token\": \"c4;nary\"} tail",
	    "x {'aws_secret_access_key':'c4nary'} tail",
	    "x \"pwd\"=c4nary tail",
	    "x 'PWD' : {c4;nary} tail",
	    "PWD='ab\\'';c4nary' tail",
	    "PWD=\"ab\\\"\";c4nary\" tail",
	    "PWD={ab\\}};c4nary} tail",
	    "aws_secret_access_key='ab\\'';c4nary' tail",
	    "x {\\\\\\\"pwd\\\\\\\": \\\\\\\"x;c4nary\\\\\\\"} tail",
	};
	for (const auto &message : messages) {
		INFO(message);
		CHECK(!Contains(RedactConnect(message), "c4"));
		CHECK(!Contains(RedactStatement(message), "c4"));
		CHECK(!Contains(RedactCutConnect(message), "c4"));
		CHECK(!Contains(RedactCutStatement(message), "c4"));
	}
}

TEST_CASE("A non-ASCII letter that Unicode case folding maps onto a keyword's letter is not that letter", "[odbc]") {
	// U+017F (long s) folds to `s`, and U+212A (Kelvin sign) to `k`, in UTF-8; matching is by byte.
	CHECK(RedactStatement("aws_\xC5\xBF"
	                      "ession_token=x") == "aws_\xC5\xBF"
	                                           "ession_token=x");
	CHECK(RedactConnect("aws_secret_access_\xE2\x84\xAA"
	                    "ey: x") == "aws_secret_access_\xE2\x84\xAA"
	                                "ey: x");
	CHECK(RedactStatement("SECRET_ACCESS_\xE2\x84\xAA"
	                      "EY 'x'") == "SECRET_ACCESS_\xE2\x84\xAA"
	                                   "EY 'x'");
}

TEST_CASE("A byte that is not UTF-8 inside a value is redacted with the rest of it", "[odbc]") {
	CHECK(RedactConnect("PWD=pa\xFFss;x") == "PWD=<redacted>;x");
	CHECK(RedactConnect("PWD={pa\xFFss;x}") == "PWD=<redacted>");
	// The first two bytes of a three-byte UTF-8 character, as a cut would leave them.
	CHECK(RedactConnect("PWD='pa\xE6\x97ss;x'") == "PWD=<redacted>");
	CHECK(RedactStatement("aws_secret_access_key=\"ab\xC0"
	                      "cd;x\"") == "aws_secret_access_key=<redacted>");
}

TEST_CASE("Every match of a credential form is redacted, not only the first", "[odbc]") {
	CHECK(RedactConnect("PWD=a;UID=u;PWD=b") == "PWD=<redacted>;UID=u;PWD=<redacted>");
	CHECK(RedactStatement("AKIAIOSFODNN7EXAMPLE and ASIAIOSFODNN7EXAMPLE") == "<redacted> and <redacted>");
	CHECK(RedactConnect("token " + std::string(40, 'A') + " and " + std::string(40, 'B')) ==
	      "token <redacted> and <redacted>");
}

TEST_CASE("An assignment's keyword and a parameter marker outrank a bare <redacted> over the same text", "[odbc]") {
	// The whole assignment is a run of token characters too, as `_` and `=` are among them.
	CHECK(RedactConnect("aws_secret_access_key=wJalrXUtnFEMI/K7MDENG/bPxRfiCYEXAMPLEKEY") ==
	      "aws_secret_access_key=<redacted>");
	CHECK(RedactStatement("bad AKIAIOSFODNN7EXAMPLE", {"AKIAIOSFODNN7EXAMPLE"}) == "bad <param 1>");
}

TEST_CASE("Text that only looks like a credential form is left as it is", "[odbc]") {
	CHECK(RedactConnect("AKIAIOSFODNN7EXAMPL") == "AKIAIOSFODNN7EXAMPL");
	CHECK(RedactStatement("XAKIAIOSFODNN7EXAMPLE") == "XAKIAIOSFODNN7EXAMPLE");
	CHECK(RedactStatement("AKIAIOSFODNN7EXAMPLEX") == "AKIAIOSFODNN7EXAMPLEX");
	CHECK(RedactStatement("akiaiosfodnn7example") == "akiaiosfodnn7example");
	CHECK(RedactStatement("aws_secret_access_key_hint=value") == "aws_secret_access_key_hint=value");
	CHECK(RedactStatement("my_aws_session_token=value") == "my_aws_session_token=value");
	CHECK(RedactStatement("MYPWD=value") == "MYPWD=value");
	CHECK(RedactStatement("PWD is not set") == "PWD is not set");
	CHECK(RedactConnect("PWD=;UID=u") == "PWD=;UID=u");
	CHECK(RedactStatement("aws_secret_access_key_hint: value") == "aws_secret_access_key_hint: value");
	CHECK(RedactStatement("my_aws_session_token: value") == "my_aws_session_token: value");
	CHECK(RedactStatement("MYPWD: value") == "MYPWD: value");
	CHECK(RedactStatement("PWD:") == "PWD:");
	CHECK(RedactStatement("\"my_pwd\": value") == "\"my_pwd\": value");
	CHECK(RedactStatement("\"pwd_hint\": value") == "\"pwd_hint\": value");
	CHECK(RedactStatement("\"pwd\" is required") == "\"pwd\" is required");
	// One quote may follow a keyword, escaped or doubled, but not two different quotes or three, and
	// nothing but blanks between it and the separator.
	CHECK(RedactStatement("\"pwd\"': value") == "\"pwd\"': value");
	CHECK(RedactStatement("'pwd'\": value") == "'pwd'\": value");
	CHECK(RedactStatement("\"pwd\"\"\": value") == "\"pwd\"\"\": value");
	// A key quote doubled twice, as a SQL literal inside another writes it, is a known limit.
	CHECK(RedactStatement("'{''''pwd'''': ''''x''''}'") == "'{''''pwd'''': ''''x''''}'");
	CHECK(RedactStatement("'pwd') = value") == "'pwd') = value");
	// One byte that is not a quote, and a quote after the blanks, are both near-misses.
	CHECK(RedactStatement("WHERE (pwd) = 'x'") == "WHERE (pwd) = 'x'");
	CHECK(RedactStatement("pwd]: value") == "pwd]: value");
	CHECK(RedactStatement("pwd \": value") == "pwd \": value");
	// Only `=` or `:` separates a value, so other punctuation after a keyword leaves the text alone.
	CHECK(RedactStatement("column \"pwd\" does not exist") == "column \"pwd\" does not exist");
	CHECK(RedactConnect("column \"pwd\" does not exist") == "column \"pwd\" does not exist");
	CHECK(RedactStatement("PWD, UID and DSN are required") == "PWD, UID and DSN are required");
	CHECK(RedactConnect("PWD, UID and DSN are required") == "PWD, UID and DSN are required");
	CHECK(RedactStatement("missing PWD.") == "missing PWD.");
	CHECK(RedactConnect("missing PWD.") == "missing PWD.");
	// Only spaces or tabs may come between a keyword and its separator, not a line end.
	CHECK(RedactStatement("missing PWD\n: next") == "missing PWD\n: next");
	CHECK(RedactConnect("PWD\r\n=x") == "PWD\r\n=x");
}

TEST_CASE("A refused keyword costs only its own bytes, so a credential after it is still redacted", "[odbc]") {
	CHECK(RedactStatement("bad myPWD=x PWD=hunter22") == "bad myPWD=x PWD=<redacted>");
	CHECK(RedactConnect("bad myPWD=x PWD=hunter22") == "bad myPWD=x PWD=<redacted>");
	CHECK(SanitizeServerMessage(RedactStatement("bad myPWD=x PWD=hunter22")) == "bad myPWD=x PWD=<redacted>");
}

TEST_CASE("The text on either side of a redacted value is kept", "[odbc]") {
	CHECK(RedactConnect("left;PWD=secret;UID=user;right") == "left;PWD=<redacted>;UID=user;right");
	// Without a quote after the keyword, the byte before it is kept, whatever it is.
	CHECK(RedactStatement("set PWD = x") == "set PWD=<redacted>");
	CHECK(RedactConnect("a=PWD=b") == "a=PWD=<redacted>");
}

TEST_CASE("Only a connect diagnostic redacts a run of 40 or more token characters", "[odbc]") {
	const std::string token = "Ab0+/=_-" + std::string(32, 'x');
	CHECK(RedactConnect("token " + token + " end") == "token <redacted> end");
	const std::string identifier(60, 'a');
	CHECK(RedactStatement("column " + identifier) == "column " + identifier);
}

TEST_CASE("A run of 39 token characters is not redacted", "[odbc]") {
	const std::string shorter(39, 'A');
	CHECK(RedactConnect("token " + shorter) == "token " + shorter);
}

TEST_CASE("A statement diagnostic replaces a bound value of 8 bytes or more with its marker", "[odbc]") {
	CHECK(RedactStatement("invalid input syntax for integer: \"12345678\"", {"short", "12345678"}) ==
	      "invalid input syntax for integer: \"<param 2>\"");
	CHECK(RedactStatement("a 12345678 b 12345678", {"12345678"}) == "a <param 1> b <param 1>");
	CHECK(RedactStatement("1234567812345678", {"12345678"}) == "<param 1><param 1>");
	CHECK(RedactStatement("v: \"abcabcabcabc\"", {"abcabcabc"}) == "v: \"<param 1>\"");
}

TEST_CASE("A bound value shorter than 8 bytes stays readable", "[odbc]") {
	CHECK(RedactStatement("value 1234567 and x", {"1234567", "x"}) == "value 1234567 and x");
}

TEST_CASE("A bound value's length is counted in bytes, not characters", "[odbc]") {
	// Three characters, nine bytes.
	CHECK(RedactStatement("value \xE6\x97\xA5\xE6\x9C\xAC\xE8\xAA\x9E here",
	                      {"\xE6\x97\xA5\xE6\x9C\xAC\xE8\xAA\x9E"}) == "value <param 1> here");
}

TEST_CASE("Where two bound values match at one place, the longer wins, then the one bound first", "[odbc]") {
	CHECK(RedactStatement("value abcdefghij!", {"abcdefgh", "abcdefghij"}) == "value <param 2>!");
	CHECK(RedactStatement("value 12345678!", {"12345678", "12345678"}) == "value <param 1>!");
	CHECK(RedactCutStatement("value 1234 5", {"1234 5678", "1234 5678"}) == "value <param 1>...");
}

TEST_CASE("Many equal bound values cost redaction no more than as many values the message lacks", "[odbc]") {
	// Each copy of a value matches at every place the first does, so a copy that is not skipped
	// multiplies the spans; the fastest of five runs keeps other work on the host out of the ratio.
	constexpr double MAX_RATIO = 40;
	constexpr int VALUES = 32767;
	const std::string message(64, '0');
	const duckdb::vector<duckdb::string> equal(VALUES, std::string(8, '0'));
	duckdb::vector<duckdb::string> absent;
	for (int i = 0; i < VALUES; i++) {
		absent.push_back("x" + std::to_string(10000000 + i));
	}
	CHECK(RedactStatement(message, equal) == "<param 1>");
	auto fastest_equal = RedactionCpuTime(message, equal);
	auto fastest_absent = RedactionCpuTime(message, absent);
	for (int run = 1; run < 5; run++) {
		fastest_equal = std::min(fastest_equal, RedactionCpuTime(message, equal));
		fastest_absent = std::min(fastest_absent, RedactionCpuTime(message, absent));
	}
	INFO("thread CPU time: equal values " << fastest_equal << ", absent ones " << fastest_absent);
	CHECK(fastest_equal < MAX_RATIO * fastest_absent);
}

TEST_CASE("A bound value that carries a credential form becomes its parameter marker", "[odbc]") {
	CHECK(RedactStatement("rejected PWD=hunter22; here", {"PWD=hunter22"}) == "rejected <param 1>; here");
}

TEST_CASE("The end of a cut message that starts a bound value of 8 bytes or more becomes its marker", "[odbc]") {
	CHECK(RedactCutStatement("invalid: \"abc defgh", {"short", "abc defghijk"}) == "invalid: \"<param 2>...");
	CHECK(RedactCutStatement("value: 1234 5", {"1234 567"}) == "value: <param 1>...");
}

TEST_CASE("A cut message's end that is not the start of a bound value of 8 bytes or more stays", "[odbc]") {
	CHECK(RedactCutStatement("value: ab cde", {"ab cdef"}) == "value: ab...");
	CHECK(RedactCutStatement("value: bc defgh", {"abc defgh"}) == "value: bc...");
	CHECK(RedactCutStatement("abcd efgh then more", {"abcd efgh12"}) == "abcd efgh then...");
}

TEST_CASE("A cut message's end inside its last word is not taken as a bound value", "[odbc]") {
	CHECK(RedactCutStatement("a b c", {"credential"}) == "a b...");
	CHECK(RedactCutStatement("a b secretv", {"secretval1"}) == "a b...");
}

TEST_CASE("A whole message's end stays, even where it starts a bound value", "[odbc]") {
	CHECK(RedactStatement("invalid: \"abc defgh", {"abc defghijk"}) == "invalid: \"abc defgh");
	CHECK(RedactStatement("column \"x\" does not exist", {"testvalue1"}) == "column \"x\" does not exist");
}

TEST_CASE("The longest end that starts a bound value is replaced, not a shorter one", "[odbc]") {
	CHECK(RedactCutStatement("x ab ab ab", {"ab ab ab ab xyz"}) == "x <param 1>...");
}

TEST_CASE("An end that is a whole bound value and the start of another takes the whole one's marker", "[odbc]") {
	CHECK(RedactCutStatement("x a 12345678", {"a 12345678XY", "a 12345678"}) == "x <param 2>...");
}

TEST_CASE("A cut message loses the word it ends in to the cut mark", "[odbc]") {
	CHECK(RedactCutConnect("first second thi") == "first second...");
	CHECK(RedactCutConnect("key AKIAIOSF") == "key...");
	CHECK(RedactCutStatement("first\tsecond") == "first...");
	CHECK(RedactCutStatement("first second\nthi") == "first second...");
	CHECK(RedactCutStatement("first second\rthi") == "first second...");
	CHECK(RedactCutStatement("nowhitespace") == "...");
}

TEST_CASE("A whole message keeps the word it ends in", "[odbc]") {
	CHECK(RedactConnect("first second thi") == "first second thi");
	CHECK(RedactStatement("first second thi") == "first second thi");
}

TEST_CASE("A redacted form that reaches a cut message's last word keeps its replacement", "[odbc]") {
	CHECK(RedactCutStatement("bad PWD=hunter2 more") == "bad PWD=<redacted>...");
	CHECK(RedactCutStatement("a b secret val", {"secret value1"}) == "a b <param 1>...");
	CHECK(RedactCutConnect("token " + std::string(40, 'Z')) == "token <redacted>...");
	CHECK(RedactCutConnect("token key:" + std::string(40, 'Z')) == "token <redacted>...");
}

TEST_CASE("A braced or quoted value that a cut left open is redacted to the end", "[odbc]") {
	CHECK(RedactCutConnect("bad PWD={pa;ss wo") == "bad PWD=<redacted>...");
	CHECK(RedactCutConnect("bad PWD='pa;ss wo") == "bad PWD=<redacted>...");
	CHECK(RedactCutConnect("bad PWD=\"pa;ss wo") == "bad PWD=<redacted>...");
	CHECK(RedactCutStatement("bad aws_session_token={pa;ss wo") == "bad aws_session_token=<redacted>...");
}

TEST_CASE("A connect diagnostic has no bound-value rule", "[odbc]") {
	CHECK(RedactConnect("value 12345678") == "value 12345678");
}

TEST_CASE("Redaction avoids the restart-scan blow-up near the maximum diagnostic size", "[odbc]") {
	// A regression guard against a scan that restarts inside each match, which is quadratic here,
	// not a test of growth order; the ratios were measured separately. Thread CPU time, sizes timed
	// in turn and the fastest of five runs keep other work on the host out of the ratio.
	constexpr double MAX_RATIO = 40;
	for (const std::string unit : {"PWD=a ", "pwd:", "PWD:{", "PWD: h://;", "pwd\\\\\\\":"}) {
		INFO(unit);
		const auto short_text = Repeated(unit, 4096);
		// 32,760 bytes, which each unit divides, within the 32,766 message bytes the reader returns.
		const auto long_text = Repeated(unit, 32760);
		REQUIRE(long_text.size() == 32760);
		CHECK(RedactStatement(long_text) == unit.substr(0, 3) + "=<redacted>");
		auto fastest_small = RedactionCpuTime(short_text);
		auto fastest_large = RedactionCpuTime(long_text);
		for (int run = 1; run < 5; run++) {
			fastest_small = std::min(fastest_small, RedactionCpuTime(short_text));
			fastest_large = std::min(fastest_large, RedactionCpuTime(long_text));
		}
		INFO("thread CPU time: 32,760 bytes " << fastest_large << ", 4 KiB " << fastest_small);
		CHECK(fastest_large < MAX_RATIO * fastest_small);
	}
}

TEST_CASE("Redaction and the sanitizer give the same answer on several threads at once", "[odbc]") {
	const std::string message = "PWD=secret;AKIAIOSFODNN7EXAMPLE;" + std::string(48, 'B');
	std::vector<std::string> results(8);
	std::vector<std::thread> workers;
	for (size_t i = 0; i < results.size(); i++) {
		workers.emplace_back([&results, &message, i]() {
			for (int run = 0; run < 20; run++) {
				results[i] = RedactConnect(message) + " " + SanitizeServerMessage("pid 1 at a.ec2.internal");
			}
		});
	}
	for (auto &worker : workers) {
		worker.join();
	}
	for (const auto &result : results) {
		CHECK(result == "PWD=<redacted>;<redacted>;<redacted> pid <pid> at <host>");
	}
}

TEST_CASE("A server message with nothing to remove passes the sanitizer unchanged", "[odbc]") {
	// The first line of the syntax error measured on driver 2.3.0.0.
	const std::string measured =
	    "[Redshift][ODBC Driver][Server]42601:ERROR:  syntax error at or near \"FROM\" in context \"SELECT FROM\", "
	    "at line 1";
	CHECK(SanitizeServerMessage(measured) == measured);
	// Look-alikes of the address rules: times, casts, a date, groups joined to a word, a bare `::`, a
	// three-part version, and dotted numbers longer than an address.
	const std::string address_look_alikes = "time 12:34:56.5, at:12:34:56, cast 1::int, x::date, 2024-01-02, "
	                                        "id1:2:3:4, a :: b, version 2.3.0, 1234.1.1.1, 1.1.1.1234, 1.2.3.4.5";
	CHECK(SanitizeServerMessage(address_look_alikes) == address_look_alikes);
	// A `::` cast to a type with no digit in its name, from a value of digits or of letters only.
	CHECK(SanitizeServerMessage("cast 1::dec, a::dec, '1'::dec(10,2)") == "cast 1::dec, a::dec, '1'::dec(10,2)");
	// An array slice in brackets, with a number after it.
	CHECK(SanitizeServerMessage("slice a[1:2]:5 and a[1:2]") == "slice a[1:2]:5 and a[1:2]");
	// Seven groups after a hex word longer than a group, or before one, and a time after a lone colon.
	CHECK(SanitizeServerMessage("deadbeef:1:2:3:4:5:6:7") == "deadbeef:1:2:3:4:5:6:7");
	CHECK(SanitizeServerMessage("at deadbeef:1:2:3:4:5:6:7") == "at deadbeef:1:2:3:4:5:6:7");
	CHECK(SanitizeServerMessage("1:2:3:4:5:6:7:deadbeef") == "1:2:3:4:5:6:7:deadbeef");
	CHECK(SanitizeServerMessage(":12:34:56") == ":12:34:56");
	// Look-alikes of the host, EC2 name, URL and pid rules.
	const std::string other_look_alikes = "x_y.z, 1.5e3, v1.2, end. Next, note: //x, a:/b, 10://x, the process "
	                                      "ended, rapid 5, pid 12ab, ship-10-0-0-1";
	CHECK(SanitizeServerMessage(other_look_alikes) == other_look_alikes);
}

TEST_CASE("The sanitizer keeps the first line of a message only", "[odbc]") {
	CHECK(SanitizeServerMessage("ERROR:  primary\nDETAIL:  at relay-1 10.0.0.1") == "ERROR:  primary");
	CHECK(SanitizeServerMessage("ERROR:  primary\r\nHINT:  more") == "ERROR:  primary");
}

TEST_CASE("A dotted name with an infrastructure suffix or a port becomes a host marker", "[odbc]") {
	CHECK(SanitizeServerMessage("at relay-1.us-east-1.example.amazonaws.com:5439 now") == "at <host> now");
	CHECK(SanitizeServerMessage("via ip-10-0-0-1.ec2.internal") == "via <host>");
	CHECK(SanitizeServerMessage("cluster.abc123.us-east-1.redshift.amazonaws.com and relay.corp.internal") ==
	      "<host> and <host>");
	// Built from two pieces.
	CHECK(SanitizeServerMessage("at relay-1.example.amazon" + std::string(".com") + " now") == "at <host> now");
	CHECK(SanitizeServerMessage("node ip-10-0-0-1.compute.us-west-2:5439 down") == "node <host> down");
	CHECK(SanitizeServerMessage("db.example.org:5439 refused") == "<host> refused");
	CHECK(SanitizeServerMessage("db.example.org.:5439 refused") == "<host> refused");
	CHECK(SanitizeServerMessage("host1.cluster2:5439 refused") == "<host> refused");
	CHECK(SanitizeServerMessage("at relay-1.us-east-1:5439 and node-1.abc123:5439") == "at <host> and <host>");
	CHECK(SanitizeServerMessage("via ip-10-0-0-1.ec2.internal:5439 and ip-10-0-0-2.compute.us-west-2 now") ==
	      "via <host> and <host> now");
	CHECK(SanitizeServerMessage("via IP-10-0-0-1.EC2.INTERNAL") == "via <host>");
	CHECK(SanitizeServerMessage("db.example.org:543999 refused") == "<host>9 refused");
}

TEST_CASE("A single label with a port becomes a host marker", "[odbc]") {
	CHECK(SanitizeServerMessage("at leader:5439 and relay-1:5439 now") == "at <host> and <host> now");
	CHECK(SanitizeServerMessage("leader:5439 refused, via LEADER:5439") == "<host> refused, via <host>");
	// A colon with nothing beyond it, as libpq frames a host and its port, joins no longer run.
	CHECK(SanitizeServerMessage("connect to leader:5439: refused") == "connect to <host>: refused");
	CHECK(SanitizeServerMessage("at :leader:5439 now") == "at :<host> now");
	// A word and a number joined by a colon read as one too.
	CHECK(SanitizeServerMessage("at T10:30 and a:1") == "at <host> and <host>");
	// A control character inside the label does not split it.
	CHECK(SanitizeServerMessage("at lea\x01"
	                            "der:5439 now") == "at <host> now");
	// A `_` before the label, or a root `.` after it, leaves it a host.
	CHECK(SanitizeServerMessage("at _leader:5439 and leader.:5439 now") == "at <host> and <host> now");
}

TEST_CASE("A label of digits, a label in a run of colons and a dotted number with a port stay", "[odbc]") {
	const std::string look_alikes =
	    "at 12:34, 2024-01-01T10:30, x:node:5, node:5:6, 1.5:30, 1.2.3:4 and 1.2.3.4.5:80, "
	    "arn:aws:redshift:us-west-2:123456789012:dbuser:c/u, error:14094410:SSL, x:us-west-2:5";
	CHECK(SanitizeServerMessage(look_alikes) == look_alikes);
	CHECK(SanitizeServerMessage("x\x01 " + look_alikes) == "x  " + look_alikes);
	// A `-` before the label, or a word right after the port, joins it to a longer run, with or without a root `.`.
	CHECK(SanitizeServerMessage("at -leader:5439 now") == "at -leader:5439 now");
	CHECK(SanitizeServerMessage("at leader:5439x now") == "at leader:5439x now");
	CHECK(SanitizeServerMessage("at -leader.:5439 and leader.:5439x now") == "at -leader.:5439 and leader.:5439x now");
	// A root `.` or `_` around one does not make it a host.
	CHECK(SanitizeServerMessage("at 1.5.:30, _1.5:30 and __:5") == "at 1.5.:30, _1.5:30 and __:5");
	// An IPv4 address standing alone still goes with its port, and a pid word keeps its pid marker.
	CHECK(SanitizeServerMessage("at 10.0.0.1:5439 now") == "at <address> now");
	CHECK(SanitizeServerMessage("pid:1234 and backend_pid:5") == "pid <pid> and backend_pid <pid>");
}

TEST_CASE("An IPv4 address that ends a longer dotted number, or a host that starts in its port, goes", "[odbc]") {
	CHECK(SanitizeServerMessage("at 1.2.3.4.5-255.255.255.255:5439 now") == "at 1.2.3.4.5-<address> now");
	CHECK(SanitizeServerMessage("at 1.2.3.4.5.6.7.8.9:5.leader:5439 now") == "at 1.2.3.4.5.6.7.8.9:<host> now");
}

TEST_CASE("A colon or word that a character left out parts from a label joins it to no longer run", "[odbc]") {
	const auto shy = Utf8(0x00AD);
	CHECK(SanitizeServerMessage("x:" + shy + "node:5 ok") == "x:<host> ok");
	CHECK(SanitizeServerMessage("at node:5" + shy + ":6 ok") == "at <host>:6 ok");
	// The joined line reads the label with the word before it, as it reads a dotted host.
	CHECK(SanitizeServerMessage("key" + shy + "leader:5439") == "<host>");
}

TEST_CASE("An EC2 short name, an AWS endpoint and a name with `_` in a label become host markers", "[odbc]") {
	CHECK(SanitizeServerMessage("at ip-10-0-0-1, IP-10-0-0-3 and ip-10-0-0-2:5439 now") ==
	      "at <host>, <host> and <host> now");
	CHECK(SanitizeServerMessage("via fn.lambda-url.us-east-1.on.aws and "
	                            "wg.123456789012.us-east-1.redshift-serverless.api.aws") == "via <host> and <host>");
	CHECK(SanitizeServerMessage("at cluster.cn-north-1.redshift.amazonaws.com.cn now") == "at <host> now");
	CHECK(SanitizeServerMessage("at my_host.corp.example:5439 now") == "at <host> now");
}

TEST_CASE("An EC2 short name glued to `_`, or after `-`, becomes a host marker", "[odbc]") {
	CHECK(SanitizeServerMessage("node_ip-10-0-0-1 down") == "node_<host> down");
	CHECK(SanitizeServerMessage("ip-10-0-0-1: refused") == "<host>: refused");
	CHECK(SanitizeServerMessage("relay-ip-10-0-0-1 and ip-10-0-0-2_x down") == "relay-<host> and <host>_x down");
}

TEST_CASE("A dotted name with no infrastructure suffix and no port passes the sanitizer unchanged", "[odbc]") {
	CHECK(SanitizeServerMessage("relation \"public.users\" does not exist") ==
	      "relation \"public.users\" does not exist");
	// `internal` after one label only is a table's name.
	CHECK(SanitizeServerMessage("relation \"sales.internal\" does not exist") ==
	      "relation \"sales.internal\" does not exist");
	// Look-alikes of the suffixes: two that run on into a longer word, and a domain that is none of them.
	const std::string suffix_look_alikes = "db.sales.internal_notes, my.amazonaws.community, docs.example.com";
	CHECK(SanitizeServerMessage(suffix_look_alikes) == suffix_look_alikes);
}

TEST_CASE("An IPv4 address, with or without a port, becomes an address marker", "[odbc]") {
	CHECK(SanitizeServerMessage("from 10.1.2.3 and 192.168.0.1:5439.") == "from <address> and <address>.");
	// A four-part version has an address's shape, so it goes too.
	CHECK(SanitizeServerMessage("driver 2.3.0.0") == "driver <address>");
	CHECK(SanitizeServerMessage("10.0.0.1 refused") == "<address> refused");
	CHECK(SanitizeServerMessage(".10.0.0.1 refused") == ".<address> refused");
	// A six-digit port is no port of the address's own; the host rule takes five of its digits.
	CHECK(SanitizeServerMessage("at 10.0.0.1:543999") == "at <address>9");
	// The port goes too after the name's last `_` or before a root `.`.
	CHECK(SanitizeServerMessage("at _10.0.0.1:5439, 1.2._10.0.0.2:5439 and 10.0.0.3.:5439 now") ==
	      "at <address>, <address> and <address> now");
	// After a host that ends at the `.` before it, too.
	CHECK(SanitizeServerMessage("at a:1.2._10.0.0.1:5439") == "at <host>.<address>");
	// A known limit: with `_` or `-` between an address and its port, the port stays.
	CHECK(SanitizeServerMessage("at 10.0.0.1_:5439 and 10.0.0.1-1:5439 now") ==
	      "at <address>_:5439 and <address>-1:5439 now");
}

TEST_CASE("An address or URL glued to a word or to `_` becomes a marker", "[odbc]") {
	CHECK(SanitizeServerMessage("see foo_https://relay.corp.example/x?token=abc") == "see foo_<url>");
	CHECK(SanitizeServerMessage("node_10.0.0.1 down, x10.0.0.1 down") == "node_<address> down, x<address> down");
	CHECK(SanitizeServerMessage("addr_fe80::1 down, id_1:2:3:4") == "addr_<address> down, id_<address>");
}

TEST_CASE("An IPv6 address, full or compressed, becomes an address marker", "[odbc]") {
	CHECK(SanitizeServerMessage("peer 2001:db8:0:0:0:0:0:1 here") == "peer <address> here");
	CHECK(SanitizeServerMessage("peer fe80::1%eth0, ::1 and ::ffff:10.0.0.1.") ==
	      "peer <address>, <address> and <address>.");
	CHECK(SanitizeServerMessage("v 1:2:3:4") == "v <address>");
	CHECK(SanitizeServerMessage("::1 refused by fe80::2") == "<address> refused by <address>");
	CHECK(SanitizeServerMessage("peer 2001:DB8::1 here") == "peer <address> here");
	CHECK(SanitizeServerMessage("peer 2001:db8::abcd here") == "peer <address> here");
	CHECK(SanitizeServerMessage("to fe80::1%eth0:5439 now") == "to <address> now");
}

TEST_CASE("A compressed IPv6 address whose group before `::` mixes digits with letters becomes an address marker",
          "[odbc]") {
	CHECK(SanitizeServerMessage("peer fd00:: and [fd00::] down") == "peer <address> and [<address>] down");
	CHECK(SanitizeServerMessage("peer fd00::beef, ff02::fb and fe80::abcd") ==
	      "peer <address>, <address> and <address>");
}

TEST_CASE("A full eight-group IPv6 address with a port becomes one address marker", "[odbc]") {
	CHECK(SanitizeServerMessage("to 2600:1f18:61e2:8a00:7c3d:4e5f:6a7b:8c9d:5439 failed") == "to <address> failed");
	CHECK(SanitizeServerMessage("peer 2001:db8:0:0:0:0:0:1%eth0 here") == "peer <address> here");
	// A number after the colon too long for a port goes with it.
	CHECK(SanitizeServerMessage("at 2600:1f18:61e2:8a00:7c3d:4e5f:6a7b:8c9d:543999 now") == "at <address> now");
}

TEST_CASE("A port of five digits or more after a compressed IPv6 address goes with its marker", "[odbc]") {
	CHECK(SanitizeServerMessage("to fe80::1:54321 refused") == "to <address> refused");
	CHECK(SanitizeServerMessage("to 2600:1f18::5:65535 refused") == "to <address> refused");
	CHECK(SanitizeServerMessage("to fd00::a:12345 refused") == "to <address> refused");
	CHECK(SanitizeServerMessage("to cafe:fe80::1:54321 refused") == "to <address> refused");
	CHECK(SanitizeServerMessage("::1:55555") == "<address>");
	CHECK(SanitizeServerMessage("2001:DB8::FF00:42:8329:55555") == "<address>");
	CHECK(SanitizeServerMessage("fd00:::55555") == "<address>");
	// A port of four digits reads as a group, and one after a zone or a bracket goes with the address.
	CHECK(SanitizeServerMessage("to fe80::1:5439, fe80::1%eth0:54321 and [fe80::1]:54321") ==
	      "to <address>, <address> and <address>");
}

TEST_CASE("An IPv6 address in brackets with a port becomes one address marker", "[odbc]") {
	CHECK(SanitizeServerMessage("to [fe80::1]:54321 refused") == "to <address> refused");
	CHECK(SanitizeServerMessage("to [fe80::1]:5439 refused") == "to <address> refused");
	CHECK(SanitizeServerMessage("to [fe80::1%eth0]:54321 refused") == "to <address> refused");
	CHECK(SanitizeServerMessage("to [::ffff:10.0.0.1]:5439 refused") == "to <address> refused");
	// The bracket parts the address from a word before it.
	CHECK(SanitizeServerMessage("at host[fe80::1]:54321") == "at host<address>");
	// In brackets with a port, an address of letters alone is one too, but not a time or a pair.
	CHECK(SanitizeServerMessage("to [face::beef]:5439 and [a:b:c:d]:5 refused") ==
	      "to <address> and <address> refused");
	CHECK(SanitizeServerMessage("at [ab:cd]:5, [12:34:56]:7 and [:a:b:c]:5") ==
	      "at [ab:cd]:5, [12:34:56]:7 and [:a:b:c]:5");
	// A dot after the `[` goes inside the marker, even before a run that alone stays.
	CHECK(SanitizeServerMessage("to [.fe80::1]:5439, [.10.0.0.1::1]:5439 and [.:a:b:c]:5") ==
	      "to <address>, <address> and <address>");
}

TEST_CASE("A zone name with `-` or `.` in it stays inside the address marker", "[odbc]") {
	CHECK(SanitizeServerMessage("at fe80::1%en-0 and fe80::1%eth0.100 down") == "at <address> and <address> down");
	CHECK(SanitizeServerMessage("at 2001:db8:0:0:0:0:0:1%en-0:5439 down") == "at <address> down");
	// A `.` that ends the sentence is not part of the zone.
	CHECK(SanitizeServerMessage("at fe80::1%eth0.") == "at <address>.");
}

TEST_CASE("A colon after an IPv6 address, with or without its port, stays after the address marker", "[odbc]") {
	CHECK(SanitizeServerMessage("connect to 2600:1f18:61e2:8a00:7c3d:4e5f:6a7b:8c9d:5439: refused") ==
	      "connect to <address>: refused");
	CHECK(SanitizeServerMessage("connect to 2600:1f18:61e2:8a00:7c3d:4e5f:6a7b:8c9d: refused") ==
	      "connect to <address>: refused");
	CHECK(SanitizeServerMessage("to fe80::1: refused, to ff02::fb:x") == "to <address>: refused, to <address>:x");
	CHECK(SanitizeServerMessage("at 2600:1f18:61e2:8a00:7c3d:4e5f:6a7b:8c9d::x") == "at <address>::x");
}

TEST_CASE("Eight colon-joined groups or more become one address marker", "[odbc]") {
	CHECK(SanitizeServerMessage(
	          "peers 2600:1f18:61e2:8a00:7c3d:4e5f:6a7b:8c9d:2600:1f18:61e2:8a00:7c3d:4e5f:6a7b:8c9d down") ==
	      "peers <address> down");
	// Colon-joined numbers have the same shape, so they go too.
	CHECK(SanitizeServerMessage("1:2:3:4:5:6:7:8:9 opens the line") == "<address> opens the line");
	CHECK(SanitizeServerMessage("1:2:3:4:5:6:7:8:9:10:11") == "<address>");
}

TEST_CASE("An IPv6 address after a word and a colon becomes an address marker", "[odbc]") {
	CHECK(SanitizeServerMessage("peer ip:2001:db8::1 refused") == "peer ip:<address> refused");
	CHECK(SanitizeServerMessage("IPv6:fe80::1 or server:fe80::2") == "IPv6:<address> or server:<address>");
	CHECK(SanitizeServerMessage("addr:fe80::1") == "addr:<address>");
	CHECK(SanitizeServerMessage("peer :fe80::1") == "peer :<address>");
	// A word of hex digits joins only through a colon.
	CHECK(SanitizeServerMessage("bad fe80::1") == "bad <address>");
	// The word runs on past `_`, so a hex part longer than a group does not end it.
	CHECK(SanitizeServerMessage("peer my_cafe1:fe80::1") == "peer my_cafe1:<address>");
}

TEST_CASE("A full IPv6 address after a hex word and a colon becomes an address marker", "[odbc]") {
	CHECK(SanitizeServerMessage("x :2600:1f18:61e2:8a00:7c3d:4e5f:6a7b:8c9d") == "x :<address>");
	CHECK(SanitizeServerMessage("x:deadbeef:2600:1f18:61e2:8a00:7c3d:4e5f:6a7b:8c9d failed") ==
	      "x:deadbeef:<address> failed");
}

TEST_CASE("A word of one to four hex digits and a colon before a full IPv6 address join its marker", "[odbc]") {
	CHECK(SanitizeServerMessage("db:2600:1f18:61e2:8a00:7c3d:4e5f:6a7b:8c9d failed") == "<address> failed");
	CHECK(SanitizeServerMessage("cafe:2001:db8:0:0:0:0:0:1:5439 failed") == "<address> failed");
	CHECK(SanitizeServerMessage(
	          "x:db:2600:1f18:61e2:8a00:7c3d:4e5f:6a7b:8c9d and host :db:2600:1f18:61e2:8a00:7c3d:4e5f:6a7b:8c9d") ==
	      "x:<address> and host :<address>");
}

TEST_CASE("A process ID after pid, backend or process becomes a pid marker", "[odbc]") {
	CHECK(SanitizeServerMessage("pid 1234, PID=99, process id 77 and backend PID 5") ==
	      "pid <pid>, PID <pid>, process <pid> and backend <pid>");
	CHECK(SanitizeServerMessage("the process ended, backend is busy") == "the process ended, backend is busy");
	CHECK(SanitizeServerMessage("PID: 1234 and process #77") == "PID <pid> and process <pid>");
	CHECK(SanitizeServerMessage("could not send signal to process 1234: No such process") ==
	      "could not send signal to process <pid>: No such process");
	CHECK(SanitizeServerMessage("process=4, process(4, process-4 and Process PID 5") ==
	      "process <pid>, process <pid>, process <pid> and Process <pid>");
	// A word after the process ID does not make it a count.
	CHECK(SanitizeServerMessage("Process 7224 waits for ShareLock") == "Process <pid> waits for ShareLock");
	CHECK(SanitizeServerMessage("process 1234 terminated") == "process <pid> terminated");
	CHECK(SanitizeServerMessage("sent a signal to process 1234 because") == "sent a signal to process <pid> because");
	CHECK(SanitizeServerMessage("process 1234 (worker) ended") == "process <pid> (worker) ended");
	// Before a colon too, in any case, where the host rule reads no pid word as a host.
	CHECK(SanitizeServerMessage("PID:1234 and Backend:5") == "PID <pid> and Backend <pid>");
}

TEST_CASE("A count after could not process or failed to process and before a word stays", "[odbc]") {
	const std::string counts = "could not process 3 rows and failed to Process 12 files";
	CHECK(SanitizeServerMessage(counts) == counts);
	CHECK(SanitizeServerMessage("Could  Not  process  3  rows, Failed  To  process  4  files") ==
	      "Could  Not  process  3  rows, Failed  To  process  4  files");
	CHECK(SanitizeServerMessage("pid 5 could not process 3 rows") == "pid <pid> could not process 3 rows");
	// A count's word may open the next phrase.
	CHECK(SanitizeServerMessage("could not process 3 failed to process 4 rows") ==
	      "could not process 3 failed to process 4 rows");
}

TEST_CASE("A number after process is a process ID unless could not or failed to, spaces and a letter make it a count",
          "[odbc]") {
	// Only spaces between the words and around the number, and a letter after it, make it a count.
	CHECK(SanitizeServerMessage("could not process: 3 rows") == "could not process <pid> rows");
	CHECK(SanitizeServerMessage("could not process3 rows") == "could not process <pid> rows");
	CHECK(SanitizeServerMessage("could not process 3\trows") == "could not process <pid> rows");
	CHECK(SanitizeServerMessage("could not\tprocess 3 rows") == "could not process <pid> rows");
	// A known limit: so does a tab between a word and `could` or `failed`, which the joined line glues together.
	CHECK(SanitizeServerMessage("x\tcould not process 3 rows, y\tfailed to process 4 files") ==
	      "x could not process <pid> rows, y failed to process <pid> files");
	CHECK(SanitizeServerMessage("could not process 3 (rows)") == "could not process <pid> (rows)");
	// Only `could not` or `failed to` makes `process` a verb, and only as words of their own.
	CHECK(SanitizeServerMessage("cannot process 3 rows, to process 4 rows") ==
	      "cannot process <pid> rows, to process <pid> rows");
	CHECK(SanitizeServerMessage("xcould not process 3 rows, couldnot process 4 rows, failedto process 5 rows") ==
	      "xcould not process <pid> rows, couldnot process <pid> rows, failedto process <pid> rows");
}

TEST_CASE("A process ID after a word that ends in pid, backend or process_id becomes a pid marker", "[odbc]") {
	CHECK(SanitizeServerMessage("backend_pid=1234, pg_backend_pid() 4321 and process_id=77") ==
	      "backend_pid <pid>, pg_backend_pid <pid> and process_id <pid>");
	// Before a colon too, where the host rule reads no pid word as a host.
	CHECK(SanitizeServerMessage("pg_backend_pid:5") == "pg_backend_pid <pid>");
}

TEST_CASE("A process ID after an opening parenthesis or a hyphen becomes a pid marker", "[odbc]") {
	CHECK(SanitizeServerMessage("pg_terminate_backend(1234), pid(99) and PID-77") ==
	      "pg_terminate_backend <pid>), pid <pid>) and PID <pid>");
}

TEST_CASE("An address inside a pid or a URL takes the marker of its larger form", "[odbc]") {
	CHECK(SanitizeServerMessage("pid 10.0.0.1") == "<address>");
	CHECK(SanitizeServerMessage("at \"https://10.0.0.1/y\" now") == "at \"<url>\" now");
}

TEST_CASE("A URL becomes a url marker", "[odbc]") {
	CHECK(SanitizeServerMessage("endpoint https://relay.internal:8443/v1/creds?x=1 failed") == "endpoint <url> failed");
	CHECK(SanitizeServerMessage("see s3://bucket/key") == "see <url>");
	CHECK(SanitizeServerMessage("see HTTPS://x.example/a") == "see <url>");
}

TEST_CASE("Markup characters from the server are replaced", "[odbc]") {
	CHECK(SanitizeServerMessage("bad <script>alert(1)</script> &amp; <b>") ==
	      "bad (script)alert(1)(/script) +amp; (b)");
}

TEST_CASE("The markers redaction writes pass the sanitizer unchanged", "[odbc]") {
	CHECK(SanitizeServerMessage("value <param 2> and PWD=<redacted>; and <redacted>") ==
	      "value <param 2> and PWD=<redacted>; and <redacted>");
	CHECK(SanitizeServerMessage("bad PWD=<redacted>") == "bad PWD=<redacted>");
	CHECK(SanitizeServerMessage("<param x> <redacted") == "(param x) (redacted");
	CHECK(SanitizeServerMessage(RedactCutStatement("bad aws_session_token={pa;ss wo")) ==
	      "bad aws_session_token=<redacted>...");
}

TEST_CASE("Control and escape characters in a server message become spaces", "[odbc]") {
	CHECK(SanitizeServerMessage(std::string("a\x1b[31mb\x07"
	                                        "c\x7f"
	                                        "d\t\xC2\x9B"
	                                        "e\0f",
	                                        17)) == "a [31mb c d  e f");
	// The first and last C1 controls, and the last C0 one.
	CHECK(SanitizeServerMessage("a\xC2\x80"
	                            "b\xC2\x9F"
	                            "c\x1F"
	                            "d") == "a b c d");
	// A character that shares a byte with a C1 control is kept, and so are those next to DEL and to
	// C1 and the no-break space, and a four-byte character.
	CHECK(SanitizeServerMessage("caf\xC3\xA9 \xC2\xA9 \xE6\x97\xA5 ~ \xC2\xA1 \xF0\x9F\x98\x80") ==
	      "caf\xC3\xA9 \xC2\xA9 \xE6\x97\xA5 ~ \xC2\xA1 \xF0\x9F\x98\x80");
}

TEST_CASE("Every control, line-separator, space and private-use character, U+FFFC and U+FFFD, becomes one space",
          "[odbc]") {
	// Spaces, separators, then the private-use ends and U+FFFC and U+FFFD, which a font may draw as
	// nothing or as a box.
	std::vector<int32_t> spaced {0x1680, 0x2028,  0x2029,  0x202F,   0x205F,   0x3000, 0xE000,
	                             0xF8FF, 0xF0000, 0xFFFFD, 0x100000, 0x10FFFD, 0xFFFC, 0xFFFD};
	for (int32_t codepoint = 0; codepoint < 0x20; codepoint++) {
		// A line feed or carriage return ends the first line instead.
		if (codepoint != '\n' && codepoint != '\r') {
			spaced.push_back(codepoint);
		}
	}
	for (const auto &range : std::vector<std::pair<int32_t, int32_t>> {{0x7F, 0xA0}, {0x2000, 0x200A}}) {
		for (auto codepoint = range.first; codepoint <= range.second; codepoint++) {
			spaced.push_back(codepoint);
		}
	}
	for (const auto codepoint : spaced) {
		INFO("U+" << std::hex << codepoint);
		CHECK(SanitizeServerMessage("a" + Utf8(codepoint) + "b") == "a b");
	}
}

TEST_CASE("Every format or default-ignorable character, noncharacter and braille blank is left out", "[odbc]") {
	// Format characters (bidirectional controls among them), then default-ignorable ones that are not
	// format characters (the combining grapheme joiner, a variation selector and the Hangul fillers),
	// format characters that are not default-ignorable (U+0600, U+FFF9), unassigned ones Unicode
	// reserves as default-ignorable, noncharacters and U+2800.
	std::vector<int32_t> dropped {0x00AD,  0x061C, 0x180E, 0x200B, 0x200C, 0x200D, 0x200E,  0x200F,
	                              0x2060,  0x2061, 0x2062, 0x2063, 0x206A, 0xFEFF, 0xE0001, 0x034F,
	                              0xFE0F,  0x115F, 0x1160, 0x3164, 0xFFA0, 0x0600, 0xFFF9,  0x2065,
	                              0xE0000, 0x2800, 0xFDD0, 0xFDEF, 0xFFFE, 0xFFFF, 0x1FFFF, 0x10FFFE};
	for (const auto &range : std::vector<std::pair<int32_t, int32_t>> {{0x202A, 0x202E},
	                                                                   {0x2066, 0x2069},
	                                                                   {0xFFF0, 0xFFF8},
	                                                                   {0xE0002, 0xE001F},
	                                                                   {0xE0080, 0xE00FF},
	                                                                   {0xE01F0, 0xE0FFF}}) {
		for (auto codepoint = range.first; codepoint <= range.second; codepoint++) {
			dropped.push_back(codepoint);
		}
	}
	for (const auto codepoint : dropped) {
		INFO("U+" << std::hex << codepoint);
		CHECK(SanitizeServerMessage("a" + Utf8(codepoint) + "b") == "ab");
	}
}

TEST_CASE("A character newer than utf8proc's Unicode version, or one still unassigned, stays", "[odbc]") {
	// Letters of Unicode 16 (Todhri, Garay) and 17 (Sidetic, Beria Erfe), which DuckDB's utf8proc
	// predates; U+3FFFD, U+FFEF and U+E1000, unassigned, just outside the reserved ranges and
	// noncharacters; and U+FDCF and U+FDF0, assigned, beside the noncharacters.
	for (const auto codepoint : {0x105C0, 0x10D50, 0x10940, 0x16EA0, 0x3FFFD, 0xFFEF, 0xE1000, 0xFDCF, 0xFDF0}) {
		INFO("U+" << std::hex << codepoint);
		CHECK(SanitizeServerMessage("a" + Utf8(codepoint) + "b") == "a" + Utf8(codepoint) + "b");
	}
}

TEST_CASE("A control, bidirectional or space character inside a form does not hide it", "[odbc]") {
	CHECK(SanitizeServerMessage("pid\x01"
	                            "1234") == "pid <pid>");
	CHECK(SanitizeServerMessage("pid\xE2\x80\xAE"
	                            "1234") == "pid <pid>");
	CHECK(SanitizeServerMessage("id\x01"
	                            "1:2:3:4") == "id <address>");
	CHECK(SanitizeServerMessage("pid\xC2\xA0"
	                            "1234") == "pid <pid>");
}

TEST_CASE("A control character, a character left out or a bad byte inside a form does not split it", "[odbc]") {
	CHECK(SanitizeServerMessage("10.0.\xE2\x80\x8E"
	                            "0.1 down") == "<address> down");
	CHECK(SanitizeServerMessage("at 10.0.\x01"
	                            "0.1 and host-1.corp\x01.example:5439") == "at <address> and <host>");
	CHECK(SanitizeServerMessage("at 10.0.\xFF"
	                            "0.1 and https://x.example\x01/caf\xC3\xA9 now") == "at <address> and <url> now");
	CHECK(SanitizeServerMessage("at 10.0.\xE2\x80\x8B"
	                            "0.1 and https:/\xE2\x80\x8B/relay.example/x?t=abc") == "at <address> and <url>");
}

TEST_CASE("A format or default-ignorable character inside a form does not split it", "[odbc]") {
	for (const auto codepoint : std::vector<int32_t> {0x200C, 0x200D, 0x2062, 0x2063, 0x034F, 0xFE0F, 0xE0001}) {
		INFO("U+" << std::hex << codepoint);
		CHECK(SanitizeServerMessage(RedactStatement("key AKIA" + Utf8(codepoint) + "IOSFODNN7EXAMPLE now")) ==
		      "key <redacted> now");
	}
	for (const auto codepoint : std::vector<int32_t> {0x200C, 0x200D, 0x2062, 0x034F, 0xE0001, 0x115F, 0xFFA0}) {
		INFO("U+" << std::hex << codepoint);
		CHECK(SanitizeServerMessage("at 10.0.0" + Utf8(codepoint) + ".1 down") == "at <address> down");
	}
	const auto zwnj = Utf8(0x200C);
	CHECK(SanitizeServerMessage("at ip" + zwnj + "-10-0-0-1") == "at <host>");
	CHECK(SanitizeServerMessage(RedactStatement("bad PWD" + zwnj + "=hunter22")) == "bad PWD=<redacted>");
	CHECK(SanitizeServerMessage("via http" + zwnj + "s://relay.example/v1") == "via <url>");
	CHECK(SanitizeServerMessage("at fe80::" + zwnj + "1:54321") == "at <address>");
}

TEST_CASE("A Hangul filler or the braille blank inside a pid does not hide it", "[odbc]") {
	CHECK(SanitizeServerMessage("pid" + Utf8(0x3164) + "4242") == "pid <pid>");
	CHECK(SanitizeServerMessage("pid" + Utf8(0x2800) + "4242") == "pid <pid>");
}

TEST_CASE("U+1D159, which a font may draw as nothing, is left out, so it cannot split a form", "[odbc]") {
	const auto notehead = Utf8(0x1D159);
	CHECK(SanitizeServerMessage("a" + notehead + "b") == "ab");
	CHECK(SanitizeServerMessage("pid" + notehead + "4242") == "pid <pid>");
	CHECK(SanitizeServerMessage(RedactStatement("key AKIA" + notehead + "IOSFODNN7EXAMPLE now")) ==
	      "key <redacted> now");
	CHECK(SanitizeServerMessage("at 172" + notehead + ".31.45.67 ok") == "at <address> ok");
}

TEST_CASE("A credential that a character left out rejoins is redacted", "[odbc]") {
	CHECK(SanitizeServerMessage("bad PW\xE2\x80\xAE"
	                            "D=hunter22; next") == "bad PWD=<redacted>; next");
	CHECK(SanitizeServerMessage("key AKIA\xE2\x80\xAE"
	                            "IOSFODNN7EXAMPLE now") == "key <redacted> now");
	CHECK(SanitizeServerMessage("key AKIA\x01"
	                            "IOSFODNN7EXAMPLE now") == "key <redacted> now");
}

TEST_CASE("A colon or quoted-key credential that a character left out rejoins is redacted", "[odbc]") {
	const std::string rlo = "\xE2\x80\xAE";
	CHECK(SanitizeServerMessage(RedactStatement("bad PW" + rlo + "D: hunter22; next")) == "bad PWD=<redacted>; next");
	CHECK(SanitizeServerMessage(RedactStatement("bad {\"aws_session" + rlo + "_token\": \"tok;en\"} next")) ==
	      "bad {aws_session_token=<redacted>");
	// The form redaction wrote for a colon assignment passes the sanitizer as it is.
	CHECK(SanitizeServerMessage(RedactStatement("aws_session_token: x")) == "aws_session_token=<redacted>");
}

TEST_CASE("A credential that a character left out rejoins reads as it would whole, beside the forms around it",
          "[odbc]") {
	// Each message, with U+200B after its first part and without it, and what both show: redaction's
	// marker, not an address, URL or host marker, and the text beside the credential as it was.
	const std::vector<std::vector<std::string>> cases = {
	    {"x P", "WD=10.0.0.1 y", "x PWD=<redacted>"},
	    {"\xE2\x80\x8Cx P", "WD=10.0.0.1 y", "x PWD=<redacted>"},
	    // A mark that opens the line belongs to no character before it.
	    {"\xCC\x81x P", "WD=10.0.0.1 y", "\xCC\x81x PWD=<redacted>"},
	    {"x P", "WD=https://h.example.org/a y", "x PWD=<redacted>"},
	    {"x P", "WD=a;10.0.0.1 z", "x PWD=<redacted>;<address> z"},
	    {"a.b.P", "WD:5439 y", "a.b.PWD=<redacted>"},
	    {"x 10.0.0.1:P", "WD=x y", "x <address>:PWD=<redacted>"},
	    {"db.example.amazonaws.com.P", "WD=x y", "<host>.PWD=<redacted>"},
	    {"x AK", "IAIOSFODNN7EXAMPLE.amazonaws.com y", "x <redacted>.amazonaws.com y"},
	    {"key AK", "IAIOSFODNN7EXAMPLE\tnext", "key <redacted> next"},
	    // A mark, or a letter newer than utf8proc's data, at the value's end goes with it.
	    {"x P", "WD=abc\xCC\x81", "x PWD=<redacted>"},
	    {"x P", "WD=abc\xF0\x90\x97\x80;rest", "x PWD=<redacted>;rest"},
	    // So does a mark after a character the line leaves out.
	    {"x P", "WD=abc\xE2\x80\x8B\xCC\x81;rest", "x PWD=<redacted>;rest"},
	    // A literal after a URL's tail keeps its marker beside the URL's.
	    {"see https://h.example.org/PASS", "WORD 'x' y", "see <url> '<redacted>' y"},
	    // A URL that runs on through the marker still takes it in.
	    {"see https://h.example.org/a?P", "WD=x y", "see <url>"}};
	for (const auto &entry : cases) {
		INFO(entry[0] + entry[1]);
		CHECK(SanitizeServerMessage(RedactStatement(entry[0] + "\xE2\x80\x8B" + entry[1])) == entry[2]);
		CHECK(SanitizeServerMessage(RedactStatement(entry[0] + entry[1])) == entry[2]);
	}
}

TEST_CASE("A character shown as a space at a split credential's end still shows as one", "[odbc]") {
	// The line whole drops it with the value; split, the sanitizer rejoins the credential but keeps
	// the space it showed for the character.
	for (const std::string shown_as_space : {"\t", "\x01", "\xC2\xA0", "\xEE\x80\x80", "\xEF\xBF\xBD", "\xFF"}) {
		INFO(shown_as_space);
		CHECK(SanitizeServerMessage(RedactStatement("x P\xE2\x80\x8BWD=abc" + shown_as_space + ";rest")) ==
		      "x PWD=<redacted> ;rest");
		CHECK(SanitizeServerMessage(RedactStatement("x PWD=abc" + shown_as_space + ";rest")) ==
		      "x PWD=<redacted>;rest");
	}
}

TEST_CASE("A split colon credential whose value opens with a colon or slashes after a character left out shows, an "
          "accepted residual",
          "[odbc]") {
	// The joined line reads the colon and the value as a cast or a URL, not as a credential.
	const std::string split = "x P\xE2\x80\x8BWD:";
	// Each character between the colons, and what the message shows in its place.
	const std::vector<std::pair<std::string, std::string>> betweens {
	    {"\t", " "},          {"\xC2\xA0", " "},    {"\x01", " "},   {"\xEF\xBF\xBD", " "},
	    {"\xE2\x80\x8B", ""}, {"\xEF\xBB\xBF", ""}, {"\xC2\xAD", ""}};
	for (const auto &between : betweens) {
		INFO(between.first);
		CHECK(SanitizeServerMessage(RedactStatement(split + between.first + ":abc PLACEHOLDER_SECRET;rest")) ==
		      "x PWD:" + between.second + ":abc PLACEHOLDER_SECRET;rest");
		CHECK(SanitizeServerMessage(RedactStatement("x PWD:" + between.first + ":abc PLACEHOLDER_SECRET;rest")) ==
		      "x PWD=<redacted>;rest");
	}
	CHECK(SanitizeServerMessage(RedactStatement(split + "\xCC\x81:abc PLACEHOLDER_SECRET;rest")) ==
	      "x PWD:\xCC\x81:abc PLACEHOLDER_SECRET;rest");
	CHECK(SanitizeServerMessage(RedactStatement(split + "\t//abc PLACEHOLDER_SECRET;rest")) ==
	      "x <url> PLACEHOLDER_SECRET;rest");
}

TEST_CASE("A URL that holds a redacted value becomes one url marker", "[odbc]") {
	CHECK(SanitizeServerMessage(RedactStatement("failed for https://u.example.org/a?pwd:SECRET;tail/b rest")) ==
	      "failed for <url> rest");
	CHECK(SanitizeServerMessage(RedactStatement("at jdbc:redshift://h.example.org:5439/dev;pwd=x;ssl=true ok")) ==
	      "at jdbc:<url> ok");
	// A bound value inside a URL becomes its parameter marker, and the URL runs on through that too.
	CHECK(SanitizeServerMessage(RedactStatement("failed for https://u.example.org/a?q=SECRETVALUE/tail/b rest",
	                                            {"SECRETVALUE"})) == "failed for <url> rest");
	CHECK(SanitizeServerMessage("at https://u.example.org/<param 12>/tail ok") == "at <url> ok");
}

TEST_CASE("A private-use character, U+FFFC or U+FFFD inside a form does not split it", "[odbc]") {
	CHECK(SanitizeServerMessage(RedactStatement("at AKIA" + Utf8(0xFFFC) + "IOSFODNN7EXAMPLE ok")) ==
	      "at <redacted> ok");
	CHECK(SanitizeServerMessage(RedactStatement("AKIA" + Utf8(0xE000) + "IOSFODNN7EXAMPLE")) == "<redacted>");
	CHECK(SanitizeServerMessage(RedactStatement("at PWD" + Utf8(0xE000) + "=hunter22 ok")) == "at PWD=<redacted>");
	CHECK(SanitizeServerMessage(RedactStatement("at aws_secret_access_key" + Utf8(0xFFFD) + "=wJalrXUtnFEMI ok")) ==
	      "at aws_secret_access_key=<redacted>");
	CHECK(SanitizeServerMessage("at 10.0" + Utf8(0xFFFC) + ".0.1 ok") == "at <address> ok");
}

TEST_CASE("A control or other space beside a credential parts it from the word it touches", "[odbc]") {
	const std::string rlo = "\xE2\x80\xAE";
	for (const std::string space : {"\x01", "\xC2\xA0"}) {
		INFO("space of " << space.size() << " bytes");
		CHECK(SanitizeServerMessage(RedactStatement("key" + space + "AKIA" + rlo + "IOSFODNN7EXAMPLE")) ==
		      "key <redacted>");
		CHECK(SanitizeServerMessage(RedactStatement("key AKIA" + rlo + "IOSFODNN7EXAMPLE" + space + "x")) ==
		      "key <redacted> x");
		CHECK(SanitizeServerMessage(RedactStatement("x" + space + "PWD" + rlo + "=hunter22")) == "x PWD=<redacted>");
		CHECK(SanitizeServerMessage(RedactStatement("x" + space + "PASSWORD" + rlo + " 'placeholder'")) ==
		      "x PASSWORD '<redacted>'");
		// The same control both beside the key and inside it.
		CHECK(SanitizeServerMessage(RedactStatement("key" + space + "AKIA" + space + "IOSFODNN7EXAMPLE")) ==
		      "key <redacted>");
	}
	// A character left out beside it parts it too, as it does in redaction's own reading.
	const std::string zwsp = "\xE2\x80\x8B";
	CHECK(SanitizeServerMessage(RedactStatement("key" + zwsp + "AKIA" + zwsp + "IOSFODNN7EXAMPLE")) == "key<redacted>");
}

TEST_CASE("A credential that touches a word with nothing between stays, as in redaction", "[odbc]") {
	const std::string rlo = "\xE2\x80\xAE";
	CHECK(SanitizeServerMessage("keyAKIA" + rlo + "IOSFODNN7EXAMPLE") == "keyAKIAIOSFODNN7EXAMPLE");
	CHECK(SanitizeServerMessage("AKIA" + rlo + "IOSFODNN7EXAMPLEx") == "AKIAIOSFODNN7EXAMPLEx");
	CHECK(SanitizeServerMessage("myPWD" + rlo + "=value") == "myPWD=value");
}

TEST_CASE("A control or other space beside a host, address or pid parts it from the word it touches", "[odbc]") {
	// One such character inside the form, which the joined line rejoins, and one beside it, which
	// the joined line leaves out but which still parts the form from its neighbour.
	const std::string nbsp = "\xC2\xA0";
	for (const std::string &space : std::vector<std::string> {"\x01", nbsp}) {
		INFO("space of " << space.size() << " bytes");
		CHECK(SanitizeServerMessage("host x" + space + "ip-10-0" + space + "-0-1 x") == "host x <host> x");
		CHECK(SanitizeServerMessage("host 7" + space + "ip" + space + "-10-0-0-1 x") == "host 7 <host> x");
		CHECK(SanitizeServerMessage("at 1234" + space + "10.0" + space + ".0.1 x") == "at 1234 <address> x");
		CHECK(SanitizeServerMessage("at 1." + space + "10.0" + space + ".0.1 x") == "at 1. <address> x");
		CHECK(SanitizeServerMessage("at 10.0" + space + ".0.123" + space + "5 x") == "at <address> 5 x");
		CHECK(SanitizeServerMessage("at 10.0" + space + ".0.1" + space + ".5 x") == "at <address> .5 x");
		CHECK(SanitizeServerMessage("x" + space + "pi" + space + "d 4242") == "x pid <pid>");
		CHECK(SanitizeServerMessage("pi" + space + "d 4242" + space + "x") == "pid <pid> x");
		CHECK(SanitizeServerMessage("at a.b.us-east-1.redshift.amazon" + space + "aws.com" + space + "w") ==
		      "at <host> w");
		CHECK(SanitizeServerMessage("at a.b.int" + space + "ernal" + space + "w") == "at <host> w");
	}
	CHECK(SanitizeServerMessage("at 1234\xE3\x80\x80"
	                            "10.0\xE2\x80\xA8.0.1 x") == "at 1234 <address> x");
	CHECK(SanitizeServerMessage("key" + nbsp + "2001:db8:85a3:0:0:8a2e:" + std::string(1, '\0') + "370:7334" + nbsp +
	                            "w") == "key <address> w");
	CHECK(SanitizeServerMessage("key" + nbsp + "fe80::1%eth0" + std::string(1, '\0') + ":54321" + nbsp + "w") ==
	      "key <address> w");
	// A colon that the line parts from the word before it or from the run after it joins neither. A
	// character left out inside the word does not make it a word of hex digits.
	const auto shy = Utf8(0x00AD);
	CHECK(SanitizeServerMessage("at deadbeef" + shy + ":1:2:3:4 ok") == "at deadbeef:<address> ok");
	CHECK(SanitizeServerMessage("at deadbeef:" + shy + "1:2:3:4 ok") == "at deadbeef:<address> ok");
	CHECK(SanitizeServerMessage("at g" + shy + "beefcafe:1:2:3:4 ok") == "at gbeefcafe:<address> ok");
}

TEST_CASE("A character left out beside a host, address or pid parts it from the word it touches", "[odbc]") {
	for (const auto codepoint : std::vector<int32_t> {0x00AD, 0x200B, 0x2060, 0xFE0F, 0x3164}) {
		INFO("U+" << std::hex << codepoint);
		const auto dropped = Utf8(codepoint);
		CHECK(SanitizeServerMessage("key" + dropped + "ip-10-0-0-1 ok") == "key<host> ok");
		CHECK(SanitizeServerMessage("key" + dropped + "fe80::1%eth0:54321 ok") == "key<address> ok");
		CHECK(SanitizeServerMessage("at db-1.abc.us-east-1.redshift.amazonaws.com" + dropped + "w") == "at <host>w");
		CHECK(SanitizeServerMessage("at relay.corp.internal" + dropped + "w") == "at <host>w");
		CHECK(SanitizeServerMessage("at 2001:db8:85a3:0:0:8a2e:370:7334" + dropped + "w") == "at <address>w");
		CHECK(SanitizeServerMessage("at 1234" + dropped + "10.0.0.1 ok") == "at 1234<address> ok");
		CHECK(SanitizeServerMessage("at 10.0.0.123" + dropped + "5 ok") == "at <address>5 ok");
		CHECK(SanitizeServerMessage("key" + dropped + "pid 4242 ok") == "keypid <pid> ok");
		CHECK(SanitizeServerMessage("pid 4242" + dropped + "x ok") == "pid <pid>x ok");
	}
	// Each host suffix, the second built from two pieces.
	const auto shy = Utf8(0x00AD);
	for (const auto &suffix :
	     std::vector<std::string> {"amazonaws.com", "amazon" + std::string(".com"), "amazonaws.com.cn", "on.aws",
	                               "api.aws", "compute.us-west-2", "corp.internal"}) {
		INFO("suffix " << suffix);
		CHECK(SanitizeServerMessage("at x.y." + suffix + shy + "w") == "at <host>w");
	}
	// A word longer than a label, and a name with a port.
	const std::string long_word(70, 'a');
	CHECK(SanitizeServerMessage(long_word + shy + "db.amazonaws.com ok") == long_word + "<host> ok");
	CHECK(SanitizeServerMessage(long_word + shy + "db.example.org:5439 ok") == long_word + "<host> ok");
	CHECK(SanitizeServerMessage("at db.example.org:5439" + shy + "w") == "at <host>w");
	// Upper case, and a host that holds a pid, whose marker it outranks.
	CHECK(SanitizeServerMessage("at X.Y.AMAZONAWS.COM" + shy + "w") == "at <host>w");
	CHECK(SanitizeServerMessage("at process-1.amazonaws.com" + shy + "w") == "at <host>w");
	// The line as shown reads its edges from the line too: only it holds `pid id`, which the joined
	// line runs together, here after a control and after a bad byte.
	CHECK(SanitizeServerMessage("at\x01x" + shy + "pid\x01id 5") == "at xpid <pid>");
	CHECK(SanitizeServerMessage("at\xFFx" + shy + "pid\x01id 5") == "at xpid <pid>");
}

TEST_CASE("A host, address or pid that touches a word in the line itself stays", "[odbc]") {
	// The look-alikes that the rules leave, with a control elsewhere in the line.
	const std::string look_alikes = "x\x01 ship-10-0-0-1, 1.2.3.4.5, 1234.1.1.1, 1.1.1.1234, rapid 5, pid 12ab, "
	                                "my.amazonaws.community, db.sales.internal_notes, id1:2:3:4, deadbeef:1:2:3";
	CHECK(SanitizeServerMessage(look_alikes) == "x  ship-10-0-0-1, 1.2.3.4.5, 1234.1.1.1, 1.1.1.1234, rapid 5, "
	                                            "pid 12ab, my.amazonaws.community, db.sales.internal_notes, "
	                                            "id1:2:3:4, deadbeef:1:2:3");
	// A `_` before pid stands for the longer word it ends.
	CHECK(SanitizeServerMessage("_pid 5 and my_backend_pid 6") == "_pid <pid> and my_backend_pid <pid>");
}

TEST_CASE("An assignment whose value is white space alone is redacted", "[odbc]") {
	CHECK(SanitizeServerMessage("ERROR:  PWD\x01= ") == "ERROR:  PWD=<redacted>");
	CHECK(SanitizeServerMessage("ERROR:  PWD\xE2\x80\xAE= \nsecond line") == "ERROR:  PWD=<redacted>");
}

TEST_CASE("A marker that only starts an assignment's value does not keep the rest of it shown", "[odbc]") {
	const std::string rlo = "\xE2\x80\xAE";
	const std::string zwsp = "\xE2\x80\x8B";
	CHECK(SanitizeServerMessage(RedactStatement("ERROR:  PWD\x01=AKIAIOSFODNN7EXAMPLE-rest-of-pw")) ==
	      "ERROR:  PWD=<redacted>");
	CHECK(SanitizeServerMessage(RedactStatement("PWD" + rlo + "=AKIAIOSFODNN7EXAMPLE hunter22")) == "PWD=<redacted>");
	CHECK(SanitizeServerMessage(
	          RedactStatement("aws_secret_access_key" + zwsp + "=ASIAIOSFODNN7EXAMPLE/wJalrXUtnFEMI")) ==
	      "aws_secret_access_key=<redacted>");
	CHECK(SanitizeServerMessage(RedactStatement("PWD" + rlo + "=<redacted>hunter22")) == "PWD=<redacted>");
	CHECK(SanitizeServerMessage(RedactStatement("PWD" + rlo + "=<redacted>...hunter22")) == "PWD=<redacted>");
}

TEST_CASE("A marker that ends an assignment's value is left as it is", "[odbc]") {
	// The control the user sees as a space shows whether the assignment was redacted again.
	CHECK(SanitizeServerMessage("bad P\x01WD=<redacted>;hunter22") == "bad P WD=<redacted>;hunter22");
	CHECK(SanitizeServerMessage("bad P\x01WD=<redacted>") == "bad P WD=<redacted>");
	CHECK(SanitizeServerMessage("bad P\x01WD=<redacted>...") == "bad P WD=<redacted>...");
	CHECK(SanitizeServerMessage("bad P\x01WD: <redacted>;hunter22") == "bad P WD: <redacted>;hunter22");
}

TEST_CASE("A combining mark stays shown but cannot split a form", "[odbc]") {
	// An acute accent (Mn), an enclosing circle (Me) and a Devanagari visarga (Mc).
	for (const auto codepoint : std::vector<int32_t> {0x0301, 0x20DD, 0x0903}) {
		INFO("U+" << std::hex << codepoint);
		const auto mark = Utf8(codepoint);
		CHECK(SanitizeServerMessage(RedactStatement("key AKIA" + mark + "IOSFODNN7EXAMPLE now")) ==
		      "key <redacted> now");
		CHECK(SanitizeServerMessage(RedactStatement("bad PW" + mark + "D=hunter22")) == "bad PWD=<redacted>");
		CHECK(SanitizeServerMessage("at 10.0" + mark + ".0.1 down") == "at <address> down");
		CHECK(SanitizeServerMessage("cafe" + mark + " ok") == "cafe" + mark + " ok");
	}
}

TEST_CASE("A code point newer than utf8proc's data stays shown but cannot split a form", "[odbc]") {
	// Two marks of Unicode 16 (Mn), which utf8proc reads as unassigned, and a letter.
	for (const auto codepoint : std::vector<int32_t> {0x0897, 0x10D69, 0x105C0}) {
		INFO("U+" << std::hex << codepoint);
		const auto newer = Utf8(codepoint);
		CHECK(SanitizeServerMessage(RedactStatement("key AKIA" + newer + "IOSFODNN7EXAMPLE now")) ==
		      "key <redacted> now");
		CHECK(SanitizeServerMessage(RedactStatement("bad PW" + newer + "D=hunter22")) == "bad PWD=<redacted>");
		CHECK(SanitizeServerMessage("at 10.0" + newer + ".0.1 down") == "at <address> down");
		CHECK(SanitizeServerMessage("cafe" + newer + " ok") == "cafe" + newer + " ok");
	}
}

TEST_CASE("A 100 KB line of refused forms is sanitized in under two seconds", "[odbc]") {
	// Were a refused match to send the search back over the rest of the line, these would take time
	// quadratic in their length, over a minute for the first. Two seconds leaves room for a slow build.
	const size_t bytes = 100 * 1024;
	// Each line, and the unit a user sees it repeat, cut at 256 bytes.
	const std::vector<std::pair<std::string, std::string>> lines {
	    {Repeated("1:", bytes) + "1x", "1:"},
	    {Repeated("1.", bytes) + "1:5", "1."},
	    {Repeated("1._", bytes) + "1.:5", "1._"},
	    {Repeated("P\x01WD=<redacted>;", bytes), "P WD=<redacted>;"},
	    {Repeated("xPWD=", bytes), "xPWD="},
	    {Repeated("xAKIA", bytes), "xAKIA"},
	    {Repeated("a.", bytes) + "amazonaws.comx", "a."},
	    {Repeated("a.", bytes) + "internalx", "a."},
	    {Repeated("xpid 1", bytes), "xpid 1"}};
	for (const auto &line : lines) {
		INFO("line of " << line.second);
		const auto started = std::chrono::steady_clock::now();
		const auto sanitized = SanitizeServerMessage(RedactStatement(line.first));
		const auto took = std::chrono::steady_clock::now() - started;
		CHECK(sanitized == Repeated(line.second, 256).substr(0, 256) + "...");
		CHECK(took < std::chrono::seconds(2));
	}
	// A word longer than a label before a host: each match that starts inside the word, up to a label's
	// length from its end, is refused on its left before the host after the word is found.
	const auto long_word = std::string(100, 'a');
	const auto started = std::chrono::steady_clock::now();
	const auto sanitized = SanitizeServerMessage(long_word + Repeated(".a", bytes) + ".amazonaws.com");
	const auto took = std::chrono::steady_clock::now() - started;
	CHECK(sanitized == long_word + ".<host>");
	CHECK(took < std::chrono::seconds(2));
}

TEST_CASE("A server message loses its bidirectional controls, and its line separators become spaces", "[odbc]") {
	// U+061C, U+200E and U+200F, the ends of U+202A to U+202E and of U+2066 to U+2069, and U+2028
	// and U+2029.
	CHECK(SanitizeServerMessage("a\xD8\x9C"
	                            "b\xE2\x80\x8E"
	                            "c\xE2\x80\x8F"
	                            "d\xE2\x80\xAA"
	                            "e\xE2\x80\xAE"
	                            "f\xE2\x81\xA6"
	                            "g\xE2\x81\xA9"
	                            "h\xE2\x80\xA8"
	                            "i\xE2\x80\xA9"
	                            "j") == "abcdefgh i j");
	// Neighbours that show something are kept: U+061B, U+061D, U+2010, U+2027, U+2030 and U+2070,
	// U+00AE, the combining U+034E, the Hangul letters U+115E, U+1161, U+3163 and U+FFA1, U+2801,
	// U+F900 after the private-use area, and U+FFEE, a symbol (So) as U+FFFC is.
	const std::string neighbours = "\xD8\x9B \xD8\x9D \xE2\x80\x90 \xE2\x80\xA7 \xE2\x80\xB0 \xE2\x81\xB0 \xC2\xAE "
	                               "\xCD\x8E \xE1\x85\x9E \xE1\x85\xA1 \xE3\x85\xA3 \xEF\xBE\xA1 \xE2\xA0\x81 "
	                               "\xEF\xA4\x80 \xEF\xBF\xAE";
	CHECK(SanitizeServerMessage(neighbours) == neighbours);
}

TEST_CASE("A byte that is no part of a UTF-8 character becomes a space", "[odbc]") {
	// A raw 0x9B is the C1 control U+009B written as one byte rather than as UTF-8's C2 9B.
	CHECK(SanitizeServerMessage("a\x9B"
	                            "31mb") == "a 31mb");
	// A lead byte with no continuation goes alone, so the address after it is still found.
	CHECK(SanitizeServerMessage("a\xC2x b") == "a x b");
	CHECK(SanitizeServerMessage("at \xC2"
	                            "10.0.0.1") == "at  <address>");
}

TEST_CASE("A server message over 256 bytes is cut, with a mark", "[odbc]") {
	CHECK(SanitizeServerMessage(std::string(256, 'a')) == std::string(256, 'a'));
	CHECK(SanitizeServerMessage(std::string(300, 'a')) == std::string(256, 'a') + "...");
}

TEST_CASE("A cut never splits a UTF-8 sequence or a marker", "[odbc]") {
	// 255 bytes, then a two-byte character that would end at byte 257.
	CHECK(SanitizeServerMessage(std::string(255, 'a') + "\xC3\xA9 tail") == std::string(255, 'a') + "...");
	// A marker that would end past byte 256 is left out whole; one that ends at byte 256 is kept.
	CHECK(SanitizeServerMessage(std::string(250, 'a') + " pid 12345") == std::string(250, 'a') + " ...");
	// Nothing after a marker left out is kept, though it would fit.
	CHECK(SanitizeServerMessage(std::string(250, 'a') + " pid 12345 b") == std::string(250, 'a') + " ...");
	CHECK(SanitizeServerMessage(std::string(246, 'a') + " pid 1 b") == std::string(246, 'a') + " pid <pid>...");
}

TEST_CASE("Redaction says where a byte of the message lands in its result", "[odbc]") {
	const auto from = [](const std::string &message, MessageExtent extent, duckdb::idx_t at) {
		const auto redacted = RedactStatementDiagnostic(message, {}, extent, at);
		return redacted.text.substr(redacted.from);
	};
	CHECK(from("ab PWD=x; cd", MessageExtent::WHOLE, 0) == "ab PWD=<redacted>; cd");
	CHECK(from("ab PWD=x; cd", MessageExtent::WHOLE, 10) == "cd");
	// The byte just past a replacement lands just past it, not at its start.
	CHECK(from("ab PWD=x; cd", MessageExtent::WHOLE, 8) == "; cd");
	// A byte a replacement takes in lands at the replacement's start.
	CHECK(from("ab PWD=x; cd", MessageExtent::WHOLE, 7) == "PWD=<redacted>; cd");
	CHECK(from("ab PWD=x; cd", MessageExtent::WHOLE, 12).empty());
	CHECK(from("ab PWD=x; cd", MessageExtent::WHOLE, 99).empty());
	// In a cut message, a byte of the word it loses lands at the `...`.
	CHECK(from("ab cd efgh", MessageExtent::CUT, 8) == "...");
	const auto connect = RedactConnectDiagnostic("ab PWD=x; cd", MessageExtent::WHOLE, 10);
	CHECK(connect.text.substr(connect.from) == "cd");
}

TEST_CASE("A driver AccessKeyID value is redacted, whatever it looks like", "[odbc]") {
	CHECK(RedactConnect("AccessKeyID={not-an-akia-id};UID=u") == "AccessKeyID=<redacted>;UID=u");
	CHECK(RedactStatement("accesskeyid = custom-key-1") == "accesskeyid=<redacted>");
	CHECK(RedactStatement("\"AccessKeyID\": \"custom-key-1\"") == "AccessKeyID=<redacted>");
	CHECK(RedactStatement("MyAccessKeyID=value") == "MyAccessKeyID=value");
	CHECK(RedactStatement("AccessKeyID without an assignment") == "AccessKeyID without an assignment");
	const auto all = RedactConnect("AccessKeyID=k1 SecretAccessKey=s1 SessionToken=t1");
	for (auto canary : {"k1", "s1", "t1"}) {
		CHECK(all.find(canary) == std::string::npos);
	}
}
