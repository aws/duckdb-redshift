// The ODBC client's process state (src/transport/odbc/odbc_process_state.cpp) allocates the process's
// environment handle through the loader's AllocateEnvironmentHandle, here in the embedded build, where that
// sets ODBCINI=/dev/null for the one call. Against the client's fake driver, its
// SQLAllocHandle wrapped to record what ODBCINI was at each call, and whether the process state's lock was held
// at the environment's; that the client's refused-variable check waits for that lock; and that the client
// refuses a set variable in RefusedVariableError's words. A REDSHIFT_DRIVER=none build links the plain allocation
// instead, so this lives in the driver suite.

// catch.hpp comes first, as in the client's tests.
#include "catch.hpp"

#include "../odbc/fake_driver_fixture.hpp"
#include "driver_loader_core.hpp"
#include "loader_test.hpp"
#include "odbc/odbc_process_state.hpp"
#include "odbc/trust_store.hpp"

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdio>
#include <cstdlib>
#include <fcntl.h>
#include <mutex>
#include <string>
#include <sys/syscall.h>
#include <thread>
#include <unistd.h>
#include <utility>
#include <vector>

using namespace duckdb;
using namespace duckdb::redshift_driver;
using duckdb::redshift_driver::testing::FakeOdbcDriver;
using redshift_driver_test::Expect;
using redshift_driver_test::InBoundedChild;
using redshift_driver_test::LOCK_CASE_BOUND_SECONDS;
using redshift_driver_test::WAIT_LIMIT;

namespace {

decltype(&::SQLAllocHandle) g_fake_allocate = nullptr;
std::mutex g_record_lock; // two connects allocate at once in the lock test
std::vector<std::pair<SQLSMALLINT, std::string>> g_odbcini_at_allocation; // "(unset)" when it was not set
void (*g_during_environment)() = nullptr; // runs inside the environment's allocation, ODBCINI set

SQLRETURN RecordingAllocate(SQLSMALLINT type, SQLHANDLE input, SQLHANDLE *output) {
	{
		std::lock_guard<std::mutex> guard(g_record_lock);
		const char *odbcini = std::getenv("ODBCINI");
		g_odbcini_at_allocation.emplace_back(type, odbcini != nullptr ? odbcini : "(unset)");
	}
	if (type == SQL_HANDLE_ENV && g_during_environment != nullptr) {
		g_during_environment();
	}
	return g_fake_allocate(type, input, output);
}

// The fake's driver with its SQLAllocHandle recording.
DriverLoadResult RecordingSource() {
	static LoadedDriver wrapped;
	auto result = FakeOdbcDriver::Source();
	if (!result.ok()) {
		return result;
	}
	wrapped = *result.driver;
	g_fake_allocate = result.driver->api.AllocHandle;
	wrapped.api.AllocHandle = RecordingAllocate;
	result.driver = &wrapped;
	return result;
}

// In the environment's allocation: whether another thread could take the process state's lock, which the
// allocation must hold while ODBCINI is set (the next case shows the refused-variable check waits for it).
bool g_state_lock_free_during_environment = true;
void ProbeStateLock() {
	bool taken = false;
	std::thread probe([&] {
		auto &lock = CurrentOdbcProcessState()->lock;
		taken = lock.try_lock();
		if (taken) {
			lock.unlock();
		}
	});
	probe.join();
	g_state_lock_free_during_environment = taken;
}

// The recorded allocations, for a failure's message: each handle type with what ODBCINI was.
std::string Allocations() {
	std::string text;
	for (const auto &allocation : g_odbcini_at_allocation) {
		text += " (" + std::to_string(allocation.first) + ", " + allocation.second + ")";
	}
	return text;
}

// Every case here connects through the client's process state and the loader, and so takes their locks; each runs in a
// bounded child, so a lock that is never released (a self-deadlock on the process state's lock, a thread that never
// finishes) fails its case instead of hanging make test_driver. The child's process state and driver source are its
// own.
//
// The fake's own leak and misuse checks, which its destructor would report only to the child's copy of Catch2, run
// here instead, after the process state's reset that the fixture's destructor also makes.
bool FakeLeftClean(redshift_test::FakeDriverFixture &fixture) {
	ResetProcessStateForTesting();
	const auto leaked = fixture.fake.AllocatedHandles();
	const auto misused = fixture.fake.MisusedAbandonedHandles();
	const bool ok = Expect(leaked.empty(), "the fake still has handles allocated: " + leaked);
	return Expect(misused.empty(), "the fake was called on handles it had abandoned: " + misused) && ok;
}

// The client's connect resolves the host's CA certificates before it loads the driver. On a platform with no trust
// resolver (its build compiles trust_store_unsupported.cpp), every connect stops at that step, before the
// allocation these cases are about, so they warn and stop there.
bool ClientConnects() {
#if defined(__linux__)
	return true;
#else
	struct Quiet : RedshiftLogSink {
		void Debug(const std::string &) noexcept override {
		}
	} sink;
	try {
		ResolveTrustSource(sink);
		return true;
	} catch (const std::exception &error) {
		WARN(std::string("The client's connect stops at its trust store on this platform, before the driver: ") +
		     error.what() + "; not tested");
		return false;
	}
#endif
}

} // namespace

