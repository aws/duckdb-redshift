// Statement and transport failures and informational log lines: the exception type by phase, the
// SQLSTATE fields and suffix, and server text for the listed classes only, sanitized, with no
// detail, hint or position. Every expected
// message is written out here.

// catch.hpp comes first, as the extension's C++ tests include it: DuckDB's copy declares the
// duckdb_base_std aliases it needs only while no DuckDB header has opened that namespace.
#include "catch.hpp"

#include "diagnostic_fixtures.hpp"
#include "duckdb/common/exception.hpp"
#include "odbc/odbc_errors.hpp"
#include "thrown.hpp"

#include <string>
#include <utility>
#include <vector>

using duckdb::ExceptionType;
using duckdb::StatementPhase;
using duckdb::redshift_driver::testing::FakeDiag;
using redshift_test::Capture;
using redshift_test::CaptureError;
using redshift_test::ConnectRecords;
using redshift_test::DRIVER_FAILURE_TRAILER;
using redshift_test::RecordingSink;
using redshift_test::StatementRecords;
using redshift_test::StatementWarnings;
using redshift_test::Thrown;

namespace {

using ExtraInfo = duckdb::unordered_map<duckdb::string, duckdb::string>;

//! What driver 2.3.0.0 writes before a message it took from libpq, ahead of libpq's SQLSTATE.
const std::string SERVER_PREFIX = "[Redshift][ODBC Driver][Server]";

//! What driver 2.3.0.0 writes for a failed streaming-cursor read, ahead of ` Detail: ` and libpq's
//! text.
const std::string STREAMING_READ_PREFIX =
    "[Redshift][ODBC Driver]An I/O error occurred while reading streaming cursor.";
const std::string STREAMING_READ_ERROR = STREAMING_READ_PREFIX + " Detail: ";

Thrown StatementFailure(std::vector<FakeDiag> diags, StatementPhase phase) {
	const auto records = StatementRecords(std::move(diags));
	return Capture([&]() { duckdb::ThrowStatementFailure(records, phase); });
}

} // namespace

TEST_CASE("The exception type follows the phase: BinderException from prepare to describe, the executor type otherwise",
          "[odbc]") {
	const std::vector<FakeDiag> diags {{"42601", 0, "ERROR:  syntax error at or near \"FROM\""}};
	CHECK(StatementFailure(diags, StatementPhase::ALLOCATE) ==
	      Thrown {ExceptionType::EXECUTOR, "the Redshift driver could not allocate a statement handle: ERROR:  syntax "
	                                       "error at or near \"FROM\"\nSQLSTATE: 42601"});
	CHECK(StatementFailure(diags, StatementPhase::PREPARE) ==
	      Thrown {ExceptionType::BINDER,
	              "Redshift refused the statement: ERROR:  syntax error at or near \"FROM\"\nSQLSTATE: 42601"});
	CHECK(StatementFailure(diags, StatementPhase::PARAMETERS) ==
	      Thrown {ExceptionType::BINDER, "the Redshift driver did not report the statement's parameter markers: "
	                                     "ERROR:  syntax error at or near \"FROM\"\nSQLSTATE: 42601"});
	CHECK(StatementFailure(diags, StatementPhase::DESCRIBE) ==
	      Thrown {ExceptionType::BINDER, "Redshift did not describe the statement's result columns: ERROR:  syntax "
	                                     "error at or near \"FROM\"\nSQLSTATE: 42601"});
	CHECK(StatementFailure(diags, StatementPhase::BIND) ==
	      Thrown {ExceptionType::EXECUTOR, "the Redshift driver could not bind the statement's parameters: ERROR:  "
	                                       "syntax error at or near \"FROM\"\nSQLSTATE: 42601"});
	CHECK(StatementFailure(diags, StatementPhase::EXECUTE) ==
	      Thrown {ExceptionType::EXECUTOR,
	              "Redshift could not run the statement: ERROR:  syntax error at or near \"FROM\"\nSQLSTATE: 42601"});
	CHECK(StatementFailure(diags, StatementPhase::FETCH) ==
	      Thrown {ExceptionType::EXECUTOR, "Redshift could not return the rest of the result: ERROR:  syntax error at "
	                                       "or near \"FROM\"\nSQLSTATE: 42601"});
}

TEST_CASE("A captured statement failure differs from one of another type or message", "[odbc]") {
	const auto thrown = StatementFailure({{"42601", 0, "syntax"}}, StatementPhase::PREPARE);
	REQUIRE(thrown.type == ExceptionType::BINDER);
	CHECK_FALSE(thrown == Thrown {ExceptionType::EXECUTOR, thrown.message});
	CHECK_FALSE(thrown == Thrown {thrown.type, thrown.message + "."});
}

TEST_CASE("A statement failure acts on the first SQLSTATE other than HY000, listing every one", "[odbc]") {
	const auto records = StatementRecords({{"HY000", 0, "generic"}, {"25P02", 0, "current transaction is aborted"}});
	const auto error = CaptureError([&]() { duckdb::ThrowStatementFailure(records, StatementPhase::EXECUTE); });
	CHECK(error.RawMessage() ==
	      "Redshift could not run the statement: current transaction is aborted\nSQLSTATE: 25P02");
	CHECK(error.ExtraInfo() == ExtraInfo {{"sqlstate", "25P02"}, {"sqlstates", "HY000,25P02"}});
}

TEST_CASE("A statement failure passes over an empty SQLSTATE to act on a later one", "[odbc]") {
	const auto records = StatementRecords({{"", 0, "first"}, {"25P02", 0, "current transaction is aborted"}});
	const auto error = CaptureError([&]() { duckdb::ThrowStatementFailure(records, StatementPhase::EXECUTE); });
	CHECK(error.ExtraInfo() == ExtraInfo {{"sqlstate", "25P02"}, {"sqlstates", "?????,25P02"}});
	CHECK(error.RawMessage() ==
	      "Redshift could not run the statement: current transaction is aborted\nSQLSTATE: 25P02");
}

TEST_CASE("A statement failure with only HY000 records acts on the first", "[odbc]") {
	const auto records = StatementRecords({{"HY000", 0, "first"}, {"HY000", 0, "second"}});
	const auto error = CaptureError([&]() { duckdb::ThrowStatementFailure(records, StatementPhase::EXECUTE); });
	CHECK(error.ExtraInfo() == ExtraInfo {{"sqlstate", "HY000"}, {"sqlstates", "HY000,HY000"}});
}

