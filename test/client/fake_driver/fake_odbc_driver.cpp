#include "fake_odbc_driver.hpp"

// Before the DuckDB headers, for the reason fake_odbc_driver_test.cpp gives.
#include "catch.hpp"

#include "duckdb/common/exception.hpp"
#include "duckdb/common/numeric_utils.hpp"
#include "duckdb/common/string_util.hpp"
#include "duckdb/main/database.hpp"

#include <algorithm>
#include <cctype>
#include <cerrno>
#include <cmath>
#include <cstdlib>
#include <cstring>
#include <iterator>
#include <limits>
#include <mutex>
#include <utility>

namespace duckdb {
namespace redshift_driver {
namespace testing {

namespace {

// A driver citation below names a file and function of the driver's public source, v2.3.0.

//! Taken by every call into the fake, so those calls are serialised; guards current_fake, fake_generation, what
//! those calls write, and last_handle_value. Readers of the driver that Source hands out, and of Calls(), do not
//! take it.
std::mutex fake_mutex;
FakeOdbcDriver *current_fake = nullptr;
//! Bumped each time a fake is destroyed, so a call held at a gate can tell its fake is gone, even
//! when a new one sits at the same address.
uint64_t fake_generation = 0;
//! Handle values are unique in the process, so a handle kept from an earlier fake is invalid in the
//! next one rather than naming one of its handles.
uintptr_t last_handle_value = 0;

const char *const DEFAULT_SQL = "SELECT 1";

//! The script every fake starts with: SELECT 1 as one INTEGER of column size 10, derived from
//! driver source v2.3.0 (rslibpq.c mapPgTypeToSqlType, rsutil.c getTypeName and getSize). The live
//! trace of the embedded 2.3.0.0 driver (first_query_test.cpp) gives the same calls, bound fetch
//! included, the same described name, type, size and digits, and the same value.
FakeResultSet DefaultResult() {
	FakeColumn column;
	column.name = "?column?";
	column.type_name = "INTEGER";
	column.sql_type = SQL_INTEGER;
	column.column_size = 10;
	FakeResultSet result;
	result.columns.push_back(column);
	result.rows.push_back({"1"});
	return result;
}

template <class T>
void Store(T *target, T value) {
	if (target) {
		*target = value;
	}
}

std::string ReadText(const SQLCHAR *text, SQLINTEGER length) {
	const auto chars = reinterpret_cast<const char *>(text);
	return length == SQL_NTS ? std::string(chars) : std::string(chars, length);
}

//! Copies as many of the bytes as fit and returns how many. Its callers have refused a null buffer
//! and a negative capacity, SQLGetData with the driver's HY009 and HY090.
size_t CopyBytes(const char *bytes, size_t size, SQLPOINTER buffer, SQLLEN capacity) {
	const auto copied = std::min(size, static_cast<size_t>(capacity));
	memcpy(buffer, bytes, copied);
	return copied;
}

//! As CopyBytes, keeping the last byte for the NUL that ODBC appends to character data. Nothing fits
//! a null or empty buffer, which is how a metadata call asks for a length alone.
size_t CopyText(const char *bytes, size_t size, SQLPOINTER buffer, SQLLEN capacity) {
	if (!buffer || capacity <= 0) {
		return 0;
	}
	const auto copied = CopyBytes(bytes, size, buffer, capacity - 1);
	static_cast<char *>(buffer)[copied] = '\0';
	return copied;
}

//! Returns text as ODBC returns character data: as much as fits, NUL-terminated, with the full length,
//! and SQL_SUCCESS_WITH_INFO when it was cut. WriteDescription adds the driver's 01004 for metadata.
template <class LENGTH>
SQLRETURN WriteText(const std::string &text, SQLPOINTER buffer, SQLLEN capacity, LENGTH *length) {
	Store(length, NumericCast<LENGTH>(text.size()));
	const auto copied = CopyText(text.data(), text.size(), buffer, capacity);
	return copied == text.size() ? SQL_SUCCESS : SQL_SUCCESS_WITH_INFO;
}

//! What the driver's copyStrDataSmallLen did with a text (rsutil.c).
struct SmallLenCopy {
	SQLRETURN rc;
	bool is_cut;
};

//! Copies text as copyStrDataSmallLen does: as WriteText, but the int length it stores in the
//! SQLSMALLINT keeps only its low 16 bits past 32767, and a cut copy reports SQL_SUCCESS when there
//! is no length pointer.
SmallLenCopy CopySmallLenText(const std::string &text, SQLPOINTER buffer, SQLSMALLINT capacity, SQLSMALLINT *length) {
	const auto low_bits = NumericCast<int32_t>(text.size() % 65536);
	Store(length, NumericCast<SQLSMALLINT>(low_bits < 32768 ? low_bits : low_bits - 65536));
	const auto is_cut = CopyText(text.data(), text.size(), buffer, capacity) < text.size();
	return {is_cut && length ? SQLRETURN(SQL_SUCCESS_WITH_INFO) : SQLRETURN(SQL_SUCCESS), is_cut};
}

//! Whether the strto* call that set end read the whole of text.
bool ParsedWhole(const std::string &text, const char *end) {
	return !text.empty() && end == text.c_str() + text.size();
}

//! Whether the text is a number at all, as strtod reads it. The driver trims blanks and takes only a
//! decimal or its Infinity and NaN tokens (rsutil.c prepareStringForNumericConversion), so the two
//! differ on "12 ", "0x10", "nan(1)" and "-nan".
bool IsNumber(const std::string &text) {
	char *end = nullptr;
	std::strtod(text.c_str(), &end);
	return ParsedWhole(text, end);
}

//! What converting a cell's text to one fixed-size C type produced.
enum class NumberOutcome : uint8_t {
	WRITTEN,      //!< the value is at the target
	NOT_A_NUMBER, //!< the text is not a number, which ODBC reports as 22018
	OUT_OF_RANGE  //!< the text is a number the C type cannot hold, which ODBC reports as 22003
};

//! The width written, when the outcome is WRITTEN.
struct NumberResult {
	NumberOutcome outcome;
	SQLLEN width;
};

//! Writes a cell's text at target as one fixed-size C type.
using NumberWriter = NumberResult (*)(const std::string &text, SQLPOINTER target);

NumberResult Wrote(SQLLEN width) {
	return NumberResult {NumberOutcome::WRITTEN, width};
}

NumberResult Refused(NumberOutcome outcome) {
	return NumberResult {outcome, 0};
}

//! Measured on driver 2.3.0.0: a BOOLEAN arrives as "1" or "0", not "true" or "false", so only those
//! two are read. The driver reads other spellings of 0 or 1 ("-0", "+1", "1e0") and truncates "1.7"
//! with 01S07, where the fake reports out of range (rsutil.c parseAndBuildInteger).
NumberResult WriteBit(const std::string &text, SQLPOINTER target) {
	if (text != "0" && text != "1") {
		return Refused(IsNumber(text) ? NumberOutcome::OUT_OF_RANGE : NumberOutcome::NOT_A_NUMBER);
	}
	*static_cast<SQLCHAR *>(target) = NumericCast<SQLCHAR>(text[0] - '0');
	return Wrote(sizeof(SQLCHAR));
}

//! The driver truncates "1.7" to 1 with 01S07 and reads "1e3" as 1000, where the fake reports both out
//! of range; both report NaN and Infinity as 22003 (rsutil.c parseAndBuildInteger). From a SMALLINT or
//! INTEGER column the driver's atoi wraps a wide value and reads a non-number as 0; the fake does not.
NumberResult WriteBigInt(const std::string &text, SQLPOINTER target) {
	char *end = nullptr;
	errno = 0;
	const auto value = std::strtoll(text.c_str(), &end, 10);
	if (!ParsedWhole(text, end)) {
		return Refused(IsNumber(text) ? NumberOutcome::OUT_OF_RANGE : NumberOutcome::NOT_A_NUMBER);
	}
	// strtoll clamps a number beyond long long to that limit and says so only in errno.
	if (errno == ERANGE) {
		return Refused(NumberOutcome::OUT_OF_RANGE);
	}
	*static_cast<SQLBIGINT *>(target) = value;
	return Wrote(sizeof(SQLBIGINT));
}

//! strtod reads the server's NaN and Infinity and the locale's decimal point, so under a comma locale
//! the read fails rather than returning a number the server never sent. The driver's strtold refuses
//! an underflow ("1e-5000") with 22003; the fake reads it as 0 (convertStringNumericToFloatCType).
NumberResult WriteCheckedDouble(const std::string &text, SQLPOINTER target) {
	char *end = nullptr;
	errno = 0;
	const auto value = std::strtod(text.c_str(), &end);
	if (!ParsedWhole(text, end)) {
		return Refused(NumberOutcome::NOT_A_NUMBER);
	}
	// ERANGE marks an underflow too, and only an overflow returns an infinity with it.
	if (errno == ERANGE && std::isinf(value)) {
		return Refused(NumberOutcome::OUT_OF_RANGE);
	}
	*static_cast<SQLDOUBLE *>(target) = value;
	return Wrote(sizeof(SQLDOUBLE));
}

//! A cell's text parsed as the driver's atof parses it, with no range check, so an overflow becomes
//! an infinity. is_number is false where strtod stops short: the fake fails "0.3" with 22018 under a
//! comma locale, where the driver reads 0 (rsutil.c getRsVal).
struct ParsedNumber {
	bool is_number;
	double value;
};

ParsedNumber ParseNumber(const std::string &text) {
	char *end = nullptr;
	const auto value = std::strtod(text.c_str(), &end);
	return ParsedNumber {ParsedWhole(text, end), value};
}

//! Measured on driver 2.3.0.0: a stored DOUBLE PRECISION "1.79769313486232e+308" read natively as
//! +infinity and succeeded. The driver range-checks only a character column's text; a float column's
//! goes through atof (rsutil.c, the SQL_C_DOUBLE cases of convertSQLDataToCData and getRsVal).
NumberResult WriteUncheckedDouble(const std::string &text, SQLPOINTER target) {
	const auto parsed = ParseNumber(text);
	if (!parsed.is_number) {
		return Refused(NumberOutcome::NOT_A_NUMBER);
	}
	*static_cast<SQLDOUBLE *>(target) = parsed.value;
	return Wrote(sizeof(SQLDOUBLE));
}

//! Measured on driver 2.3.0.0: stored REALs "0.1" and "3.4028235e+38" read natively as
//! 0.10000000149011612 and 3.4028234663852886e+38, float precision widened to double, as the driver's
//! (float)atof gives (rsutil.c, the SQL_REAL cases of getRsVal and convertSQLDataToCData).
NumberResult WriteUncheckedReal(const std::string &text, SQLPOINTER target) {
	const auto parsed = ParseNumber(text);
	if (!parsed.is_number) {
		return Refused(NumberOutcome::NOT_A_NUMBER);
	}
	*static_cast<SQLDOUBLE *>(target) = static_cast<double>(static_cast<float>(parsed.value));
	return Wrote(sizeof(SQLDOUBLE));
}

//! The writer for an SQL_C_DOUBLE read, chosen by column type as the driver's switch chooses: a float
//! column unchecked at its own precision, any other through the checked parse. The driver reports
//! float4 as SQL_REAL and float8 as SQL_DOUBLE, never SQL_FLOAT (rslibpq.c mapPgTypeToSqlType).
NumberWriter FindDoubleWriter(SQLSMALLINT sql_type) {
	switch (sql_type) {
	case SQL_REAL:
		return WriteUncheckedReal;
	case SQL_DOUBLE:
		return WriteUncheckedDouble;
	default:
		return WriteCheckedDouble;
	}
}

//! The writer for reading a cell of sql_type as c_type, or null for its bytes unconverted. These are
//! the ODBC client's native carriers: BOOLEAN as SQL_C_BIT, every integer width as
//! SQL_C_SBIGINT and both float widths as SQL_C_DOUBLE.
NumberWriter FindNumberWriter(SQLSMALLINT c_type, SQLSMALLINT sql_type) {
	switch (c_type) {
	case SQL_C_BIT:
		return WriteBit;
	case SQL_C_SBIGINT:
		return WriteBigInt;
	case SQL_C_DOUBLE:
		return FindDoubleWriter(sql_type);
	default:
		return nullptr;
	}
}

//! Every environment and connection attribute the ODBC client sets is an integer, which ODBC passes in the
//! pointer itself; statement attributes such as SQL_ATTR_ROWS_FETCHED_PTR are real pointers.
std::string AttributeText(SQLINTEGER attribute, SQLPOINTER value) {
	return std::to_string(attribute) + "=" + std::to_string(reinterpret_cast<uintptr_t>(value));
}

//! As AttributeText for the row-array size, the one integer statement attribute the fake sets; any
//! other logs the attribute alone, as a pointer's address differs from run to run.
std::string StatementAttributeText(SQLINTEGER attribute, SQLPOINTER value) {
	return attribute == SQL_ATTR_ROW_ARRAY_SIZE ? AttributeText(attribute, value) : std::to_string(attribute);
}

//! The bytes a value of one of FindNumberWriter's carriers takes, and 0 for any other C type; a new
//! carrier goes in both switches.
SQLLEN FixedWidth(SQLSMALLINT c_type) {
	switch (c_type) {
	case SQL_C_BIT:
		return sizeof(SQLCHAR);
	case SQL_C_SBIGINT:
		return sizeof(SQLBIGINT);
	case SQL_C_DOUBLE:
		return sizeof(SQLDOUBLE);
	default:
		return 0;
	}
}

//! The bytes between a column's array elements, as the driver's getOctetLenUsingCType gives them:
//! the BufferLength for SQL_C_CHAR, a fixed-size type's own width, and 0 for SQL_C_BINARY, which it
//! does not list (rsutil.c), so the driver cannot fetch SQL_C_BINARY over a row array.
SQLLEN ElementLength(SQLSMALLINT c_type, SQLLEN capacity) {
	return c_type == SQL_C_CHAR ? capacity : FixedWidth(c_type);
}

//! The row status FetchRow gives when the fetch ends at a column with no element length; no row
//! status ODBC defines has this value.
constexpr SQLUSMALLINT FETCH_STOPPED = 0xffff;

//! Whether the fake returns a cell as c_type: its bytes, or one of the native carriers.
bool IsReturnedType(SQLSMALLINT c_type) {
	return c_type == SQL_C_CHAR || c_type == SQL_C_BINARY || FindNumberWriter(c_type, SQL_VARCHAR);
}

bool IsEntryPoint(const std::string &name) {
#define REDSHIFT_FAKE_MATCH(entry)                                                                                     \
	if (name == "SQL" #entry) {                                                                                        \
		return true;                                                                                                   \
	}
	REDSHIFT_ODBC_ENTRY_POINTS(REDSHIFT_FAKE_MATCH)
#undef REDSHIFT_FAKE_MATCH
	return false;
}

//! SQL_SUCCESS when diags has record `number`, counting from 1; otherwise what ODBC returns instead.
SQLRETURN CheckRecord(const std::vector<FakeDiag> &diags, SQLSMALLINT number) {
	if (number < 1) {
		return SQL_ERROR;
	}
	return static_cast<size_t>(number) <= diags.size() ? SQL_SUCCESS : SQL_NO_DATA;
}

//! Whether every row, and every nulls entry if any, holds one value per column, and row_status, if
//! given, one entry per row.
bool RowsFit(const FakeResultSet &result) {
	const auto width = result.columns.size();
	if (!result.nulls.empty() && result.nulls.size() != result.rows.size()) {
		return false;
	}
	if (!result.row_status.empty() && result.row_status.size() != result.rows.size()) {
		return false;
	}
	for (size_t row = 0; row < result.rows.size(); row++) {
		if (result.rows[row].size() != width) {
			return false;
		}
		if (!result.nulls.empty() && result.nulls[row].size() != width) {
			return false;
		}
	}
	return true;
}

//! Whether every row_status entry is one the fake answers: a scripted row succeeds or fails, and
//! SQL_ROW_SUCCESS_WITH_INFO is what a cut bound cell gives its row, never a script.
bool StatusesFit(const FakeResultSet &result) {
	for (size_t row = 0; row < result.row_status.size(); row++) {
		if (result.row_status[row] == SQL_ROW_SUCCESS) {
			continue;
		}
		if (result.row_status[row] != SQL_ROW_ERROR || result.row_diags.find(row) == result.row_diags.end()) {
			return false;
		}
	}
	return true;
}

//! Whether every row_diags entry is for a row scripted SQL_ROW_ERROR, the only row whose fetch posts
//! its diagnostics.
bool RowDiagsFit(const FakeResultSet &result) {
	for (const auto &entry : result.row_diags) {
		const auto row = entry.first;
		if (row >= result.row_status.size() || result.row_status[row] != SQL_ROW_ERROR) {
			return false;
		}
	}
	return true;
}

//! Whether `sql` holds only blanks and semicolons, an empty query, whose result carries no command
//! tag, so the driver's SQLRowCount reports -1 for it with success (rsresult.cpp SQLRowCount).
bool IsEmptyQuery(const std::string &sql) {
	return sql.find_first_not_of(" \t\r\n;") == std::string::npos;
}

//! Refuses a scripted result set the fake could not answer from.
void CheckShape(const std::string &sql, const FakeResultSet &result) {
	// A command that returns no result set has no rows either, so SQLFetch has nothing to step to.
	if (result.columns.empty() && !result.rows.empty()) {
		throw InternalException("a result set for \"%s\" has rows but no columns, which no statement returns; "
		                        "script the columns its rows hold, or no rows",
		                        sql);
	}
	if (!RowsFit(result)) {
		throw InternalException("a result set for \"%s\" needs one cell per column in every row, one nulls entry per "
		                        "cell if any are given, and one row_status entry per row if any are",
		                        sql);
	}
	if (!StatusesFit(result)) {
		throw InternalException("a result set for \"%s\" takes a row_status of SQL_ROW_SUCCESS or SQL_ROW_ERROR only, "
		                        "and an SQL_ROW_ERROR row needs its diagnostics in row_diags",
		                        sql);
	}
	if (!RowDiagsFit(result)) {
		throw InternalException("a result set for \"%s\" has row_diags for a row not scripted SQL_ROW_ERROR, whose "
		                        "fetch would never post them; script that row's row_status SQL_ROW_ERROR, or drop "
		                        "its row_diags entry",
		                        sql);
	}
	if (result.columns.empty() && result.row_count < 0 && !IsEmptyQuery(sql)) {
		throw InternalException("a result set for \"%s\" with no columns reports the rows its command affected, which "
		                        "is 0 for a command whose tag carries no count, such as SET or CREATE; script "
		                        "row_count, not -1",
		                        sql);
	}
}

//! Whether the server would tag this text UPDATE, INSERT or DELETE, the tags for which a statement
//! that affected no rows reports SQL_NO_DATA (rslibpq.c setResultInStmt). With no server
//! the fake reads the leading word, so text that starts with a comment or WITH is not DML to it.
bool IsDml(const std::string &sql) {
	const auto start = std::min(sql.find_first_not_of(" \t\r\n"), sql.size());
	auto end = start;
	while (end < sql.size() && std::isalpha(static_cast<unsigned char>(sql[end]))) {
		end++;
	}
	const auto keyword = sql.substr(start, end - start);
	return StringUtil::CIEquals(keyword, "UPDATE") || StringUtil::CIEquals(keyword, "INSERT") ||
	       StringUtil::CIEquals(keyword, "DELETE");
}

//! The precision the driver derives from a column's type and size (rsutil.c getPrecision): the size
//! for NUMERIC and DECIMAL, fixed digits for each integer and float width, 6 for TIME, TIMESTAMP and
//! a day-to-second interval, else 0. TIMETZ needs no case, arriving as SQL_TYPE_TIME.
SQLLEN DriverPrecision(const FakeColumn &column) {
	switch (column.sql_type) {
	case SQL_NUMERIC:
	case SQL_DECIMAL:
		return NumericCast<SQLLEN>(column.column_size);
	case SQL_SMALLINT:
		return 5;
	case SQL_INTEGER:
		return 10;
	case SQL_BIGINT:
		return 19;
	case SQL_REAL:
		return 7;
	case SQL_FLOAT:
	case SQL_DOUBLE:
		return 15;
	case SQL_TIME:
	case SQL_TYPE_TIME:
	case SQL_TIMESTAMP:
	case SQL_TYPE_TIMESTAMP:
	case SQL_INTERVAL_DAY_TO_SECOND:
		return 6;
	default:
		return 0;
	}
}

//! Only the three handle types the fake models have a name; the fallback is unreachable because
//! AllocHandle refuses every other type.
const char *HandleTypeName(SQLSMALLINT type) {
	switch (type) {
	case SQL_HANDLE_ENV:
		return "environment";
	case SQL_HANDLE_DBC:
		return "connection";
	case SQL_HANDLE_STMT:
		return "statement";
	default:
		return "unknown";
	}
}

} // namespace

struct FakeOdbcDriver::EntryPoints {
	//! Records the call on the current fake and answers it there, under the fake's mutex, given
	//! what FailCall and WarnCall scripted for it. A call BlockCall holds first waits at its gate
	//! with the mutex released, so other threads can call the fake meanwhile.
	template <class ANSWER>
	static SQLRETURN Dispatch(FakeCall call, ANSWER answer) {
		std::unique_lock<std::mutex> lock(fake_mutex);
		if (!current_fake) {
			return SQL_INVALID_HANDLE;
		}
		auto &fake = *current_fake;
		// Taken before the call is moved into the log, after which its entry point is unspecified.
		auto &script = fake.scripts[call.entry_point];
		const auto logged = fake.calls.size();
		fake.calls.push_back(std::move(call));
		const auto call_number = ++script.calls_made;
		const auto gate = script.gates.find(call_number);
		if (gate != script.gates.end()) {
			const auto held = gate->second;
			const auto generation = fake_generation;
			lock.unlock();
			held->Hold();
			lock.lock();
			// A fake destroyed meanwhile, even one replaced at the same address, gets an invalid handle.
			if (fake_generation != generation) {
				return SQL_INVALID_HANDLE;
			}
		}
		const auto failure = script.failures.find(call_number);
		const auto warning = script.warnings.find(call_number);
		fake.answering_call = logged;
		const Scripted scripted {failure == script.failures.end() ? nullptr : &failure->second,
		                         warning == script.warnings.end() ? nullptr : &warning->second};
		return answer(fake, scripted);
	}

