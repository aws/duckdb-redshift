// What the process keeps across connections: one ODBC environment, allocated once and retried after
// a failure, every connect serialised, the environment variables a connect reads taken under the
// state's lock, and SQL_GETDATA_EXTENSIONS read once.

// catch.hpp comes first, as in this directory's other tests.
#include "catch.hpp"

#include "driver_source_hook.hpp"
#include "duckdb/common/numeric_utils.hpp"
#include "fake_driver_fixture.hpp"
#include "odbc/odbc_process_state.hpp"
#include "odbc/trust_store.hpp"
#include "thrown.hpp"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdlib>
#include <functional>
#include <future>
#include <memory>
#include <mutex>
#include <stdexcept>
#include <string>
#include <thread>
#include <vector>

using namespace duckdb;
using redshift_driver::testing::FakeGate;
using redshift_driver::testing::ScopedEnvironmentVariable;
using redshift_test::CapturingLogSink;
using redshift_test::ExecuteStatement;
using redshift_test::FakeDriverFixture;
using redshift_test::NoTestVariables;
using redshift_test::OneColumn;
using redshift_test::ReadBatches;
using redshift_test::ReadyWithin;
using redshift_test::Thrown;
using redshift_test::VarcharColumn;

namespace {

//! How long a test waits for a call it expects. It bounds the wait for a call that does not come;
//! a connect that deadlocks still hangs the case, whose futures wait for their threads.
const std::chrono::milliseconds EXPECTED_WAIT(10000);
//! How long a test waits for a call it expects not to come. Correct code never makes it, so a
//! short wait cannot fail spuriously; it only bounds how long a regression takes to be seen.
const std::chrono::milliseconds UNEXPECTED_WAIT(200);

//! A query whose one column is long, so its result reads SQL_GETDATA_EXTENSIONS.
const char *const LONG_QUERY = "SELECT c FROM t";

//! The rows LONG_QUERY returns through `connection`, read on its own thread.
std::future<size_t> ReadLongQueryAsync(RedshiftConnection &connection) {
	return std::async(std::launch::async, [&connection]() {
		return ReadBatches(*ExecuteStatement(connection, LONG_QUERY, {})).rows.size();
	});
}

//! Opens a connection on its own thread, keeping it until the future is read.
std::future<unique_ptr<RedshiftConnection>> OpenAsync(FakeDriverFixture &fixture) {
	return std::async(std::launch::async, [&fixture]() { return fixture.OpenConnection(); });
}

//! What two connects started one after the other did while the first was held in its SQLDriverConnect.
struct GatedConnects {
	bool first_held;
	bool second_overtook;
	bool second_followed;
	bool both_returned;
	bool both_opened;
};

//! Starts a connect whose SQLDriverConnect, the fake's call `first_call`, is held, then a second, and
//! releases the first and then the second, so each case checks the same five answers.
GatedConnects ConnectTwiceThroughGates(FakeDriverFixture &fixture, int first_call) {
	auto first_gate = std::make_shared<FakeGate>();
	auto second_gate = std::make_shared<FakeGate>();
	fixture.fake.BlockCall("SQLDriverConnect", first_call, first_gate);
	fixture.fake.BlockCall("SQLDriverConnect", first_call + 1, second_gate);

	GatedConnects answers {};
	auto first = OpenAsync(fixture);
	answers.first_held = first_gate->WaitUntilHeldFor(EXPECTED_WAIT);
	auto second = OpenAsync(fixture);
	answers.second_overtook = second_gate->WaitUntilHeldFor(UNEXPECTED_WAIT);
	first_gate->Release();
	answers.second_followed = second_gate->WaitUntilHeldFor(EXPECTED_WAIT);
	second_gate->Release();
	answers.both_returned = ReadyWithin(first, EXPECTED_WAIT) && ReadyWithin(second, EXPECTED_WAIT);
	answers.both_opened = answers.both_returned && first.get() != nullptr && second.get() != nullptr;
	return answers;
}

//! Checks that the second connect waited for the first and that both opened.
void CheckSerialised(const GatedConnects &answers) {
	CHECK(answers.first_held);
	CHECK_FALSE(answers.second_overtook);
	CHECK(answers.second_followed);
	CHECK(answers.both_returned);
	CHECK(answers.both_opened);
}

//! `read`, started on its own thread while this one holds the process state's lock. `change` runs
//! once `read` has had UNEXPECTED_WAIT to finish, then the lock is released, so a read that waits
//! for the lock sees the change; `finished_early` says whether `read` did not wait.
template <class T>
std::future<T> ReadAcrossLockedChange(std::function<T()> read, const std::function<void()> &change,
                                      bool &finished_early) {
	// Declared before the lock, so a `change` that throws releases the lock before the read, which
	// needs it, is waited for.
	std::future<T> reading;
	std::lock_guard<std::mutex> held(CurrentOdbcProcessState()->lock);
	reading = std::async(std::launch::async, std::move(read));
	finished_early = ReadyWithin(reading, UNEXPECTED_WAIT);
	change();
	return reading;
}

} // namespace

