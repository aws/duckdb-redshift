// Helpers shared by the Catch2 tests.

#pragma once

#include "catch.hpp"

#include <chrono>
#include <condition_variable>
#include <mutex>

#include "duckdb.hpp"
#include "duckdb/common/error_data.hpp"
#include "duckdb/common/string_util.hpp"
#include "redshift_attach.hpp"
#include "storage/redshift_connection_ownership.hpp"

namespace redshift_test {

//! How long a test waits for another thread to reach a point before it fails, rather than hangs.
constexpr auto BOUND = std::chrono::seconds(10);

//! What a call threw, as DuckDB reports it to a user: the exception type, the raw message and the
//! extra_info. `type` is INVALID when the call returned normally.
struct Thrown {
	duckdb::ExceptionType type = duckdb::ExceptionType::INVALID;
	std::string message;
	duckdb::unordered_map<std::string, std::string> extra_info;

	//! The extra_info value for `key`, or "<none>".
	std::string Extra(const std::string &key) const {
		auto entry = extra_info.find(key);
		return entry == extra_info.end() ? "<none>" : entry->second;
	}
};

//! A config with persistent secrets off, so a secret stored in ~/.duckdb/stored_secrets never
//! clashes with a test's. Pass it when constructing the test's DuckDB.
inline duckdb::unique_ptr<duckdb::DBConfig> NoPersistentSecrets() {
	auto config = duckdb::make_uniq<duckdb::DBConfig>();
	config->SetOptionByName("allow_persistent_secrets", duckdb::Value::BOOLEAN(false));
	return config;
}

//! Run `fn` and capture what it throws. Exception::what() is DuckDB's JSON encoding, so the
//! message is read back through ErrorData.
template <class FN>
Thrown Capture(FN &&fn) {
	Thrown thrown;
	try {
		fn();
	} catch (std::exception &ex) {
		duckdb::ErrorData error(ex);
		thrown.type = error.Type();
		thrown.message = error.RawMessage();
		thrown.extra_info = error.ExtraInfo();
	}
	return thrown;
}

//! What a failed query reported, as Capture reports a throw. DuckDB adds the query's location, after
//! a blank line, to an error raised while the statement binds; the message here leaves it out.
inline Thrown FailureOf(duckdb::BaseQueryResult &result) {
	Thrown thrown;
	if (!result.HasError()) {
		return thrown;
	}
	auto &error = result.GetErrorObject();
	thrown.type = error.Type();
	thrown.message = error.RawMessage();
	auto location = thrown.message.find("\n\nLINE ");
	if (location != std::string::npos) {
		thrown.message.resize(location);
	}
	thrown.extra_info = error.ExtraInfo();
	return thrown;
}

//! `result`'s error, empty when it succeeded. GetError() asserts that there is one.
inline std::string ErrorIn(duckdb::BaseQueryResult &result) {
	return result.HasError() ? result.GetError() : std::string();
}

//! Runs `sql`, which must succeed.
inline void Run(duckdb::Connection &con, const std::string &sql) {
	auto result = con.Query(sql);
	INFO(sql << ": " << ErrorIn(*result));
	REQUIRE_FALSE(result->HasError());
}

//! Runs `sql` and returns its error, empty when it succeeded. Safe on any thread, unlike REQUIRE.
inline std::string ErrorOf(duckdb::Connection &con, const std::string &sql) {
	return ErrorIn(*con.Query(sql));
}

//! A configuration a test opens the mock with directly: an account-backed Serverless workgroup in
//! `region`, with the default credential chain. The mock refuses one with no target.
inline duckdb::RedshiftConfig MockConfig(const std::string &region = "us-east-1") {
	duckdb::RedshiftConfig config;
	config.region = region;
	config.target = duckdb::RedshiftTarget::SERVERLESS;
	config.workgroup = "test-wg";
	config.account_id = "012345678901";
	return config;
}

//! MockConfig(region)'s Describe(), which the mock's lines and messages carry.
inline std::string MockTarget(const std::string &region = "us-east-1") {
	return "region " + region + ", workgroup test-wg, account 012345678901";
}

//! The statement that attaches a Redshift handle named `handle` to MockConfig(region)'s target, from
//! options, so its Describe() is MockTarget(region).
inline std::string Attach(const std::string &handle, const std::string &region = "us-east-1") {
	return duckdb::StringUtil::Format("ATTACH '' AS %s (TYPE %s, REGION '%s', CREDENTIAL_PROVIDER aws_default_chain, "
	                                  "WORKGROUP 'test-wg', ACCOUNT_ID '012345678901')",
	                                  handle, duckdb::REDSHIFT_ATTACH_TYPE, region);
}

//! Attach(handle, region) as ATTACH OR REPLACE.
inline std::string AttachOrReplace(const std::string &handle, const std::string &region = "us-east-1") {
	return "ATTACH OR REPLACE" + Attach(handle, region).substr(std::string("ATTACH").size());
}

//! Holds the next call at a test-only pause point, from whichever DuckDB connection makes it, until
//! the test releases it or BOUND passes: the next call after Arm(), or the one after `skip` more,
//! such as an execution's after its bind's. By default the point is the pin request, after the
//! caller found the handle and before it asks for the pin; given another point's setter, such as
//! duckdb::SetRedshiftLostSessionPauseForTesting, it is that point. Installed for the case's
//! lifetime, and removed when the case ends.
//!
//! Its waits are against the system clock, as MockGate's are, for GCC 11's ThreadSanitizer, which
//! does not see a steady-clock wait release its mutex (pthread_cond_clockwait).
class PauseHold {
public:
	using SetPause = void (*)(std::function<void()>);

	explicit PauseHold(SetPause set_pause_p = duckdb::SetRedshiftPinPauseForTesting) : set_pause(set_pause_p) {
		set_pause([this]() { Pause(); });
	}
	~PauseHold() {
		Release();
		set_pause(nullptr);
	}

	//! Holds the next call at the pause point after `skip` more; later ones pass.
	void Arm(duckdb::idx_t skip = 0) {
		std::lock_guard<std::mutex> guard(lock);
		armed = true;
		to_skip = skip;
	}
	//! Returns whether a call arrived at the pause point and is held, within `timeout`.
	bool WaitUntilHeldFor(std::chrono::milliseconds timeout) {
		std::unique_lock<std::mutex> guard(lock);
		return changed.wait_until(guard, std::chrono::system_clock::now() + timeout, [this]() { return held; });
	}
	void Release() {
		std::lock_guard<std::mutex> guard(lock);
		released = true;
		changed.notify_all();
	}

private:
	void Pause() {
		std::unique_lock<std::mutex> guard(lock);
		if (!armed) {
			return;
		}
		if (to_skip > 0) {
			to_skip--;
			return;
		}
		armed = false;
		held = true;
		changed.notify_all();
		// Bounded, so a case that never releases it fails on its own checks rather than hanging.
		changed.wait_until(guard, std::chrono::system_clock::now() + BOUND, [this]() { return released; });
	}

	SetPause set_pause;
	std::mutex lock;
	std::condition_variable changed;
	bool armed = false;
	duckdb::idx_t to_skip = 0;
	bool held = false;
	bool released = false;
};

} // namespace redshift_test