	//! Puts a scripted warning's records ahead of the call's own and reports SQL_SUCCESS_WITH_INFO if
	//! the call succeeded; one that failed or found no data did not warn. The driver's addError puts
	//! each new record first (rsutil.c addError, getNextError); the scripted list keeps its order.
	static SQLRETURN Warn(Handle &handle, const std::vector<FakeDiag> &diags, SQLRETURN rc) {
		if (rc != SQL_SUCCESS && rc != SQL_SUCCESS_WITH_INFO) {
			return rc;
		}
		handle.diags.insert(handle.diags.begin(), diags.begin(), diags.end());
		return SQL_SUCCESS_WITH_INFO;
	}

	//! The handle if the fake holds it as `type`, else null: the handle is invalid.
	static Handle *Find(FakeOdbcDriver &fake, SQLHANDLE handle, SQLSMALLINT type) {
		auto entry = fake.handles.find(handle);
		return entry == fake.handles.end() || entry->second.type != type ? nullptr : &entry->second;
	}

	//! As Dispatch, for a call on `type`'s handle, refusing an invalid or abandoned handle. Like every
	//! ODBC call but a diagnostic read it replaces the handle's records, with the scripted failure's or
	//! the answer's, and is the one place a WarnCall warning is added to them.
	template <class ANSWER>
	static SQLRETURN DispatchOnHandleWithScript(FakeCall call, SQLHANDLE handle, SQLSMALLINT type, ANSWER answer) {
		// Copied before the call is moved into the log, after which its entry point is unspecified.
		const std::string entry_point = call.entry_point;
		return Dispatch(std::move(call), [&](FakeOdbcDriver &fake, const Scripted &scripted) -> SQLRETURN {
			const auto target = Find(fake, handle, type);
			if (!target) {
				return SQL_INVALID_HANDLE;
			}
			if (target->abandoned) {
				// Recorded and reported when the fake is destroyed, rather than thrown: the ODBC client's
				// teardown runs in destructors and on its producer thread, where an exception would
				// reach a noexcept destructor and end the whole run instead of failing one case.
				// SQL_INVALID_HANDLE is what the driver answers for a handle it no longer holds,
				// and the fake no longer holds this one either.
				fake.abandoned_misuses.push_back(entry_point + " on a" + (type == SQL_HANDLE_ENV ? "n " : " ") +
				                                 HandleTypeName(type));
				return SQL_INVALID_HANDLE;
			}
			target->diags.clear();
			if (scripted.failure) {
				target->diags = scripted.failure->diags;
			}
			const auto rc = answer(fake, *target, scripted);
			// SQLFreeHandle, the one answer that erases its handle, cannot be warned.
			return scripted.warning ? Warn(*target, *scripted.warning, rc) : rc;
		});
	}