TEST_CASE("The client allocates the process's environment handle with ODBCINI at /dev/null, under the process "
          "state's lock, and nothing else",
          "[driver][environment][odbcini]") {
	if (!ClientConnects()) {
		return;
	}
	CHECK(InBoundedChild(
	    [] {
		    ::unsetenv("ODBCINI");
		    g_odbcini_at_allocation.clear();
		    redshift_test::FakeDriverFixture fixture;
		    SetDriverSourceForTesting(&RecordingSource);
		    g_during_environment = ProbeStateLock;
		    bool ok = true;
		    // Two connections, one after the other: the first allocates the process's environment handle, the
		    // second only a connection handle on it.
		    for (int i = 0; i < 2; i++) {
			    auto connection = fixture.OpenConnection();
			    ok =
			        Expect(std::getenv("ODBCINI") == nullptr, "ODBCINI unset after connect " + std::to_string(i)) && ok;
		    }
		    // The environment first, with ODBCINI at /dev/null; every later allocation (the connections, the session
		    // setup's statements) with it unset.
		    if (!Expect(!g_odbcini_at_allocation.empty() &&
		                    g_odbcini_at_allocation[0] ==
		                        std::make_pair(SQLSMALLINT {SQL_HANDLE_ENV}, std::string("/dev/null")),
		                "the environment first, with ODBCINI at /dev/null:" + Allocations())) {
			    return false;
		    }
		    size_t connections = 0;
		    for (size_t i = 1; i < g_odbcini_at_allocation.size(); i++) {
			    ok = Expect(g_odbcini_at_allocation[i].first != SQL_HANDLE_ENV &&
			                    g_odbcini_at_allocation[i].second == "(unset)",
			                "no other environment, and ODBCINI unset, at allocation " + std::to_string(i) + ":" +
			                    Allocations()) &&
			         ok;
			    connections += g_odbcini_at_allocation[i].first == SQL_HANDLE_DBC ? 1 : 0;
		    }
		    ok = Expect(connections == 2, "two connection handles:" + Allocations()) && ok;
		    ok = Expect(!g_state_lock_free_during_environment,
		                "the process state's lock was free during the environment's allocation") &&
		         ok;
		    ok = Expect(std::getenv("ODBCINI") == nullptr, "ODBCINI unset at the end") && ok;
		    return FakeLeftClean(fixture) && ok;
	    },
	    LOCK_CASE_BOUND_SECONDS));
}