TEST_CASE("A statement failure of a server class shows the server's first line, sanitized", "[odbc]") {
	// A statement naming a catalog the workgroup has not mounted: the server's line names the database.
	CHECK(StatementFailure({{"3D000", 0, SERVER_PREFIX + "3D000:ERROR:  \"nope@nope\" does not exist"}},
	                       StatementPhase::PREPARE) ==
	      Thrown {ExceptionType::BINDER,
	              "Redshift refused the statement: ERROR:  \"nope@nope\" does not exist\nSQLSTATE: "
	              "3D000"});
	CHECK(StatementFailure({{"42P01", 0, "ERROR:  relation \"orders_2024\" does not exist\nDETAIL:  more"}},
	                       StatementPhase::PREPARE) ==
	      Thrown {ExceptionType::BINDER,
	              "Redshift refused the statement: ERROR:  relation \"orders_2024\" does not exist\nSQLSTATE: 42P01"});
	CHECK(StatementFailure({{"22012", 0, "ERROR:  division by zero at 10.0.0.1"}}, StatementPhase::FETCH) ==
	      Thrown {ExceptionType::EXECUTOR,
	              "Redshift could not return the rest of the result: ERROR:  division by zero at <address>\nSQLSTATE: "
	              "22012"});
	CHECK(StatementFailure({{"23505", 0, "ERROR:  duplicate key <b>k</b>"}}, StatementPhase::EXECUTE) ==
	      Thrown {ExceptionType::EXECUTOR,
	              "Redshift could not run the statement: ERROR:  duplicate key (b)k(/b)\nSQLSTATE: 23505"});
	CHECK(StatementFailure({{"0A000", 0, SERVER_PREFIX + "0A000:ERROR:  Local storage is not supported\nHINT:  x"}},
	                       StatementPhase::EXECUTE) ==
	      Thrown {ExceptionType::EXECUTOR,
	              "Redshift could not run the statement: ERROR:  Local storage is not supported\nSQLSTATE: 0A000"});
	// Redshift reports many fixable statement problems as XX000, and keeps its process, query and
	// source location in the lines after the first.
	CHECK(
	    StatementFailure({{"XX000", 0,
	                       SERVER_PREFIX + "XX000:ERROR:  Invalid datetime part in EXTRACT().\n  location: "
	                                       "any_scanner.cpp:884\n  process: padbmaster [pid=1073807483]\n"}},
	                     StatementPhase::EXECUTE) ==
	    Thrown {ExceptionType::EXECUTOR,
	            "Redshift could not run the statement: ERROR:  Invalid datetime part in EXTRACT().\nSQLSTATE: XX000"});
}

TEST_CASE("A class 0A message passes through the same redaction as the other shown classes", "[odbc]") {
	const auto text = SERVER_PREFIX + "0A000:ERROR:  Local storage is not supported for key AKIAIOSFODNN7EXAMPLE at "
	                                  "10.0.0.1, aws_session_token=FQoGZXIvYXdzEJr";
	CHECK(StatementFailure({{"0A000", 0, text}}, StatementPhase::EXECUTE) ==
	      Thrown {ExceptionType::EXECUTOR, "Redshift could not run the statement: ERROR:  Local storage is not "
	                                       "supported for key <redacted> at <address>, aws_session_token=<redacted>\n"
	                                       "SQLSTATE: 0A000"});
	RecordingSink sink;
	duckdb::LogInformationalDiagnostics(sink, StatementWarnings({{"0A000", 0, text}}));
	CHECK(sink.Lines() == std::vector<std::string> {"Redshift driver diagnostic, SQLSTATE 0A000: ERROR:  Local "
	                                                "storage is not supported for key <redacted> at <address>, "
	                                                "aws_session_token=<redacted>"});
	// A bound value it echoes shows only as its position, as the sanitizer alone would not do.
	duckdb::redshift_driver::testing::FakeOdbcDriver fake;
	const auto records = StatementRecords(
	    fake, {{"0A000", 0, SERVER_PREFIX + "0A000:ERROR:  Local storage is not supported for token-12345678"}},
	    {"token-12345678"});
	CHECK(Capture([&]() { duckdb::ThrowStatementFailure(records, StatementPhase::EXECUTE); }) ==
	      Thrown {ExceptionType::EXECUTOR,
	              "Redshift could not run the statement: ERROR:  Local storage is not supported for <param 1>\n"
	              "SQLSTATE: 0A000"});
}

// An XX first line can name the caller's own objects, such as an IAM role ARN with its account ID,
// which stay shown; a credential on it is redacted as for the other shown classes.
TEST_CASE("A class XX first line passes through the same redaction as the other shown classes", "[odbc]") {
	const auto text = SERVER_PREFIX + "XX000:ERROR:  S3ServiceException:Access Denied for role "
	                                  "arn:aws:iam::123456789012:role/PlaceholderRole, SECRET_ACCESS_KEY "
	                                  "'PLACEHOLDER_SECRET' at 10.0.0.1";
	CHECK(StatementFailure({{"XX000", 0, text}}, StatementPhase::EXECUTE) ==
	      Thrown {ExceptionType::EXECUTOR, "Redshift could not run the statement: ERROR:  S3ServiceException:Access "
	                                       "Denied for role arn:aws:iam::123456789012:role/PlaceholderRole, "
	                                       "SECRET_ACCESS_KEY '<redacted>' at <address>\n"
	                                       "SQLSTATE: XX000"});
	RecordingSink sink;
	duckdb::LogInformationalDiagnostics(sink, StatementWarnings({{"XX000", 0, text}}));
	CHECK(sink.Lines() == std::vector<std::string> {"Redshift driver diagnostic, SQLSTATE XX000: ERROR:  "
	                                                "S3ServiceException:Access Denied for role "
	                                                "arn:aws:iam::123456789012:role/PlaceholderRole, "
	                                                "SECRET_ACCESS_KEY '<redacted>' at <address>"});
}

// Driver 2.3.0.0 has no field for the server's detail, hint or position; they reach the client only
// in its message. The class 42 and 22 texts below are
// adapted from ones it wrote in a live probe.
TEST_CASE("A syntax error's position reaches neither the message nor extra_info", "[odbc]") {
	const auto records = StatementRecords({{"42601", 0,
	                                        SERVER_PREFIX +
	                                            "42601:ERROR:  syntax error at end of input\n LINE 1, "
	                                            "POSITION 13: SELECT 1 FROM\n" +
	                                            std::string(35, ' ') + "^\n"}});
	const auto error = CaptureError([&]() { duckdb::ThrowStatementFailure(records, StatementPhase::PREPARE); });
	CHECK(Thrown {error.Type(), error.RawMessage()} ==
	      Thrown {ExceptionType::BINDER,
	              "Redshift refused the statement: ERROR:  syntax error at end of input\nSQLSTATE: 42601"});
	CHECK(error.ExtraInfo() == ExtraInfo {{"sqlstate", "42601"}, {"sqlstates", "42601"}});
}

TEST_CASE("A shown failure's hint reaches neither the message nor extra_info", "[odbc]") {
	const auto records =
	    StatementRecords({{"42883", 0,
	                       SERVER_PREFIX + "42883:ERROR:  function no_such_function(integer) does not exist\nHINT:  No "
	                                       "function matches the given name and argument types. You may need to add "
	                                       "explicit type casts.\n"}});
	const auto error = CaptureError([&]() { duckdb::ThrowStatementFailure(records, StatementPhase::PREPARE); });
	CHECK(Thrown {error.Type(), error.RawMessage()} ==
	      Thrown {ExceptionType::BINDER, "Redshift refused the statement: ERROR:  function no_such_function(integer) "
	                                     "does not exist\nSQLSTATE: 42883"});
	CHECK(error.ExtraInfo() == ExtraInfo {{"sqlstate", "42883"}, {"sqlstates", "42883"}});
}