	//! As DispatchOnHandleWithScript, for a call whose whole answer to a scripted failure is its code,
	//! and which has nothing to do for a warning.
	template <class ANSWER>
	static SQLRETURN DispatchOnHandle(FakeCall call, SQLHANDLE handle, SQLSMALLINT type, ANSWER answer) {
		return DispatchOnHandleWithScript(
		    std::move(call), handle, type,
		    [&](FakeOdbcDriver &fake, Handle &target, const Scripted &scripted) -> SQLRETURN {
			    return scripted.failure ? scripted.failure->rc : answer(fake, target);
		    });
	}

	//! As DispatchOnHandle, for a diagnostic read, which leaves the handle's diagnostics as they are and
	//! is answered from the fake and the records as the driver keeps them.
	template <class ANSWER>
	static SQLRETURN DispatchOnDiagnostics(FakeCall call, SQLHANDLE handle, SQLSMALLINT type, ANSWER answer) {
		return Dispatch(std::move(call), [&](FakeOdbcDriver &fake, const Scripted &scripted) -> SQLRETURN {
			const auto target = Find(fake, handle, type);
			if (!target) {
				return SQL_INVALID_HANDLE;
			}
			return scripted.failure ? scripted.failure->rc : answer(fake, Kept(target->diags, fake.message_limit));
		});
	}

	//! The records as the driver keeps them, each message cut to `message_limit` bytes, as its
	//! addError cuts one to 326 (public source v2.3.0, rsutil.c addError).
	static std::vector<FakeDiag> Kept(const std::vector<FakeDiag> &diags, size_t message_limit) {
		auto kept = diags;
		for (auto &diag : kept) {
			diag.message = diag.message.substr(0, message_limit);
		}
		return kept;
	}

	//! Posts 01004 after a cut copy, in ODBC's words; the driver says "String data, right truncation"
	//! (rsutil.c copyStrDataSmallLen). A cut SQL_C_BINARY part of a binary column posts nothing, as the
	//! driver's copyHexToBinaryDataBigLen does, so ReadBytes skips this there.
	static void PostTruncation(Handle &handle) {
		handle.diags = {{"01004", 0, "String data, right truncated", {}}};
	}

	//! Leaves one diagnostic record on the handle and fails the call, as the driver does when it
	//! refuses one. Dispatch has already cleared the earlier records.
	static SQLRETURN Fail(Handle &handle, const char *sqlstate, std::string message) {
		handle.diags = {{sqlstate, 0, std::move(message), {}}};
		return SQL_ERROR;
	}

	//! The driver's records for a null pointer, a call out of sequence and a cursor in the wrong state.
	static SQLRETURN FailNullPointer(Handle &handle) {
		return Fail(handle, "HY009", "Invalid use of null pointer");
	}

	static SQLRETURN FailSequence(Handle &handle) {
		return Fail(handle, "HY010", "Function sequence error");
	}

	static SQLRETURN FailCursorState(Handle &handle) {
		return Fail(handle, "24000", "Invalid cursor state");
	}

	static SQLRETURN FailUnreturnedType(Handle &handle) {
		return Fail(handle, "HYC00",
		            "the fake driver returns a cell only as SQL_C_CHAR, SQL_C_BINARY, SQL_C_BIT, SQL_C_SBIGINT "
		            "or SQL_C_DOUBLE; read it as one of those");
	}

	static SQLRETURN Succeed(FakeOdbcDriver &, Handle &) {
		return SQL_SUCCESS;
	}

	//! A null output pointer fails with no record, as the driver's does once the input handle has
	//! passed its check (rsconnect.cpp RS_SQLAllocEnv, RS_SQLAllocConnect, RS_SQLAllocStmt).
	static SQLRETURN Allocate(FakeOdbcDriver &fake, SQLSMALLINT type, SQLHANDLE parent, SQLHANDLE *output) {
		if (!output) {
			return SQL_ERROR;
		}
		*output = reinterpret_cast<SQLHANDLE>(++last_handle_value);
		fake.handles[*output] = Handle {type, parent};
		return SQL_SUCCESS;
	}

	// The fake models no descriptors, which the ODBC client never allocates, so any type but these three fails,
	// as ODBC fails an invalid HandleType, rather than being allocated as another type's child.
	static SQLRETURN AllocHandle(SQLSMALLINT type, SQLHANDLE input, SQLHANDLE *output) {
		// The driver writes a null handle whenever an allocation fails, a null input handle included,
		// so a caller's stale value never survives one. It writes nothing for a type that is none of the
		// four, where the fake writes one too; the ODBC client allocates only the three.
		Store(output, SQLHANDLE(SQL_NULL_HANDLE));
		switch (type) {
		case SQL_HANDLE_ENV:
			// An environment allocation has no input handle to put a WarnCall warning on, so it is
			// dropped; ODBC would put it on the new environment, which no ODBC client test reads.
			return Dispatch({"SQLAllocHandle"}, [&](FakeOdbcDriver &fake, const Scripted &scripted) {
				return scripted.failure ? scripted.failure->rc : Allocate(fake, type, SQL_NULL_HANDLE, output);
			});
		// A warning's records go on the input handle, as a failure's do: ODBC reads an allocation's
		// records there, and the driver clears and posts them there (rsconnect.cpp RS_SQLAllocConnect,
		// RS_SQLAllocStmt).
		case SQL_HANDLE_DBC:
			return DispatchOnHandle({"SQLAllocHandle"}, input, SQL_HANDLE_ENV, [&](FakeOdbcDriver &fake, Handle &) {
				return Allocate(fake, type, input, output);
			});
		case SQL_HANDLE_STMT:
			return DispatchOnHandle({"SQLAllocHandle"}, input, SQL_HANDLE_DBC, [&](FakeOdbcDriver &fake, Handle &) {
				return Allocate(fake, type, input, output);
			});
		default:
			// Refused whatever a test scripted: the fake has no such handle to allocate.
			return Dispatch({"SQLAllocHandle"},
			                [](FakeOdbcDriver &, const Scripted &) { return SQLRETURN(SQL_ERROR); });
		}
	}

	static SQLRETURN FreeHandle(SQLSMALLINT type, SQLHANDLE handle) {
		return DispatchOnHandleWithScript(
		    {"SQLFreeHandle", HandleTypeName(type)}, handle, type,
		    [&](FakeOdbcDriver &fake, Handle &target, const Scripted &scripted) -> SQLRETURN {
			    if (scripted.failure) {
				    // A free that did not complete keeps the handle, abandoned (Handle::abandoned), and a
				    // connection's statements with it, since the driver frees them with the connection.
				    target.abandoned = true;
				    if (type == SQL_HANDLE_DBC) {
					    for (auto &entry : fake.handles) {
						    if (entry.second.parent == handle) {
							    entry.second.abandoned = true;
						    }
					    }
				    }
				    return scripted.failure->rc;
			    }
			    fake.handles.erase(handle);
			    // Freeing a connection frees its statements, as the driver does; freeing an environment
			    // frees nothing more.
			    if (type == SQL_HANDLE_DBC) {
				    for (auto entry = fake.handles.begin(); entry != fake.handles.end();) {
					    entry = entry->second.parent == handle ? fake.handles.erase(entry) : std::next(entry);
				    }
			    }
			    return SQL_SUCCESS;
		    });
	}

	static SQLRETURN SetEnvAttr(SQLHENV environment, SQLINTEGER attribute, SQLPOINTER value, SQLINTEGER) {
		return DispatchOnHandle({"SQLSetEnvAttr", AttributeText(attribute, value)}, environment, SQL_HANDLE_ENV,
		                        Succeed);
	}

	static SQLRETURN SetConnectAttr(SQLHDBC connection, SQLINTEGER attribute, SQLPOINTER value, SQLINTEGER) {
		return DispatchOnHandle({"SQLSetConnectAttr", AttributeText(attribute, value)}, connection, SQL_HANDLE_DBC,
		                        Succeed);
	}

	// Measured on driver 2.3.0.0: the dead flag is written as a 4-byte SQLUINTEGER and the length
	// out-param is left untouched, so this writes neither more nor less.
	static SQLRETURN GetConnectAttr(SQLHDBC connection, SQLINTEGER attribute, SQLPOINTER value, SQLINTEGER,
	                                SQLINTEGER *) {
		return DispatchOnHandle({"SQLGetConnectAttr", std::to_string(attribute)}, connection, SQL_HANDLE_DBC,
		                        [&](FakeOdbcDriver &fake, Handle &) -> SQLRETURN {
			                        if (attribute != SQL_ATTR_CONNECTION_DEAD) {
				                        return SQL_ERROR;
			                        }
			                        *static_cast<SQLUINTEGER *>(value) =
			                            fake.connection_dead ? SQL_CD_TRUE : SQL_CD_FALSE;
			                        return SQL_SUCCESS;
		                        });
	}

	// Measured on driver 2.3.0.0: with a zero-capacity output buffer the driver still reports the
	// completed string's required length and writes nothing to the buffer. The fake completes
	// nothing beyond its input, so the length it reports is the input's.
	static SQLRETURN DriverConnect(SQLHDBC connection, SQLHWND, SQLCHAR *text, SQLSMALLINT length, SQLCHAR *completed,
	                               SQLSMALLINT capacity, SQLSMALLINT *completed_length, SQLUSMALLINT completion) {
		const auto connection_string = ReadText(text, length);
		const FakeConnectOutput output {completed != nullptr, capacity, completion};
		return DispatchOnHandle({"SQLDriverConnect", connection_string}, connection, SQL_HANDLE_DBC,
		                        [&](FakeOdbcDriver &fake, Handle &handle) -> SQLRETURN {
			                        fake.last_connection_string = connection_string;
			                        fake.last_connect_output = output;
			                        // The driver refuses a connect on a connection still open
			                        // (rsconnect.cpp RS_SQLDriverConnect); only SQLDisconnect
			                        // closes one, so a failed connect leaves it closed.
			                        if (handle.connected) {
				                        handle.diags = {{"08002", 0, "Connection name in use", {}}};
				                        return SQL_ERROR;
			                        }
			                        handle.diags = fake.connect_outcome.diags;
			                        const auto rc = fake.connect_outcome.rc;
			                        handle.connected = rc == SQL_SUCCESS || rc == SQL_SUCCESS_WITH_INFO;
			                        if (completed_length && handle.connected) {
				                        *completed_length = NumericCast<SQLSMALLINT>(connection_string.size());
			                        }
			                        return rc;
		                        });
	}

	static SQLRETURN Disconnect(SQLHDBC connection) {
		return DispatchOnHandle({"SQLDisconnect"}, connection, SQL_HANDLE_DBC,
		                        [](FakeOdbcDriver &, Handle &handle) -> SQLRETURN {
			                        handle.connected = false;
			                        return SQL_SUCCESS;
		                        });
	}

	static SQLRETURN EndTran(SQLSMALLINT type, SQLHANDLE handle, SQLSMALLINT completion) {
		return DispatchOnHandle({"SQLEndTran", std::to_string(completion)}, handle, type, Succeed);
	}

	// Measured on driver 2.3.0.0: SQLGetInfo(SQL_GETDATA_EXTENSIONS) reports length 4.
	static SQLRETURN GetInfo(SQLHDBC connection, SQLUSMALLINT info_type, SQLPOINTER value, SQLSMALLINT,
	                         SQLSMALLINT *length) {
		return DispatchOnHandle({"SQLGetInfo", std::to_string(info_type)}, connection, SQL_HANDLE_DBC,
		                        [&](FakeOdbcDriver &fake, Handle &) -> SQLRETURN {
			                        auto answer = fake.info.find(info_type);
			                        if (answer == fake.info.end()) {
				                        return SQL_ERROR;
			                        }
			                        *static_cast<SQLUINTEGER *>(value) = answer->second;
			                        if (length) {
				                        *length = NumericCast<SQLSMALLINT>(sizeof(SQLUINTEGER));
			                        }
			                        return SQL_SUCCESS;
		                        });
	}

	//! The result set the statement is on. The caller has checked HasResult, so the script has one,
	//! and nothing steps the cursor past the last.
	static const FakeResultSet &CurrentResult(const Statement &statement) {
		return statement.results[statement.cursor.result];
	}

	//! Whether the statement has a current result set: a prepared one describes its columns, and an
	//! executed one has its cursor's set until SQLMoreResults passes the last or a failed SQLExecute
	//! releases it. An allocated or closed statement has none.
	static bool HasResult(const Statement &statement) {
		if (statement.step == StatementStep::PREPARED) {
			return true;
		}
		return statement.step == StatementStep::EXECUTED && !statement.cursor.drained;
	}

	//! The current result set's column `number`, counting from 1, or null when there is none.
	static const FakeColumn *FindColumn(const Statement &statement, SQLUSMALLINT number) {
		const auto &columns = CurrentResult(statement).columns;
		return number >= 1 && number <= columns.size() ? &columns[number - 1] : nullptr;
	}

