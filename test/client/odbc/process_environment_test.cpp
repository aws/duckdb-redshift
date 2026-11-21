// ReadEnvironmentVariable on its own: unset, set to empty, removed, a UTF-8 value, and an
// end-to-end test variable it reads for the refusal. On Windows each is set the way a host's own
// runtime sets one after the extension has loaded, in the process's environment block alone. Its
// pass loop and what each end reads as, ReadWholeValue and EnvironmentValue, are driven by a fake
// reader on every platform.

// catch.hpp comes first, as in this directory's other tests.
#include "catch.hpp"

#include "duckdb/common/exception.hpp"
#include "odbc/connection_settings.hpp"
#include "odbc/process_environment.hpp"
#include "thrown.hpp"

#include <cstdint>
#include <cstdlib>
#include <functional>
#include <string>

#ifdef _WIN32
#include "odbc/windows_text.hpp"
#endif

using duckdb::EnvironmentCall;
using duckdb::EnvironmentValue;
using duckdb::ExceptionType;
using duckdb::ReadEnvironmentVariable;
using duckdb::ReadWholeValue;
using duckdb::WholeValue;
using redshift_test::Capture;
using redshift_test::Thrown;

namespace {

const char *const NAME = "REDSHIFT_TEST_READ_ENVIRONMENT";

//! Sets `name` to `value`, or removes it for nullptr: on Windows in the environment block alone,
//! with SetEnvironmentVariableW, so this test's C runtime copy, like the extension's, is unchanged.
void SetVariable(const char *name, const char *value) {
#ifdef _WIN32
	const auto wide = value ? duckdb::Utf8ToWide(value) : std::wstring();
	REQUIRE(SetEnvironmentVariableW(duckdb::Utf8ToWide(name).c_str(), value ? wide.c_str() : nullptr) != 0);
#else
	REQUIRE((value ? ::setenv(name, value, 1) : ::unsetenv(name)) == 0);
#endif
}

//! `name` unset when the case starts and when it ends.
class UnsetAround {
public:
	explicit UnsetAround(const char *name_p) : name(name_p) {
		SetVariable(name, nullptr);
	}
	~UnsetAround() {
		SetVariable(name, nullptr);
	}
	UnsetAround(const UnsetAround &) = delete;
	UnsetAround &operator=(const UnsetAround &) = delete;

private:
	const char *name;
};

//! A variable read the way GetEnvironmentVariableW reads one, with `after_call` run after each
//! call, so a case can change the value between two calls.
struct FakeVariable {
	bool set = true;
	std::u16string value;
	std::function<void(FakeVariable &)> after_call;
	int calls = 0;

	EnvironmentCall Read(char16_t *buffer, uint32_t capacity) {
		calls++;
		EnvironmentCall result {0, !set};
		const auto length = static_cast<uint32_t>(value.size());
		if (set && length < capacity) {
			value.copy(buffer, length);
			buffer[length] = u'\0';
			result.length = length;
		} else if (set && length > 0) {
			// Too small a buffer: the capacity the value and its terminator need.
			result.length = length + 1;
		}
		if (after_call) {
			after_call(*this);
		}
		return result;
	}
};

//! `variable` read through ReadWholeValue.
duckdb::WholeRead<char16_t> ReadWhole(FakeVariable &variable) {
	return ReadWholeValue<char16_t>(
	    [&](char16_t *buffer, uint32_t capacity) { return variable.Read(buffer, capacity); });
}

//! A stand-in for WideToUtf8 that converts only an empty value, as it does, and u"abc", so a case
//! can give a value that does not convert.
duckdb::optional<std::string> AbcOnly(const std::u16string &value) {
	if (value.empty()) {
		return std::string();
	}
	if (value != u"abc") {
		return duckdb::nullopt;
	}
	return std::string("abc");
}

} // namespace

TEST_CASE("ReadEnvironmentVariable tells an unset variable from one set to an empty value", "[odbc]") {
	UnsetAround unset(NAME);
	CHECK_FALSE(ReadEnvironmentVariable(NAME).has_value());
	SetVariable(NAME, "");
#ifdef _WIN32
	// A stale not-found error from an earlier call must not make the empty value read as unset.
	SetLastError(ERROR_ENVVAR_NOT_FOUND);
#endif
	const auto value = ReadEnvironmentVariable(NAME);
	REQUIRE(value.has_value());
	CHECK(value->empty());
	SetVariable(NAME, nullptr);
	CHECK_FALSE(ReadEnvironmentVariable(NAME).has_value());
}

TEST_CASE("ReadEnvironmentVariable returns a UTF-8 value whole, however long", "[odbc]") {
	UnsetAround unset(NAME);
	SetVariable(NAME, "C:\\Users\\j\xc3\xa9r\xc3\xb4me\\ca.pem");
	CHECK(ReadEnvironmentVariable(NAME) == std::string("C:\\Users\\j\xc3\xa9r\xc3\xb4me\\ca.pem"));
#ifdef _WIN32
	CHECK(std::getenv(NAME) == nullptr);
#endif
	const std::string long_value(5000, 'v');
	SetVariable(NAME, long_value.c_str());
	CHECK(ReadEnvironmentVariable(NAME) == long_value);
}

TEST_CASE("An end-to-end test variable that ReadEnvironmentVariable sees is refused", "[odbc]") {
	UnsetAround unset(duckdb::TEST_CASE_VARIABLE);
	SetVariable(duckdb::TEST_CASE_VARIABLE, "");
	CHECK(Capture([] { duckdb::ReadTestEndpoints(false); }) ==
	      Thrown {ExceptionType::INVALID_INPUT,
	              "DUCKDB_REDSHIFT_TEST_E2E is set, but this build has no test-endpoint override, so the connection "
	              "would go to the endpoint the target derives and is refused. Unset it to connect."});
}

