//===----------------------------------------------------------------------===//
//                         DuckDB - Redshift
//
// test/client/odbc/fake_driver_fixture.hpp
//
// The fixture the ODBC client's offline tests install the fake driver with. Constructing it makes a fresh
// fake the process's driver source; destroying it resets the ODBC client's process state first, so the
// environment is freed and no driver pointer outlives the fake that served the test. It also
// gives a connection's trust search a CA bundle of the test's own, and unsets the end-to-end test
// variables.
//===----------------------------------------------------------------------===//

#pragma once

#include "driver_source_hook.hpp"
#include "duckdb/common/mutex.hpp"
#include "duckdb/common/numeric_utils.hpp"
#include "../fake_driver/fake_odbc_driver.hpp"
#include "../interface/scoped_environment_variable.hpp"
#include "odbc/connection_settings.hpp"
#include "odbc/type_classification.hpp"
#include "redshift_transport.hpp"

#include <algorithm>
#include <chrono>
#include <climits>
#include <cstdlib>
#include <future>
#include <memory>
#include <stdexcept>
#include <string>
#include <thread>
#include <utility>
#include <vector>

#ifdef _WIN32
#include "duckdb/common/windows.hpp"
#include "odbc/windows_text.hpp"

#include <filesystem>
#include <random>
#include <system_error>
#else
#include <unistd.h>
#endif

namespace redshift_test {

//! A non-empty file under $TMPDIR (or /tmp), or on Windows the user's temporary directory, that
//! SSL_CERT_FILE names while this lives, so a connection's trust search resolves to it whatever CA
//! bundles the host has installed. The search checks that the file is a readable, non-empty
//! regular file at a usable path and never parses it, so it holds no real certificate.
class ScopedCaBundle {
public:
	ScopedCaBundle() : path(CreateBundle()), variable("SSL_CERT_FILE", path.c_str()) {
	}
	~ScopedCaBundle() {
#ifdef _WIN32
		std::error_code ignored;
		std::filesystem::remove(std::filesystem::path(duckdb::Utf8ToWide(path)), ignored);
#else
		unlink(path.c_str());
#endif
	}
	ScopedCaBundle(const ScopedCaBundle &) = delete;
	ScopedCaBundle &operator=(const ScopedCaBundle &) = delete;

	//! The bundle's path, in UTF-8, which a connection passes as both TrustStore and CaFile.
	const std::string path;

private:
#ifdef _WIN32
	//! Created exclusively, under a new name in the user's temporary directory, so no file another
	//! process left there is written through; canonical, as on POSIX below.
	static std::string CreateBundle() {
		const auto base = std::filesystem::temp_directory_path();
		const std::string content = "-----BEGIN CERTIFICATE-----\n";
		std::random_device random;
		for (int attempt = 0; attempt < 8; attempt++) {
			const auto name = base / ("redshift-ca-" + std::to_string(random()) + std::to_string(random()) + ".pem");
			const HANDLE file =
			    CreateFileW(name.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_NEW, FILE_ATTRIBUTE_NORMAL, nullptr);
			if (file == INVALID_HANDLE_VALUE) {
				if (GetLastError() == ERROR_FILE_EXISTS) {
					continue;
				}
				break;
			}
			DWORD written = 0;
			const bool whole = WriteFile(file, content.data(), static_cast<DWORD>(content.size()), &written, nullptr) &&
			                   written == content.size();
			CloseHandle(file);
			// Through the wide form, which names the same text under every C++ standard DuckDB takes. A
			// path that does not resolve fails as one that does not convert: removed, then the message.
			std::error_code unresolved;
			const auto canonical = whole ? std::filesystem::canonical(name, unresolved) : std::filesystem::path();
			const auto resolved =
			    whole && !unresolved ? duckdb::WideToUtf8(canonical.wstring()) : duckdb::optional<std::string>();
			if (resolved) {
				return *resolved;
			}
			std::error_code ignored;
			std::filesystem::remove(name, ignored);
			break;
		}
		throw std::runtime_error("the test could not write a CA bundle under " +
		                         duckdb::WideToUtf8(base.wstring()).value_or(std::string()));
	}
#else
	//! Resolved, so a temporary directory under a symbolic link (macOS's /tmp) leaves the path the
	//! tests compare with the one the trust search passes on.
	static std::string CreateBundle() {
		const char *root = std::getenv("TMPDIR");
		std::string name = std::string(root && root[0] == '/' ? root : "/tmp") + "/redshift-ca-XXXXXX";
		const int fd = mkstemp(&name[0]);
		const std::string content = "-----BEGIN CERTIFICATE-----\n";
		const bool written =
		    fd >= 0 && write(fd, content.data(), content.size()) == static_cast<ssize_t>(content.size());
		if (fd >= 0) {
			close(fd);
		}
		char resolved[PATH_MAX];
		if (!written || realpath(name.c_str(), resolved) == nullptr) {
			if (fd >= 0) {
				unlink(name.c_str());
			}
			throw std::runtime_error("the test could not write a CA bundle at " + name);
		}
		return resolved;
	}
#endif

