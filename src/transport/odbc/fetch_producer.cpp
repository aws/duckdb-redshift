#include "odbc/fetch_producer.hpp"

#include "duckdb/common/exception.hpp"

namespace duckdb {

FetchProducer::~FetchProducer() {
	Stop();
}

void FetchProducer::Start(idx_t sets_p, SetFiller fill) {
	if (thread.joinable()) {
		throw InternalException("a Redshift result started its producer thread while the last one still ran");
	}
	{
		lock_guard<mutex> guard(lock);
		sets = sets_p;
		filled = 0;
		taken = 0;
		released = 0;
		ended = false;
		stop = false;
	}
	thread = std::thread([this, filler = std::move(fill)]() { Run(filler); });
}

void FetchProducer::Run(const SetFiller &fill) {
	for (idx_t next = 0;; next++) {
		{
			unique_lock<mutex> guard(lock);
			// A set is filled again only once NextBatch has built its batch and given it back.
			changed.wait(guard, [&]() { return stop || next - released < sets; });
			if (stop) {
				break;
			}
		}
		const bool more = fill(next % sets);
		{
			lock_guard<mutex> guard(lock);
			filled = next + 1;
		}
		changed.notify_all();
		if (!more) {
			break;
		}
	}
	{
		lock_guard<mutex> guard(lock);
		ended = true;
	}
	changed.notify_all();
}

bool FetchProducer::WaitForFilledSet() {
	unique_lock<mutex> guard(lock);
	changed.wait(guard, [&]() { return filled > taken || ended; });
	if (filled <= taken) {
		return false;
	}
	taken++;
	return true;
}

void FetchProducer::Release() {
	{
		lock_guard<mutex> guard(lock);
		released++;
	}
	changed.notify_all();
}

void FetchProducer::Stop() noexcept {
	{
		lock_guard<mutex> guard(lock);
		stop = true;
	}
	changed.notify_all();
	if (thread.joinable()) {
		// Waits for the driver call in flight: SQLCancel is never called, so a live peer that stops
		// sending holds it.
		thread.join();
	}
}

} // namespace duckdb