TEST_CASE("A value that grows on every pass is still growing after exactly four calls", "[odbc]") {
	FakeVariable variable;
	variable.value = u"a";
	variable.after_call = [](FakeVariable &grown) {
		grown.value += u"bc";
	};
	const auto read = ReadWhole(variable);
	CHECK(read.state == WholeValue::STILL_GROWING);
	CHECK(read.value.empty());
	// The size query and three reads, written out so that a change to the pass count fails here.
	CHECK(variable.calls == 4);
}

TEST_CASE("A value that grows once after the size query is read whole", "[odbc]") {
	FakeVariable variable;
	variable.value = u"abc";
	variable.after_call = [](FakeVariable &grown) {
		if (grown.calls == 1) {
			grown.value = u"abcdef";
		}
	};
	const auto read = ReadWhole(variable);
	CHECK(read.state == WholeValue::READ);
	CHECK(read.value == u"abcdef");
	CHECK(variable.calls == 3);
}

TEST_CASE("A value removed after the size query reads as unset", "[odbc]") {
	FakeVariable variable;
	variable.value = u"abc";
	variable.after_call = [](FakeVariable &removed) {
		removed.set = false;
	};
	const auto read = ReadWhole(variable);
	CHECK(read.state == WholeValue::UNSET);
	CHECK(variable.calls == 2);
}

TEST_CASE("A length of 0 for a variable that is found reads as an empty value", "[odbc]") {
	FakeVariable variable;
	const auto read = ReadWhole(variable);
	CHECK(read.state == WholeValue::READ);
	CHECK(read.value.empty());
	CHECK(variable.calls == 1);
}

TEST_CASE("A value that grows on every pass reads as one control character", "[odbc]") {
	FakeVariable variable;
	variable.value = u"abc";
	variable.after_call = [](FakeVariable &grown) {
		grown.value += u"d";
	};
	CHECK(EnvironmentValue(ReadWhole(variable), AbcOnly) == std::string("\x1a"));
}

TEST_CASE("A value read whole that does not convert reads as one control character", "[odbc]") {
	FakeVariable variable;
	variable.value = u"abd";
	CHECK(EnvironmentValue(ReadWhole(variable), AbcOnly) == std::string("\x1a"));
}

TEST_CASE("A value read whole that converts reads as its text", "[odbc]") {
	FakeVariable variable;
	variable.value = u"abc";
	CHECK(EnvironmentValue(ReadWhole(variable), AbcOnly) == std::string("abc"));
}

TEST_CASE("A variable that is not set reads as no value", "[odbc]") {
	FakeVariable variable;
	variable.set = false;
	CHECK_FALSE(EnvironmentValue(ReadWhole(variable), AbcOnly).has_value());
}

#ifdef _WIN32
TEST_CASE("On Windows a value that is no UTF-16 still counts as set, as one control character", "[odbc]") {
	UnsetAround unset(NAME);
	// An unpaired high surrogate, which no UTF-8 text encodes.
	const wchar_t unpaired[] = {L'a', static_cast<wchar_t>(0xD800), L'\0'};
	REQUIRE(SetEnvironmentVariableW(duckdb::Utf8ToWide(NAME).c_str(), unpaired) != 0);
	CHECK(ReadEnvironmentVariable(NAME) == std::string("\x1a"));
}

TEST_CASE("On Windows a test host that is no UTF-16 is refused by the value rule that reads it", "[odbc]") {
	UnsetAround unset_host(duckdb::TEST_HOST_VARIABLE);
	UnsetAround unset_url(duckdb::TEST_URL_VARIABLE);
	const wchar_t unpaired[] = {L'h', static_cast<wchar_t>(0xDC00), L'\0'};
	REQUIRE(SetEnvironmentVariableW(duckdb::Utf8ToWide(duckdb::TEST_HOST_VARIABLE).c_str(), unpaired) != 0);
	SetVariable(duckdb::TEST_URL_VARIABLE, "https://url.example");
	const auto endpoints = duckdb::ReadTestEndpoints(true);
	duckdb::RedshiftConfig config;
	config.region = "us-east-1";
	config.target = duckdb::RedshiftTarget::SERVERLESS;
	config.workgroup = "analytics";
	config.account_id = "012345678901";
	CHECK(Capture([&] { duckdb::BuildConnectionString(config, duckdb::TrustSource {"C:\\ca.pem"}, endpoints); }) ==
	      Thrown {
	          ExceptionType::INVALID_INPUT,
	          "Invalid value for the Redshift connection setting 'Host': it holds an ASCII control character (0x1a)"});
}

TEST_CASE("On Windows text that does not convert is reported, not replaced", "[odbc]") {
	CHECK(duckdb::Utf8ToWide("a\xff").empty());
	CHECK(duckdb::Utf8ToWide("\xc3\xa9") == std::wstring(L"\u00e9"));
	const std::wstring unpaired {L'a', static_cast<wchar_t>(0xD800)};
	CHECK_FALSE(duckdb::WideToUtf8(unpaired).has_value());
	CHECK(duckdb::WideToUtf8(L"\u00e9") == std::string("\xc3\xa9"));
	// A name that does not convert names no variable, so it reads as unset.
	CHECK_FALSE(ReadEnvironmentVariable("A\xff").has_value());
}
#endif