	duckdb::redshift_driver::testing::ScopedEnvironmentVariable variable;
};

//! The end-to-end test variables unset while this lives, so the caller's environment can neither
//! refuse nor redirect a case's connect. A case that needs one sets it after.
class NoTestVariables {
	duckdb::redshift_driver::testing::ScopedEnvironmentVariable test_case {duckdb::TEST_CASE_VARIABLE, nullptr};
	duckdb::redshift_driver::testing::ScopedEnvironmentVariable url {duckdb::TEST_URL_VARIABLE, nullptr};
	duckdb::redshift_driver::testing::ScopedEnvironmentVariable host {duckdb::TEST_HOST_VARIABLE, nullptr};
};

//! Keeps the lines the client logs. Thread-safe, as the interface requires of a sink.
class CapturingLogSink : public duckdb::RedshiftLogSink {
public:
	void Debug(const std::string &message) noexcept override {
		duckdb::lock_guard<duckdb::mutex> guard(lock);
		lines.push_back(message);
	}
	std::vector<std::string> Lines() const {
		duckdb::lock_guard<duckdb::mutex> guard(lock);
		return lines;
	}

private:
	mutable duckdb::mutex lock;
	std::vector<std::string> lines;
};

//! Whether `future` became ready within `timeout`, against the system clock.
template <class T>
bool ReadyWithin(std::future<T> &future, std::chrono::milliseconds timeout) {
	return future.wait_until(std::chrono::system_clock::now() + timeout) == std::future_status::ready;
}

//! The statements a connection runs right after it connects, in order.
inline std::vector<std::string> SessionSetupStatements() {
	return {"SET extra_float_digits TO 2", "SET timezone TO 'UTC'", "SET datestyle TO 'ISO, MDY'",
	        "SET intervalstyle TO 'postgres'"};
}

//! The account-backed Serverless workgroup the tests connect to, in an account whose leading zero
//! must survive as text.
const char *const TEST_WORKGROUP = "analytics";
const char *const TEST_ACCOUNT_ID = "012345678901";

//! TEST_WORKGROUP in `region`, with the default credential chain and the default database.
inline duckdb::RedshiftConfig ServerlessConfig(const std::string &region) {
	duckdb::RedshiftConfig config;
	config.region = region;
	config.target = duckdb::RedshiftTarget::SERVERLESS;
	config.workgroup = TEST_WORKGROUP;
	config.account_id = TEST_ACCOUNT_ID;
	return config;
}

//! ServerlessConfig in us-east-1.
inline duckdb::RedshiftConfig UsEast1Config() {
	return ServerlessConfig("us-east-1");
}

//! The host ResolveTarget derives for ServerlessConfig(region), written out rather than read from
//! the client.
inline std::string ServerlessHost(const std::string &region) {
	return std::string(TEST_WORKGROUP) + "." + TEST_ACCOUNT_ID + "." + region + ".redshift-serverless.amazonaws.com";
}

//! How a connection to ServerlessConfig(region), with `profile` when not empty, names its target.
inline std::string ServerlessTarget(const std::string &region, const std::string &profile = "") {
	return ServerlessHost(region) + " (region " + region + (profile.empty() ? "" : ", profile " + profile) +
	       ", workgroup " + TEST_WORKGROUP + ", account " + TEST_ACCOUNT_ID + ")";
}

//! The provisioned cluster the tests connect to.
const char *const TEST_CLUSTER = "warehouse-1";

//! TEST_CLUSTER in `region`, with the default credential chain and the default database.
inline duckdb::RedshiftConfig ClusterConfig(const std::string &region) {
	duckdb::RedshiftConfig config;
	config.region = region;
	config.target = duckdb::RedshiftTarget::PROVISIONED;
	config.cluster = TEST_CLUSTER;
	return config;
}

//! ClusterConfig(region) named by its endpoint, as a redshift:// URI with `port` names it: label
//! abc123xyz.
inline duckdb::RedshiftConfig ClusterEndpointConfig(const std::string &region, int port) {
	auto config = ClusterConfig(region);
	config.endpoint_label = "abc123xyz";
	config.endpoint_port = port;
	return config;
}

//! How a connection to ClusterConfig(region) names its target: by the identifier, as for every
//! cluster. ClusterConfig names no endpoint, so no host is sent.
inline std::string ClusterTarget(const std::string &region) {
	return std::string(TEST_CLUSTER) + " (region " + region + ", cluster " + TEST_CLUSTER + ")";
}

//! The clock every connection opened after it reads, standing still until the test moves it. Make it
//! after the FakeDriverFixture, whose process-state reset would drop it.
class ManualClock {
public:
	ManualClock() : now(std::make_shared<std::chrono::steady_clock::time_point>()) {
		auto reading = now;
		duckdb::SetConnectClockForTesting([reading] { return *reading; });
	}
	void Advance(std::chrono::milliseconds by) {
		*now += by;
	}

private:
	std::shared_ptr<std::chrono::steady_clock::time_point> now;
};

class FakeDriverFixture {
public:
	FakeDriverFixture() {
		duckdb::SetDriverSourceForTesting(&duckdb::redshift_driver::testing::FakeOdbcDriver::Source);
		// The fake prepares only scripted text, and every connection runs these before it returns. A
		// SET reports no result columns and a row count of 0.
		duckdb::redshift_driver::testing::FakeResultSet no_columns;
		no_columns.row_count = 0;
		for (const auto &sql : SessionSetupStatements()) {
			fake.AddResult(sql, {no_columns});
		}
	}
	~FakeDriverFixture() {
		// A destructor's body runs before its members are destroyed, so this precedes `fake`'s.
		duckdb::ResetProcessStateForTesting();
	}
	FakeDriverFixture(const FakeDriverFixture &) = delete;
	FakeDriverFixture &operator=(const FakeDriverFixture &) = delete;