TEST_CASE("A class XX failure shows its first line, and its hint is left out with the rest", "[odbc]") {
	const auto records =
	    StatementRecords({{"XX000", 0, SERVER_PREFIX + "XX000:ERROR:  internal failure\nHINT:  Ask the operator.\n"}});
	const auto error = CaptureError([&]() { duckdb::ThrowStatementFailure(records, StatementPhase::EXECUTE); });
	CHECK(Thrown {error.Type(), error.RawMessage()} ==
	      Thrown {ExceptionType::EXECUTOR, "Redshift could not run the statement: ERROR:  internal failure\n"
	                                       "SQLSTATE: XX000"});
	CHECK(error.ExtraInfo() == ExtraInfo {{"sqlstate", "XX000"}, {"sqlstates", "XX000"}});
}

TEST_CASE("A failure whose first line is empty shows the error line of Redshift's box and nothing else of it",
          "[odbc]") {
	// The box Redshift sends as DETAIL for a compute node's error, as measured on a Serverless workgroup
	// for a cast (whose primary message held the reason too), here with the primary message empty, as
	// a write's XX000 can have it.
	const std::string boxed = SERVER_PREFIX + "XX000:ERROR:  \n"
	                                          "DETAIL:  \n"
	                                          "  -----------------------------------------------\n"
	                                          "  error:  Invalid digit, Value 'a', Pos 0, Type: Integer \n"
	                                          "  code:      1207\n"
	                                          "  context:   a\n"
	                                          "  query:     3106[child_sequence:1]\n"
	                                          "  location:  :0\n"
	                                          "  process:   query0_1023_3107 [pid=4242]\n"
	                                          "  -----------------------------------------------\n";
	const std::string reason = "ERROR:  Invalid digit, Value 'a', Pos 0, Type: Integer\nSQLSTATE: XX000";
	CHECK(boxed.find("4242") != std::string::npos);
	CHECK(StatementFailure({{"XX000", 0, boxed}}, StatementPhase::PREPARE) ==
	      Thrown {ExceptionType::BINDER, "Redshift refused the statement: " + reason});
	CHECK(StatementFailure({{"XX000", 0, boxed}}, StatementPhase::EXECUTE) ==
	      Thrown {ExceptionType::EXECUTOR, "Redshift could not run the statement: " + reason});
	// The error line is sanitized as a first line is, and only it is read.
	const auto sanitized = StatementFailure(
	    {{"XX000", 0,
	      "ERROR:\nDETAIL:  \n  ------\n  error:  failed at 10.20.30.40 pid 4242\n  location:  relay-1.internal\n"}},
	    StatementPhase::EXECUTE);
	CHECK(sanitized ==
	      Thrown {ExceptionType::EXECUTOR,
	              "Redshift could not run the statement: ERROR:  failed at <address> pid <pid>\nSQLSTATE: XX000"});
	// Only that box's shape: a DETAIL line with nothing after its label, a rule of dashes, then the error
	// line. An error line anywhere else, a box with no error line or an empty one, a DETAIL with text, or
	// a first line that is no severity word, still says the server gave no message.
	for (const auto *empty :
	     {"ERROR:  \n  code:  1207\n", "ERROR:  \n  error:  unboxed\n",
	      "ERROR:  \nHINT:  \n  ------\n  error:  under a hint\n", "ERROR:  \nDETAIL:  \n  error:  with no rule\n",
	      "ERROR:  \nDETAIL:  more\n  ------\n  error:  after a detail with text\n",
	      "ERROR:  \nDETAIL:  \n  ------\n  code:  1207\n  error:  after the code\n",
	      "ERROR:  \nDETAIL:  \n  ------\n  error:   \n", "\nDETAIL:  \n  ------\n  error:  no severity\n",
	      "ERROR:  \nDETAIL:  \n  ----\n  error:  after a rule too short\n"}) {
		INFO(empty);
		const auto failure = StatementFailure({{"XX000", 0, empty}}, StatementPhase::EXECUTE);
		CHECK(failure.message.find("the server gave no message") != std::string::npos);
	}
	// A first line with text keeps it, whatever the box says.
	CHECK(StatementFailure({{"XX000", 0, "ERROR:  internal failure\n  error:  other\n"}}, StatementPhase::EXECUTE) ==
	      Thrown {ExceptionType::EXECUTOR,
	              "Redshift could not run the statement: ERROR:  internal failure\nSQLSTATE: XX000"});
}

TEST_CASE("The box's error line is redacted, cut with its severity counted, and only the first is read", "[odbc]") {
	const std::string box = "ERROR:  \nDETAIL:  \n  ------\n  error:  ";
	// A bound value of 8 bytes or more on the error line is redacted, as on a first line.
	{
		duckdb::redshift_driver::testing::FakeOdbcDriver fake;
		const auto bound = StatementRecords(
		    fake, {{"XX000", 0, box + "Invalid digit, Value 'placeholder-value-1'\n", {}}}, {"placeholder-value-1"});
		const auto redacted = Capture([&]() { duckdb::ThrowStatementFailure(bound, StatementPhase::EXECUTE); }).message;
		CHECK(redacted.find("ERROR:  Invalid digit, Value '<param 1>'") != std::string::npos);
		CHECK(redacted.find("placeholder-value-1") == std::string::npos);
	}
	// The 256-byte cut takes in the severity word: the shown text is at most the cut, starting with it. Words,
	// since the driver's own 326-byte cut of the record ends it with "..." in place of its last word.
	std::string words;
	for (int i = 0; i < 60; i++) {
		words += "word ";
	}
	const auto long_line = StatementFailure({{"XX000", 0, box + words + "\n"}}, StatementPhase::EXECUTE).message;
	const std::string lead = "Redshift could not run the statement: ";
	REQUIRE(long_line.rfind(lead, 0) == 0);
	const auto shown = long_line.substr(lead.size(), long_line.find("\nSQLSTATE") - lead.size());
	CHECK(shown.rfind("ERROR:  word word", 0) == 0);
	CHECK(shown.size() <= 260);
	// Two error lines: the one right after the rule is the box's.
	CHECK(StatementFailure({{"XX000", 0, box + "the first\n  error:  the second\n"}}, StatementPhase::EXECUTE) ==
	      Thrown {ExceptionType::EXECUTOR, "Redshift could not run the statement: ERROR:  the first\nSQLSTATE: XX000"});
}

TEST_CASE("A statement informational record's hint stays out of its log line", "[odbc]") {
	RecordingSink sink;
	duckdb::LogInformationalDiagnostics(
	    sink, StatementWarnings({{"22008", 0,
	                              SERVER_PREFIX + "22008:ERROR:  date/time field value out of range: \"2020-13-45\"\n"
	                                              "HINT:  Perhaps you need a different \"datestyle\" setting.\n"}}));
	CHECK(sink.Lines() == std::vector<std::string> {"Redshift driver diagnostic, SQLSTATE 22008: ERROR:  date/time "
	                                                "field value out of range: \"2020-13-45\""});
}

TEST_CASE("A statement failure of each other reviewed request class shows the server's first line too", "[odbc]") {
	for (const auto *sqlstate : {"3D000", "3F000", "2BP01", "25P02", "40001", "40P01", "55000", "57014", "P0001"}) {
		INFO(sqlstate);
		CHECK(StatementFailure({{sqlstate, 0, "ERROR:  server says hello at 10.0.0.1\nDETAIL:  more"}},
		                       StatementPhase::EXECUTE) ==
		      Thrown {ExceptionType::EXECUTOR, "Redshift could not run the statement: ERROR:  server says hello at "
		                                       "<address>\nSQLSTATE: " +
		                                           std::string(sqlstate)});
	}
}

