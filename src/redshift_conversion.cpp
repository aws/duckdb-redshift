#include "redshift_conversion.hpp"

#include "duckdb/common/exception/binder_exception.hpp"
#include "duckdb/common/operator/cast_operators.hpp"
#include "duckdb/common/operator/decimal_cast_operators.hpp"
#include "duckdb/common/string_util.hpp"
#include "duckdb/common/sql_identifier.hpp"
#include "duckdb/common/types/date.hpp"
#include "duckdb/common/types/time.hpp"
#include "duckdb/common/types/timestamp.hpp"
#include "duckdb/function/cast/default_casts.hpp"
#include "duckdb/main/query_result.hpp"
#include "redshift_messages.hpp"
#include "redshift_transport.hpp"

#include <cstring>

namespace duckdb {

namespace {

//! The label an error message uses for a column: its 1-based position, and its name when it has one.
string ColumnLabel(const RedshiftColumn &column, idx_t column_index) {
	auto position = "column " + std::to_string(column_index + 1);
	// Quoted as an identifier, so a name containing " reads unambiguously.
	return column.name.empty() ? position : position + " (" + SQLQuotedIdentifier::ToString(column.name) + ")";
}

//! Refuse a column at bind. Nothing has run on Redshift yet, so the message says so.
BinderException ErrorUnsupportedColumn(const RedshiftColumn &column, idx_t column_index, const string &reason) {
	return BinderException("Redshift %s %s. Cast it to a supported type in the Redshift SQL. No Redshift statement "
	                       "was executed.",
	                       ColumnLabel(column, column_index), reason);
}

//! Refuse a described shape whose returns_rows flag and columns disagree, as `description` says. A
//! correct client reports columns exactly when the statement has a result set, so this is a fault in
//! the client, not in the user's SQL. Nothing has run on Redshift yet, so the message says so.
BinderException ErrorInvalidShape(const RedshiftCaller &caller, const string &description) {
	return BinderException("%s: the Redshift client described the statement on handle %s as %s, which is not a valid "
	                       "result shape. This is a fault in the client, not in the SQL. No Redshift statement was "
	                       "executed.",
	                       caller.Name(), SQLQuotedIdentifier::ToString(caller.handle), description);
}

//! Build DECIMAL(precision, scale) after checking DuckDB can represent it.
//!
//! LogicalType::DECIMAL takes uint8_t width and scale and checks them only with a debug
//! assertion, so out-of-range metadata must be refused before it is called.
LogicalType MapDecimal(const RedshiftColumn &column, idx_t column_index) {
	auto max_width = static_cast<int32_t>(DecimalType::MaxWidth());
	if (column.precision < 1 || column.precision > max_width || column.scale < 0 || column.scale > column.precision) {
		throw ErrorUnsupportedColumn(
		    column, column_index,
		    StringUtil::Format("is DECIMAL(%d,%d), which DuckDB cannot represent; DuckDB supports "
		                       "precision 1 to %d and scale 0 to the precision",
		                       column.precision, column.scale, max_width));
	}
	return LogicalType::DECIMAL(static_cast<uint8_t>(column.precision), static_cast<uint8_t>(column.scale));
}

//! A transport contract violation found during the scan.
ExecutorException ErrorContractViolation(const string &detail) {
	return ExecutorException("Redshift returned a result that does not match the shape described at bind: %s. The "
	                         "remote table may have changed since the statement was bound. %s",
	                         detail, REDSHIFT_ALREADY_EXECUTED);
}

//! One cell being written: the column it belongs to, which errors name, and where its value goes.
struct Cell {
	const RedshiftColumn &column;
	idx_t column_index;
	Vector &target;
	idx_t row;

