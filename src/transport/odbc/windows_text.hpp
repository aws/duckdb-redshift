//===----------------------------------------------------------------------===//
//                         DuckDB - Redshift
//
// src/transport/odbc/windows_text.hpp
//
// UTF-8 to and from the UTF-16 that Windows' wide calls take, for the client's environment reads
// on Windows. Strict (MB_ERR_INVALID_CHARS, WC_ERR_INVALID_CHARS), so text that does not convert is
// reported, where DuckDB's WindowsUtil would replace it silently.
//===----------------------------------------------------------------------===//

#pragma once

#ifdef _WIN32

#include "duckdb/common/optional.hpp"
#include "duckdb/common/string.hpp"
#include "duckdb/common/windows.hpp"

#include <string>

namespace duckdb {

//! `text` as UTF-16, or empty when it is empty or not UTF-8, which names no variable.
inline std::wstring Utf8ToWide(const string &text) {
	const auto size = static_cast<int>(text.size());
	const int length =
	    text.empty() ? 0 : MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, text.data(), size, nullptr, 0);
	if (length <= 0) {
		return std::wstring();
	}
	std::wstring wide(static_cast<size_t>(length), L'\0');
	MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, text.data(), size, &wide[0], length);
	return wide;
}

//! `wide` as UTF-8, or nullopt when it is not UTF-16, such as a value with an unpaired surrogate.
inline optional<string> WideToUtf8(const std::wstring &wide) {
	if (wide.empty()) {
		return string();
	}
	const auto size = static_cast<int>(wide.size());
	const int length =
	    WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, wide.data(), size, nullptr, 0, nullptr, nullptr);
	if (length <= 0) {
		return nullopt;
	}
	string text(static_cast<size_t>(length), '\0');
	WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, wide.data(), size, &text[0], length, nullptr, nullptr);
	return text;
}

} // namespace duckdb

#endif