TEST_CASE("A statement failure of class 08, 53 or 58, or of a class no one reviewed, shows the class only", "[odbc]") {
	// 08, 53 and 58 describe the server's connections, resources and system, where a host the
	// sanitizer's forms do not take can stand; a class not on the list shows no text either.
	const std::string tail = "). The server's message for this class is not shown; act on the SQLSTATE.";
	const std::string lead = "Redshift could not run the statement (SQLSTATE class ";
	const std::vector<std::pair<std::string, std::string>> cases {{"08006", "08, connection exception"},
	                                                              {"53100", "53, insufficient resources"},
	                                                              {"58030", "58, system error"},
	                                                              {"0B000", "0B"},
	                                                              {"21000", "21"},
	                                                              {"28000", "28"},
	                                                              {"ZZ999", "ZZ"}};
	for (const auto &entry : cases) {
		CHECK(StatementFailure({{entry.first, 0, "ERROR:  failed on relay-1.internal"}}, StatementPhase::EXECUTE) ==
		      Thrown {ExceptionType::EXECUTOR, lead + entry.second + tail + "\nSQLSTATE: " + entry.first});
	}
}

TEST_CASE("A server message with nothing after its severity word says the server gave no message", "[odbc]") {
	// Seen live: an XX000 on a write whose text was "ERROR:" alone.
	CHECK(StatementFailure({{"XX000", 0, SERVER_PREFIX + "XX000:ERROR:  "}}, StatementPhase::PREPARE) ==
	      Thrown {ExceptionType::BINDER, "Redshift refused the statement (SQLSTATE class XX, internal error), and the "
	                                     "server gave no message. Retry the statement. If it fails the same way, "
	                                     "report it with the extension and driver versions.\nSQLSTATE: XX000"});
	CHECK(StatementFailure({{"XX000", 0, "ERROR:"}}, StatementPhase::EXECUTE) ==
	      Thrown {ExceptionType::EXECUTOR,
	              "Redshift could not run the statement (SQLSTATE class XX, internal error), and the server gave no "
	              "message. The statement may have run; check before running it again. If it fails the same way, "
	              "report it with the extension and driver versions.\nSQLSTATE: XX000"});
	CHECK(StatementFailure({{"57014", 0, ""}}, StatementPhase::EXECUTE).message.find("the server gave no message") !=
	      std::string::npos);
	// Each severity word, in any case, says nothing by itself.
	for (const auto *empty : {"FATAL:", "PANIC:  ", "error:", "  Error:  "}) {
		INFO(empty);
		const auto failure = StatementFailure({{"XX000", 0, empty}}, StatementPhase::EXECUTE);
		CHECK(failure.message.find("the server gave no message") != std::string::npos);
	}
}

TEST_CASE("A statement failure with no record carries no SQLSTATE", "[odbc]") {
	const auto records = StatementRecords({});
	const auto error = CaptureError([&]() { duckdb::ThrowStatementFailure(records, StatementPhase::EXECUTE); });
	CHECK(Thrown {error.Type(), error.RawMessage()} == Thrown {ExceptionType::EXECUTOR,
	                                                           "Redshift could not run the statement, and the driver "
	                                                           "returned no diagnostic record. The statement may have "
	                                                           "run; check before running it again."});
	CHECK(error.ExtraInfo().empty());
}

TEST_CASE("A statement failure with no record says a retry is safe only before the statement runs", "[odbc]") {
	const auto records = StatementRecords({});
	const std::string may_have_run =
	    ", and the driver returned no diagnostic record. The statement may have run; check before "
	    "running it again.";
	CHECK(Capture([&]() { duckdb::ThrowStatementFailure(records, StatementPhase::ALLOCATE); }) ==
	      Thrown {ExceptionType::EXECUTOR, "the Redshift driver could not allocate a statement handle, and the driver "
	                                       "returned no diagnostic record. Retry the statement."});
	CHECK(Capture([&]() { duckdb::ThrowStatementFailure(records, StatementPhase::BIND); }) ==
	      Thrown {ExceptionType::EXECUTOR, "the Redshift driver could not bind the statement's parameters, and the "
	                                       "driver returned no diagnostic record. Retry the statement."});
	CHECK(Capture([&]() { duckdb::ThrowStatementFailure(records, StatementPhase::FETCH); }) ==
	      Thrown {ExceptionType::EXECUTOR, "Redshift could not return the rest of the result" + may_have_run});
	CHECK(Capture([&]() { duckdb::ThrowCellReadFailure(records, 2); }) ==
	      Thrown {ExceptionType::EXECUTOR, "Redshift could not return column 2 of the current row" + may_have_run});
}

TEST_CASE("A class HY failure is the driver's own, with neither the server's message nor its text", "[odbc]") {
	CHECK(StatementFailure({{"HY001", 0, "Memory allocation error"}}, StatementPhase::ALLOCATE) ==
	      Thrown {ExceptionType::EXECUTOR,
	              "the Redshift driver failed while allocating a statement handle (SQLSTATE class HY, driver error), "
	              "and its message is not shown. Retry the statement." +
	                  DRIVER_FAILURE_TRAILER + "\nSQLSTATE: HY001"});
	CHECK(StatementFailure({{"HY010", 0, "Function sequence error"}}, StatementPhase::PREPARE) ==
	      Thrown {ExceptionType::BINDER,
	              "the Redshift driver failed while preparing the statement (SQLSTATE class HY, driver error), "
	              "and its message is not shown. Retry the statement." +
	                  DRIVER_FAILURE_TRAILER + "\nSQLSTATE: HY010"});
	CHECK(StatementFailure({{"HY000", 0, "Unicode conversion failed."}}, StatementPhase::EXECUTE) ==
	      Thrown {ExceptionType::EXECUTOR,
	              "the Redshift driver failed while running the statement (SQLSTATE class HY, driver error), "
	              "and its message is not shown. The statement may have run; check before running it again." +
	                  DRIVER_FAILURE_TRAILER + "\nSQLSTATE: HY000"});
	const auto records = StatementRecords({{"HY000", 0, "Unicode conversion failed."}});
	CHECK(
	    Capture([&]() { duckdb::ThrowCellReadFailure(records, 3); }) ==
	    Thrown {
	        ExceptionType::EXECUTOR,
	        "the Redshift driver failed while returning column 3 of the current row (SQLSTATE class HY, "
	        "driver error), and its message is not shown. The statement may have run; check before running it again." +
	            DRIVER_FAILURE_TRAILER + "\nSQLSTATE: HY000"});
}

TEST_CASE("A connect-phase record shows no text in a statement failure, whatever its class", "[odbc]") {
	const auto records = ConnectRecords(SQL_ERROR, {{"42601", 0, "ERROR:  syntax error near relay-1.internal"}});
	CHECK(Capture([&]() { duckdb::ThrowStatementFailure(records, StatementPhase::PREPARE); }) ==
	      Thrown {ExceptionType::BINDER, "Redshift refused the statement (SQLSTATE class 42). The server's message for "
	                                     "this class is not shown; act on the SQLSTATE.\nSQLSTATE: 42601"});
}