	template <class T>
	void Store(T value) const {
		FlatVector::GetDataMutable<T>(target)[row] = value;
	}
};

//! A value DuckDB refuses for its column. Running the statement again gives the same value, so the
//! message says how to read it anyway.
ConversionException ErrorValueRefused(const Cell &cell, const string &detail) {
	return ConversionException("Could not convert Redshift %s: %s. To read the value as text, cast the column to "
	                           "VARCHAR in the Redshift SQL. %s",
	                           ColumnLabel(cell.column, cell.column_index), detail, REDSHIFT_ALREADY_EXECUTED);
}

//! Helper for error messages. Describes a cell's value kind in words, such as "a TEXT value", so
//! the error raised when a cell arrives with the wrong kind can say what actually arrived.
const char *KindName(RedshiftValueKind kind) {
	switch (kind) {
	case RedshiftValueKind::NULL_VALUE:
		return "NULL";
	case RedshiftValueKind::BOOLEAN:
		return "a BOOLEAN value";
	case RedshiftValueKind::INTEGER:
		return "an INTEGER value";
	case RedshiftValueKind::REAL:
		return "a REAL value";
	case RedshiftValueKind::TEXT:
		return "a TEXT value";
	case RedshiftValueKind::BLOB:
		return "a BLOB value";
	}
	return "an unrecognised value kind";
}

//! Move the transport's trailing " BC" to the " (BC)" DuckDB reads directly after the date, so
//! "0044-03-15 12:00:00+00 BC" becomes "0044-03-15 (BC) 12:00:00+00". Returns an empty string,
//! which allocates nothing, when `text` has no such suffix.
string RewriteBC(string_t text) {
	auto data = text.GetData();
	auto size = text.GetSize();
	if (size <= 3 || data[size - 3] != ' ' || data[size - 2] != 'B' || data[size - 1] != 'C') {
		return string();
	}
	string body(data, size - 3);
	auto date_end = body.find(' ');
	if (date_end == string::npos) {
		return body + " (BC)";
	}
	return body.substr(0, date_end) + " (BC)" + body.substr(date_end);
}

//! Reads text against a fixed grammar, one part at a time. Each Read method consumes its part and
//! returns whether it was there. ReadChar and ReadSuffix consume nothing when they return false, so
//! a caller can try one and then another; after any other false Read the position
//! is not meaningful.
class TextReader {
public:
	//! `text` must outlive the reader: a string_t of up to 12 bytes keeps its characters inside
	//! itself, so the reader points into that object, not into a separate buffer.
	explicit TextReader(const string_t &text) : data(text.GetData()), size(text.GetSize()) {
	}
	//! A temporary would die at the end of the statement that builds the reader.
	explicit TextReader(string_t &&text) = delete;

	bool AtEnd() const {
		return pos == size;
	}

	bool ReadChar(char c) {
		if (pos < size && data[pos] == c) {
			pos++;
			return true;
		}
		return false;
	}

	//! A run of digits, all of them, whose length is between `min` and `max`.
	bool ReadDigits(idx_t min, idx_t max) {
		auto start = pos;
		while (pos < size && StringUtil::CharacterIsDigit(data[pos])) {
			pos++;
		}
		return pos - start >= min && pos - start <= max;
	}

	//! Two digits below 60, as an offset's minutes or seconds.
	bool ReadSexagesimal() {
		return ReadDigits(2, 2) && data[pos - 2] < '6';
	}

	//! Two digits from 00 to 15, as an offset's hours: DuckDB's TIMETZ range is +-15:59:59.
	bool ReadOffsetHours() {
		return ReadDigits(2, 2) && (data[pos - 2] == '0' || (data[pos - 2] == '1' && data[pos - 1] < '6'));
	}

