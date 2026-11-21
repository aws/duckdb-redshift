#include "driver_source_hook.hpp"

#include "odbc/odbc_process_state.hpp"

namespace duckdb {

void SetDriverSourceForTesting(redshift_driver::DriverSource source) {
	ResetProcessStateForTesting();
	CurrentOdbcProcessState()->source = source;
}

void ResetProcessStateForTesting() {
	auto &state = CurrentOdbcProcessState();
	// Only a loaded driver's api allocates the environment, so the driver is there to free it.
	if (state->environment != SQL_NULL_HANDLE) {
		state->driver->api.FreeHandle(SQL_HANDLE_ENV, state->environment);
	}
	// Replaced whole rather than cleared, so no test inherits another's state.
	state = make_uniq<OdbcProcessState>();
}

void SetConnectClockForTesting(ConnectClock clock) {
	auto &state = CurrentOdbcProcessState();
	lock_guard<mutex> serialised(state->connect_lock);
	state->connect_clock = std::move(clock);
}

void SetNumberBindingForTesting(NumberBinding binding) {
	auto &state = CurrentOdbcProcessState();
	lock_guard<mutex> serialised(state->connect_lock);
	state->number_binding = binding;
}

void SetFetchThreadForTesting(FetchThread thread) {
	auto &state = CurrentOdbcProcessState();
	lock_guard<mutex> serialised(state->connect_lock);
	state->fetch_thread = thread;
}

} // namespace duckdb