TEST_CASE("A class 22 record the driver writes itself is shown without the driver's prefix", "[odbc]") {
	CHECK(
	    StatementFailure({{"22003", 0, "[Redshift][ODBC Driver]Numeric value out of range"}}, StatementPhase::FETCH) ==
	    Thrown {ExceptionType::EXECUTOR,
	            "Redshift could not return the rest of the result: Numeric value out of range\nSQLSTATE: 22003"});
	CHECK(StatementFailure({{"22018", 0, SERVER_PREFIX + "Invalid numeric format"}}, StatementPhase::FETCH) ==
	      Thrown {ExceptionType::EXECUTOR,
	              "Redshift could not return the rest of the result: Invalid numeric format\nSQLSTATE: 22018"});
}

TEST_CASE("A statement failure whose records could not be read says so, with no SQLSTATE", "[odbc]") {
	duckdb::redshift_driver::testing::FakeOdbcDriver fake;
	fake.FailCall("SQLGetDiagRec", 1, SQL_ERROR, {});
	const auto records = StatementRecords(fake, {{"42601", 0, "syntax"}});
	CHECK(Capture([&]() { duckdb::ThrowStatementFailure(records, StatementPhase::PREPARE); }) ==
	      Thrown {ExceptionType::BINDER, "Redshift refused the statement, and the driver's diagnostic records could "
	                                     "not be read. Retry the statement."});
}

TEST_CASE("A transport failure says the statement was not replayed, with no driver text", "[odbc]") {
	const auto records = StatementRecords({{"HY000", 0, "generic"}, {"08006", 0, "server closed at relay-1.internal"}});
	const auto error = CaptureError(
	    [&]() { duckdb::ThrowTransportFailure(records, "wg.example (region us-east-1)", StatementPhase::EXECUTE); });
	CHECK(Thrown {error.Type(), error.RawMessage()} ==
	      Thrown {ExceptionType::IO,
	              "The connection to Redshift at wg.example (region us-east-1) was lost (SQLSTATE class 08, connection "
	              "exception). The statement was not replayed, because a write it carried may already have taken "
	              "effect; check before running it again.\nSQLSTATE: 08006"});
	CHECK(error.ExtraInfo() == ExtraInfo {{"sqlstate", "08006"}, {"sqlstates", "HY000,08006"}});
}

TEST_CASE("A transport failure says a write may have taken effect only from SQLExecute on", "[odbc]") {
	const auto records = StatementRecords({{"08006", 0, "server closed at relay-1.internal"}});
	const std::string lost = "The connection to Redshift at wg.example was lost (SQLSTATE class 08, connection "
	                         "exception). ";
	const std::string before_execute = "The statement was not replayed.\nSQLSTATE: 08006";
	const std::string not_replayed = "The statement was not replayed, because a write it carried may already have "
	                                 "taken effect; check before running it again.\nSQLSTATE: 08006";
	const std::vector<std::pair<StatementPhase, std::string>> cases {
	    {StatementPhase::ALLOCATE, before_execute},   {StatementPhase::PREPARE, before_execute},
	    {StatementPhase::PARAMETERS, before_execute}, {StatementPhase::DESCRIBE, before_execute},
	    {StatementPhase::BIND, before_execute},       {StatementPhase::EXECUTE, not_replayed},
	    {StatementPhase::FETCH, not_replayed}};
	for (const auto &entry : cases) {
		CHECK(Capture([&]() { duckdb::ThrowTransportFailure(records, "wg.example", entry.first); }) ==
		      Thrown {ExceptionType::IO, lost + entry.second});
	}
}

TEST_CASE("A bound value that matches the driver's framing does not keep a carried-over SQLSTATE", "[odbc]") {
	duckdb::redshift_driver::testing::FakeOdbcDriver fake;
	const auto records = StatementRecords(
	    fake, {{"25P02", 0, SERVER_PREFIX + "25P02:server closed the connection unexpectedly", {}}}, {"Redshift"});
	REQUIRE(records.records.size() == 1);
	CHECK(records.records[0].Message().find("Redshift") == std::string::npos);
	CHECK(CaptureError([&]() {
		      duckdb::ThrowTransportFailure(records, "wg.example", StatementPhase::EXECUTE);
	      }).ExtraInfo() == ExtraInfo {{"sqlstate", "HY000"}, {"sqlstates", "25P02"}});
}

TEST_CASE("A bound value that matches the driver's framing does not keep the framing shown", "[odbc]") {
	// Each record, the bound value, and the text the failure shows after its lead.
	const std::vector<std::vector<std::string>> cases = {
	    {"42601", SERVER_PREFIX + "42601:ERROR:  syntax error at or near \"FROM\"", "ODBC Driver",
	     "ERROR:  syntax error at or near \"FROM\""},
	    // A value that runs from the framing into libpq's text shows as its marker, then the rest.
	    {"42601", SERVER_PREFIX + "42601:ERROR:  syntax error at or near \"FROM\"", "42601:ERROR:  syntax",
	     "<param 1> error at or near \"FROM\""},
	    {"22012", STREAMING_READ_ERROR + "ERROR:  division by zero", "streaming cursor", "ERROR:  division by zero"},
	    {"22003", "[Redshift][ODBC Driver]Numeric value out of range", "[Redshift]", "Numeric value out of range"}};
	for (const auto &entry : cases) {
		INFO(entry[1] + " with " + entry[2]);
		duckdb::redshift_driver::testing::FakeOdbcDriver fake;
		const auto records = StatementRecords(fake, {{entry[0], 0, entry[1], {}}}, {entry[2]});
		REQUIRE(records.records.size() == 1);
		REQUIRE(records.records[0].Message().find("<param 1>") != std::string::npos);
		CHECK(Capture([&]() { duckdb::ThrowStatementFailure(records, StatementPhase::FETCH); }).message ==
		      "Redshift could not return the rest of the result: " + entry[3] + "\nSQLSTATE: " + entry[0]);
	}
}

TEST_CASE("A streaming read with no libpq text keeps nothing after its framing", "[odbc]") {
	// The trim takes the lead-in's last space too, so the framing ends past the message.
	const auto records = StatementRecords({{"22012", 0, STREAMING_READ_ERROR + "\n"}});
	REQUIRE(records.records.size() == 1);
	CHECK(records.records[0].UnframedMessage().empty());
}

TEST_CASE("A [Server] record without the driver's stamp keeps none of its framing", "[odbc]") {
	// Until the server's first error or notice, libpq holds no state, so the driver writes its prefix
	// and "[Server]" alone, under its own HY000 (source v2.3.0 rsutil.c addError, fe-protocol3.c).
	const auto records = StatementRecords({{"HY000", 0, SERVER_PREFIX + "server closed the connection unexpectedly"}});
	REQUIRE(records.records.size() == 1);
	CHECK(records.records[0].UnframedMessage() == "server closed the connection unexpectedly");
}

TEST_CASE("A transport failure with no record carries no SQLSTATE", "[odbc]") {
	const auto records = StatementRecords({});
	const auto error =
	    CaptureError([&]() { duckdb::ThrowTransportFailure(records, "wg.example", StatementPhase::EXECUTE); });
	CHECK(error.RawMessage() == "The connection to Redshift at wg.example was lost, and the driver returned no "
	                            "diagnostic record. The statement was not replayed, because a write it carried may "
	                            "already have taken effect; check before running it again.");
	CHECK(error.ExtraInfo().empty());
}

