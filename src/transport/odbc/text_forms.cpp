#include "odbc/text_forms.hpp"

#include "duckdb/common/numeric_utils.hpp"

namespace duckdb {

namespace {

//! An unsigned DATE year has 4 to 7 digits and a TIMESTAMP year 4 to 6.
const idx_t MIN_YEAR_DIGITS = 4;
const idx_t DATE_MAX_YEAR_DIGITS = 7;
const idx_t TIMESTAMP_MAX_YEAR_DIGITS = 6;
//! The fewest digits after the sign of an astronomical year, as in `-043-03-15`.
const idx_t SIGNED_YEAR_MIN_DIGITS = 3;
const idx_t MAX_FRACTION_DIGITS = 6;

bool IsAsciiDigit(char c) {
	return c >= '0' && c <= '9';
}

//! The server's spellings of a float that is not finite: a table's, then ::VARCHAR's. A table's look
//! like C printf's, which writes a NaN whose sign bit is set, as an invalid operation gives on x86,
//! as `-nan`, so that spelling is accepted too.
const char *const FLOAT_SPECIAL_FORMS[] = {"nan", "-nan", "inf", "-inf", "NaN", "Infinity", "-Infinity"};
//! A bound on a float's digit runs, well past the longest the server prints: 17 significant digits,
//! after at most 4 zeros in a fraction, before it switches to an exponent.
const idx_t MAX_FLOAT_DIGITS = 32;

//! Reads a value's text from the front. Each Consume method either moves past what it matched and
//! returns true, or moves nowhere and returns false; the free Consume* helpers may stop part-way.
class FormReader {
public:
	explicit FormReader(const string &text) : text(text), position(0), digits_start(0), digits_end(0) {
	}

	bool AtEnd() const {
		return position == text.size();
	}

	bool Consume(char expected) {
		if (AtEnd() || text[position] != expected) {
			return false;
		}
		position++;
		return true;
	}

	bool Consume(const string &expected) {
		if (text.compare(position, expected.size(), expected) != 0) {
			return false;
		}
		position += expected.size();
		return true;
	}

	//! Moves past a run of `min_digits` to `max_digits` digits. A longer run is refused whole
	//! rather than read in part, so a field with one digit too many fails here.
	bool ConsumeDigits(idx_t min_digits, idx_t max_digits) {
		idx_t end = position;
		while (end < text.size() && IsAsciiDigit(text[end])) {
			end++;
		}
		const idx_t count = end - position;
		if (count < min_digits || count > max_digits) {
			return false;
		}
		digits_start = position;
		digits_end = end;
		position = end;
		return true;
	}

	//! Whether the run the last successful ConsumeDigits moved past is all zeros.
	bool LastDigitsAreZero() const {
		for (idx_t i = digits_start; i < digits_end; i++) {
			if (text[i] != '0') {
				return false;
			}
		}
		return true;
	}

