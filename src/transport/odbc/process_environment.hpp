//===----------------------------------------------------------------------===//
//                         DuckDB - Redshift
//
// src/transport/odbc/process_environment.hpp
//
// Reading an environment variable beside the process state's environment allocation. Apart from
// odbc_process_state.hpp, so a file that reads the environment includes no driver header.
//===----------------------------------------------------------------------===//

#pragma once

#include "duckdb/common/optional.hpp"
#include "duckdb/common/string.hpp"

#include <cstdint>
#include <string>

namespace duckdb {

//! Variable `name`'s value, or nullopt when it is unset; an empty value counts as set. Copied under
//! the process state's lock: on Linux and macOS the environment's allocation calls setenv under it,
//! and a setenv can move the array a getenv on another thread is reading. On Windows read from the
//! process's environment block and returned as UTF-8; a value that cannot be read still counts as
//! set, as one control character, which every setting's value rule refuses.
optional<string> ReadEnvironmentVariable(const char *name);

//! How many times a read asks for a value: the size query, then reads that each take the size the
//! one before reported, as another module can grow the value between two calls.
constexpr int ENVIRONMENT_READ_PASSES = 4;

//! What one call of a read reports, as GetEnvironmentVariableW does: the value's length when it fit
//! the buffer, else the capacity it needs; and, for a length of 0, whether the variable is unset.
struct EnvironmentCall {
	uint32_t length;
	bool not_found;
};

//! How a read ended: unset, read whole, or still growing on every pass.
enum class WholeValue { UNSET, READ, STILL_GROWING };

//! A read's end, with the value when it was read whole.
template <class Char>
struct WholeRead {
	WholeValue state;
	std::basic_string<Char> value;
};

//! The value `call(buffer, capacity)` reads, asking again with the capacity it reports while the
//! value has grown. A template over the reader, so a fake drives the loop on every platform.
template <class Char, class Call>
WholeRead<Char> ReadWholeValue(Call &&call) {
	std::basic_string<Char> value;
	uint32_t capacity = 0;
	for (int pass = 0; pass < ENVIRONMENT_READ_PASSES; pass++) {
		value.assign(capacity, Char(0));
		const EnvironmentCall result = call(capacity == 0 ? nullptr : &value[0], capacity);
		if (result.length == 0) {
			// Unset only when the call says so; any other 0 is an empty value, which counts as set.
			return WholeRead<Char> {result.not_found ? WholeValue::UNSET : WholeValue::READ, {}};
		}
		if (result.length < capacity) {
			value.resize(result.length);
			return WholeRead<Char> {WholeValue::READ, value};
		}
		capacity = result.length;
	}
	// Still growing on every pass, so another module is rewriting it now: set, but not read whole.
	return WholeRead<Char> {WholeValue::STILL_GROWING, {}};
}

//! What a variable that is set but cannot be read reads as, so that it still counts as set: the
//! ASCII substitute character, a control byte that every setting's value rule refuses.
constexpr const char *UNUSABLE_ENVIRONMENT_VALUE = "\x1a";

//! `read` as ReadEnvironmentVariable returns it: unset, the value `to_utf8` converts, or the
//! unusable value for one still growing or one `to_utf8` cannot convert (nullopt).
template <class Char, class ToUtf8>
optional<string> EnvironmentValue(const WholeRead<Char> &read, ToUtf8 &&to_utf8) {
	if (read.state == WholeValue::UNSET) {
		return nullopt;
	}
	if (read.state == WholeValue::STILL_GROWING) {
		return string(UNUSABLE_ENVIRONMENT_VALUE);
	}
	const optional<string> text = to_utf8(read.value);
	return text ? *text : string(UNUSABLE_ENVIRONMENT_VALUE);
}

} // namespace duckdb