	//! Whether the statement stands on one of the current set's rows.
	static bool IsOnRow(const Statement &statement) {
		if (statement.step != StatementStep::EXECUTED || !HasResult(statement)) {
			return false;
		}
		const auto fetched = statement.cursor.fetched;
		return fetched >= 1 && fetched <= CurrentResult(statement).rows.size();
	}

	//! Cell `number`, counting from 1, on the row the statement stands on.
	static const std::string &CurrentCell(const Statement &statement, SQLUSMALLINT number) {
		return CurrentResult(statement).rows[statement.cursor.fetched - 1][number - 1];
	}

	//! Whether cell `number` of `row`, counting from 0, is SQL NULL. A result set with no nulls entry
	//! at all has no NULL cell.
	static bool IsNullCell(const FakeResultSet &result, size_t row, SQLUSMALLINT number) {
		return !result.nulls.empty() && result.nulls[row][number - 1];
	}

	//! Whether that cell, on the row the statement stands on, is SQL NULL.
	static bool IsNullCell(const Statement &statement, SQLUSMALLINT number) {
		return IsNullCell(CurrentResult(statement), statement.cursor.fetched - 1, number);
	}

	//! Leaves the statement as SQLFreeStmt(SQL_CLOSE) and SQLCloseCursor leave it: its script intact,
	//! its results unreadable. One never prepared stays as it is, where the driver marks it closed,
	//! so a later SQLFetch there posts 24000, not the driver's HY010 (rsconnect.cpp RS_SQLFreeStmt).
	static void Close(Statement &statement) {
		if (statement.step == StatementStep::ALLOCATED) {
			return;
		}
		statement.step = StatementStep::CLOSED;
		statement.cursor = Cursor();
	}

	//! Leaves the statement with no result to read and its step as it was, as the driver's release
	//! leaves its status (rsutil.c makeItReadyForNewQueryExecution): SQLFetch then posts 24000, or
	//! HY010 once closed. Only SQLExecute uses it; a failed SQLPrepare also drops the prepare.
	static void ReleaseResults(Statement &statement) {
		statement.cursor = Cursor();
		statement.cursor.drained = true;
	}

	//! Posts what the driver's SQLFetch posts with no result: HY010 once the cursor is closed, else
	//! 24000 (rsresult.cpp RS_SQLFetchScroll). SQLGetData posts 24000 throughout, so it does not come here.
	static SQLRETURN FailWithoutCursor(Handle &handle) {
		if (handle.statement.step == StatementStep::CLOSED) {
			return FailSequence(handle);
		}
		return FailCursorState(handle);
	}

	//! SQL_SUCCESS when the statement has a column to describe, else the driver's answer: 07005 for an
	//! executed set with no columns, HY000 for no result at all. A command with no row description has
	//! no result before it runs (rslibpq.c libpqPrepareOnThread), so it gets HY000, not 07005.
	static SQLRETURN CheckDescribable(Handle &handle) {
		const auto &statement = handle.statement;
		const bool before_execution = statement.step == StatementStep::PREPARED;
		if (!HasResult(statement) || (before_execution && CurrentResult(statement).columns.empty())) {
			return Fail(handle, "HY000", "No result found");
		}
		if (CurrentResult(statement).columns.empty()) {
			return Fail(handle, "07005", "No columns found");
		}
		return SQL_SUCCESS;
	}

	//! Replaces the handle's statement with the one scripted for `sql`, as SQLPrepare and
	//! SQLExecDirect both release what the handle held first, or fails naming AddResult. Copied, not
	//! consumed: the ODBC client prepares the same text to describe it and again to run it.
	static SQLRETURN LoadScript(FakeOdbcDriver &fake, Handle &handle, const std::string &sql) {
		const auto script = fake.scripted_statements.find(sql);
		if (script == fake.scripted_statements.end()) {
			handle.statement = Statement();
			return Fail(handle, "HY000",
			            "the fake driver has no result for \"" + sql + "\"; script one with AddResult");
		}
		handle.statement = script->second;
		return SQL_SUCCESS;
	}
	//! A scripted failure stands for the server's, which the driver reaches after releasing both the
	//! prepare and the results (rsprepare.cpp RS_SQLPrepare), so the statement is left never prepared.
	//! The driver's status survives, so here the fake departs: SQLNumParams posts HY010 (driver 0),
	//! SQLCloseCursor 24000 (driver succeeds) and SQLFetch after a close 24000 (driver HY010).
	static SQLRETURN Prepare(SQLHSTMT statement, SQLCHAR *text, SQLINTEGER length) {
		const auto sql = ReadText(text, length);
		return DispatchOnHandleWithScript(
		    {"SQLPrepare", sql}, statement, SQL_HANDLE_STMT,
		    [&](FakeOdbcDriver &fake, Handle &handle, const Scripted &scripted) -> SQLRETURN {
			    if (scripted.failure) {
				    handle.statement = Statement();
				    return scripted.failure->rc;
			    }
			    // The driver's SQLPrepare also releases an open result and leaves the text prepared.
			    return LoadScript(fake, handle, sql);
		    });
	}

	//! Reports the count AddResult was given, never one counted from the text: measured on driver
	//! 2.3.0.0, "$1, $2" and "? ?" both report 2. The driver answers only between SQLPrepare and
	//! SQLExecute, posting HY010 otherwise (rsparameter.cpp RS_SQLNumParams), and the ODBC client reads it there.
	static SQLRETURN NumParams(SQLHSTMT statement, SQLSMALLINT *count) {
		return DispatchOnHandle({"SQLNumParams"}, statement, SQL_HANDLE_STMT,
		                        [&](FakeOdbcDriver &, Handle &handle) -> SQLRETURN {
			                        if (!count) {
				                        return FailNullPointer(handle);
			                        }
			                        // Zeroed before the state check, as the driver zeroes it.
			                        *count = 0;
			                        if (handle.statement.step != StatementStep::PREPARED) {
				                        return FailSequence(handle);
			                        }
			                        *count = handle.statement.param_count;
			                        return SQL_SUCCESS;
		                        });
	}

	//! Reports the current set's column count, and 0, not a refusal, for a statement with no result
	//! set, as the driver does; a null pointer is HY009 (rsresult.cpp SQLNumResultCols).
	static SQLRETURN NumResultCols(SQLHSTMT statement, SQLSMALLINT *count) {
		return DispatchOnHandle(
		    {"SQLNumResultCols"}, statement, SQL_HANDLE_STMT, [&](FakeOdbcDriver &, Handle &handle) -> SQLRETURN {
			    if (!count) {
				    return FailNullPointer(handle);
			    }
			    const auto &state = handle.statement;
			    *count =
			        HasResult(state) ? NumericCast<SQLSMALLINT>(CurrentResult(state).columns.size()) : SQLSMALLINT(0);
			    return SQL_SUCCESS;
		    });
	}

	//! Writes a column's name or type name as the driver's copyStrDataSmallLen does, posting 01004
	//! when cut. Its cut for a null buffer holding empty text is not modelled.
	static SQLRETURN WriteDescription(Handle &handle, const std::string &text, SQLPOINTER buffer, SQLSMALLINT capacity,
	                                  SQLSMALLINT *length) {
		const auto copy = CopySmallLenText(text, buffer, capacity, length);
		if (copy.is_cut) {
			PostTruncation(handle);
		}
		return copy.rc;
	}

	//! Writes one column's metadata into SQLDescribeCol's out-params, cutting its name as the driver
	//! does.
	static SQLRETURN WriteColumn(Handle &handle, const FakeColumn &column, SQLCHAR *name, SQLSMALLINT capacity,
	                             SQLSMALLINT *name_length, SQLSMALLINT *data_type, SQLULEN *column_size,
	                             SQLSMALLINT *decimal_digits, SQLSMALLINT *nullable) {
		Store(data_type, column.sql_type);
		Store(column_size, column.column_size);
		Store(decimal_digits, column.decimal_digits);
		Store(nullable, column.nullable);
		return WriteDescription(handle, column.name, name, capacity, name_length);
	}

	static SQLRETURN DescribeCol(SQLHSTMT statement, SQLUSMALLINT number, SQLCHAR *name, SQLSMALLINT capacity,
	                             SQLSMALLINT *name_length, SQLSMALLINT *data_type, SQLULEN *column_size,
	                             SQLSMALLINT *decimal_digits, SQLSMALLINT *nullable) {
		return DispatchOnHandle({"SQLDescribeCol"}, statement, SQL_HANDLE_STMT,
		                        [&](FakeOdbcDriver &, Handle &handle) -> SQLRETURN {
			                        const auto describable = CheckDescribable(handle);
			                        if (describable != SQL_SUCCESS) {
				                        return describable;
			                        }
			                        const auto column = FindColumn(handle.statement, number);
			                        if (!column) {
				                        // The driver's own answer for a column the result lacks: HY000
				                        // here, 07009 from SQLColAttribute and SQLGetData.
				                        return Fail(handle, "HY000", "SQLDescribeCol: Invalid column number");
			                        }
			                        return WriteColumn(handle, *column, name, capacity, name_length, data_type,
			                                           column_size, decimal_digits, nullable);
		                        });
	}

	//! Answers one numeric column attribute; a field the fake does not model is HY091, as the driver's
	//! answer for one it does not know.
	static SQLRETURN WriteColumnAttribute(Handle &handle, const FakeColumn &column, SQLUSMALLINT field,
	                                      SQLLEN *numeric) {
		switch (field) {
		case SQL_DESC_CONCISE_TYPE:
			Store(numeric, SQLLEN(column.sql_type));
			return SQL_SUCCESS;
		case SQL_DESC_LENGTH:
			Store(numeric, NumericCast<SQLLEN>(column.column_size));
			return SQL_SUCCESS;
		case SQL_DESC_PRECISION:
			Store(numeric, DriverPrecision(column));
			return SQL_SUCCESS;
		case SQL_DESC_SCALE:
			Store(numeric, SQLLEN(column.decimal_digits));
			return SQL_SUCCESS;
		case SQL_DESC_NULLABLE:
			Store(numeric, SQLLEN(column.nullable));
			return SQL_SUCCESS;
		default:
			return Fail(handle, "HY091", "Invalid descriptor field identifier");
		}
	}

	//! Answers SQL_DESC_TYPE_NAME as text, truncating as SQLDescribeCol's name does, and the numeric
	//! fields the ODBC client reads to classify a column and build its type.
	static SQLRETURN ColAttribute(SQLHSTMT statement, SQLUSMALLINT number, SQLUSMALLINT field, SQLPOINTER text,
	                              SQLSMALLINT capacity, SQLSMALLINT *text_length, SQLLEN *numeric) {
		return DispatchOnHandle({"SQLColAttribute"}, statement, SQL_HANDLE_STMT,
		                        [&](FakeOdbcDriver &, Handle &handle) -> SQLRETURN {
			                        const auto describable = CheckDescribable(handle);
			                        if (describable != SQL_SUCCESS) {
				                        return describable;
			                        }
			                        const auto column = FindColumn(handle.statement, number);
			                        if (!column) {
				                        return Fail(handle, "07009", "SQLColAttribute: Invalid column number");
			                        }
			                        if (field != SQL_DESC_TYPE_NAME) {
				                        return WriteColumnAttribute(handle, *column, field, numeric);
			                        }
			                        return WriteDescription(handle, column->type_name, text, capacity, text_length);
		                        });
	}

	//! Keeps the buffers unread, as ODBC reads a parameter at SQLExecute. Measured on driver 2.3.0.0:
	//! a text parameter, which is all the ODBC client binds, binds as SQL_C_CHAR into SQL_VARCHAR for $n and ?;
	//! anything else is refused. A null buffer and indicator unbind it, as the driver's do.
	static SQLRETURN BindParameter(SQLHSTMT statement, SQLUSMALLINT number, SQLSMALLINT direction, SQLSMALLINT c_type,
	                               SQLSMALLINT sql_type, SQLULEN, SQLSMALLINT, SQLPOINTER value, SQLLEN buffer_length,
	                               SQLLEN *indicator) {
		return DispatchOnHandle(
		    {"SQLBindParameter"}, statement, SQL_HANDLE_STMT, [&](FakeOdbcDriver &, Handle &handle) -> SQLRETURN {
			    if (direction != SQL_PARAM_INPUT || c_type != SQL_C_CHAR || sql_type != SQL_VARCHAR) {
				    return Fail(handle, "HYC00",
				                "the fake driver binds only SQL_C_CHAR input parameters as "
				                "SQL_VARCHAR; bind the value as text");
			    }
			    if (!value && !indicator) {
				    handle.parameters.erase(number);
				    return SQL_SUCCESS;
			    }
			    handle.parameters[number] = BoundParameter {value, buffer_length, indicator};
			    return SQL_SUCCESS;
		    });
	}