	//! A connection with UsEast1Config(), logging through `log_sink`.
	duckdb::unique_ptr<duckdb::RedshiftConnection> OpenConnection() {
		return duckdb::OpenRedshiftConnection(UsEast1Config(), log_sink);
	}

	//! The entry points the fake was called on, oldest first.
	std::vector<std::string> EntryPoints() const {
		std::vector<std::string> names;
		for (const auto &call : fake.Calls()) {
			names.push_back(call.entry_point);
		}
		return names;
	}
	//! What each call to `entry_point` logged, oldest first.
	std::vector<std::string> DetailsOf(const std::string &entry_point) const {
		std::vector<std::string> details;
		for (const auto &call : fake.Calls()) {
			if (call.entry_point == entry_point) {
				details.push_back(call.detail);
			}
		}
		return details;
	}
	//! How many calls the fake received to `entry_point`.
	size_t CallsTo(const std::string &entry_point) const {
		const auto names = EntryPoints();
		return duckdb::NumericCast<size_t>(std::count(names.begin(), names.end(), entry_point));
	}
	//! Makes the next call to `entry_point` fail with `rc` and `diags`, counting the calls already
	//! made, the session setup's included.
	void FailNextCall(const char *entry_point, SQLRETURN rc,
	                  std::vector<duckdb::redshift_driver::testing::FakeDiag> diags) {
		fake.FailCall(entry_point, duckdb::NumericCast<int>(CallsTo(entry_point)) + 1, rc, std::move(diags));
	}
	//! The calls the fake received after the first `skipped`, leaving out the diagnostic reads.
	std::vector<std::string> CallsAfter(size_t skipped) const {
		std::vector<std::string> names;
		const auto &calls = fake.Calls();
		for (size_t index = skipped; index < calls.size(); index++) {
			const auto &name = calls[index].entry_point;
			if (name != "SQLGetDiagRec" && name != "SQLGetDiagField") {
				names.push_back(name);
			}
		}
		return names;
	}