	//! Whether the same run has a `0` before another digit, as `007` and `01` do.
	bool LastDigitsHaveLeadingZero() const {
		return digits_end - digits_start > 1 && text[digits_start] == '0';
	}

private:
	const string &text;
	idx_t position;
	idx_t digits_start;
	idx_t digits_end;
};

bool ConsumeTwoDigits(FormReader &reader) {
	return reader.ConsumeDigits(2, 2);
}

//! `-MM-DD`, the part of a date after its year.
bool ConsumeMonthAndDay(FormReader &reader) {
	return reader.Consume('-') && ConsumeTwoDigits(reader) && reader.Consume('-') && ConsumeTwoDigits(reader);
}

//! `YYYY-MM-DD` with an unsigned year of 4 to `max_year_digits` digits.
bool ConsumeDate(FormReader &reader, idx_t max_year_digits) {
	return reader.ConsumeDigits(MIN_YEAR_DIGITS, max_year_digits) && ConsumeMonthAndDay(reader);
}

//! `HH:MM:SS`, then optionally `.` and 1 to 6 fractional digits.
bool ConsumeTime(FormReader &reader) {
	if (!ConsumeTwoDigits(reader) || !reader.Consume(':') || !ConsumeTwoDigits(reader) || !reader.Consume(':') ||
	    !ConsumeTwoDigits(reader)) {
		return false;
	}
	return !reader.Consume('.') || reader.ConsumeDigits(1, MAX_FRACTION_DIGITS);
}

//! `+HH` or `-HH`, then `:MM` when the minutes or the seconds are non-zero, then `:SS` when the
//! seconds are. A zero field that could be left off is refused, so a non-zero offset has one form.
bool ConsumeOffset(FormReader &reader) {
	if (!(reader.Consume('+') || reader.Consume('-')) || !ConsumeTwoDigits(reader)) {
		return false;
	}
	if (!reader.Consume(':')) {
		return true;
	}
	if (!ConsumeTwoDigits(reader)) {
		return false;
	}
	const bool zero_minutes = reader.LastDigitsAreZero();
	if (!reader.Consume(':')) {
		return !zero_minutes;
	}
	return ConsumeTwoDigits(reader) && !reader.LastDigitsAreZero();
}

//! `YYYY-MM-DD HH:MM:SS[.f]`, with a year of 4 to 6 digits.
bool ConsumeTimestamp(FormReader &reader) {
	return ConsumeDate(reader, TIMESTAMP_MAX_YEAR_DIGITS) && reader.Consume(' ') && ConsumeTime(reader);
}

//! Whether all that is left is nothing, or ` BC` and nothing.
bool OnlyBcSuffixRemains(FormReader &reader) {
	reader.Consume(" BC");
	return reader.AtEnd();
}

bool IsDecimalForm(const string &text, int32_t scale) {
	FormReader reader(text);
	const bool negative = reader.Consume('-');
	// A leading zero is refused: `0.50`, never `00.50` or `007`.
	if (!reader.ConsumeDigits(1, text.size()) || reader.LastDigitsHaveLeadingZero()) {
		return false;
	}
	bool zero = reader.LastDigitsAreZero();
	if (scale > 0) {
		// The leader node can print a computed value with fewer places than its declared scale, so 1 to
		// `scale` digits pass through unchanged and the conversion to DuckDB reads them at the scale.
		// More digits would need rounding, so they are refused.
		const idx_t fraction_digits = NumericCast<idx_t>(scale);
		if (!reader.Consume('.') || !reader.ConsumeDigits(1, fraction_digits)) {
			return false;
		}
		zero = zero && reader.LastDigitsAreZero();
	}
	return reader.AtEnd() && !(negative && zero);
}

//! A year after a `-` is astronomical (a year y <= 0 is (1 - y) BC) and takes no ` BC`; an unsigned
//! year may take one. Only digits are counted, so `-000-12-31` and `0000-12-31 BC` pass as forms.
bool IsDateForm(const string &text) {
	FormReader reader(text);
	if (reader.Consume('-')) {
		return reader.ConsumeDigits(SIGNED_YEAR_MIN_DIGITS, DATE_MAX_YEAR_DIGITS) && ConsumeMonthAndDay(reader) &&
		       reader.AtEnd();
	}
	return ConsumeDate(reader, DATE_MAX_YEAR_DIGITS) && OnlyBcSuffixRemains(reader);
}

bool IsTimeForm(const string &text) {
	FormReader reader(text);
	return ConsumeTime(reader) && reader.AtEnd();
}

bool IsTimetzForm(const string &text) {
	FormReader reader(text);
	return ConsumeTime(reader) && ConsumeOffset(reader) && reader.AtEnd();
}

bool IsTimestampForm(const string &text) {
	FormReader reader(text);
	return ConsumeTimestamp(reader) && OnlyBcSuffixRemains(reader);
}

//! The offset comes before any ` BC`.
bool IsTimestamptzForm(const string &text) {
	FormReader reader(text);
	return ConsumeTimestamp(reader) && ConsumeOffset(reader) && OnlyBcSuffixRemains(reader);
}

//! A float as the server prints one: an optional `-`, an integer part with no leading zero, an
//! optional fraction, and an optional exponent `e` with its sign; or a special spelling. No padding,
//! `+`, digit separator or other case is the server's.
bool IsFloatForm(const string &text) {
	for (const auto special : FLOAT_SPECIAL_FORMS) {
		if (text == special) {
			return true;
		}
	}
	FormReader reader(text);
	reader.Consume('-');
	if (!reader.ConsumeDigits(1, MAX_FLOAT_DIGITS) || reader.LastDigitsHaveLeadingZero()) {
		return false;
	}
	if (reader.Consume('.') && !reader.ConsumeDigits(1, MAX_FLOAT_DIGITS)) {
		return false;
	}
	if (reader.Consume('e') &&
	    (!(reader.Consume('+') || reader.Consume('-')) || !reader.ConsumeDigits(1, MAX_FLOAT_DIGITS))) {
		return false;
	}
	return reader.AtEnd();
}

} // namespace

bool MatchesTextForm(const RedshiftColumn &column, const string &text) {
	switch (column.type) {
	case RedshiftTypeId::DECIMAL:
		return IsDecimalForm(text, column.scale);
	case RedshiftTypeId::DATE:
		return IsDateForm(text);
	case RedshiftTypeId::TIME:
		return IsTimeForm(text);
	case RedshiftTypeId::TIMETZ:
		return IsTimetzForm(text);
	case RedshiftTypeId::TIMESTAMP:
		return IsTimestampForm(text);
	case RedshiftTypeId::TIMESTAMPTZ:
		return IsTimestamptzForm(text);
	case RedshiftTypeId::SMALLINT:
	case RedshiftTypeId::INTEGER:
	case RedshiftTypeId::BIGINT:
		return IsDecimalForm(text, 0);
	case RedshiftTypeId::FLOAT:
	case RedshiftTypeId::DOUBLE:
		return IsFloatForm(text);
	case RedshiftTypeId::VARCHAR:
	case RedshiftTypeId::CHAR:
		return true;
	default:
		return false;
	}
}

} // namespace duckdb