TEST_CASE("Connections share one environment, which the first allocates", "[odbc]") {
	FakeDriverFixture fixture;
	auto first = fixture.OpenConnection();
	auto second = fixture.OpenConnection();

	CHECK(fixture.fake.AllocatedHandles() == "environment, connection, connection");
	CHECK(fixture.CallsTo("SQLSetEnvAttr") == 1);
}

TEST_CASE("A destroyed connection leaves the process's environment allocated", "[odbc]") {
	FakeDriverFixture fixture;
	{ auto connection = fixture.OpenConnection(); }

	CHECK(fixture.fake.AllocatedHandles() == "environment");
}

TEST_CASE("The test-only reset frees the environment through the driver", "[odbc]") {
	FakeDriverFixture fixture;
	{ auto connection = fixture.OpenConnection(); }

	ResetProcessStateForTesting();
	CHECK(fixture.fake.AllocatedHandles().empty());
	CHECK(fixture.EntryPoints().back() == "SQLFreeHandle");
}

TEST_CASE("A failed first environment allocation is retried by the next connect, with one driver load", "[odbc]") {
	FakeDriverFixture fixture;
	fixture.fake.FailCall("SQLAllocHandle", 1, SQL_ERROR, {});

	CHECK(redshift_test::Capture([&] { fixture.OpenConnection(); }) ==
	      Thrown {ExceptionType::IO,
	              "the Redshift driver could not allocate an ODBC environment; run the statement again"});
	auto connection = fixture.OpenConnection();
	CHECK(fixture.fake.AllocatedHandles() == "environment, connection");
	CHECK(fixture.fake.SourceCalls() == 1);
}

TEST_CASE("A refused ODBC version frees the environment, and the next connect allocates another", "[odbc]") {
	FakeDriverFixture fixture;
	fixture.fake.FailCall("SQLSetEnvAttr", 1, SQL_ERROR, {});

	CHECK(redshift_test::Capture([&] { fixture.OpenConnection(); }) ==
	      Thrown {ExceptionType::IO, "the Redshift driver refused ODBC version 3, which this extension needs; install "
	                                 "a published release of the extension"});
	CHECK(fixture.fake.AllocatedHandles().empty());
	auto connection = fixture.OpenConnection();
	CHECK(fixture.CallsTo("SQLSetEnvAttr") == 2);
}