	//! Exactly `suffix` and nothing after it. Consumes nothing when the rest of the text differs.
	bool ReadSuffix(const char *suffix) {
		auto length = strlen(suffix);
		if (size - pos != length || memcmp(data + pos, suffix, length) != 0) {
			return false;
		}
		pos = size;
		return true;
	}

private:
	const char *data;
	idx_t size;
	idx_t pos = 0;
};

//! Whether temporal text is in the grammar of the client's text forms for its type:
//!
//!   DATE         YYYY-MM-DD[ BC], or -YYY-MM-DD for an astronomical year before 1
//!   TIME         HH:MM:SS[.f]
//!   TIMETZ       HH:MM:SS[.f]+HH[:MM[:SS]]
//!   TIMESTAMP    YYYY-MM-DD HH:MM:SS[.f][ BC]
//!   TIMESTAMPTZ  YYYY-MM-DD HH:MM:SS[.f]+HH[:MM[:SS]][ BC]
//!
//! where `.f` is a '.' and 1 to 6 digits, a DATE year is 4 to 7 digits (3 to 7 after a '-'), a
//! timestamp year is 4 to 6 digits, the offset sign is '+' or '-', and offset minutes and seconds
//! are below 60. Offset hours are 00 to 15: that is DuckDB's TIMETZ range rather than the client's
//! grammar, and this check applies it to TIMESTAMPTZ too until the client's grammar states it.
//! DuckDB's parsers accept more than this: words such as
//! `epoch` and `infinity`, short years, and for times they keep only 6 fraction digits, read
//! "+0099" or "+00:99" as +01:39, apply any offset hour to a timestamp, and skip spaces and a 'T'
//! separator. This is a safety net: it refuses text DuckDB would misread, and accepts lossless
//! variants of the client's exact output, such as "+05:00" for "+05"; the ODBC client checks its exact output
//! itself. Other field ranges are left to the parser.
bool IsPlainTemporal(RedshiftTypeId type, string_t text) {
	TextReader in(text);
	if (type == RedshiftTypeId::DATE) {
		auto astronomical = in.ReadChar('-');
		if (!(in.ReadDigits(astronomical ? 3 : 4, 7) && in.ReadChar('-') && in.ReadDigits(2, 2) && in.ReadChar('-') &&
		      in.ReadDigits(2, 2))) {
			return false;
		}
		return in.AtEnd() || (!astronomical && in.ReadSuffix(" BC"));
	}
	auto is_timestamp = type == RedshiftTypeId::TIMESTAMP || type == RedshiftTypeId::TIMESTAMPTZ;
	auto has_offset = type == RedshiftTypeId::TIMETZ || type == RedshiftTypeId::TIMESTAMPTZ;
	if (is_timestamp && !(in.ReadDigits(4, 6) && in.ReadChar('-') && in.ReadDigits(2, 2) && in.ReadChar('-') &&
	                      in.ReadDigits(2, 2) && in.ReadChar(' '))) {
		return false;
	}
	if (!(in.ReadDigits(2, 2) && in.ReadChar(':') && in.ReadDigits(2, 2) && in.ReadChar(':') && in.ReadDigits(2, 2))) {
		return false;
	}
	if (in.ReadChar('.') && !in.ReadDigits(1, 6)) {
		return false;
	}
	if (has_offset) {
		if (!(in.ReadChar('+') || in.ReadChar('-')) || !in.ReadOffsetHours()) {
			return false;
		}
		// The offset's minutes, then its seconds.
		for (idx_t part = 0; part < 2 && in.ReadChar(':'); part++) {
			if (!in.ReadSexagesimal()) {
				return false;
			}
		}
	}
	return in.AtEnd() || (is_timestamp && in.ReadSuffix(" BC"));
}

//! Refuse temporal text outside IsPlainTemporal's grammar for its type, quoting the text as sent.
//! Called after the parse and the checks for missing or extra parts, whose messages are more
//! specific.
void CheckPlainTemporal(const Cell &cell, string_t sent) {
	if (IsPlainTemporal(cell.column.type, sent)) {
		return;
	}
	const char *form = "";
	switch (cell.column.type) {
	case RedshiftTypeId::DATE:
		form = "DATE text form, YYYY-MM-DD[ BC] or -YYY-MM-DD";
		break;
	case RedshiftTypeId::TIME:
		form = "TIME text form, HH:MM:SS[.f] where .f is 1 to 6 digits";
		break;
	case RedshiftTypeId::TIMETZ:
		form = "TIME WITH TIME ZONE text form, HH:MM:SS[.f]{+|-}HH[:MM[:SS]] where .f is 1 to 6 digits, HH is "
		       "00 to 15, and MM and SS are below 60";
		break;
	case RedshiftTypeId::TIMESTAMP:
		form = "TIMESTAMP text form, YYYY-MM-DD HH:MM:SS[.f][ BC] where .f is 1 to 6 digits";
		break;
	case RedshiftTypeId::TIMESTAMPTZ:
		form = "TIMESTAMP WITH TIME ZONE text form, YYYY-MM-DD HH:MM:SS[.f]{+|-}HH[:MM[:SS]][ BC] where .f is 1 to 6 "
		       "digits, HH is 00 to 15, and MM and SS are below 60";
		break;
	case RedshiftTypeId::BOOLEAN:
	case RedshiftTypeId::SMALLINT:
	case RedshiftTypeId::INTEGER:
	case RedshiftTypeId::BIGINT:
	case RedshiftTypeId::FLOAT:
	case RedshiftTypeId::DOUBLE:
	case RedshiftTypeId::DECIMAL:
	case RedshiftTypeId::VARCHAR:
	case RedshiftTypeId::CHAR:
	case RedshiftTypeId::BLOB:
	case RedshiftTypeId::UNKNOWN:
		// Only the temporal types' Store functions check text against a form.
		throw InternalException("RedshiftCellWriter: no text form for a non-temporal column");
	}
	throw ErrorValueRefused(cell, StringUtil::Format("\"%s\" is not in Redshift's %s", sent.GetString(), form));
}

//! Store an INTEGER value into a narrower integer column, refusing a value out of its range.
template <class T>
void StoreNarrowInteger(const Cell &cell, int64_t value, const char *type_name) {
	T narrowed;
	if (!TryCast::Operation<int64_t, T>(value, narrowed)) {
		throw ErrorValueRefused(cell, StringUtil::Format("value %d is out of range for %s", value, type_name));
	}
	cell.Store(narrowed);
}

//! Store a REAL value into a FLOAT column by DuckDB's own DOUBLE to FLOAT rule: NaN and infinities
//! pass, a finite value that overflows FLOAT is refused rather than becoming an infinity.
void StoreFloat(const Cell &cell, double value) {
	float narrowed;
	if (!TryCast::Operation<double, float>(value, narrowed)) {
		throw ErrorValueRefused(cell, StringUtil::Format("value %g is out of range for FLOAT", value));
	}
	cell.Store(narrowed);
}

//! Parse a TIME value from text, refusing anything after the time, so "12:34:56 junk" is refused
//! rather than read as 12:34:56. Uses the function DuckDB's VARCHAR to TIME cast calls for each
//! value.
void StoreTime(const Cell &cell, string_t text) {
	string error;
	// true: refuse anything after the time.
	CastParameters parameters(true, &error);
	dtime_t parsed;
	if (!TryCastErrorMessage::Operation<string_t, dtime_t>(text, parsed, parameters)) {
		throw ErrorValueRefused(cell, error);
	}
	CheckPlainTemporal(cell, text);
	cell.Store(parsed);
}

//! Parse a TIMETZ value from text. It must end with an offset, with nothing after it.
//!
//! DuckDB's cast gives text without an offset the offset +00, so a TIME value would pass as one.
void StoreTimeTZ(const Cell &cell, string_t text) {
	idx_t pos = 0;
	dtime_tz_t parsed;
	bool has_offset = false;
	// true: refuse anything after the offset.
	if (!Time::TryConvertTimeTZ(text.GetData(), text.GetSize(), pos, parsed, has_offset, true)) {
		throw ErrorValueRefused(cell, Time::ConversionError(text));
	}
	if (!has_offset) {
		throw ErrorValueRefused(cell, "\"" + text.GetString() +
		                                  "\" has no time zone offset, which a TIME WITH TIME ZONE needs");
	}
	CheckPlainTemporal(cell, text);
	cell.Store(parsed);
}

//! Parse a DATE value from text, refusing anything after the date, after moving a trailing ` BC`,
//! then refuse text outside the client's DATE form, such as `epoch` or `infinity`, which DuckDB's parser
//! reads as special dates.
void StoreDate(const Cell &cell, string_t sent) {
	auto rewritten = RewriteBC(sent);
	auto text = rewritten.empty() ? sent : string_t(rewritten);
	idx_t pos = 0;
	bool special = false;
	date_t parsed;
	switch (Date::TryConvertDate(text.GetData(), text.GetSize(), pos, parsed, special, true)) {
	case DateCastResult::SUCCESS:
		CheckPlainTemporal(cell, sent);
		cell.Store(parsed);
		return;
	case DateCastResult::ERROR_RANGE:
		// Errors quote the text as sent, not the rewritten form.
		throw ErrorValueRefused(cell, Date::RangeError(sent));
	default:
		throw ErrorValueRefused(cell, Date::FormatError(sent));
	}
}

//! Whether timestamp text has a time of day after its date. DuckDB's timestamp parser reads a
//! date alone as midnight, which would let a DATE value pass as a timestamp. Callers parse the
//! timestamp first, so the date here always parses.
bool HasTimeOfDay(string_t text) {
	idx_t pos = 0;
	date_t date;
	bool special = false;
	if (Date::TryConvertDate(text.GetData(), text.GetSize(), pos, date, special) != DateCastResult::SUCCESS) {
		// Not reached after a successful timestamp parse, which reads the same date first.
		return true;
	}
	while (pos < text.GetSize() && StringUtil::CharacterIsSpace(text.GetData()[pos])) {
		pos++;
	}
	return pos < text.GetSize();
}

//! A timestamp parsed from text, with what the text carried beyond the value.
struct ParsedTimestamp {
	//! The instant, with any offset already applied.
	timestamp_t value;
	bool has_time_of_day;
	//! A `Z` or `+HH[:MM[:SS]]`; the client's form has only the latter.
	bool has_offset;
	//! A zone name, such as ` UTC`.
	bool has_zone_name;
};

//! Parse timestamp text as sent, after moving a trailing ` BC`, raising DuckDB's own reason when the
//! text is malformed or out of range, including a value an offset moves out of range. Errors quote
//! the text as sent.
ParsedTimestamp ParseTimestamp(const Cell &cell, string_t sent) {
	auto rewritten = RewriteBC(sent);
	auto text = rewritten.empty() ? sent : string_t(rewritten);

	ParsedTimestamp parsed;
	string_t zone(nullptr, 0);
	switch (
	    Timestamp::TryConvertTimestampTZ(text.GetData(), text.GetSize(), parsed.value, true, parsed.has_offset, zone)) {
	case TimestampCastResult::SUCCESS:
		break;
	case TimestampCastResult::ERROR_RANGE:
		throw ErrorValueRefused(cell, Timestamp::RangeError(sent));
	default:
		throw ErrorValueRefused(cell, Timestamp::FormatError(sent));
	}
	// The parser returns the words `infinity` and `-infinity` as the infinity markers, and it
	// range-checks the local date and time before applying the offset with only an overflow check.
	// So an offset can carry a value onto the markers, or below DuckDB's earliest timestamp without
	// reaching `-infinity`, to a value DuckDB cannot convert back to a date and time. This refuses
	// all three.
	static const auto earliest = Timestamp::FromDatetime(
	    Date::FromDate(Timestamp::MIN_YEAR, Timestamp::MIN_MONTH, Timestamp::MIN_DAY), dtime_t(0));
	if (!parsed.value.IsFinite() || parsed.value < earliest) {
		throw ErrorValueRefused(cell, Timestamp::RangeError(sent));
	}
	// A date alone parses as midnight, so only a midnight value needs its text checked. A TIMESTAMP
	// with an offset is refused anyway, so its offset may move it to or from midnight.
	parsed.has_time_of_day = Timestamp::GetTime(parsed.value) != dtime_t(0) || HasTimeOfDay(text);
	parsed.has_zone_name = zone.GetSize() > 0;
	return parsed;
}

//! Parse a TIMESTAMP value from text. It must have a time of day and no time zone.
//!
//! DuckDB's string-to-TIMESTAMP cast cannot be told to refuse a trailing zone: its parser reads a
//! trailing `Z`, `+HH[:MM]` or zone name such as ` UTC` on a TIMESTAMP and drops it, so
//! "12:00:00+05" would be stored as 12:00. It also reads a date alone as midnight.
void StoreTimestamp(const Cell &cell, string_t sent) {
	auto parsed = ParseTimestamp(cell, sent);
	if (!parsed.has_time_of_day) {
		throw ErrorValueRefused(cell, "\"" + sent.GetString() + "\" has no time of day, which a TIMESTAMP needs");
	}
	if (parsed.has_offset || parsed.has_zone_name) {
		throw ErrorValueRefused(cell, "\"" + sent.GetString() + "\" has a time zone, which a TIMESTAMP does not take");
	}
	CheckPlainTemporal(cell, sent);
	cell.Store(parsed.value);
}

//! Parse a TIMESTAMPTZ value from text. It must have a time of day and a numeric offset, which is
//! applied; a zone name is refused, and so is a `Z`, which the client's form never has.
//!
//! DuckDB's cast reads text without an offset as UTC and a date alone as midnight UTC, so a
//! TIMESTAMP or DATE value would pass as an instant.
void StoreTimestampTZ(const Cell &cell, string_t sent) {
	auto parsed = ParseTimestamp(cell, sent);
	if (!parsed.has_time_of_day) {
		throw ErrorValueRefused(cell, "\"" + sent.GetString() +
		                                  "\" has no time of day, which a TIMESTAMP WITH TIME ZONE needs");
	}
	if (parsed.has_zone_name) {
		throw ErrorValueRefused(cell, "\"" + sent.GetString() +
		                                  "\" names a time zone; a TIMESTAMP WITH TIME ZONE needs a numeric offset");
	}
	if (!parsed.has_offset) {
		throw ErrorValueRefused(cell, "\"" + sent.GetString() +
		                                  "\" has no time zone offset, which a TIMESTAMP WITH TIME ZONE needs");
	}
	CheckPlainTemporal(cell, sent);
	cell.Store(timestamp_tz_t(parsed.value));
}

//! Checks that a received DECIMAL value is in a form the column's described type can hold
//! losslessly: an optional '-' and digits, with no more fraction digits than the described scale.
//! That is the client's plain-number form plus its lossless variants (fewer fraction digits, "-0").
//! DuckDB's parser would not refuse text outside it but change it: it rounds "12.345" in a
//! DECIMAL(10,2) to 12.35, and reads "1e2", "+1" and " 1" as numbers. Whatever made the text
//! differ, storing a different number is wrong, so such text is refused instead.
//!
//! Accepts an optional `-`, one or more digits, and then, only when `scale` > 0, optionally a `.`
//! followed by 1 to `scale` digits; nothing else. For DECIMAL(5,2), "12", "-0.5" and "12.34" pass,
//! while "12.345", "1e2", "+1", " 1", ".5" and "1." do not. A value with too many digits before the
//! point passes here and DuckDB's parser refuses it.
bool IsPlainDecimal(string_t text, int32_t scale) {
	TextReader in(text);
	in.ReadChar('-');
	if (!in.ReadDigits(1, text.GetSize())) {
		return false;
	}
	if (in.AtEnd()) {
		return true;
	}
	// With a scale of 0 no fraction digit is allowed, so a '.' is refused.
	return in.ReadChar('.') && in.ReadDigits(1, static_cast<idx_t>(scale)) && in.AtEnd();
}

//! Parse DECIMAL text into the column's storage type `T`, by the function DuckDB's VARCHAR to
//! DECIMAL cast calls for each value.
template <class T>
void StoreDecimalAs(const Cell &cell, string_t text) {
	string error;
	// DuckDB v2.0.1's DECIMAL parser does not read this flag; IsPlainDecimal is what refuses text
	// it would round or reinterpret.
	CastParameters parameters(true, &error);
	T parsed;
	if (!TryCastToDecimal::Operation<string_t, T>(text, parsed, parameters, static_cast<uint8_t>(cell.column.precision),
	                                              static_cast<uint8_t>(cell.column.scale))) {
		throw ErrorValueRefused(cell, error);
	}
	cell.Store(parsed);
}

//! Parse a DECIMAL value from text, after checking it with IsPlainDecimal.
void StoreDecimal(const Cell &cell, string_t text) {
	auto scale = cell.column.scale;
	if (!IsPlainDecimal(text, scale)) {
		auto form = scale == 0
		                ? string("an optional '-' and digits")
		                : StringUtil::Format("an optional '-', digits, and at most %d digits after a '.'", scale);
		throw ErrorValueRefused(cell, "\"" + text.GetString() + "\" is not a plain decimal number: " + form);
	}
	// DuckDB stores a DECIMAL in the narrowest integer that holds its precision.
	switch (cell.target.GetType().InternalType()) {
	case PhysicalType::INT16:
		return StoreDecimalAs<int16_t>(cell, text);
	case PhysicalType::INT32:
		return StoreDecimalAs<int32_t>(cell, text);
	case PhysicalType::INT64:
		return StoreDecimalAs<int64_t>(cell, text);
	case PhysicalType::INT128:
		return StoreDecimalAs<hugeint_t>(cell, text);
	default:
		throw InternalException("RedshiftCellWriter: unexpected DECIMAL storage type");
	}
}

//! A CHAR value without its trailing blanks (0x20), which Redshift ignores in comparisons, LENGTH and
//! conversions to another string type, so DuckDB compares, joins and measures it as Redshift does.
//! Other trailing whitespace, and leading and inner blanks, are kept. VARCHAR is never trimmed.
void StoreChar(const Cell &cell, const string &text) {
	auto end = text.size();
	while (end > 0 && text[end - 1] == ' ') {
		end--;
	}
	// AddStringOrBlob, as VARCHAR is written, so both take the same path: neither checks UTF-8 here,
	// which the client validates.
	cell.Store(StringVector::AddStringOrBlob(cell.target, text.data(), end));
}

} // namespace

//===--------------------------------------------------------------------===//
// 1. Column types: RedshiftTypeMapping
//===--------------------------------------------------------------------===//

LogicalType RedshiftTypeMapping::ToLogicalType(const RedshiftColumn &column, idx_t column_index) {
	switch (column.type) {
	case RedshiftTypeId::BOOLEAN:
		return LogicalType::BOOLEAN;
	case RedshiftTypeId::SMALLINT:
		return LogicalType::SMALLINT;
	case RedshiftTypeId::INTEGER:
		return LogicalType::INTEGER;
	case RedshiftTypeId::BIGINT:
		return LogicalType::BIGINT;
	case RedshiftTypeId::FLOAT:
		return LogicalType::FLOAT;
	case RedshiftTypeId::DOUBLE:
		return LogicalType::DOUBLE;
	case RedshiftTypeId::DECIMAL:
		return MapDecimal(column, column_index);
	case RedshiftTypeId::VARCHAR:
	case RedshiftTypeId::CHAR:
		return LogicalType::VARCHAR;
	case RedshiftTypeId::DATE:
		return LogicalType::DATE;
	case RedshiftTypeId::TIME:
		return LogicalType::TIME;
	case RedshiftTypeId::TIMETZ:
		return LogicalType::TIME_TZ;
	case RedshiftTypeId::TIMESTAMP:
		return LogicalType::TIMESTAMP;
	case RedshiftTypeId::TIMESTAMPTZ:
		return LogicalType::TIMESTAMP_TZ;
	case RedshiftTypeId::BLOB:
		return LogicalType::BLOB;
	case RedshiftTypeId::UNKNOWN:
		break;
	}
	// UNKNOWN, and any value outside the enum, is refused rather than read as text.
	throw ErrorUnsupportedColumn(column, column_index, "has a type this extension does not support");
}

vector<LogicalType> RedshiftTypeMapping::ToLogicalTypes(const vector<RedshiftColumn> &columns) {
	vector<LogicalType> types;
	types.reserve(columns.size());
	for (idx_t i = 0; i < columns.size(); i++) {
		types.push_back(ToLogicalType(columns[i], i));
	}
	return types;
}

RedshiftOutputSchema RedshiftTypeMapping::ToOutputSchema(const RedshiftStatementShape &shape,
                                                         const RedshiftCaller &caller) {
	if (shape.returns_rows && shape.columns.empty()) {
		throw ErrorInvalidShape(caller, "returning rows with no columns");
	}
	if (!shape.returns_rows && !shape.columns.empty()) {
		throw ErrorInvalidShape(caller, StringUtil::Format("returning no rows with %llu columns",
		                                                   static_cast<uint64_t>(shape.columns.size())));
	}
	RedshiftOutputSchema schema;
	if (!shape.returns_rows) {
		schema.types.push_back(LogicalType::BIGINT);
		schema.names.emplace_back("rowcount");
		return schema;
	}
	schema.types = ToLogicalTypes(shape.columns);
	for (idx_t c = 0; c < shape.columns.size(); c++) {
		// DuckDB names an empty column C<i> only after bind returns, too late for the renaming below.
		auto &name = shape.columns[c].name;
		schema.names.emplace_back(name.empty() ? "C" + std::to_string(c) : name);
	}
	// DuckDB refuses a scan whose columns share a name, which a Redshift join's SELECT * returns. Rename
	// each repeat as DuckDB's own scans do, to <name>_<n>; the shape keeps the reported names. A later
	// name a generated one already took is renamed in turn: a, a, a_1 publish a, a_1, a_1_1.
	QueryResult::DeduplicateColumns(schema.names);
	return schema;
}

//===--------------------------------------------------------------------===//
// 2. Cell values: RedshiftCellWriter
//===--------------------------------------------------------------------===//

RedshiftCellWriter::RedshiftCellWriter(vector<RedshiftColumn> redshift_columns_p) {
	redshift_columns = std::move(redshift_columns_p);
	// Derived here rather than passed in, so the types can never disagree with the columns.
	duckdb_column_types = RedshiftTypeMapping::ToLogicalTypes(redshift_columns);
}

RedshiftValueKind RedshiftCellWriter::RequiredKind(RedshiftTypeId type) {
	switch (type) {
	case RedshiftTypeId::BOOLEAN:
		return RedshiftValueKind::BOOLEAN;
	case RedshiftTypeId::SMALLINT:
	case RedshiftTypeId::INTEGER:
	case RedshiftTypeId::BIGINT:
		return RedshiftValueKind::INTEGER;
	case RedshiftTypeId::FLOAT:
	case RedshiftTypeId::DOUBLE:
		return RedshiftValueKind::REAL;
	case RedshiftTypeId::BLOB:
		return RedshiftValueKind::BLOB;
	case RedshiftTypeId::VARCHAR:
	case RedshiftTypeId::CHAR:
	case RedshiftTypeId::DECIMAL:
	case RedshiftTypeId::DATE:
	case RedshiftTypeId::TIME:
	case RedshiftTypeId::TIMETZ:
	case RedshiftTypeId::TIMESTAMP:
	case RedshiftTypeId::TIMESTAMPTZ:
		return RedshiftValueKind::TEXT;
	case RedshiftTypeId::UNKNOWN:
		break;
	}
	// The constructor's RedshiftTypeMapping has already refused UNKNOWN.
	throw InternalException("RedshiftCellWriter: unmapped column type");
}

void RedshiftCellWriter::WriteRow(const vector<RedshiftValue> &row, DataChunk &output, idx_t output_row) {
	D_ASSERT(output_row < STANDARD_VECTOR_SIZE);
	// Cells are written through typed pointers into output.data[i], so the chunk must have one vector
	// per described column, of that column's type. That holds because redshift_query leaves
	// projection pushdown off, so DuckDB passes every bound column.
	D_ASSERT(output.ColumnCount() == redshift_columns.size());
#ifdef D_ASSERT_IS_ENABLED
	for (idx_t i = 0; i < output.ColumnCount(); i++) {
		D_ASSERT(output.data[i].GetType() == duckdb_column_types[i]);
	}
#endif
	if (row.size() != redshift_columns.size()) {
		throw ErrorContractViolation(StringUtil::Format("a row has %d values, but %d columns were described",
		                                                row.size(), redshift_columns.size()));
	}

	for (idx_t col = 0; col < row.size(); col++) {
		WriteCell(col, row[col], output, output_row);
	}
}

// A DataChunk is column-major: output.data holds one Vector per column, and each Vector holds
// that column's values for every row of the chunk. So cell `column_index` of a transport row
// is written to output.data[column_index] at `output_row`, the row's position in this chunk
// (not in its batch). A NULL clears that row's validity bit; any other value is converted to
// the column's DuckDB type and stored in the vector's data array.
void RedshiftCellWriter::WriteCell(idx_t column_index, const RedshiftValue &value, DataChunk &output,
                                   idx_t output_row) {
	const RedshiftColumn &redshift_column = redshift_columns[column_index];
	auto &target_column = output.data[column_index];
	if (value.kind == RedshiftValueKind::NULL_VALUE) {
		FlatVector::SetNull(target_column, output_row, true);
		return;
	}

	if (value.kind != RequiredKind(redshift_column.type)) {
		throw ErrorContractViolation(
		    StringUtil::Format("%s is declared %s but a cell arrived as %s", ColumnLabel(redshift_column, column_index),
		                       duckdb_column_types[column_index].ToString(), KindName(value.kind)));
	}

	const Cell cell {redshift_column, column_index, target_column, output_row};
	string_t text(value.text);
	switch (redshift_column.type) {
	case RedshiftTypeId::BOOLEAN:
		return cell.Store(value.boolean);
	case RedshiftTypeId::SMALLINT:
		return StoreNarrowInteger<int16_t>(cell, value.integer, "SMALLINT");
	case RedshiftTypeId::INTEGER:
		return StoreNarrowInteger<int32_t>(cell, value.integer, "INTEGER");
	case RedshiftTypeId::BIGINT:
		return cell.Store(value.integer);
	case RedshiftTypeId::FLOAT:
		return StoreFloat(cell, value.real);
	case RedshiftTypeId::DOUBLE:
		return cell.Store(value.real);
	case RedshiftTypeId::VARCHAR:
	case RedshiftTypeId::BLOB:
		return cell.Store(StringVector::AddStringOrBlob(target_column, value.text));
	case RedshiftTypeId::CHAR:
		return StoreChar(cell, value.text);
	case RedshiftTypeId::DECIMAL:
		return StoreDecimal(cell, text);
	case RedshiftTypeId::DATE:
		return StoreDate(cell, text);
	case RedshiftTypeId::TIME:
		return StoreTime(cell, text);
	case RedshiftTypeId::TIMETZ:
		return StoreTimeTZ(cell, text);
	case RedshiftTypeId::TIMESTAMP:
		return StoreTimestamp(cell, text);
	case RedshiftTypeId::TIMESTAMPTZ:
		return StoreTimestampTZ(cell, text);
	case RedshiftTypeId::UNKNOWN:
		// RequiredKind has already refused it.
		throw InternalException("RedshiftCellWriter: unmapped column type");
	}
}

} // namespace duckdb
