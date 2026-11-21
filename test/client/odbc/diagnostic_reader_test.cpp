// Reading a handle's diagnostic records through the fake driver, each redacted for its phase.

// catch.hpp comes first, as the extension's C++ tests include it: DuckDB's copy declares the
// duckdb_base_std aliases it needs only while no DuckDB header has opened that namespace.
#include "catch.hpp"

#include "diagnostic_fixtures.hpp"
#include "odbc/odbc_diagnostics.hpp"

#include <cstdint>
#include <string>
#include <type_traits>
#include <vector>

using duckdb::DiagnosticPhase;
using duckdb::DiagnosticSet;
using duckdb::ReadConnectDiagnostics;
using duckdb::redshift_driver::testing::Connect;
using duckdb::redshift_driver::testing::FakeDiag;
using duckdb::redshift_driver::testing::FakeOdbcDriver;
using duckdb::redshift_driver::testing::OpenConnection;
using redshift_test::ConnectRecords;
using redshift_test::StatementRecords;

namespace {

//! A message limit above every message here, for a case about a message longer than driver
//! 2.3.0.0 keeps, which that driver never reports.
constexpr size_t LIFTED_LIMIT = 65536;

//! The message of the one record in `set`.
std::string OnlyMessage(const DiagnosticSet &set) {
	REQUIRE(set.records.size() == 1);
	return set.records[0].Message();
}

//! Each record as "SQLSTATE native message", one per line.
std::string Describe(const DiagnosticSet &set) {
	std::string text;
	for (const auto &record : set.records) {
		text += record.SqlState() + " " + std::to_string(record.Native()) + " " + record.Message() + "\n";
	}
	return text;
}

std::vector<FakeDiag> NumberedDiags(int count) {
	std::vector<FakeDiag> diags;
	for (int i = 1; i <= count; i++) {
		diags.push_back({"HY000", i, "record " + std::to_string(i)});
	}
	return diags;
}

} // namespace

TEST_CASE("The records a failed call left are read in order, with their SQLSTATE and native code", "[odbc]") {
	FakeOdbcDriver fake;
	const auto set =
	    ConnectRecords(fake, SQL_ERROR, {{"HY000", 0, "first"}, {"08001", 7, "second"}, {"28000", -1, "third"}});
	CHECK(Describe(set) == "HY000 0 first\n08001 7 second\n28000 -1 third\n");
	CHECK(set.SqlStates() == "HY000,08001,28000");
	CHECK(set.phase == DiagnosticPhase::CONNECT);
	CHECK_FALSE(set.is_incomplete);
}

TEST_CASE("An empty SQLSTATE keeps its place in the list of SQLSTATEs, as unreadable", "[odbc]") {
	FakeOdbcDriver fake;
	CHECK(ConnectRecords(fake, SQL_ERROR, {{"", 0, "first"}, {"08001", 0, "second"}}).SqlStates() == "?????,08001");
}

TEST_CASE("A SQLSTATE that is not five characters from [0-9A-Z] reads as unreadable", "[odbc]") {
	for (const std::string sqlstate : {"", "4200", "42p01", "42-01", "\x1b[2J", "4\n<b", "42\n<b"}) {
		INFO("SQLSTATE of " << sqlstate.size() << " bytes");
		const auto set = StatementRecords({{sqlstate, 0, "text"}});
		REQUIRE(set.records.size() == 1);
		CHECK(set.records[0].SqlState() == "?????");
		CHECK_FALSE(set.records[0].HasReadableSqlState());
	}
	const auto set = StatementRecords({{"42P01", 0, "a"}, {"HY000", 0, "b"}, {"0A000", 0, "c"}});
	CHECK(set.SqlStates() == "42P01,HY000,0A000");
	CHECK(set.records[0].HasReadableSqlState());
}

TEST_CASE("A connect-phase record is redacted with the connect rules", "[odbc]") {
	FakeOdbcDriver fake;
	const auto set = ConnectRecords(fake, SQL_ERROR, {{"HY000", 0, "token " + std::string(40, 'Z') + " PWD=x"}});
	CHECK(Describe(set) == "HY000 0 token <redacted> PWD=<redacted>\n");
}

TEST_CASE("A statement-phase record is redacted against the bound values", "[odbc]") {
	FakeOdbcDriver fake;
	const auto set = StatementRecords(
	    fake, {{"22P02", 0, "invalid input syntax: \"secret-value\" " + std::string(40, 'Z')}}, {"secret-value"});
	CHECK(Describe(set) == "22P02 0 invalid input syntax: \"<param 1>\" " + std::string(40, 'Z') + "\n");
	CHECK(set.phase == DiagnosticPhase::STATEMENT);
}