TEST_CASE("Eight threads opening connections at once each connect, sharing one environment and one load", "[odbc]") {
	FakeDriverFixture fixture;
	const int thread_count = 8;
	std::vector<unique_ptr<RedshiftConnection>> connections(thread_count);
	std::vector<std::string> failures(thread_count);
	std::atomic<int> ready {0};
	std::vector<std::thread> threads;
	for (int index = 0; index < thread_count; index++) {
		threads.emplace_back([&, index]() {
			// Each waits for the others to start, so the connects overlap.
			ready++;
			while (ready < thread_count) {
				std::this_thread::yield();
			}
			try {
				connections[NumericCast<size_t>(index)] = fixture.OpenConnection();
			} catch (const std::exception &caught) {
				failures[NumericCast<size_t>(index)] = ErrorData(caught).RawMessage();
			}
		});
	}
	for (auto &thread : threads) {
		thread.join();
	}

	CHECK(failures == std::vector<std::string>(thread_count));
	CHECK(fixture.fake.SourceCalls() == 1);
	CHECK(fixture.CallsTo("SQLSetEnvAttr") == 1);
	CHECK(fixture.CallsTo("SQLDriverConnect") == thread_count);
}

TEST_CASE("A connect waits while the process's first connect is still connecting", "[odbc]") {
	FakeDriverFixture fixture;

	CheckSerialised(ConnectTwiceThroughGates(fixture, 1));
}

TEST_CASE("Once a connect has succeeded, later connects still wait for each other", "[odbc]") {
	FakeDriverFixture fixture;
	auto connected = fixture.OpenConnection();

	CheckSerialised(ConnectTwiceThroughGates(fixture, 2));
}

TEST_CASE("After a failed first connect, the next connect is still serialised", "[odbc]") {
	FakeDriverFixture fixture;
	fixture.fake.FailCall("SQLDriverConnect", 1, SQL_ERROR, {});
	CHECK_THROWS_AS(fixture.OpenConnection(), IOException);

	CheckSerialised(ConnectTwiceThroughGates(fixture, 2));
}

TEST_CASE("Two tests in a row, each with its own fake, both pass the fake's leak check", "[odbc]") {
	// The fake's destructor fails the case when a handle is still allocated, which the environment
	// would be without the reset each fixture runs on teardown.
	for (int round = 0; round < 2; round++) {
		FakeDriverFixture fixture;
		auto connection = fixture.OpenConnection();
		CHECK(fixture.fake.SourceCalls() == 1);
		connection.reset();
	}
}

// Each later result waits for the lock the first holds through its read, then finds the value
// cached. Without the lock each reads it too, which test_client_tsan reports as a race.
TEST_CASE("Results with a long column on four threads read SQL_GETDATA_EXTENSIONS once", "[odbc]") {
	FakeDriverFixture fixture;
	fixture.fake.AddResult(LONG_QUERY, {OneColumn(VarcharColumn("c", 0), {"x"})});
	const size_t thread_count = 4;
	std::vector<std::shared_ptr<FakeGate>> gates;
	std::vector<unique_ptr<RedshiftConnection>> connections;
	for (size_t index = 0; index < thread_count; index++) {
		gates.push_back(std::make_shared<FakeGate>());
		fixture.fake.BlockCall("SQLGetInfo", NumericCast<int>(index + 1), gates.back());
		connections.push_back(fixture.OpenConnection());
	}

	std::vector<std::future<size_t>> reads;
	reads.push_back(ReadLongQueryAsync(*connections[0]));
	const bool first_held = gates[0]->WaitUntilHeldFor(EXPECTED_WAIT);
	for (size_t index = 1; index < thread_count; index++) {
		reads.push_back(ReadLongQueryAsync(*connections[index]));
	}
	const bool second_read = gates[1]->WaitUntilHeldFor(UNEXPECTED_WAIT);
	for (auto &gate : gates) {
		gate->Release();
	}
	size_t rows = 0;
	bool all_returned = true;
	for (auto &read : reads) {
		all_returned = all_returned && ReadyWithin(read, EXPECTED_WAIT);
		rows += all_returned ? read.get() : 0;
	}

	CHECK(first_held);
	CHECK_FALSE(second_read);
	CHECK(all_returned);
	CHECK(rows == thread_count);
	CHECK(fixture.CallsTo("SQLGetInfo") == 1);
	const auto lines = fixture.log_sink->Lines();
	const std::string logged = "Redshift driver's SQL_GETDATA_EXTENSIONS: 0xb";
	CHECK(std::count(lines.begin(), lines.end(), logged) == 1);
}

