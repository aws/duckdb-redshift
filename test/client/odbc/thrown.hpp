//===----------------------------------------------------------------------===//
//                         DuckDB - Redshift
//
// test/client/odbc/thrown.hpp
//
// What a call threw, and the class HY trailer of the client's messages, shared by the Catch2 tests in
// this directory.
//
// The extension's copy in test/mock/cpp/test_util.hpp is not on this suite's include path. This one adds
// operators, and keeps the extension's default type: C++17 still brace-initialises a struct that has one.
//===----------------------------------------------------------------------===//

#pragma once

#include "duckdb/common/error_data.hpp"
#include "duckdb/common/exception.hpp"

#include <ostream>
#include <string>

namespace redshift_test {

//! What a call threw, as DuckDB reports it to a user.
struct Thrown {
	//! The exception type: INVALID when the call returned normally, and also for an exception that is not
	//! DuckDB's; CaptureError's HasError tells the two apart.
	duckdb::ExceptionType type = duckdb::ExceptionType::INVALID;
	//! The message the exception was constructed with, read back through ErrorData.
	std::string message;
};

//! Two of these are equal when the type and the whole message are, so a case checks a refusal
//! against the message it expects rather than a fragment of it.
inline bool operator==(const Thrown &left, const Thrown &right) {
	return left.type == right.type && left.message == right.message;
}

//! So that a failing CHECK prints what was thrown rather than "{?}".
inline std::ostream &operator<<(std::ostream &out, const Thrown &thrown) {
	return out << duckdb::Exception::ExceptionTypeToString(thrown.type) << ": " << thrown.message;
}

//! The advice that ends a class HY statement failure's message, after its retry or check sentence.
const std::string DRIVER_FAILURE_TRAILER =
    " If it fails the same way, report it with the extension and driver versions.";

//! What a failure to reach a workgroup on 5439, named without a port, adds after its hint
//! (TargetHints::default_port).
const std::string DEFAULT_PORT_HINT =
    " With no port in the path, the connection uses 5439; for an endpoint on another "
    "port, give the endpoint with its port as the path, redshift://<endpoint>:<port>.";

//! What DuckDB makes of what `call` threw; INVALID-typed when it returned, and for a std::exception that
//! is not DuckDB's. The one decoding place: Exception::what() is DuckDB's JSON encoding, which only
//! ErrorData reads back.
template <class CALL>
duckdb::ErrorData CaptureError(CALL &&call) {
	try {
		call();
	} catch (const std::exception &caught) {
		return duckdb::ErrorData(caught);
	}
	return duckdb::ErrorData();
}

//! `error`'s extra_info value for `key`, or empty when it has none.
inline std::string InfoOf(const duckdb::ErrorData &error, const std::string &key) {
	const auto found = error.ExtraInfo().find(key);
	return found == error.ExtraInfo().end() ? std::string() : found->second;
}

//! The type and message of what `call` threw.
template <class CALL>
Thrown Capture(CALL &&call) {
	auto error = CaptureError(call);
	return Thrown {error.Type(), error.RawMessage()};
}

} // namespace redshift_test