TEST_CASE("A connect-phase informational record is logged by its SQLSTATE alone", "[odbc]") {
	RecordingSink sink;
	duckdb::LogInformationalDiagnostics(
	    sink, ConnectRecords(SQL_SUCCESS_WITH_INFO, {{"01000", 0, "token PWD=x"}, {"42000", 0, "at relay.internal"}}));
	CHECK(sink.Lines() == std::vector<std::string> {"Redshift driver diagnostic, SQLSTATE 01000",
	                                                "Redshift driver diagnostic, SQLSTATE 42000"});
}

TEST_CASE("A statement informational record is logged with its sanitized message for a server class only", "[odbc]") {
	RecordingSink sink;
	duckdb::LogInformationalDiagnostics(sink, StatementWarnings({{"01004", 0, "String data, right truncation"},
	                                                             {"22001", 0, "value too long at 10.0.0.1"}}));
	CHECK(sink.Lines() == std::vector<std::string> {"Redshift driver diagnostic, SQLSTATE 01004",
	                                                "Redshift driver diagnostic, SQLSTATE 22001: value too long at "
	                                                "<address>"});
	// Classes 08, 53 and 58 log their SQLSTATE alone, as their failures show only the class: their text
	// can hold a host the sanitizer does not take. Any other server class logs its line.
	RecordingSink classes;
	duckdb::LogInformationalDiagnostics(classes, StatementWarnings({{"08006", 0, "lost at relay-1.internal"},
	                                                                {"53300", 0, "full at relay-1.internal"},
	                                                                {"58030", 0, "failed at relay-1.internal"},
	                                                                {"25P02", 0, "transaction is aborted"}}));
	CHECK(classes.Lines() == std::vector<std::string> {"Redshift driver diagnostic, SQLSTATE 08006",
	                                                   "Redshift driver diagnostic, SQLSTATE 53300",
	                                                   "Redshift driver diagnostic, SQLSTATE 58030",
	                                                   "Redshift driver diagnostic, SQLSTATE 25P02: transaction is "
	                                                   "aborted"});
}

TEST_CASE("A statement informational record is logged with libpq's text, without the driver's framing", "[odbc]") {
	RecordingSink sink;
	duckdb::LogInformationalDiagnostics(
	    sink, StatementWarnings({{"22001", 0, SERVER_PREFIX + "22001:ERROR:  value too long at 10.0.0.1"}}));
	CHECK(sink.Lines() == std::vector<std::string> {"Redshift driver diagnostic, SQLSTATE 22001: ERROR:  value too "
	                                                "long at <address>"});
}

TEST_CASE("A statement phase no enumerator names is an internal error", "[odbc]") {
#ifdef DUCKDB_CRASH_ON_ASSERT
	return; // InternalException aborts in this build
#endif
	CHECK(StatementFailure({{"42601", 0, "syntax"}}, static_cast<StatementPhase>(9)) ==
	      Thrown {ExceptionType::INTERNAL, "unknown Redshift statement phase"});
}

TEST_CASE("A SQLSTATE that the server's ERROR, FATAL or PANIC follows is the failure's own", "[odbc]") {
	// The shown message is libpq's text, without the driver's prefix and stamp.
	CHECK(StatementFailure({{"42601", 0, SERVER_PREFIX + "42601:ERROR:  syntax error at or near \"FROM\""}},
	                       StatementPhase::PREPARE) ==
	      Thrown {ExceptionType::BINDER,
	              "Redshift refused the statement: ERROR:  syntax error at or near \"FROM\"\nSQLSTATE: 42601"});
	const std::string tail = "). The statement was not replayed, because a write it carried may already have taken "
	                         "effect; check before running it again.\nSQLSTATE: ";
	const auto fatal = StatementRecords({{"57P01", 0, SERVER_PREFIX + "57P01:FATAL:  terminating connection"}});
	CHECK(Capture([&]() { duckdb::ThrowTransportFailure(fatal, "wg.example", StatementPhase::EXECUTE); }) ==
	      Thrown {ExceptionType::IO,
	              "The connection to Redshift at wg.example was lost (Redshift ended the session" + tail + "57P01"});
	const auto panic = StatementRecords({{"XX000", 0, SERVER_PREFIX + "XX000:PANIC:  could not write"}});
	CHECK(Capture([&]() { duckdb::ThrowTransportFailure(panic, "wg.example", StatementPhase::EXECUTE); }) ==
	      Thrown {ExceptionType::IO,
	              "The connection to Redshift at wg.example was lost (SQLSTATE class XX, internal error" + tail +
	                  "XX000"});
}

TEST_CASE("A lost connection names Redshift or the network as its cause, never the driver", "[odbc]") {
	const auto dropped = StatementRecords({{"HY000", 0, "[Redshift][ODBC Driver]SSL SYSCALL error: EOF detected"}});
	CHECK(Capture([&]() { duckdb::ThrowTransportFailure(dropped, "wg.example", StatementPhase::PREPARE); }) ==
	      Thrown {ExceptionType::IO, "The connection to Redshift at wg.example was lost (Redshift, or the network "
	                                 "between this host and Redshift, closed it). The statement was not replayed."
	                                 "\nSQLSTATE: HY000"});
	CHECK(
	    Capture(
	        [&]() { duckdb::ThrowSessionSetupLoss(dropped, "wg.example", duckdb::redshift_driver::DriverIdentity()); })
	        .message.find("lost during the setup (Redshift, or the network between this host and Redshift, closed it)."
	                      " No statement of yours was sent") != std::string::npos);
	for (const auto *sqlstate : {"57P01", "57P02", "57P03"}) {
		INFO(sqlstate);
		const auto ended = StatementRecords({{sqlstate, 0, SERVER_PREFIX + sqlstate + ":FATAL:  terminating"}});
		CHECK(Capture([&]() {
			      duckdb::ThrowTransportFailure(ended, "wg.example", StatementPhase::EXECUTE);
		      }).message.find("was lost (Redshift ended the session). ") != std::string::npos);
	}
	// Every other class keeps its class description.
	const auto other = StatementRecords({{"57014", 0, SERVER_PREFIX + "57014:FATAL:  cancelled"}});
	CHECK(Capture([&]() {
		      duckdb::ThrowTransportFailure(other, "wg.example", StatementPhase::EXECUTE);
	      }).message.find("was lost (SQLSTATE class 57, operator intervention). ") != std::string::npos);
}

TEST_CASE("A SQLSTATE the driver carried over from an earlier server message acts as HY000", "[odbc]") {
	// libpq keeps the last SQLSTATE the server sent, and the driver stamps a failure libpq raises
	// itself with it: here a 25P02 from before a ROLLBACK, on a connection that later reset.
	const auto lost =
	    StatementRecords({{"25P02", 0, SERVER_PREFIX + "25P02:server closed the connection unexpectedly"}});
	const auto error =
	    CaptureError([&]() { duckdb::ThrowTransportFailure(lost, "wg.example", StatementPhase::EXECUTE); });
	CHECK(error.RawMessage() ==
	      "The connection to Redshift at wg.example was lost (Redshift, or the network between this host and "
	      "Redshift, closed it). The statement was not replayed, because a write it carried may already have "
	      "taken effect; "
	      "check before running it again.\nSQLSTATE: HY000");
	CHECK(error.ExtraInfo() == ExtraInfo {{"sqlstate", "HY000"}, {"sqlstates", "25P02"}});
	// A 42P01 carried over does not open the class 42 window to text libpq wrote.
	CHECK(StatementFailure({{"42P01", 0, SERVER_PREFIX + "42P01:could not receive data from 10.0.0.1"}},
	                       StatementPhase::EXECUTE) ==
	      Thrown {ExceptionType::EXECUTOR,
	              "the Redshift driver failed while running the statement (SQLSTATE class HY, driver error), "
	              "and its message is not shown. The statement may have run; check before running it again." +
	                  DRIVER_FAILURE_TRAILER + "\nSQLSTATE: HY000"});
}