TEST_CASE("A message longer than the first buffer is read whole before it is redacted", "[odbc]") {
	// A statement record, because a connect record would lose the run of letters as a token.
	FakeOdbcDriver fake;
	fake.SetMessageLimit(LIFTED_LIMIT);
	const auto set = StatementRecords(fake, {{"42601", 0, std::string(1000, 'a') + " AKIAIOSFODNN7EXAMPLE"}});
	CHECK(Describe(set) == "42601 0 " + std::string(1000, 'a') + " <redacted>\n");
}

TEST_CASE("A message of exactly the first buffer's size is read again whole", "[odbc]") {
	FakeOdbcDriver fake;
	fake.SetMessageLimit(LIFTED_LIMIT);
	CHECK(OnlyMessage(StatementRecords(fake, {{"42601", 0, std::string(512, 'a')}})) == std::string(512, 'a'));
}

TEST_CASE("A message whose first buffer ends in white space is read again whole", "[odbc]") {
	FakeOdbcDriver fake;
	fake.SetMessageLimit(LIFTED_LIMIT);
	const auto message = std::string(510, 'a') + " tail";
	CHECK(OnlyMessage(StatementRecords(fake, {{"42601", 0, message}})) == message);
}

TEST_CASE("A bound value that a cut at 32766 bytes splits becomes its marker", "[odbc]") {
	// 32767 bytes: the bound value starts at byte 32757, so the cut keeps 10 of its 11 bytes.
	FakeOdbcDriver fake;
	fake.SetMessageLimit(LIFTED_LIMIT);
	const auto set = StatementRecords(fake, {{"22P02", 0, std::string(32755, 'a') + " secret val1"}}, {"secret val1"});
	CHECK(OnlyMessage(set) == std::string(32755, 'a') + " <param 1>...");
}

TEST_CASE("A cut message with no white space keeps only the cut mark", "[odbc]") {
	FakeOdbcDriver fake;
	fake.SetMessageLimit(LIFTED_LIMIT);
	CHECK(OnlyMessage(StatementRecords(fake, {{"42601", 0, std::string(32767, 'a')}})) == "...");
}

TEST_CASE("A message the driver cut at 326 bytes loses the word the cut may have split", "[odbc]") {
	std::string words;
	for (int i = 0; i < 80; i++) {
		words += "abcd ";
	}
	// The fake keeps 326 bytes, as the driver does: 65 words and the "a" of the 66th.
	std::string expected;
	for (int i = 0; i < 64; i++) {
		expected += "abcd ";
	}
	FakeOdbcDriver fake;
	CHECK(OnlyMessage(ConnectRecords(fake, SQL_ERROR, {{"HY000", 0, words}})) == expected + "abcd...");
}

TEST_CASE("A message the driver cut just after white space is taken as cut, keeping the word before it", "[odbc]") {
	// The fake keeps 326 bytes: 64 words, then "abcde" and the space after it, so no word is split.
	std::string words;
	for (int i = 0; i < 64; i++) {
		words += "abcd ";
	}
	FakeOdbcDriver fake;
	CHECK(OnlyMessage(ConnectRecords(fake, SQL_ERROR, {{"HY000", 0, words + "abcde fgh"}})) == words + "abcde...");
}

TEST_CASE("Only a message of exactly 326 bytes is taken as cut by the driver", "[odbc]") {
	{
		FakeOdbcDriver fake;
		CHECK(OnlyMessage(StatementRecords(fake, {{"HY000", 0, std::string(323, 'a') + " b"}})) ==
		      std::string(323, 'a') + " b");
	}
	FakeOdbcDriver fake;
	fake.SetMessageLimit(LIFTED_LIMIT);
	CHECK(OnlyMessage(StatementRecords(fake, {{"HY000", 0, std::string(325, 'a') + " b"}})) ==
	      std::string(325, 'a') + " b");
}

TEST_CASE("A 300-byte bound value that the driver's 326-byte cut splits becomes its marker", "[odbc]") {
	// The prefix is the one addError writes for a server error with a native SQLSTATE (rsodbc.h
	// PADB_ERROR_PREFIX, rsutil.c addError). The message is 373 bytes, so the cut keeps 254 of the
	// value's 300; white space inside it would leave its earlier words behind a dropped last word.
	std::string value;
	for (int i = 0; i < 24; i++) {
		value += "pass phrase ";
	}
	value += "pass phrase1";
	const std::string start = "[Redshift][ODBC Driver][Server]22P02:invalid input syntax for integer: \"";
	FakeOdbcDriver fake;
	const auto set = StatementRecords(fake, {{"22P02", 0, start + value + "\""}}, {value});
	CHECK(OnlyMessage(set) == start + "<param 1>...");
}

