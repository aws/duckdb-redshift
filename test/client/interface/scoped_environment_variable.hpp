//===----------------------------------------------------------------------===//
//                         DuckDB - Redshift
//
// test/client/interface/scoped_environment_variable.hpp
//
// An RAII guard over one environment variable, shared by the interface tests that read the
// environment through the driver interface.
//===----------------------------------------------------------------------===//

#pragma once

#include <cstdlib>
#include <stdexcept>
#include <string>

#if defined(_WIN32)
#include "duckdb/common/windows.hpp"
#include "duckdb/common/windows_undefs.hpp"
#endif

namespace duckdb {
namespace redshift_driver {
namespace testing {

#if defined(_WIN32)
//! `text`, UTF-8, as the UTF-16 Windows' wide calls take; empty when it is empty or not UTF-8; the
//! guard refuses such text.
inline std::wstring WideText(const char *text) {
	const int length = MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, text, -1, nullptr, 0);
	if (length <= 1) {
		return std::wstring();
	}
	std::wstring wide(static_cast<size_t>(length), L'\0');
	MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, text, -1, &wide[0], length);
	wide.resize(static_cast<size_t>(length - 1));
	return wide;
}

//! GetEnvironmentVariableW's form, so a test can grow a value between two of its calls.
using EnvironmentReader = DWORD(WINAPI *)(LPCWSTR name, LPWSTR buffer, DWORD size);

//! Whether `name` is in the process's environment block, with its value in `value` when it is.
//! A size query, then reads at the size the call before reported, since another module can grow
//! the value between two calls; a value still growing after the last pass, set but never read
//! whole, is one substitute character (U+001A), which no test value holds.
inline bool ReadEnvironmentBlock(const std::wstring &name, std::wstring &value,
                                 EnvironmentReader reader = GetEnvironmentVariableW) {
	// Bounded, so a writer that keeps growing the value cannot keep the read looping.
	constexpr int READ_PASSES = 4;
	DWORD capacity = 0;
	for (int pass = 0; pass < READ_PASSES; pass++) {
		value.assign(capacity, L'\0');
		SetLastError(ERROR_SUCCESS);
		const DWORD length = reader(name.c_str(), capacity == 0 ? nullptr : &value[0], capacity);
		if (length == 0) {
			// Unset only when the call says so; any other 0 is an empty value, which counts as set.
			value.clear();
			return GetLastError() != ERROR_ENVVAR_NOT_FOUND;
		}
		if (length < capacity) {
			value.resize(length);
			return true;
		}
		capacity = length;
	}
	value.assign(1, L'\x1a');
	return true;
}
#endif

//! Holds one environment variable for a scope, so no test sees another's environment. On Windows
//! the earlier state is read from the process's environment block, and both the block and this
//! test binary's C runtime copy are written and restored, since the interface reads the block.
class ScopedEnvironmentVariable {
public:
	//! Sets `name_p` to `value`, or unsets it when `value` is nullptr.
	ScopedEnvironmentVariable(const char *name_p, const char *value) : name(name_p) {
#if defined(_WIN32)
		// Before any read or write, so a refused name or value changes nothing.
		const auto wide_name = WideText(name_p);
		if (wide_name.empty() || (value != nullptr && *value != '\0' && WideText(value).empty())) {
			throw std::invalid_argument("ScopedEnvironmentVariable takes UTF-8 names and values");
		}
		had_previous = ReadEnvironmentBlock(wide_name, previous_value);
		Write(value ? WideText(value) : std::wstring(), value != nullptr);
#else
		const char *previous = std::getenv(name.c_str());
		had_previous = previous != nullptr;
		previous_value = had_previous ? previous : "";
		if (value == nullptr) {
			unsetenv(name.c_str());
		} else {
			setenv(name.c_str(), value, 1);
		}
#endif
	}
	//! Restores the variable to what it was before construction.
	~ScopedEnvironmentVariable() {
#if defined(_WIN32)
		Write(previous_value, had_previous);
#else
		if (had_previous) {
			setenv(name.c_str(), previous_value.c_str(), 1);
		} else {
			unsetenv(name.c_str());
		}
#endif
	}
	ScopedEnvironmentVariable(const ScopedEnvironmentVariable &) = delete;
	ScopedEnvironmentVariable &operator=(const ScopedEnvironmentVariable &) = delete;

private:
#if defined(_WIN32)
	//! _wputenv_s writes both copies, but given an empty value it removes the name from both. So the
	//! name is removed first, a value that is not empty goes to both copies, and the block is then
	//! written on its own: unset, the value, or an empty value, which the block alone can hold.
	void Write(const std::wstring &value, bool set) const {
		const auto wide_name = WideText(name.c_str());
		_wputenv_s(wide_name.c_str(), L"");
		if (set && !value.empty()) {
			_wputenv_s(wide_name.c_str(), value.c_str());
		}
		SetEnvironmentVariableW(wide_name.c_str(), set ? value.c_str() : nullptr);
	}
#endif

	std::string name;
	bool had_previous;
#if defined(_WIN32)
	std::wstring previous_value;
#else
	std::string previous_value;
#endif
};

} // namespace testing
} // namespace redshift_driver
} // namespace duckdb