	//! The fake this fixture made current. Scripts go through it.
	duckdb::redshift_driver::testing::FakeOdbcDriver fake;
	duckdb::shared_ptr<CapturingLogSink> log_sink = duckdb::make_shared_ptr<CapturingLogSink>();
	//! The trust source every connection this fixture opens resolves to.
	ScopedCaBundle ca_bundle;
	NoTestVariables no_test_variables;
};

//! Waits, against the system clock, until the fake holds no statement handle, as when the producer
//! has read the end of a result's rows and freed its statement.
inline bool StatementFreedWithin(const FakeDriverFixture &fixture, std::chrono::milliseconds timeout) {
	const auto deadline = std::chrono::system_clock::now() + timeout;
	while (fixture.fake.AllocatedHandles() != "environment, connection") {
		if (std::chrono::system_clock::now() > deadline) {
			return false;
		}
		std::this_thread::sleep_for(std::chrono::milliseconds(5));
	}
	return true;
}

//! A VARCHAR column named `name` as the driver reports one declared at `size` bytes: its declared
//! length, under the MaxVarcharSize=0 every connection sets. Above 1024 bytes the column is long.
inline duckdb::redshift_driver::testing::FakeColumn VarcharColumn(const std::string &name, SQLULEN size) {
	duckdb::redshift_driver::testing::FakeColumn column;
	column.name = name;
	column.type_name = "CHARACTER VARYING";
	column.sql_type = SQL_VARCHAR;
	column.column_size = size;
	return column;
}

//! A varchar(65535) named `name`, the longest VARCHAR, which is long, so its cells are read with
//! SQLGetData.
inline duckdb::redshift_driver::testing::FakeColumn LongVarcharColumn(const std::string &name) {
	return VarcharColumn(name, 65535);
}

//! A result of the one `column`, holding `cells`, one row each.
inline duckdb::redshift_driver::testing::FakeResultSet
OneColumn(const duckdb::redshift_driver::testing::FakeColumn &column, const std::vector<std::string> &cells) {
	duckdb::redshift_driver::testing::FakeResultSet result;
	result.columns.push_back(column);
	for (const auto &cell : cells) {
		result.rows.push_back({cell});
	}
	return result;
}

//! The shape Describe reports for `sql` on the current fake, worked out from its script with the
//! client's own classifier rather than through a Describe, so the fake records no call for it. A type
//! name too long for the client's buffer, which the client reads as none, is not modelled.
inline duckdb::RedshiftStatementShape ScriptedShape(const std::string &sql) {
	duckdb::RedshiftStatementShape shape;
	for (const auto &column : duckdb::redshift_driver::testing::FakeOdbcDriver::ScriptedColumns(sql)) {
		shape.columns.push_back(duckdb::ClassifyColumn(
		    {column.name, column.type_name, column.sql_type, column.column_size, column.decimal_digits}));
	}
	shape.returns_rows = !shape.columns.empty();
	return shape;
}

//! Runs `sql` with `params` through `connection`'s Execute, as the SQL surface calls it, passing the
//! shape the SQL surface would have bound: ScriptedShape's, which matches the prepared one, so
//! Execute's shape check passes. A test of that check calls Execute itself.
inline duckdb::unique_ptr<duckdb::RedshiftResult> ExecuteStatement(duckdb::RedshiftConnection &connection,
                                                                   const std::string &sql,
                                                                   const duckdb::vector<std::string> &params) {
	return connection.Execute(sql, params, ScriptedShape(sql));
}

//! The size of each batch `result` returns until NextBatch returns false, and every row in order.
struct ReadBatches {
	explicit ReadBatches(duckdb::RedshiftResult &result) {
		duckdb::RedshiftBatch batch;
		while (result.NextBatch(batch)) {
			sizes.push_back(batch.rows.size());
			rows.insert(rows.end(), batch.rows.begin(), batch.rows.end());
		}
	}
	std::vector<duckdb::idx_t> sizes;
	std::vector<std::vector<duckdb::RedshiftValue>> rows;
};

} // namespace redshift_test