TEST_CASE("A change that throws across a locked read releases the lock before the read is waited for", "[odbc]") {
	// The read gives up after EXPECTED_WAIT, so a lock still held fails this case instead of hanging it.
	std::atomic<bool> locked {false};
	const auto read = [&locked] {
		auto &lock = CurrentOdbcProcessState()->lock;
		const auto deadline = std::chrono::system_clock::now() + EXPECTED_WAIT;
		while (!locked && std::chrono::system_clock::now() < deadline) {
			if (lock.try_lock()) {
				lock.unlock();
				locked = true;
			} else {
				std::this_thread::sleep_for(std::chrono::milliseconds(1));
			}
		}
		return true;
	};
	const auto fail = [] {
		throw std::runtime_error("change failed");
	};
	bool finished_early = false;
	CHECK_THROWS_WITH(ReadAcrossLockedChange<bool>(read, fail, finished_early), "change failed");
	CHECK(locked);
}

// On Linux and macOS the environment's allocation calls setenv under the state's lock, and a setenv
// can move the array a getenv on another thread is reading, so each variable a connect reads is read
// under that lock; Windows reads the environment block under it too.
#if defined(__linux__) || defined(__APPLE__)
TEST_CASE("The trust search reads SSL_CERT_FILE under the process state's lock", "[odbc]") {
	redshift_test::ScopedCaBundle later;
	redshift_test::ScopedCaBundle first;
	CapturingLogSink log_sink;
	const auto read = [&] {
		return ResolveTrustSource(log_sink);
	};
	unique_ptr<ScopedEnvironmentVariable> changed;
	const auto set_later = [&] {
		changed = make_uniq<ScopedEnvironmentVariable>("SSL_CERT_FILE", later.path.c_str());
	};
	bool finished_early = false;
	auto reading = ReadAcrossLockedChange<TrustSource>(read, set_later, finished_early);

	CHECK_FALSE(finished_early);
	REQUIRE(ReadyWithin(reading, EXPECTED_WAIT));
	CHECK(reading.get().path == later.path);
}
#endif

TEST_CASE("A build without the test-endpoint override reads their variables under the process state's lock", "[odbc]") {
	NoTestVariables none;
	const auto read = [] {
		return redshift_test::Capture([] { ReadTestEndpoints(false); });
	};
	unique_ptr<ScopedEnvironmentVariable> changed;
	const auto set_case = [&] {
		changed = make_uniq<ScopedEnvironmentVariable>(TEST_CASE_VARIABLE, "set");
	};
	bool finished_early = false;
	auto reading = ReadAcrossLockedChange<Thrown>(read, set_case, finished_early);

	CHECK_FALSE(finished_early);
	REQUIRE(ReadyWithin(reading, EXPECTED_WAIT));
	CHECK(reading.get().type == ExceptionType::INVALID_INPUT);
}

TEST_CASE("A build with the test-endpoint override reads their variables under the process state's lock", "[odbc]") {
	NoTestVariables none;
	const auto read = [] {
		return ReadTestEndpoints(true);
	};
	unique_ptr<ScopedEnvironmentVariable> url;
	unique_ptr<ScopedEnvironmentVariable> host;
	const auto set_endpoints = [&] {
		url = make_uniq<ScopedEnvironmentVariable>(TEST_URL_VARIABLE, "https://url.example");
		host = make_uniq<ScopedEnvironmentVariable>(TEST_HOST_VARIABLE, "host.example");
	};
	bool finished_early = false;
	auto reading = ReadAcrossLockedChange<TestEndpoints>(read, set_endpoints, finished_early);

	CHECK_FALSE(finished_early);
	REQUIRE(ReadyWithin(reading, EXPECTED_WAIT));
	const auto endpoints = reading.get();
	CHECK(endpoints.host == "host.example");
	CHECK(endpoints.endpoint_url == "https://url.example");
}