	//! How many of a bound parameter's bytes the driver copies to send, SQL_NTS for text up to its NUL,
	//! or another negative value for one not sent as text. A negative indicator decides alone; else a
	//! negative BufferLength sends NULL, and an indicator replaces a BufferLength of 0 and caps a larger
	//! one (rsutil.c getParamVal). The driver's SQL_NEED_DATA for data-at-exec is not modelled.
	static SQLLEN SentLength(const BoundParameter &parameter) {
		const auto indicator = parameter.indicator;
		if (indicator && *indicator < 0) {
			return *indicator;
		}
		const auto buffer_length = parameter.buffer_length;
		if (buffer_length < 0) {
			return SQL_NULL_DATA;
		}
		if (!indicator) {
			return buffer_length;
		}
		return buffer_length == 0 ? *indicator : std::min(buffer_length, *indicator);
	}

	//! Whether SQLExecute can send this parameter. The ODBC client binds every one as text there to read, counted
	//! or NUL-terminated, and binds no NULL, so a NULL or another negative length is a violation.
	static bool IsSendable(const BoundParameter &parameter) {
		const auto length = SentLength(parameter);
		return parameter.value && (length >= 0 || length == SQL_NTS);
	}

	//! The text the server receives for a sendable parameter: SentLength's bytes up to their first NUL,
	//! as the driver sends every parameter as text with no lengths, which libpq measures with strlen
	//! (rslibpq.c libpqExecuteDirectOrPreparedOnThread).
	static std::string SentText(const BoundParameter &parameter) {
		const auto copied =
		    ReadText(static_cast<SQLCHAR *>(parameter.value), NumericCast<SQLINTEGER>(SentLength(parameter)));
		return copied.substr(0, copied.find('\0'));
	}

	//! "number=value" for each marker position 1 to `markers`, one per line, read from the buffers as
	//! they stand. The caller has checked each is bound and sendable; a binding past the markers is
	//! left out, as the driver never reads one.
	static std::string ParameterValues(const Handle &handle, SQLUSMALLINT markers) {
		std::string values;
		for (SQLUSMALLINT number = 1; number <= markers; number++) {
			values +=
			    (values.empty() ? "" : "\n") + std::to_string(number) + "=" + SentText(handle.parameters.at(number));
		}
		return values;
	}

	//! What SQLExecute returns once the statement has run: SQL_NO_DATA only for an UPDATE, INSERT or
	//! DELETE that returns no rows and affected none, success otherwise, SET and CREATE included. From
	//! the driver's source (rslibpq.c setResultInStmt), not checked against a live server.
	static SQLRETURN FirstResultOutcome(const Statement &statement) {
		const auto &result = CurrentResult(statement);
		return statement.is_dml && result.columns.empty() && result.row_count == 0 ? SQL_NO_DATA : SQL_SUCCESS;
	}

	//! SQL_SUCCESS when every marker 1 to `markers` has a binding SQLExecute can send, else the failure
	//! it posts.
	static SQLRETURN CheckMarkers(Handle &handle, SQLUSMALLINT markers) {
		for (SQLUSMALLINT number = 1; number <= markers; number++) {
			const auto entry = handle.parameters.find(number);
			if (entry == handle.parameters.end()) {
				return Fail(handle, "HY000", "Bind parameter not found.");
			}
			if (!IsSendable(entry->second)) {
				return Fail(handle, "HYC00",
				            "the fake driver sends only non-NULL text parameters, counted or NUL-terminated; "
				            "bind a value with a length or SQL_NTS");
			}
		}
		return SQL_SUCCESS;
	}

	//! Reads each marker's parameter by position at execution, as the driver does (rslibpq.c
	//! libpqExecuteDirectOrPreparedOnThread), so an off-by-one bind loop fails as live. It departs twice:
	//! its own HY010 for a statement never prepared, and no refusal while another statement streams on
	//! the connection, which the ODBC client refuses first.
	static SQLRETURN Execute(SQLHSTMT statement) {
		return DispatchOnHandleWithScript(
		    {"SQLExecute"}, statement, SQL_HANDLE_STMT,
		    [](FakeOdbcDriver &fake, Handle &handle, const Scripted &scripted) -> SQLRETURN {
			    if (scripted.failure) {
				    ReleaseResults(handle.statement);
				    return scripted.failure->rc;
			    }
			    if (handle.statement.step == StatementStep::ALLOCATED) {
				    return FailSequence(handle);
			    }
			    // The driver refuses more markers than bound parameters before it releases the result
			    // (rsexecute.cpp RS_SQLExecDirect), so this refusal alone keeps the previous result.
			    if (handle.parameters.size() < NumericCast<size_t>(handle.statement.param_count)) {
				    return Fail(handle, "HY000",
				                "Number of parameters in query is more than number of bind parameters.");
			    }
			    ReleaseResults(handle.statement);
			    const auto markers = NumericCast<SQLUSMALLINT>(handle.statement.param_count);
			    const auto sendable = CheckMarkers(handle, markers);
			    if (sendable != SQL_SUCCESS) {
				    return sendable;
			    }
			    fake.calls[fake.answering_call].detail = ParameterValues(handle, markers);
			    handle.statement.step = StatementStep::EXECUTED;
			    handle.statement.cursor = Cursor();
			    return FirstResultOutcome(handle.statement);
		    });
	}

	//! Prepares and runs the text in one call, as the driver's SQLExecDirect does (rsexecute.cpp
	//! RS_SQLExecDirect): it releases what the handle held, then reads the script, so a test can change
	//! a statement between the ODBC client's SQLPrepare and this call. It departs twice, each a case the
	//! ODBC client refuses first: a marker with no value runs, where the server refuses it, and nothing
	//! refuses the call while another statement streams on the connection.
	static SQLRETURN ExecDirect(SQLHSTMT statement, SQLCHAR *text, SQLINTEGER length) {
		const auto sql = ReadText(text, length);
		return DispatchOnHandleWithScript(
		    {"SQLExecDirect", sql}, statement, SQL_HANDLE_STMT,
		    [&](FakeOdbcDriver &fake, Handle &handle, const Scripted &scripted) -> SQLRETURN {
			    if (scripted.failure) {
				    handle.statement = Statement();
				    return scripted.failure->rc;
			    }
			    const auto loaded = LoadScript(fake, handle, sql);
			    if (loaded != SQL_SUCCESS) {
				    return loaded;
			    }
			    handle.statement.step = StatementStep::EXECUTED;
			    handle.statement.cursor = Cursor();
			    return FirstResultOutcome(handle.statement);
		    });
	}

	//! The current set's scripted count, or -1 with no current result, before execution or past the
	//! last set. The driver writes -1 whatever the state and posts HY009 only for a null pointer
	//! (rsresult.cpp SQLRowCount).
	static SQLRETURN RowCount(SQLHSTMT statement, SQLLEN *count) {
		return DispatchOnHandle({"SQLRowCount"}, statement, SQL_HANDLE_STMT,
		                        [&](FakeOdbcDriver &, Handle &handle) -> SQLRETURN {
			                        if (!count) {
				                        return FailNullPointer(handle);
			                        }
			                        const auto &state = handle.statement;
			                        const bool running = state.step == StatementStep::EXECUTED && HasResult(state);
			                        *count = running ? CurrentResult(state).row_count : SQLLEN(-1);
			                        return SQL_SUCCESS;
		                        });
	}

	//! Releases the current set and moves to the next, or reports no more, as for a statement with no
	//! result. Past the last set the statement stays executed: the driver's SQLMoreResults leaves its
	//! status, and only SQLCloseCursor closes it.
	static SQLRETURN MoreResults(SQLHSTMT statement) {
		return DispatchOnHandle({"SQLMoreResults"}, statement, SQL_HANDLE_STMT,
		                        [](FakeOdbcDriver &, Handle &handle) -> SQLRETURN {
			                        auto &state = handle.statement;
			                        if (state.step != StatementStep::EXECUTED || state.cursor.drained) {
				                        return SQL_NO_DATA;
			                        }
			                        const auto next = state.cursor.result + 1;
			                        if (next >= state.results.size()) {
				                        state.cursor.drained = true;
				                        return SQL_NO_DATA;
			                        }
			                        state.cursor = Cursor();
			                        state.cursor.result = next;
			                        return SQL_SUCCESS;
		                        });
	}

	//! What one SQLGetData call asks for, which each step of its answer reads.
	struct DataRequest {
		SQLUSMALLINT column;
		SQLSMALLINT c_type;
		SQLPOINTER target;
		SQLLEN capacity;
		SQLLEN *length;
		//! Set for the read SQLFetch makes of a bound cell, which the driver makes through the same
		//! RS_SQLGetData with iInternal set: it takes a null target and any row-array size.
		bool from_fetch = false;
	};

	//! SQL_SUCCESS when SQLGetData may read the column on the current row as its C type, else its
	//! failure. The driver posts 24000 with no result (rsresult.cpp RS_SQLGetData); the fake also posts
	//! it before the first fetch and past the last row, where the driver reads NULL, so a missed fetch
	//! fails visibly.
	static SQLRETURN CheckRead(Handle &handle, const DataRequest &request) {
		if (!IsOnRow(handle.statement)) {
			return FailCursorState(handle);
		}
		if (request.column == 0) {
			// The driver's answer: bookmarks are unsupported (rsresult.cpp RS_SQLGetData).
			return Fail(handle, "07009", "SQLGetData: Bookmark column is not supported");
		}
		const auto described = FindColumn(handle.statement, request.column);
		if (!described) {
			return Fail(handle, "07009", "Invalid descriptor index");
		}
		if (!IsReturnedType(request.c_type)) {
			return FailUnreturnedType(handle);
		}
		// The driver reads the rowset's last row here, where its fetch loop left the current row
		// (rsresult.cpp RS_SQLFetchScroll). ODBC needs SQL_GD_BLOCK for this, which the driver does not
		// report, so the fake refuses, a departure, rather than return a row the caller did not mean.
		if (!request.from_fetch && handle.row_array.size > 1) {
			return Fail(handle, "HYC00",
			            "the fake driver reads SQLGetData only with a row array of 1, as the driver reports no "
			            "SQL_GD_BLOCK; set SQL_ATTR_ROW_ARRAY_SIZE to 1 first");
		}
		return SQL_SUCCESS;
	}

	//! Marks the requested column's cell as returned in full, so its next read reports no more data.
	static void MarkCellRead(Handle &handle, const DataRequest &request) {
		handle.statement.cursor.reads[request.column - 1].complete = true;
	}

	//! Reports a NULL cell, writing the driver's NUL into the target's first byte when there is room.
	//! ODBC and the driver refuse the read with 22002 when there is no indicator to report it in.
	static SQLRETURN ReadNull(Handle &handle, const DataRequest &request) {
		if (request.target && request.capacity > 0) {
			*static_cast<char *>(request.target) = '\0';
		}
		// Done with either way: the driver marks the column read at its error label too, so the next read
		// reports no more data (rsresult.cpp RS_SQLGetData, iPrevhCol).
		MarkCellRead(handle, request);
		if (!request.length) {
			return Fail(handle, "22002", "Indicator variable required but not supplied");
		}
		*request.length = SQL_NULL_DATA;
		return SQL_SUCCESS;
	}

	//! The record a failed conversion posts. The driver's format check refuses text that is no number
	//! with one text whatever the type (rsutil.c prepareStringForNumericConversion), and only a double's
	//! 22003 names its type (rsutil.h convertFloatValue). The check's narrower texts are not modelled.
	static SQLRETURN FailNumber(Handle &handle, SQLSMALLINT c_type, NumberOutcome outcome) {
		if (outcome != NumberOutcome::OUT_OF_RANGE) {
			return Fail(handle, "22018", "Invalid numeric format");
		}
		if (c_type == SQL_C_DOUBLE) {
			return Fail(handle, "22003", "Numeric value out of range for DOUBLE");
		}
		return Fail(handle, "22003", "Numeric value out of range");
	}

	//! Converts the cell with write: 22018 for text that is no number and 22003 for a number the C type
	//! cannot hold, as the driver reports them for a character column, but for the spellings the writers
	//! name. A value arrives whole, so the next read, as after a failed one, reports no more data.
	static SQLRETURN ReadNumber(Handle &handle, const DataRequest &request, NumberWriter write) {
		const auto &cell = CurrentCell(handle.statement, request.column);
		// The driver writes the cell's own length before it converts, which a failed conversion leaves
		// there (rsresult.cpp RS_SQLGetData, iDataLen).
		Store(request.length, NumericCast<SQLLEN>(cell.size()));
		const auto written = write(cell, request.target);
		MarkCellRead(handle, request);
		if (written.outcome != NumberOutcome::WRITTEN) {
			return FailNumber(handle, request.c_type, written.outcome);
		}
		Store(request.length, written.width);
		return SQL_SUCCESS;
	}