// The lock case below sees a thread blocked through /proc's per-thread syscall file, which only Linux has: on macOS it
// is not built.
#if defined(__linux__)
namespace {

// A connect parked in its sink's "Redshift driver loaded:" line, which the client writes after the load, with the
// process state's lock released, and before the refused-variable check, until the case lets it go on.
std::mutex g_park_lock;
std::condition_variable g_park_changed;
bool g_parked = false, g_released = false, g_left_park = false;

// Against the system clock, not wait_for's steady clock, as FakeGate::WaitUntilHeldFor waits
// (fake_odbc_driver.cpp): libstdc++ 11 waits on a steady clock with pthread_cond_clockwait, which GCC 11's
// ThreadSanitizer does not intercept.
template <class Predicate>
bool WaitForPark(std::unique_lock<std::mutex> &guard, Predicate predicate) {
	return g_park_changed.wait_until(guard, std::chrono::system_clock::now() + WAIT_LIMIT, predicate);
}

class ParkingSink : public RedshiftLogSink {
public:
	void Debug(const std::string &message) noexcept override {
		if (message.rfind("Redshift driver loaded:", 0) != 0) {
			return;
		}
		std::unique_lock<std::mutex> guard(g_park_lock);
		g_parked = true;
		g_park_changed.notify_all();
		WaitForPark(guard, [] { return g_released; });
		g_left_park = true;
		g_park_changed.notify_all();
	}
};

// The start of /proc's syscall file for thread `tid` of this process, in `text`: the number of the call the
// thread is blocked in ("running" when it is not blocked). Read without allocating, so this thread takes no
// allocator lock the other could wait for. False when the file cannot be read.
bool ReadThreadSyscall(pid_t tid, char (&text)[32]) {
	char path[64];
	std::snprintf(path, sizeof(path), "/proc/self/task/%d/syscall", static_cast<int>(tid));
	const int fd = ::open(path, O_RDONLY | O_CLOEXEC);
	if (fd < 0) {
		return false;
	}
	const auto got = ::read(fd, text, sizeof(text) - 1);
	::close(fd);
	text[got > 0 ? got : 0] = '\0';
	return got > 0;
}

// Whether thread `tid` of this process is blocked in a futex wait, which is where a thread waiting for a mutex
// sleeps.
bool BlockedInFutexWait(pid_t tid) {
	char text[32];
	return ReadThreadSyscall(tid, text) && std::strtol(text, nullptr, 10) == SYS_futex;
}

// Until thread `tid` has stayed blocked in a futex wait for 50 ms (true), or `done` is set (false), for at most
// WAIT_LIMIT. Each poll sees the thread asleep in the kernel, so no sleep's length decides the answer; the 50 ms only
// rules out a passing wait.
bool WaitUntilBlocked(pid_t tid, const std::atomic<bool> &done) {
	const auto deadline = std::chrono::steady_clock::now() + WAIT_LIMIT;
	int blocked_polls = 0;
	while (!done && std::chrono::steady_clock::now() < deadline) {
		blocked_polls = BlockedInFutexWait(tid) ? blocked_polls + 1 : 0;
		if (blocked_polls >= 50) {
			return true;
		}
		std::this_thread::sleep_for(std::chrono::milliseconds(1));
	}
	return false;
}

} // namespace