TEST_CASE("A [Server] record without the driver's stamp keeps its own SQLSTATE", "[odbc]") {
	// The driver writes this one itself with HY001 (source v2.3.0 rslibpq.c).
	const auto records = StatementRecords({{"HY001", 0, SERVER_PREFIX + "Memory allocation error"}});
	CHECK(CaptureError([&]() { duckdb::ThrowTransportFailure(records, "wg.example", StatementPhase::EXECUTE); })
	          .ExtraInfo()
	          .at("sqlstate") == "HY001");
}

TEST_CASE("A carried-over SQLSTATE needs the server's severity to open libpq's text", "[odbc]") {
	const auto records =
	    StatementRecords({{"42P01", 0, SERVER_PREFIX + "42P01:lost synchronization with server: ERROR:  x"}});
	CHECK(CaptureError([&]() { duckdb::ThrowTransportFailure(records, "wg.example", StatementPhase::EXECUTE); })
	          .ExtraInfo()
	          .at("sqlstate") == "HY000");
}

TEST_CASE("A streaming read failure needs the exact Detail separator before the server's severity", "[odbc]") {
	const auto records = StatementRecords({{"22012", 0, STREAMING_READ_PREFIX + " Detail; ERROR:  division by zero"}});
	CHECK(CaptureError([&]() { duckdb::ThrowTransportFailure(records, "wg.example", StatementPhase::EXECUTE); })
	          .ExtraInfo()
	          .at("sqlstate") == "HY000");
}

TEST_CASE("A success or warning SQLSTATE never decides a failure", "[odbc]") {
	const std::string expected = "Redshift could not run the statement: ERROR:  division by zero\nSQLSTATE: 22012";
	CHECK(StatementFailure({{"01004", 0, "String data, right truncated"}, {"22012", 0, "ERROR:  division by zero"}},
	                       StatementPhase::EXECUTE) == Thrown {ExceptionType::EXECUTOR, expected});
	const auto records = StatementRecords({{"00000", 0, "notice"}, {"22012", 0, "ERROR:  division by zero"}});
	const auto error = CaptureError([&]() { duckdb::ThrowStatementFailure(records, StatementPhase::EXECUTE); });
	CHECK(Thrown {error.Type(), error.RawMessage()} == Thrown {ExceptionType::EXECUTOR, expected});
	CHECK(error.ExtraInfo() == ExtraInfo {{"sqlstate", "22012"}, {"sqlstates", "00000,22012"}});
}

TEST_CASE("A statement informational record whose SQLSTATE was carried over is logged without its text", "[odbc]") {
	RecordingSink sink;
	duckdb::LogInformationalDiagnostics(
	    sink, StatementWarnings({{"42P01", 0, SERVER_PREFIX + "42P01:could not receive data from 10.0.0.1"}}));
	CHECK(sink.Lines() == std::vector<std::string> {"Redshift driver diagnostic, SQLSTATE 42P01"});
}

TEST_CASE("A streaming read failure keeps its SQLSTATE only when libpq's text is a server error", "[odbc]") {
	const auto server = StatementRecords({{"22012", 0, STREAMING_READ_ERROR + "ERROR:  division by zero"}});
	// The shown message is libpq's text, without the driver's streaming-read opening.
	CHECK(Capture([&]() { duckdb::ThrowStatementFailure(server, StatementPhase::FETCH); }) ==
	      Thrown {ExceptionType::EXECUTOR,
	              "Redshift could not return the rest of the result: ERROR:  division by zero\nSQLSTATE: 22012"});
	// A 42P01 libpq kept from an earlier statement, under text libpq wrote itself.
	const auto stale = StatementRecords(
	    {{"42P01", 0, STREAMING_READ_ERROR + "lost synchronization with server: got message type \"Z\""}});
	const auto error = CaptureError([&]() { duckdb::ThrowStatementFailure(stale, StatementPhase::FETCH); });
	CHECK(error.RawMessage() ==
	      "the Redshift driver failed while returning the rest of the result (SQLSTATE class HY, "
	      "driver error), and its message is not shown. The statement may have run; check before running it again." +
	          DRIVER_FAILURE_TRAILER + "\nSQLSTATE: HY000");
	CHECK(error.ExtraInfo() == ExtraInfo {{"sqlstate", "HY000"}, {"sqlstates", "42P01"}});
	const auto no_detail = StatementRecords(
	    {{"42P01", 0, "[Redshift][ODBC Driver]An I/O error occurred while reading streaming cursor."}});
	CHECK(CaptureError([&]() { duckdb::ThrowTransportFailure(no_detail, "wg.example", StatementPhase::EXECUTE); })
	          .ExtraInfo()
	          .at("sqlstate") == "HY000");
}

TEST_CASE("A streaming read failure the driver marks as a link failure keeps 08S01", "[odbc]") {
	const auto records = StatementRecords({{"08S01", 0, STREAMING_READ_ERROR + "server closed the connection"}});
	CHECK(Capture([&]() { duckdb::ThrowTransportFailure(records, "wg.example", StatementPhase::EXECUTE); }) ==
	      Thrown {ExceptionType::IO, "The connection to Redshift at wg.example was lost (SQLSTATE class 08, connection "
	                                 "exception). The statement was not replayed, because a write it carried may "
	                                 "already have taken effect; check before running it again.\nSQLSTATE: 08S01"});
}

TEST_CASE("A client-side cursor read failure keeps the SQLSTATE the driver gave it", "[odbc]") {
	// The driver writes 24000 itself for this read, with no libpq state (source v2.3.0 rsresult.cpp).
	const auto records = StatementRecords(
	    {{"24000", 0, "[Redshift][ODBC Driver]An I/O error occurred while reading client side cursor."}});
	CHECK(CaptureError([&]() {
		      duckdb::ThrowTransportFailure(records, "wg.example", StatementPhase::EXECUTE);
	      }).ExtraInfo() == ExtraInfo {{"sqlstate", "24000"}, {"sqlstates", "24000"}});
}

TEST_CASE("A record in SQLSTATE class 08 shows a lost connection, wherever it is in the failure", "[odbc]") {
	SECTION("08S01, the driver's link failure") {
		CHECK(duckdb::RecordsShowConnectionLoss(StatementRecords({{"08S01", 0, "Streaming cursor read error"}})));
	}
	SECTION("08006, after a generic record") {
		CHECK(duckdb::RecordsShowConnectionLoss(StatementRecords({{"HY000", 0, "generic"}, {"08006", 0, "lost"}})));
	}
}