	//! Whether the column holds binary data, which the driver receives as hex text and converts back.
	//! The driver reports only SQL_LONGVARBINARY (rslibpq.c mapPgTypeToSqlType); a script may name
	//! any of the three binary types.
	static bool IsBinaryColumn(const FakeColumn &column) {
		return column.sql_type == SQL_BINARY || column.sql_type == SQL_VARBINARY ||
		       column.sql_type == SQL_LONGVARBINARY;
	}

	//! Whether a cut part of this cell posts 01004. A binary column read as SQL_C_BINARY does not:
	//! the driver's copyHexToBinaryDataBigLen posts no record, and only a GEOMETRY's
	//! copyVariableToCBinary posts 01004 (rsutil.c).
	static bool PostsTruncation(const FakeColumn &column, SQLSMALLINT c_type) {
		if (c_type != SQL_C_BINARY || !IsBinaryColumn(column)) {
			return true;
		}
		return column.type_name == "GEOMETRY";
	}

	//! The driver converts a binary cell read as SQL_C_CHAR with no read offset, so every call returns its
	//! first part again with the cell's full length (rsutil.c copyBinaryDataBigLen, and
	//! copyBinaryToHexDataBigLen for GEOMETRY, VARBYTE, GEOGRAPHY and GEOMETRYHEX). The fake does so
	//! twice, so a reader can see it made no progress, then refuses, so one that cannot fails, not hangs.
	static SQLRETURN RepeatFirstPart(Handle &handle, const DataRequest &request, ColumnRead &read,
	                                 const std::string &cell) {
		if (read.repeats == 2) {
			return Fail(handle, "HYC00",
			            "the fake driver returns the first part of a binary cell that does not fit the buffer only "
			            "twice as SQL_C_CHAR, because the driver returns it on every such call; read it as "
			            "SQL_C_BINARY");
		}
		read.repeats++;
		CopyText(cell.data(), cell.size(), request.target, request.capacity);
		Store(request.length, NumericCast<SQLLEN>(cell.size()));
		PostTruncation(handle);
		return SQL_SUCCESS_WITH_INFO;
	}

	//! Returns the cell's bytes from where the column's last read stopped, as ODBC returns long data:
	//! what fits, the length still to come or SQL_NO_TOTAL, and 01004 while more is left. No room gets
	//! the length alone, which for an empty cell succeeds, where the driver posts 01004 (rsutil.c
	//! copyStrDataBigLen).
	static SQLRETURN ReadBytes(Handle &handle, const DataRequest &request) {
		const auto &described = *FindColumn(handle.statement, request.column);
		const auto &cell = CurrentCell(handle.statement, request.column);
		auto &read = handle.statement.cursor.reads[request.column - 1];
		const auto *rest = cell.data() + read.returned;
		const auto remaining = cell.size() - read.returned;
		const auto as_text = request.c_type == SQL_C_CHAR;
		if (as_text && IsBinaryColumn(described) && NumericCast<SQLLEN>(remaining) >= request.capacity) {
			return RepeatFirstPart(handle, request, read, cell);
		}
		const auto copied = as_text ? CopyText(rest, remaining, request.target, request.capacity)
		                            : CopyBytes(rest, remaining, request.target, request.capacity);
		read.returned += copied;
		read.complete = copied == remaining;
		const bool no_total = !read.complete && described.report_no_total;
		Store(request.length, no_total ? SQLLEN(SQL_NO_TOTAL) : NumericCast<SQLLEN>(remaining));
		if (read.complete) {
			return SQL_SUCCESS;
		}
		if (PostsTruncation(described, request.c_type)) {
			PostTruncation(handle);
		}
		return SQL_SUCCESS_WITH_INFO;
	}

	//! Whether `capacity` bytes hold a NUMERIC or DECIMAL cell's whole digits, sign and NUL: the text
	//! before the point, or all of it, and one byte more (rsutil.c calculateMinNumericBufferLength).
	static bool HoldsWholeDigits(const std::string &cell, SQLLEN capacity) {
		const auto point = cell.find('.');
		const auto whole = point == std::string::npos ? cell.size() : point;
		return NumericCast<SQLLEN>(whole + 1) <= capacity;
	}

	//! SQL_SUCCESS unless a NUMERIC or DECIMAL cell is read as SQL_C_CHAR into a buffer too small for
	//! its whole digits, which the driver refuses with 22003 before copying (rsutil.c
	//! validateNumericBufferSize), leaving the length and the column read as a failed conversion does.
	//! An empty cell is checked too.
	static SQLRETURN CheckNumericBuffer(Handle &handle, const DataRequest &request) {
		const auto sql_type = FindColumn(handle.statement, request.column)->sql_type;
		const auto &cell = CurrentCell(handle.statement, request.column);
		const bool is_decimal = sql_type == SQL_NUMERIC || sql_type == SQL_DECIMAL;
		if (request.c_type != SQL_C_CHAR || !is_decimal || HoldsWholeDigits(cell, request.capacity)) {
			return SQL_SUCCESS;
		}
		Store(request.length, NumericCast<SQLLEN>(cell.size()));
		MarkCellRead(handle, request);
		return Fail(handle, "22003", "Buffer too small for numeric value");
	}

	//! Returns the requested cell on the current row. A column returned in full reports no more data
	//! until the next SQLFetch, in any read order; the driver remembers only the column it last
	//! completed (rsresult.cpp RS_SQLGetData, iPrevhCol), so a test that reads a cell twice fails
	//! here, not live.
	static SQLRETURN ReadCell(Handle &handle, const DataRequest &request) {
		if (handle.statement.cursor.reads[request.column - 1].complete) {
			return SQL_NO_DATA;
		}
		if (IsNullCell(handle.statement, request.column)) {
			return ReadNull(handle, request);
		}
		// A column bound with an indicator and no buffer gets the cell's length alone, as the driver's
		// RS_SQLGetData converts nothing into a null target. For a binary column that is its bytes,
		// a departure: the driver's is the length of its hex text.
		if (!request.target) {
			Store(request.length, NumericCast<SQLLEN>(CurrentCell(handle.statement, request.column).size()));
			return SQL_SUCCESS;
		}
		const auto sql_type = FindColumn(handle.statement, request.column)->sql_type;
		const auto write_number = FindNumberWriter(request.c_type, sql_type);
		if (write_number) {
			return ReadNumber(handle, request, write_number);
		}
		const auto fits = CheckNumericBuffer(handle, request);
		if (fits != SQL_SUCCESS) {
			return fits;
		}
		return ReadBytes(handle, request);
	}

	//! Answers one SQLGetData call, or SQLFetch's read of one bound cell, once the dispatcher has found
	//! the statement.
	static SQLRETURN AnswerGetData(Handle &handle, const DataRequest &request) {
		// The driver zeroes the indicator before any check (rsresult.cpp RS_SQLGetData), so an early
		// refusal leaves 0.
		Store(request.length, SQLLEN(0));
		// The driver checks the buffer first, HY090 for a negative length and HY009 for a null target,
		// so no SQLGetData caller can probe a length with no buffer; SQLFetch's own read takes a null
		// target, as the driver's internal one does (rsresult.cpp RS_SQLGetData).
		if (request.capacity < 0) {
			return Fail(handle, "HY090", "Invalid string or buffer length");
		}
		if (!request.target && !request.from_fetch) {
			return FailNullPointer(handle);
		}
		const auto rc = CheckRead(handle, request);
		if (rc != SQL_SUCCESS) {
			return rc;
		}
		return ReadCell(handle, request);
	}

	//! Where one array element of a bound column is: ODBC's column-wise address, moved by the bind
	//! offset, for the value and its indicator, as the driver places them (rsresult.cpp
	//! RS_SQLFetchScroll). A null indicator stays null; a null buffer stays null, a departure, where
	//! the driver offsets the null pointer by the element and the bind offset and writes through it; a
	//! negative BufferLength, which fails the cell before any write, places no value.
	struct BoundElement {
		char *target;
		SQLLEN *indicator;
	};

	static BoundElement ElementOf(const Handle &handle, const BoundColumn &bound, size_t element) {
		const auto offset = handle.row_array.bind_offset ? *handle.row_array.bind_offset : SQLULEN(0);
		BoundElement at {nullptr, nullptr};
		if (bound.target && bound.capacity >= 0) {
			const auto stride = NumericCast<SQLULEN>(ElementLength(bound.c_type, bound.capacity));
			at.target = static_cast<char *>(bound.target) + offset + element * stride;
		}
		if (bound.indicator) {
			at.indicator = reinterpret_cast<SQLLEN *>(reinterpret_cast<char *>(bound.indicator + element) + offset);
		}
		return at;
	}

	//! Reads one bound cell of the current row into its array element through SQLGetData's own answer,
	//! as the driver reads each one with RS_SQLGetData. That call clears the error list first, so only
	//! the last cell's records outlive the fetch (rsresult.cpp RS_SQLGetData, RS_SQLFetchScroll).
	static SQLRETURN ReadBoundCell(Handle &handle, size_t element, SQLUSMALLINT number, const BoundColumn &bound) {
		handle.diags.clear();
		const auto at = ElementOf(handle, bound, element);
		return AnswerGetData(handle, DataRequest {number, bound.c_type, at.target, bound.capacity, at.indicator, true});
	}

	//! Whether the driver refuses `bound` over this row array: column-wise over more than one row, a
	//! column with no element length has no stride (rsresult.cpp RS_SQLFetchScroll).
	static bool HasNoElementLength(const Handle &handle, const BoundColumn &bound) {
		return handle.row_array.size > 1 && ElementLength(bound.c_type, bound.capacity) == 0;
	}

	//! Ends the fetch where the driver's column loop meets a column with no element length: its HY000
	//! goes ahead of the records the row's earlier cells left (rsresult.cpp RS_SQLFetchScroll).
	static SQLUSMALLINT StopAtNoElementLength(Handle &handle) {
		handle.diags.insert(handle.diags.begin(), {"HY000", 0, "Array element length is zero.", {}});
		return FETCH_STOPPED;
	}

	//! The status of the current row, `element` of the rowset, once its bound cells are read in bind
	//! order, or FETCH_STOPPED. The driver checks each column's element length as its loop reaches
	//! it, and a cell that fails ends the row first, so a later column is never checked. A row
	//! scripted in error stands for a failed first cell: it zeroes that cell's indicator, as the
	//! driver's read does before any check, posts its records and writes no value.
	static SQLUSMALLINT FetchRow(Handle &handle, size_t element) {
		const auto &statement = handle.statement;
		const auto &result = CurrentResult(statement);
		const auto row = statement.cursor.fetched - 1;
		if (row < result.row_status.size() && result.row_status[row] == SQL_ROW_ERROR) {
			if (!handle.columns.empty()) {
				const auto &first = handle.columns.front().second;
				if (HasNoElementLength(handle, first)) {
					return StopAtNoElementLength(handle);
				}
				Store(ElementOf(handle, first, element).indicator, SQLLEN(0));
			}
			// AddResult refuses an SQL_ROW_ERROR row with no diagnostics, so this entry is there.
			handle.diags = result.row_diags.at(row);
			return SQL_ROW_ERROR;
		}
		SQLUSMALLINT status = SQL_ROW_SUCCESS;
		for (const auto &entry : handle.columns) {
			if (HasNoElementLength(handle, entry.second)) {
				return StopAtNoElementLength(handle);
			}
			const auto rc = ReadBoundCell(handle, element, entry.first, entry.second);
			if (rc == SQL_ERROR) {
				return SQL_ROW_ERROR;
			}
			if (rc == SQL_SUCCESS_WITH_INFO) {
				status = SQL_ROW_SUCCESS_WITH_INFO;
			}
		}
		return status;
	}

