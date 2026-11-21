//===----------------------------------------------------------------------===//
//                         DuckDB - Redshift
//
// src/transport/odbc/fetch_producer.hpp
//
// The thread that makes a result's fetches, cell reads and end-of-rows drain after Execute, filling
// its ring of buffer sets ahead of NextBatch, and the stop flag and join that end it.
//===----------------------------------------------------------------------===//

#pragma once

#include "duckdb/common/common.hpp"
#include "duckdb/common/mutex.hpp"

#include <atomic>
#include <condition_variable>
#include <functional>
#include <thread>

namespace duckdb {

//! Which thread fills a result's buffer sets: its producer thread, or the caller's thread in each
//! NextBatch, which only a test build selects.
enum class FetchThread : uint8_t { PRODUCER, CALLER };

//! One result's producer thread. The connection's shared state block holds it, so the result and
//! the connection both reach it without holding each other, and each result starts it afresh.
class FetchProducer {
public:
	//! Fills the set it is given, making the driver calls, and returns whether another set follows:
	//! false once it has read the end of the rows, stored a failure or seen the stop flag.
	using SetFiller = std::function<bool(idx_t set)>;

	FetchProducer() = default;
	//! Stops and joins a thread still running.
	~FetchProducer();
	FetchProducer(const FetchProducer &) = delete;
	FetchProducer &operator=(const FetchProducer &) = delete;

	//! Starts the thread, which fills sets 0 to `sets` - 1 in turn, each again once Release has given
	//! it back, until `fill` returns false or Stop is called. The thread before it must be joined.
	void Start(idx_t sets, SetFiller fill);
	//! Waits until the next set in turn is filled. False when the thread ended without filling it.
	bool WaitForFilledSet();
	//! Gives the set taken last back to the thread to fill again.
	void Release();
	//! Whether Stop has been called on this thread, which the fill checks between driver calls.
	bool Stopping() const {
		return stop;
	}
	//! Sets the stop flag and joins the thread. A later call finds it joined and does nothing, as
	//! does a call when no thread ran. Its callers run one at a time: the caller destroys every result
	//! before its connection.
	void Stop() noexcept;

private:
	void Run(const SetFiller &fill);

	//! Guards the counts and `ended`; changed signals each change.
	mutex lock;
	std::condition_variable changed;
	idx_t sets = 0;
	//! Sets filled, taken and released since Start.
	idx_t filled = 0;
	idx_t taken = 0;
	idx_t released = 0;
	//! Set when the thread fills no more sets.
	bool ended = false;
	//! Set under `lock`, so the thread's wait sees it, and read without it between driver calls.
	std::atomic<bool> stop {false};
	std::thread thread;
};

} // namespace duckdb
