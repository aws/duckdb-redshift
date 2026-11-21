//===----------------------------------------------------------------------===//
// The mock client's test controls, for C++ tests in a mock build.
//
// mock_transport.cpp defines them, so they exist only where the mock is compiled in, and
// the SQL surface has no way to reach them: SQL tests drive the mock through the
// directives documented there. A control is added only for a test that needs it, and only to
// reproduce something the real client can do.
//===----------------------------------------------------------------------===//

#pragma once

#include "duckdb/common/common.hpp"
#include "duckdb/common/exception.hpp"
#include "redshift_transport.hpp"

#include <chrono>
#include <condition_variable>
#include <mutex>

namespace duckdb {

class MockGates;

//! Holds one mock connection's open or close until the test releases it. HoldNextMockOpen or
//! HoldNextMockClose arms it; the test and the mock share the gate.
class MockGate {
public:
	//! Returns whether the held open or close arrived within timeout, and never throws, so a test
	//! whose call a regression stops making still releases the gate and joins its threads before it
	//! asserts.
	bool WaitUntilHeldFor(std::chrono::milliseconds timeout);
	//! Lets the held open or close continue; one that arrives later does not wait.
	void Release();

private:
	friend class MockGates;
	//! Run by the held open or close. It waits without a bound, because the test decides when it
	//! proceeds, so a test releases the gate on every path before it asserts.
	void Hold();

	//! Guards held and released; changed signals each change.
	std::mutex lock;
	std::condition_variable changed;
	bool held = false;
	bool released = false;
};

//! Holds the next mock connection open at `gate`, whichever DuckDB connection or thread makes it:
//! OpenRedshiftConnection waits there, before the connection exists, until the test releases the
//! gate. This stands in for the real client's connect, which can take several attempts. Other opens
//! run as usual meanwhile. A gate holds one open; arming another before the next open replaces it.
void HoldNextMockOpen(shared_ptr<MockGate> gate);

//! Holds the next mock connection close at `gate`, whichever connection or thread makes it: its
//! destructor waits there, before it logs the close, until the test releases the gate. This stands
//! in for the real client's teardown, which rolls back and disconnects over the network and can take
//! as long as a dead peer takes to detect. Other closes run as usual meanwhile. A gate holds one
//! close; arming another before the next close replaces it.
void HoldNextMockClose(shared_ptr<MockGate> gate);

//! Fails the next mock connection open, whichever DuckDB connection or thread makes it:
//! OpenRedshiftConnection throws IOException with `message`, as the real client's open does when it
//! cannot reach Redshift or the credentials are refused or have expired, or an exception of `type`, such
//! as an interrupt, for a test of how a caller passes one on, carrying `extra_info`, as the real client's
//! carries `sqlstate` and `sqlstates`. It
//! logs that it could not open a connection first, a line for tests to count, as they count opens.
//! Later opens run as usual. Arming another before the next open replaces it.
void FailNextMockOpen(const string &message, ExceptionType type = ExceptionType::IO,
                      const unordered_map<string, string> &extra_info = {});

//! Disarms a failure FailNextMockOpen armed and no open took, so a test case that stopped before its
//! open cannot fail the next case's. LoggedDatabase, the fixture most cases build their database
//! with, calls it first.
void DisarmMockOpenFailure();

//! The shape a mock connection's Describe reports for `sql`, without a connection, for a test that
//! calls Execute directly, as redshift_query passes the shape it bound: the mock's Execute compares
//! the shape it is given with the one the statement has, as the transport contract requires and the
//! ODBC client does too. Throws as the directive parser does, and refuses nothing
//! Describe would refuse after parsing, such as error=.
RedshiftStatementShape MockDescribedShape(const string &sql);

} // namespace duckdb