	//! Fetches `count` rows from `first` into the row array and reports them as the driver does: each
	//! row's status, SQL_ROW_NOROW in the first element past them, and the rows fetched; SQL_ERROR when
	//! every row failed, SQL_SUCCESS_WITH_INFO when one failed or was cut, and SQL_NO_DATA for none
	//! (rsresult.cpp RS_SQLFetchScroll).
	static SQLRETURN FetchRowset(Handle &handle, size_t first, size_t count) {
		const auto &rows = handle.row_array;
		auto &cursor = handle.statement.cursor;
		const auto columns = CurrentResult(handle.statement).columns.size();
		size_t failed = 0;
		bool warned = false;
		for (size_t element = 0; element < count; element++) {
			cursor.fetched = first + element + 1;
			cursor.reads.assign(columns, ColumnRead());
			const auto status = FetchRow(handle, element);
			// The driver leaves this row's status and the rows fetched unwritten.
			if (status == FETCH_STOPPED) {
				cursor.reads.assign(columns, ColumnRead());
				return SQL_ERROR;
			}
			if (rows.row_status) {
				rows.row_status[element] = status;
			}
			failed += status == SQL_ROW_ERROR ? 1 : 0;
			warned = warned || status == SQL_ROW_SUCCESS_WITH_INFO;
		}
		if (count < rows.size && rows.row_status) {
			rows.row_status[count] = SQL_ROW_NOROW;
		}
		Store(rows.rows_fetched, NumericCast<SQLULEN>(count));
		// Each row starts every column unread, so SQLGetData after the fetch reads from the start. The
		// driver resets only bound columns' offsets, so a part-read unbound column keeps its offset
		// there; the fake starts afresh, so none carries over unnoticed.
		cursor.reads.assign(columns, ColumnRead());
		if (count == 0) {
			return SQL_NO_DATA;
		}
		if (failed == count) {
			return SQL_ERROR;
		}
		return failed > 0 || warned ? SQL_SUCCESS_WITH_INFO : SQL_SUCCESS;
	}

	//! Advances one rowset of the row-array size, or reports no more, as the driver's SQLFetch does
	//! for a set with no rows, a DML statement's among them. A drained statement has no result to
	//! fetch from and posts 24000.
	static SQLRETURN Fetch(SQLHSTMT statement) {
		return DispatchOnHandle(
		    {"SQLFetch"}, statement, SQL_HANDLE_STMT, [](FakeOdbcDriver &, Handle &handle) -> SQLRETURN {
			    auto &state = handle.statement;
			    if (state.step != StatementStep::EXECUTED || state.cursor.drained) {
				    return FailWithoutCursor(handle);
			    }
			    const auto &result = CurrentResult(state);
			    auto &cursor = state.cursor;
			    const auto first = std::min(cursor.fetched, result.rows.size());
			    const auto count = std::min(NumericCast<size_t>(handle.row_array.size), result.rows.size() - first);
			    if (count == 0) {
				    cursor.fetched = result.rows.size() + 1;
			    }
			    return FetchRowset(handle, first, count);
		    });
	}

	//! Logs "column c_type", as the fake answers a binary cell read as SQL_C_CHAR with the fixture's
	//! bytes where the driver answers hex: only the log shows a VARBYTE read as text. A warned read of
	//! the rest of a cell leaves it unread, as the driver's (rsresult.cpp RS_SQLGetData, iPrevhCol).
	static SQLRETURN GetData(SQLHSTMT statement, SQLUSMALLINT column, SQLSMALLINT c_type, SQLPOINTER target,
	                         SQLLEN capacity, SQLLEN *length) {
		return DispatchOnHandleWithScript(
		    {"SQLGetData", std::to_string(column) + " " + std::to_string(c_type)}, statement, SQL_HANDLE_STMT,
		    [&](FakeOdbcDriver &, Handle &handle, const Scripted &scripted) -> SQLRETURN {
			    if (scripted.failure) {
				    return scripted.failure->rc;
			    }
			    const auto rc = AnswerGetData(handle, DataRequest {column, c_type, target, capacity, length});
			    // Only a read that returned the rest of its cell reports SQL_SUCCESS.
			    if (scripted.warning && rc == SQL_SUCCESS) {
				    handle.statement.cursor.reads[column - 1] = ColumnRead();
			    }
			    return rc;
		    });
	}

	//! SQL_UNBIND drops every bound column. Any other option fails, where the driver ignores one it
	//! does not know and frees the statement for SQL_DROP (rsconnect.cpp RS_SQLFreeStmt), so a test
	//! that needs either fails visibly.
	static SQLRETURN FreeStmt(SQLHSTMT statement, SQLUSMALLINT option) {
		return DispatchOnHandle({"SQLFreeStmt", std::to_string(option)}, statement, SQL_HANDLE_STMT,
		                        [&](FakeOdbcDriver &, Handle &handle) -> SQLRETURN {
			                        switch (option) {
			                        case SQL_CLOSE:
				                        Close(handle.statement);
				                        return SQL_SUCCESS;
			                        case SQL_UNBIND:
				                        handle.columns.clear();
				                        return SQL_SUCCESS;
			                        case SQL_RESET_PARAMS:
				                        handle.parameters.clear();
				                        return SQL_SUCCESS;
			                        default:
				                        return Fail(handle, "HY092", "Invalid attribute/option identifier");
			                        }
		                        });
	}

	//! As SQLFreeStmt(SQL_CLOSE), but refused with 24000 when there is no cursor to close: the driver
	//! refuses a statement allocated or closed, and accepts one prepared or drained (rsprepare.cpp
	//! SQLCloseCursor). Teardown code reaches both.
	static SQLRETURN CloseCursor(SQLHSTMT statement) {
		return DispatchOnHandle({"SQLCloseCursor"}, statement, SQL_HANDLE_STMT,
		                        [](FakeOdbcDriver &, Handle &handle) -> SQLRETURN {
			                        const auto step = handle.statement.step;
			                        if (step == StatementStep::ALLOCATED || step == StatementStep::CLOSED) {
				                        return FailCursorState(handle);
			                        }
			                        Close(handle.statement);
			                        return SQL_SUCCESS;
		                        });
	}

	//! Writes one record into SQLGetDiagRec's out-params.
	static SQLRETURN WriteRecord(const std::vector<FakeDiag> &diags, SQLSMALLINT record, SQLCHAR *sqlstate,
	                             SQLINTEGER *native, SQLCHAR *message, SQLSMALLINT capacity,
	                             SQLSMALLINT *message_length) {
		const auto rc = CheckRecord(diags, record);
		if (rc != SQL_SUCCESS) {
			return rc;
		}
		const auto &diag = diags[record - 1];
		// A SQLSTATE is five characters, so this buffer always holds it whole.
		WriteText<SQLSMALLINT>(diag.sqlstate, sqlstate, SQL_SQLSTATE_SIZE + 1, nullptr);
		Store(native, diag.native);
		// RS_SQLError (rserror.cpp) copies the message with copyStrDataSmallLen and no error list.
		return CopySmallLenText(diag.message, message, capacity, message_length).rc;
	}

	static SQLRETURN GetDiagRec(SQLSMALLINT type, SQLHANDLE handle, SQLSMALLINT record, SQLCHAR *sqlstate,
	                            SQLINTEGER *native, SQLCHAR *message, SQLSMALLINT capacity,
	                            SQLSMALLINT *message_length) {
		return DispatchOnDiagnostics(
		    {"SQLGetDiagRec"}, handle, type, [&](const FakeOdbcDriver &, const std::vector<FakeDiag> &diags) {
			    return WriteRecord(diags, record, sqlstate, native, message, capacity, message_length);
		    });
	}

	//! Writes one diagnostic field into SQLGetDiagField's out-params.
	static SQLRETURN WriteField(const std::vector<FakeDiag> &diags, SQLINTEGER record_count, SQLSMALLINT record,
	                            SQLSMALLINT field, SQLPOINTER value, SQLSMALLINT capacity, SQLSMALLINT *length) {
		// The record count is a header field, which ODBC reads whatever the record number.
		if (field == SQL_DIAG_NUMBER) {
			*static_cast<SQLINTEGER *>(value) = record_count;
			return SQL_SUCCESS;
		}
		const auto rc = CheckRecord(diags, record);
		if (rc != SQL_SUCCESS) {
			return rc;
		}
		const auto &diag = diags[record - 1];
		// Measured on driver 2.3.0.0: it answers these three from the record it holds.
		switch (field) {
		case SQL_DIAG_SQLSTATE:
			return WriteText(diag.sqlstate, value, capacity, length);
		case SQL_DIAG_MESSAGE_TEXT:
			return WriteText(diag.message, value, capacity, length);
		case SQL_DIAG_NATIVE:
			*static_cast<SQLINTEGER *>(value) = diag.native;
			return SQL_SUCCESS;
		default:
			break;
		}
		// As driver 2.3.0.0 answers an identifier it does not know, a driver-specific one included
		// (rserror.cpp RsError::RS_SQLGetDiagField).
		const auto entry = diag.fields.find(field);
		return entry == diag.fields.end() ? SQLRETURN(SQL_NO_DATA) : WriteText(entry->second, value, capacity, length);
	}

	static SQLRETURN GetDiagField(SQLSMALLINT type, SQLHANDLE handle, SQLSMALLINT record, SQLSMALLINT field,
	                              SQLPOINTER value, SQLSMALLINT capacity, SQLSMALLINT *length) {
		return DispatchOnDiagnostics(
		    {"SQLGetDiagField"}, handle, type, [&](const FakeOdbcDriver &fake, const std::vector<FakeDiag> &diags) {
			    const auto count = fake.record_count.value_or(NumericCast<SQLINTEGER>(diags.size()));
			    return WriteField(diags, count, record, field, value, capacity, length);
		    });
	}

	//! Sets what a row-array SQLFetch reads: the array size, which ODBC passes in the pointer itself,
	//! and the rows-fetched, row-status and bind-offset pointers. Any other attribute fails with HYC00,
	//! a departure: the driver takes the others, which the fake does not model.
	static SQLRETURN SetStmtAttr(SQLHSTMT statement, SQLINTEGER attribute, SQLPOINTER value, SQLINTEGER) {
		return DispatchOnHandle(
		    {"SQLSetStmtAttr", StatementAttributeText(attribute, value)}, statement, SQL_HANDLE_STMT,
		    [&](FakeOdbcDriver &, Handle &handle) -> SQLRETURN {
			    auto &rows = handle.row_array;
			    switch (attribute) {
			    case SQL_ATTR_ROW_ARRAY_SIZE: {
				    // The driver reads the size as an int, the value's low 32 bits, and refuses one not above
				    // 0 before storing it, with an HY024 on the ARD, whose records a statement's SQLGetDiagRec
				    // does not read (rsdesc.cpp RsDesc::RS_SQLSetDescField, rserror.cpp RS_SQLError).
				    const auto size = reinterpret_cast<uintptr_t>(value) & std::numeric_limits<uint32_t>::max();
				    if (size == 0 || size > NumericCast<uintptr_t>(std::numeric_limits<int32_t>::max())) {
					    return SQL_ERROR;
				    }
				    rows.size = size;
				    return SQL_SUCCESS;
			    }
			    case SQL_ATTR_ROWS_FETCHED_PTR:
				    rows.rows_fetched = static_cast<SQLULEN *>(value);
				    return SQL_SUCCESS;
			    case SQL_ATTR_ROW_STATUS_PTR:
				    rows.row_status = static_cast<SQLUSMALLINT *>(value);
				    return SQL_SUCCESS;
			    case SQL_ATTR_ROW_BIND_OFFSET_PTR:
				    rows.bind_offset = static_cast<SQLULEN *>(value);
				    return SQL_SUCCESS;
			    default:
				    return Fail(handle, "HYC00",
				                "the fake driver sets only the row-array size and the rows-fetched, row-status and "
				                "bind-offset pointers; set one of those");
			    }
		    });
	}

	//! Keeps where SQLFetch writes the column, column-wise from the first array element, checking no
	//! more than the driver does (rsresult.cpp RS_STMT_INFO::RS_SQLBindCol): a null buffer and indicator
	//! unbind it, column 0 is HY000, and SQLFetch finds a negative BufferLength. A C type the fake does
	//! not return is refused here, as SQLGetData refuses it, a departure: the driver binds any type.
	static SQLRETURN BindCol(SQLHSTMT statement, SQLUSMALLINT column, SQLSMALLINT c_type, SQLPOINTER target,
	                         SQLLEN capacity, SQLLEN *indicator) {
		const auto detail = std::to_string(column) + " " + std::to_string(c_type) + " " + std::to_string(capacity);
		return DispatchOnHandle(
		    {"SQLBindCol", detail}, statement, SQL_HANDLE_STMT, [&](FakeOdbcDriver &, Handle &handle) -> SQLRETURN {
			    if (column == 0) {
				    return Fail(handle, "HY000", "SQLBindCol: Invalid column number");
			    }
			    auto &columns = handle.columns;
			    auto bound = std::find_if(columns.begin(), columns.end(),
			                              [&](const BoundEntry &entry) { return entry.first == column; });
			    if (!target && !indicator) {
				    if (bound != columns.end()) {
					    columns.erase(bound);
				    }
				    return SQL_SUCCESS;
			    }
			    if (!IsReturnedType(c_type)) {
				    return FailUnreturnedType(handle);
			    }
			    // A rebind keeps the column's place in the bind order, as the driver finds
			    // its record; a new one goes last (rsutil.c addDescriptorRec).
			    const BoundColumn binding {c_type, target, capacity, indicator};
			    if (bound != columns.end()) {
				    bound->second = binding;
			    } else {
				    columns.emplace_back(column, binding);
			    }
			    return SQL_SUCCESS;
		    });
	}

