//===----------------------------------------------------------------------===//
//                         DuckDB - Redshift
//
// test/client/odbc/recorded_failures.hpp
//
// The records driver 2.3.0.0 gave for failures produced on the client's own session, kept as the
// fake's scripts. connection_loss_live_test.cpp checks the rule the client applies to them, not one
// shape, as the records of a session's end depend on the target.
//===----------------------------------------------------------------------===//

#pragma once

#include "../fake_driver/fake_odbc_driver.hpp"

#include <vector>

namespace redshift_test {

//! A statement whose session the server ended (pg_terminate_backend on its own pid), in the order
//! the driver listed them in one run, with the connection-dead answer true. libpq's own failure
//! comes first, under the state of the server's FATAL beside it.
const std::vector<duckdb::redshift_driver::testing::FakeDiag> SESSION_ENDED_RECORDS {
    {"08006", 0, "[Redshift][ODBC Driver][Server]08006:SSL error: UNEXPECTED_EOF_WHILE_READING\n", {}},
    {"08006",
     0,
     "[Redshift][ODBC Driver][Server]08006:FATAL:  Connection to the database was lost and could not be "
     "re-established. Please reconnect.\n",
     {}},
    {"57P01",
     0,
     "[Redshift][ODBC Driver][Server]57P01:FATAL:  terminating connection due to administrator command\n",
     {}}};

//! The same statement in another run, as a Serverless workgroup and a provisioned cluster both gave it,
//! with the connection-dead answer true: libpq's failure and the server's FATAL, both under 57P01,
//! without the 08006 FATAL of the first shape.
const std::vector<duckdb::redshift_driver::testing::FakeDiag> SESSION_ENDED_57P01_RECORDS {
    {"57P01", 0, "[Redshift][ODBC Driver][Server]57P01:SSL connection has been closed unexpectedly\n", {}},
    {"57P01",
     0,
     "[Redshift][ODBC Driver][Server]57P01:FATAL:  terminating connection due to administrator command\n",
     {}}};

//! A division by zero the server raised for a row after 4096 had arrived, in a streaming read,
//! with the connection-dead answer false.
const duckdb::redshift_driver::testing::FakeDiag DIVISION_BY_ZERO_RECORD {
    "22012",
    0,
    "[Redshift][ODBC Driver]An I/O error occurred while reading streaming cursor. Detail: ERROR:  division by zero",
    {}};

//! A statement in a transaction an earlier failure aborted, refused at prepare, with the
//! connection-dead answer false.
const duckdb::redshift_driver::testing::FakeDiag ABORTED_TRANSACTION_RECORD {
    "25P02",
    0,
    "[Redshift][ODBC Driver][Server]25P02:ERROR:  current transaction is aborted, commands ignored until end of "
    "transaction block\n",
    {}};

} // namespace redshift_test