TEST_CASE("Each libpq text for a dropped connection shows a lost connection, under any SQLSTATE", "[odbc]") {
	std::string text;
	SECTION("server closed the connection unexpectedly") {
		text = "[Redshift][ODBC Driver][Server]Server closed the connection unexpectedly\n\tThis probably means";
	}
	SECTION("SSL connection has been closed unexpectedly, a TLS connection the server closed") {
		text = "[Redshift][ODBC Driver][Server]SSL connection has been closed unexpectedly\n";
	}
	SECTION("SSL SYSCALL error: EOF detected") {
		text = "[Redshift][ODBC Driver][Server]SSL SYSCALL error: EOF detected";
	}
	SECTION("SSL SYSCALL error with an errno text, as a keepalive timeout gives") {
		text = "[Redshift][ODBC Driver][Server]SSL SYSCALL error: Connection timed out";
	}
	SECTION("SSL error") {
		text = "[Redshift][ODBC Driver][Server]SSL error: decryption failed or bad record mac";
	}
	SECTION("decompress error, from a zstd stream libpq cannot read") {
		text = "[Redshift][ODBC Driver][Server]decompress error: client-server buffer configration mismatch";
	}
	SECTION("could not receive data from server") {
		text = "[Redshift][ODBC Driver][Server]could not receive data from server: Connection reset by peer:104";
	}
	SECTION("could not send data to server") {
		text = "[Redshift][ODBC Driver][Server]could not send data to server: Broken pipe";
	}
	SECTION("no connection to the server") {
		text = "[Redshift][ODBC Driver][Server]no connection to the server";
	}
	SECTION("connection not open") {
		text = "[Redshift][ODBC Driver][Server]connection not open";
	}
	CHECK(duckdb::RecordsShowConnectionLoss(StatementRecords({{"HY000", 0, text}})));
	// The driver reports libpq's own text under the last SQLSTATE a server error left, here 42P01, and then writes that
	// SQLSTATE and a colon after its prefix.
	const std::string prefix = "[Redshift][ODBC Driver][Server]";
	CHECK(duckdb::RecordsShowConnectionLoss(
	    StatementRecords({{"42P01", 0, prefix + "42P01:" + text.substr(prefix.size())}})));
}

TEST_CASE("A record without the driver's prefix shows a lost connection only when a libpq text starts it", "[odbc]") {
	CHECK(duckdb::RecordsShowConnectionLoss(
	    StatementRecords({{"HY000", 0, "server closed the connection unexpectedly"}})));
	CHECK_FALSE(duckdb::RecordsShowConnectionLoss(
	    StatementRecords({{"HY000", 0, "ERROR: the text server closed the connection unexpectedly"}})));
}

TEST_CASE("A dropped-connection text shows a lost connection although a bound value redacts the driver's prefix",
          "[odbc]") {
	duckdb::redshift_driver::testing::FakeOdbcDriver fake;
	const auto records = StatementRecords(
	    fake, {{"HY000", 0, "[Redshift][ODBC Driver][Server]SSL SYSCALL error: Connection timed out", {}}},
	    {"Redshift"});
	REQUIRE(records.records.size() == 1);
	CHECK(records.records[0].Message().find("Redshift") == std::string::npos);
	CHECK(duckdb::RecordsShowConnectionLoss(records));
}

TEST_CASE("A dropped-connection text shows a lost connection although a bound value redacts libpq's own words",
          "[odbc]") {
	duckdb::redshift_driver::testing::FakeOdbcDriver fake;
	const auto records = StatementRecords(
	    fake,
	    {{"HY000",
	      0,
	      "[Redshift][ODBC Driver][Server]server closed the connection unexpectedly\n\tThis probably means",
	      {}}},
	    {"connection"});
	REQUIRE(records.records.size() == 1);
	CHECK(records.records[0].Message().find("server closed the <param 1> unexpectedly") != std::string::npos);
	CHECK(duckdb::RecordsShowConnectionLoss(records));
}

TEST_CASE("A dropped-connection text after a cursor read's lead-in shows a lost connection", "[odbc]") {
	const std::string loss = "server closed the connection unexpectedly\n\tThis probably means";
	SECTION("a streaming cursor, under the last server SQLSTATE libpq kept") {
		CHECK(duckdb::RecordsShowConnectionLoss(StatementRecords({{"42P01", 0, STREAMING_READ_ERROR + loss}})));
	}
	SECTION("a streaming cursor, under HY000") {
		CHECK(duckdb::RecordsShowConnectionLoss(StatementRecords({{"HY000", 0, STREAMING_READ_ERROR + loss}})));
	}
	SECTION("a client side cursor") {
		CHECK(duckdb::RecordsShowConnectionLoss(StatementRecords(
		    {{"HY000", 0,
		      "[Redshift][ODBC Driver]An I/O error occurred while reading client side cursor. Detail: SSL SYSCALL "
		      "error: EOF detected"}})));
	}
}

TEST_CASE("A dropped-connection text quoted inside a server error does not show a lost connection", "[odbc]") {
	SECTION("a value in an invalid-input error") {
		CHECK_FALSE(duckdb::RecordsShowConnectionLoss(
		    StatementRecords({{"22P02", 0,
		                       "[Redshift][ODBC Driver][Server]22P02:ERROR:  invalid input syntax for integer: "
		                       "\"connection not open\""}})));
	}
	SECTION("a relation name") {
		CHECK_FALSE(duckdb::RecordsShowConnectionLoss(StatementRecords(
		    {{"42P01", 0,
		      "[Redshift][ODBC Driver][Server]42P01:ERROR:  relation \"server closed the connection unexpectedly\" "
		      "does not exist"}})));
	}
	SECTION("a raised exception's whole message") {
		CHECK_FALSE(duckdb::RecordsShowConnectionLoss(StatementRecords(
		    {{"P0001", 0, "[Redshift][ODBC Driver][Server]P0001:ERROR:  no connection to the server"}})));
	}
	SECTION("a server error after a cursor read's lead-in") {
		CHECK_FALSE(duckdb::RecordsShowConnectionLoss(StatementRecords(
		    {{"42P01", 0, STREAMING_READ_ERROR + "ERROR:  relation \"connection not open\" does not exist"}})));
	}
}

TEST_CASE("A failure outside class 08 with no dropped-connection text does not show a lost connection", "[odbc]") {
	CHECK_FALSE(duckdb::RecordsShowConnectionLoss(StatementRecords({})));
	CHECK_FALSE(duckdb::RecordsShowConnectionLoss(StatementRecords({{"HY008", 0, "Operation canceled"}})));
	CHECK_FALSE(duckdb::RecordsShowConnectionLoss(
	    StatementRecords({{"57P01", 0, "terminating connection due to administrator command"}})));
	CHECK_FALSE(duckdb::RecordsShowConnectionLoss(StatementRecords({{"HY000", 0, "generic"}, {"53200", 0, "oom"}})));
	// The states the classifier leaves to the real driver's trace do not show the loss by themselves.
	CHECK_FALSE(duckdb::RecordsShowConnectionLoss(StatementRecords({{"57P02", 0, "terminating connection"}})));
	CHECK_FALSE(duckdb::RecordsShowConnectionLoss(StatementRecords({{"57P03", 0, "cannot connect now"}})));
	CHECK_FALSE(duckdb::RecordsShowConnectionLoss(StatementRecords({{"58030", 0, "could not open file"}})));
	CHECK_FALSE(duckdb::RecordsShowConnectionLoss(StatementRecords({{"HYT00", 0, "Timeout expired"}})));
	CHECK_FALSE(duckdb::RecordsShowConnectionLoss(StatementRecords({{"HYT01", 0, "Connection timeout expired"}})));
}