	//! Positions nothing: the fake's cursor is forward-only, as the driver's streaming cursor is, so
	//! SQL_POSITION with no lock change fails with the driver's HY106, and another operation or a lock
	//! change with its HYC00, each with the driver's text (rsresult.cpp SQLSetPos).
	static SQLRETURN SetPos(SQLHSTMT statement, SQLSETPOSIROW, SQLUSMALLINT operation, SQLUSMALLINT lock) {
		return DispatchOnHandle({"SQLSetPos", std::to_string(operation)}, statement, SQL_HANDLE_STMT,
		                        [&](FakeOdbcDriver &, Handle &handle) -> SQLRETURN {
			                        if (operation != SQL_POSITION) {
				                        return Fail(handle, "HYC00", "Optional feature not implemented: hOperation");
			                        }
			                        if (lock != SQL_LOCK_NO_CHANGE) {
				                        return Fail(handle, "HYC00", "Optional feature not implemented-SQL_POSITION");
			                        }
			                        return Fail(handle, "HY106", "Cursor is forward only or non-scrollable");
		                        });
	}
};

bool FakeGate::WaitUntilHeldFor(std::chrono::milliseconds timeout) {
	std::unique_lock<std::mutex> lock(mutex);
	// Against the system clock, not wait_for's steady clock: libstdc++ 11 waits on a steady clock
	// with pthread_cond_clockwait, which GCC 11's ThreadSanitizer does not intercept, so
	// test_client_tsan reported this mutex as locked twice.
	return changed.wait_until(lock, std::chrono::system_clock::now() + timeout, [this]() { return held; });
}

void FakeGate::Release() {
	std::lock_guard<std::mutex> guard(mutex);
	released = true;
	changed.notify_all();
}

void FakeGate::Hold() {
	std::unique_lock<std::mutex> lock(mutex);
	held = true;
	changed.notify_all();
	changed.wait(lock, [this]() { return released; });
}

FakeOdbcDriver::FakeOdbcDriver() {
#define REDSHIFT_FAKE_ENTRY_POINT(name) driver.api.name = &EntryPoints::name;
	REDSHIFT_ODBC_ENTRY_POINTS(REDSHIFT_FAKE_ENTRY_POINT)
#undef REDSHIFT_FAKE_ENTRY_POINT
	driver.identity.origin = DriverOrigin::TEST_FAKE;
	driver.identity.version = "fake";
	driver.identity.platform = DuckDB::Platform();
	load_result.driver = &driver;
	info[SQL_GETDATA_EXTENSIONS] = SQL_GD_ANY_COLUMN | SQL_GD_ANY_ORDER | SQL_GD_BOUND;
	AddResult(DEFAULT_SQL, {DefaultResult()});
	std::lock_guard<std::mutex> guard(fake_mutex);
	if (current_fake) {
		throw InternalException("a FakeOdbcDriver is already current; destroy it before constructing another");
	}
	current_fake = this;
}

FakeOdbcDriver::~FakeOdbcDriver() {
	bool allow_misuse = false;
	{
		std::lock_guard<std::mutex> guard(fake_mutex);
		current_fake = nullptr;
		fake_generation++;
		allow_misuse = allow_abandoned_misuse;
	}
	// Read after this fake stops being current, so no trampoline can still be changing it.
	const std::string leaked = AllocatedHandles();
	const std::string misused = allow_misuse ? std::string() : MisusedAbandonedHandles();
	// D_ASSERT is compiled out (NDEBUG) in make test_client's builds, and FAIL or REQUIRE would
	// throw out of this destructor. FAIL_CHECK still throws once Catch is aborting (-a, -x), by
	// which point the failure is recorded.
	try {
		if (!leaked.empty()) {
			FAIL_CHECK("the fake driver was destroyed with handles still allocated ("
			           << leaked
			           << "); free each before the fake is destroyed, the environment through the ODBC client's "
			              "test-only reset");
		}
		if (!misused.empty()) {
			FAIL_CHECK("the fake driver received a call on a handle it had abandoned after a scripted SQLFreeHandle "
			           "failure ("
			           << misused
			           << "); such a handle is dropped, not called again, and only SQLGetDiagRec and SQLGetDiagField "
			              "may read it");
		}
	} catch (const Catch::TestFailureException &) { // NOLINT: the failure is already recorded
	}
}

DriverLoadResult FakeOdbcDriver::Source() {
	std::lock_guard<std::mutex> guard(fake_mutex);
	if (!current_fake) {
		throw InternalException("no FakeOdbcDriver is current; construct one before connecting");
	}
	current_fake->source_calls++;
	return current_fake->load_result;
}

size_t FakeOdbcDriver::SourceCalls() const {
	std::lock_guard<std::mutex> guard(fake_mutex);
	return source_calls;
}

void FakeOdbcDriver::FailLoad(DriverErrorKind kind, std::string message) {
	std::lock_guard<std::mutex> guard(fake_mutex);
	load_result.driver = nullptr;
	load_result.error.kind = kind;
	load_result.error.message = std::move(message);
}

void FakeOdbcDriver::SetRefusedEnvironment(std::vector<std::string> names) {
	std::lock_guard<std::mutex> guard(fake_mutex);
	driver.refused_environment = std::move(names);
}

void FakeOdbcDriver::SetOverriddenEnvironment(std::vector<OverriddenVariable> variables) {
	std::lock_guard<std::mutex> guard(fake_mutex);
	driver.overridden_environment = std::move(variables);
}

void FakeOdbcDriver::SetTrustDirectory(std::string directory) {
	std::lock_guard<std::mutex> guard(fake_mutex);
	driver.trust_directory = std::move(directory);
}

void FakeOdbcDriver::SetConnectOutcome(SQLRETURN rc, std::vector<FakeDiag> diags) {
	std::lock_guard<std::mutex> guard(fake_mutex);
	connect_outcome = Outcome {rc, std::move(diags)};
}

void FakeOdbcDriver::SetConnectionDead(bool dead) {
	std::lock_guard<std::mutex> guard(fake_mutex);
	connection_dead = dead;
}

void FakeOdbcDriver::SetMessageLimit(size_t bytes) {
	std::lock_guard<std::mutex> guard(fake_mutex);
	message_limit = bytes;
}

void FakeOdbcDriver::SetRecordCount(SQLINTEGER count) {
	std::lock_guard<std::mutex> guard(fake_mutex);
	record_count = count;
}

void FakeOdbcDriver::SetInfo(SQLUSMALLINT info_type, SQLUINTEGER value) {
	std::lock_guard<std::mutex> guard(fake_mutex);
	info[info_type] = value;
}

FakeOdbcDriver::EntryPointScript &FakeOdbcDriver::ScriptFor(const char *entry_point, int nth_call) {
	// Either mistake would leave the script never firing, and a test passing without the call it scripted.
	if (!IsEntryPoint(entry_point)) {
		throw InternalException("%s is not in REDSHIFT_ODBC_ENTRY_POINTS; name an entry point such as SQLFetch",
		                        entry_point);
	}
	auto &script = scripts[entry_point];
	if (nth_call <= script.calls_made) {
		throw InternalException("%s has had %d calls on this fake; script a later one, counting from 1", entry_point,
		                        script.calls_made);
	}
	return script;
}

void FakeOdbcDriver::FailCall(const char *entry_point, int nth_call, SQLRETURN rc, std::vector<FakeDiag> diags) {
	std::lock_guard<std::mutex> guard(fake_mutex);
	auto &script = ScriptFor(entry_point, nth_call);
	// The scripted code replaces the call's work, so a success code would report that the call
	// worked while none of its effects happened: SQLExecute would leave the statement unexecuted and
	// SQLAllocHandle would leave the output handle null, neither of which a driver does.
	if (rc == SQL_SUCCESS || rc == SQL_SUCCESS_WITH_INFO) {
		throw InternalException("FailCall cannot script a success code for %s, because the scripted code replaces the "
		                        "call's work; use WarnCall to keep that work, or script an error such as SQL_ERROR",
		                        entry_point);
	}
	script.failures[nth_call] = Outcome {rc, std::move(diags)};
}

void FakeOdbcDriver::WarnCall(const char *entry_point, int nth_call, std::vector<FakeDiag> diags) {
	std::lock_guard<std::mutex> guard(fake_mutex);
	auto &script = ScriptFor(entry_point, nth_call);
	// A successful free leaves no handle for the records and, in ODBC and the driver, never reports
	// SQL_SUCCESS_WITH_INFO (rsconnect.cpp RS_SQLFreeHandle). A diagnostic read leaves its records
	// as they are, so its warning would never fire.
	const std::string name = entry_point;
	if (name == "SQLFreeHandle" || name == "SQLGetDiagRec" || name == "SQLGetDiagField") {
		throw InternalException("WarnCall cannot warn %s: a free leaves no handle to carry the records and never "
		                        "reports SQL_SUCCESS_WITH_INFO, and a diagnostic read does not change them; warn the "
		                        "call that posts the records instead",
		                        entry_point);
	}
	// A warning with no records would never arrive. The driver's own SQL_SUCCESS_WITH_INFO with no
	// record, for a cut binary read, is ReadBytes's to model.
	if (diags.empty()) {
		throw InternalException("WarnCall needs a record for %s, because adding records is what it is for; a "
		                        "SQL_SUCCESS_WITH_INFO that posts none is the fake's own answer to a cut binary read",
		                        entry_point);
	}
	script.warnings[nth_call] = std::move(diags);
}

void FakeOdbcDriver::BlockCall(const char *entry_point, int nth_call, std::shared_ptr<FakeGate> gate) {
	std::lock_guard<std::mutex> guard(fake_mutex);
	ScriptFor(entry_point, nth_call).gates[nth_call] = std::move(gate);
}

void FakeOdbcDriver::AddResult(std::string sql, std::vector<FakeResultSet> results, SQLSMALLINT param_count) {
	// A statement that returns no rows still needs a set, with no columns, to carry its row count.
	if (results.empty()) {
		throw InternalException("AddResult for \"%s\" has no result set; a statement that returns no rows takes one "
		                        "result set with no columns",
		                        sql);
	}
	for (const auto &result : results) {
		CheckShape(sql, result);
	}
	Statement statement;
	statement.step = StatementStep::PREPARED;
	statement.param_count = param_count;
	statement.is_dml = IsDml(sql);
	statement.results = std::move(results);
	std::lock_guard<std::mutex> guard(fake_mutex);
	scripted_statements[std::move(sql)] = std::move(statement);
}

std::vector<FakeColumn> FakeOdbcDriver::ScriptedColumns(const std::string &sql) {
	std::lock_guard<std::mutex> guard(fake_mutex);
	if (!current_fake) {
		throw InternalException("no FakeOdbcDriver is current; construct one before reading its scripts");
	}
	const auto script = current_fake->scripted_statements.find(sql);
	if (script == current_fake->scripted_statements.end()) {
		return {};
	}
	return script->second.results.front().columns;
}

std::string FakeOdbcDriver::LastConnectionString() const {
	std::lock_guard<std::mutex> guard(fake_mutex);
	return last_connection_string;
}

FakeConnectOutput FakeOdbcDriver::LastConnectOutput() const {
	std::lock_guard<std::mutex> guard(fake_mutex);
	return last_connect_output;
}

const std::vector<FakeCall> &FakeOdbcDriver::Calls() const {
	return calls;
}

std::string FakeOdbcDriver::AllocatedHandles() const {
	std::lock_guard<std::mutex> guard(fake_mutex);
	std::string names;
	for (const auto &entry : handles) {
		// An abandoned handle is one the test deliberately left behind, so it is nobody's leak.
		if (entry.second.abandoned) {
			continue;
		}
		names += (names.empty() ? "" : ", ") + std::string(HandleTypeName(entry.second.type));
	}
	return names;
}

std::string FakeOdbcDriver::MisusedAbandonedHandles() const {
	std::lock_guard<std::mutex> guard(fake_mutex);
	std::string names;
	for (const auto &misuse : abandoned_misuses) {
		names += (names.empty() ? "" : ", ") + misuse;
	}
	return names;
}

void FakeOdbcDriver::AllowAbandonedHandleMisuse() {
	std::lock_guard<std::mutex> guard(fake_mutex);
	allow_abandoned_misuse = true;
}

} // namespace testing
} // namespace redshift_driver
} // namespace duckdb
