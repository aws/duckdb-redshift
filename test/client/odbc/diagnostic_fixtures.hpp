//===----------------------------------------------------------------------===//
//                         DuckDB - Redshift
//
// test/client/odbc/diagnostic_fixtures.hpp
//
// Diagnostic records made the way the client makes them, through the fake driver and the readers,
// and a log sink that keeps what it is given. Include catch.hpp before this header.
//===----------------------------------------------------------------------===//

#pragma once

#include "../fake_driver/fake_handles.hpp"
#include "odbc/odbc_diagnostics.hpp"
#include "redshift_transport.hpp"

#include <mutex>
#include <string>
#include <vector>

namespace redshift_test {

//! The records a SQLDriverConnect that returned `rc` left, read for the connect phase.
inline duckdb::DiagnosticSet ConnectRecords(duckdb::redshift_driver::testing::FakeOdbcDriver &fake, SQLRETURN rc,
                                            std::vector<duckdb::redshift_driver::testing::FakeDiag> diags) {
	const duckdb::redshift_driver::testing::OpenConnection open;
	fake.SetConnectOutcome(rc, std::move(diags));
	REQUIRE(duckdb::redshift_driver::testing::Connect(open.connection, "Host={a}") == rc);
	return duckdb::ReadConnectDiagnostics(open.api, {SQL_HANDLE_DBC, open.connection.value});
}

//! The same, through a fake of its own.
inline duckdb::DiagnosticSet ConnectRecords(SQLRETURN rc,
                                            std::vector<duckdb::redshift_driver::testing::FakeDiag> diags) {
	duckdb::redshift_driver::testing::FakeOdbcDriver fake;
	return ConnectRecords(fake, rc, std::move(diags));
}

//! The records a failed SQLPrepare left, read for the statement phase and redacted against
//! `bound_values`.
inline duckdb::DiagnosticSet StatementRecords(duckdb::redshift_driver::testing::FakeOdbcDriver &fake,
                                              std::vector<duckdb::redshift_driver::testing::FakeDiag> diags,
                                              const duckdb::vector<duckdb::string> &bound_values = {}) {
	const duckdb::redshift_driver::testing::OpenStatement open;
	fake.FailCall("SQLPrepare", 1, SQL_ERROR, std::move(diags));
	REQUIRE(open.api.Prepare(open.statement.value, duckdb::redshift_driver::testing::Text("SELECT 1"), SQL_NTS) ==
	        SQL_ERROR);
	return duckdb::ReadStatementDiagnostics(open.api, {SQL_HANDLE_STMT, open.statement.value}, bound_values);
}

//! The same, through a fake of its own.
inline duckdb::DiagnosticSet StatementRecords(std::vector<duckdb::redshift_driver::testing::FakeDiag> diags) {
	duckdb::redshift_driver::testing::FakeOdbcDriver fake;
	return StatementRecords(fake, std::move(diags));
}

//! The records a SQLPrepare that succeeded with information left, read for the statement phase.
inline duckdb::DiagnosticSet StatementWarnings(std::vector<duckdb::redshift_driver::testing::FakeDiag> diags) {
	duckdb::redshift_driver::testing::FakeOdbcDriver fake;
	const duckdb::redshift_driver::testing::OpenStatement open;
	fake.WarnCall("SQLPrepare", 1, std::move(diags));
	REQUIRE(open.api.Prepare(open.statement.value, duckdb::redshift_driver::testing::Text("SELECT 1"), SQL_NTS) ==
	        SQL_SUCCESS_WITH_INFO);
	return duckdb::ReadStatementDiagnostics(open.api, {SQL_HANDLE_STMT, open.statement.value}, {});
}

//! Keeps every line it is given, in order.
class RecordingSink : public duckdb::RedshiftLogSink {
public:
	void Debug(const duckdb::string &message) noexcept override {
		std::lock_guard<std::mutex> guard(mutex);
		lines.push_back(message);
	}
	std::vector<std::string> Lines() const {
		std::lock_guard<std::mutex> guard(mutex);
		return lines;
	}

private:
	//! Guards lines.
	mutable std::mutex mutex;
	std::vector<std::string> lines;
};

} // namespace redshift_test