TEST_CASE("A connect's refused-variable check waits for the process state's lock, which the environment "
          "allocation holds",
          "[driver][environment][odbcini]") {
	// The environment's allocation sets ODBCINI=/dev/null for its own duration, under the
	// process state's lock (the case above). With ODBCINI on the refused list, a check that ran during that call
	// would see it and refuse a good connect. Here the case holds that lock with ODBCINI set, as the allocation
	// does, while a connect goes on to its check: the connect must block on the lock, not finish, and once ODBCINI
	// is unset and the lock released, connect. It is seen blocked, or finished, so no timing decides it.
	CHECK(InBoundedChild(
	    [] {
		    // Where the kernel shows no thread's system call (a sandbox without it), say so, rather than fail as if
		    // the check did not wait.
		    char own[32];
		    if (!ReadThreadSyscall(static_cast<pid_t>(::syscall(SYS_gettid)), own)) {
			    return Expect(false, "this host shows no thread's system call in /proc/self/task/<tid>/syscall, which "
			                         "this case reads");
		    }
		    ::unsetenv("ODBCINI");
		    g_parked = g_released = g_left_park = false;
		    redshift_test::FakeDriverFixture fixture;
		    fixture.fake.SetRefusedEnvironment({"ODBCINI"});
		    std::atomic<pid_t> first_tid {0};
		    std::atomic<bool> first_done {false};
		    std::string first_error;
		    std::thread first([&] {
			    first_tid = static_cast<pid_t>(::syscall(SYS_gettid));
			    try {
				    auto connection =
				        OpenRedshiftConnection(redshift_test::UsEast1Config(), make_shared_ptr<ParkingSink>());
			    } catch (const std::exception &error) {
				    first_error = error.what();
			    }
			    first_done = true;
		    });
		    bool parked;
		    {
			    std::unique_lock<std::mutex> guard(g_park_lock);
			    parked = WaitForPark(guard, [] { return g_parked; });
		    }
		    if (!parked) {
			    first.join(); // a failed case, not a std::terminate from a joinable thread
			    return Expect(false,
			                  "the first connect never reached its \"Redshift driver loaded:\" line: " + first_error);
		    }
		    bool left = false, blocked = false;
		    {
			    std::lock_guard<std::mutex> held(CurrentOdbcProcessState()->lock); // as the allocation holds it
			    ::setenv("ODBCINI", "/dev/null", 1);                               // as the allocation sets it
			    {
				    std::unique_lock<std::mutex> guard(g_park_lock);
				    g_released = true;
				    g_park_changed.notify_all();
				    left = WaitForPark(guard, [] { return g_left_park; });
			    }
			    // From here the connect runs on to its check, and blocks there on the lock, or, if the check does
			    // not wait for it, sees ODBCINI set, is refused and finishes.
			    blocked = left && WaitUntilBlocked(first_tid, first_done);
			    ::unsetenv("ODBCINI");
		    }
		    first.join();
		    bool ok = Expect(left, "the first connect did not leave its sink's line");
		    ok = Expect(blocked, "the first connect's check did not wait for the process state's lock" +
		                             (first_done ? ": it finished while the lock was held: " + first_error : "")) &&
		         ok;
		    ok = Expect(first_error.empty(), "the first connect failed: " + first_error) && ok;
		    ok = Expect(std::getenv("ODBCINI") == nullptr, "ODBCINI unset at the end") && ok;
		    return FakeLeftClean(fixture) && ok;
	    },
	    LOCK_CASE_BOUND_SECONDS));
}
#endif

TEST_CASE("The client refuses a set variable in the loader's words", "[driver][environment]") {
	if (!ClientConnects()) {
		return;
	}
	// The sentence is written twice, by the client before every connect and by the loader before the load
	// (RefusedVariableError); a user who meets both must read one sentence.
	CHECK(InBoundedChild(
	    [] {
		    redshift_test::FakeDriverFixture fixture;
		    fixture.fake.SetRefusedEnvironment({"REDSHIFT_TEST_REFUSED_BOTH"});
		    ::setenv("REDSHIFT_TEST_REFUSED_BOTH", "planted-value", 1);
		    std::string client_message;
		    try {
			    auto connection = fixture.OpenConnection();
		    } catch (const std::exception &error) {
			    client_message = error.what();
		    }
		    ::unsetenv("REDSHIFT_TEST_REFUSED_BOTH");
		    const auto loader_message =
		        duckdb::redshift_driver::detail::RefusedVariableError("REDSHIFT_TEST_REFUSED_BOTH").message;
		    bool ok = Expect(client_message.find(loader_message) != std::string::npos,
		                     "the client's refusal holds the loader's: " + client_message);
		    ok = Expect(client_message.find("planted-value") == std::string::npos,
		                "the client's refusal shows no value: " + client_message) &&
		         ok;
		    return FakeLeftClean(fixture) && ok;
	    },
	    LOCK_CASE_BOUND_SECONDS));
}