TEST_CASE("A message past 32767 bytes, whose length an SQLSMALLINT cannot hold, is read up to the cut", "[odbc]") {
	std::string words;
	for (int i = 0; i < 8000; i++) {
		words += "abcd ";
	}
	std::string expected;
	for (int i = 0; i < 6552; i++) {
		expected += "abcd ";
	}
	FakeOdbcDriver fake;
	fake.SetMessageLimit(LIFTED_LIMIT);
	CHECK(OnlyMessage(StatementRecords(fake, {{"42601", 0, words}})) == expected + "abcd...");
}

TEST_CASE("A bound value that ends in white space is redacted before the message is trimmed", "[odbc]") {
	FakeOdbcDriver fake;
	const auto set = StatementRecords(fake, {{"22P02", 0, "invalid input syntax: hunter2024 \n"}}, {"hunter2024 "});
	CHECK(OnlyMessage(set) == "invalid input syntax: <param 1>");
}

TEST_CASE("Trailing white space is removed from a message", "[odbc]") {
	FakeOdbcDriver fake;
	const auto set = ConnectRecords(fake, SQL_ERROR, {{"42601", 0, "ERROR:  message\nLINE 1 \t\r\n"}});
	CHECK(Describe(set) == "42601 0 ERROR:  message\nLINE 1\n");
}

TEST_CASE("A handle with no records gives an empty set that is complete", "[odbc]") {
	FakeOdbcDriver fake;
	const OpenConnection open;
	REQUIRE(Connect(open.connection, "Host={a}") == SQL_SUCCESS);
	const auto set = ReadConnectDiagnostics(open.api, {SQL_HANDLE_DBC, open.connection.value});
	CHECK(set.records.empty());
	CHECK(set.SqlStates().empty());
	CHECK_FALSE(set.is_incomplete);
}

TEST_CASE("Without a record count, records are read until the driver has no more", "[odbc]") {
	FakeOdbcDriver fake;
	fake.FailCall("SQLGetDiagField", 1, SQL_ERROR, {});
	const auto set = ConnectRecords(fake, SQL_ERROR, NumberedDiags(2));
	CHECK(Describe(set) == "HY000 1 record 1\nHY000 2 record 2\n");
	CHECK_FALSE(set.is_incomplete);
}

TEST_CASE("A negative record count is read as no count", "[odbc]") {
	FakeOdbcDriver fake;
	fake.SetRecordCount(-5);
	const auto set = ConnectRecords(fake, SQL_ERROR, NumberedDiags(2));
	CHECK(Describe(set) == "HY000 1 record 1\nHY000 2 record 2\n");
	CHECK_FALSE(set.is_incomplete);
	CHECK(ConnectRecords(fake, SQL_ERROR, NumberedDiags(64)).is_incomplete);
}

TEST_CASE("A failed record read marks the set incomplete, keeping the records before it", "[odbc]") {
	FakeOdbcDriver fake;
	fake.FailCall("SQLGetDiagRec", 2, SQL_ERROR, {});
	const auto set = ConnectRecords(fake, SQL_ERROR, NumberedDiags(3));
	CHECK(Describe(set) == "HY000 1 record 1\n");
	CHECK(set.is_incomplete);
}

TEST_CASE("A record the driver counted but did not deliver marks the set incomplete", "[odbc]") {
	FakeOdbcDriver fake;
	fake.FailCall("SQLGetDiagRec", 2, SQL_NO_DATA, {});
	const auto set = ConnectRecords(fake, SQL_ERROR, NumberedDiags(3));
	CHECK(Describe(set) == "HY000 1 record 1\n");
	CHECK(set.is_incomplete);
}

TEST_CASE("A list of 64 records is read whole", "[odbc]") {
	FakeOdbcDriver fake;
	const auto set = ConnectRecords(fake, SQL_ERROR, NumberedDiags(64));
	CHECK(set.records.size() == 64);
	CHECK_FALSE(set.is_incomplete);
}

TEST_CASE("A list longer than 64 records is read up to its 64th record, marked incomplete", "[odbc]") {
	FakeOdbcDriver fake;
	const auto set = ConnectRecords(fake, SQL_ERROR, NumberedDiags(65));
	CHECK(set.records.size() == 64);
	CHECK(set.records.back().Message() == "record 64");
	CHECK(set.is_incomplete);
}

TEST_CASE("Without a record count, a list that reaches 64 records is marked incomplete", "[odbc]") {
	FakeOdbcDriver fake;
	fake.FailCall("SQLGetDiagField", 1, SQL_ERROR, {});
	const auto set = ConnectRecords(fake, SQL_ERROR, NumberedDiags(64));
	CHECK(set.records.size() == 64);
	CHECK(set.is_incomplete);
}

TEST_CASE("A diagnostic record's constructors are not public", "[odbc]") {
	STATIC_REQUIRE_FALSE(std::is_constructible<duckdb::DiagnosticRecord, std::string, int32_t, std::string>::value);
	STATIC_REQUIRE_FALSE(std::is_default_constructible<duckdb::DiagnosticRecord>::value);
}
