#include "odbc/diagnostic_redaction.hpp"

#include "duckdb/common/helper.hpp"
#include "duckdb/common/numeric_utils.hpp"
#include "duckdb/common/string_util.hpp"
#include "duckdb/common/to_string.hpp"
#include "duckdb/common/unordered_set.hpp"
#include "re2/re2.h"
#include "utf8proc.hpp"
#include "utf8proc_wrapper.hpp"

#include <algorithm>
#include <functional>

namespace duckdb {

namespace {

using duckdb_re2::RE2;
using duckdb_re2::StringPiece;

//! A bound value shorter than this stays readable.
constexpr idx_t MIN_PARAMETER_BYTES = 8;

//! How much of a server message SanitizeServerMessage keeps, before the `...` that marks a cut.
constexpr idx_t MAX_SERVER_MESSAGE_BYTES = 256;

//! One stretch [start, end) of a text and what replaces it. Where stretches overlap, the one with
//! the highest rank supplies the replacement for all of them.
struct Span {
	idx_t start;
	idx_t end;
	uint8_t rank;
	string replacement;
};

//! Redaction's ranks. A parameter marker wins, because it names the value it removes; an
//! assignment keeps its keyword; a bare token can only say that something went; and the last word
//! of a cut message, which is dropped, says nothing.
constexpr uint8_t CUT_WORD_RANK = 0;
constexpr uint8_t TOKEN_RANK = 1;
constexpr uint8_t ASSIGNMENT_RANK = 2;
constexpr uint8_t PARAMETER_RANK = 3;

//! The sanitizer's ranks. Where spans overlap, the highest rank's marker replaces the whole group:
//! a URL's marker wins over a host or address inside it, and an address over a PID before it.
constexpr uint8_t MARKER_RANK = 0;
constexpr uint8_t PID_RANK = 1;
constexpr uint8_t HOST_RANK = 2;
constexpr uint8_t ADDRESS_RANK = 3;
constexpr uint8_t URL_RANK = 4;

//! What AddMatches does with a match: replace it; leave it and search on from inside it (Verdict's
//! skip), so that a match inside it can still count; or leave it and every match inside it.
enum class Action : uint8_t { REPLACE, LEAVE, LEAVE_WHOLE };

//! An Action, and for LEAVE how many bytes into the match the search goes on from: one, or more
//! where the filter knows it refuses every match that starts sooner.
struct Verdict {
	Action action;
	idx_t skip = 1;
};

//! A filter's yes or no as a Verdict: a match it refuses may hold one it allows.
Verdict ToVerdict(bool allowed) {
	return {allowed ? Action::REPLACE : Action::LEAVE};
}

Verdict ToVerdict(Verdict verdict) {
	return verdict;
}

//! Whether `character` is one that `\b` reads as part of a word.
bool IsWordCharacter(char character) {
	return StringUtil::CharacterIsAlphaNumeric(character) || character == '_';
}

//! Whether a word runs on across byte `at` of `text`: a word character on each side of it.
bool WordRunsOn(const string &text, idx_t at) {
	return at > 0 && at < text.size() && IsWordCharacter(text.at(at - 1)) && IsWordCharacter(text.at(at));
}

//! Whether `c` is a line end.
bool EndsLine(char c) {
	return c == '\r' || c == '\n';
}

//! The stretches of one text to replace, collected rule by rule and then applied in one pass, so
//! that no rule rewrites what another rule wrote.
class Replacements {
public:
	explicit Replacements(const string &text_p) : text(text_p) {
	}

	//! Adds each match of `pattern` that `verdict(start, end)` replaces, by `replace(capture)`,
	//! where `capture` is the pattern's first group. `verdict` returns a Verdict, or a bool that
	//! ToVerdict reads.
	template <class VERDICT, class REPLACE>
	void AddMatches(const RE2 &pattern, uint8_t rank, const VERDICT &verdict, const REPLACE &replace) {
		const StringPiece input(text);
		// The second submatch is the first group; for a pattern without one, RE2 leaves it empty
		// (re2.h, RE2::Match).
		StringPiece match[2];
		// A pattern here matches one character or more, and the offset moves past the start of every
		// match, so the loop ends.
		for (idx_t offset = 0; pattern.Match(input, offset, text.size(), RE2::UNANCHORED, match, 2);) {
			const auto start = NumericCast<idx_t>(match[0].data() - text.data());
			const auto end = start + match[0].size();
			const auto decided = ToVerdict(verdict(start, end));
			if (decided.action == Action::REPLACE) {
				Add(start, end, rank, replace(match[1].as_string()));
			}
			offset = decided.action == Action::LEAVE ? start + decided.skip : end;
		}
	}

	//! Adds the stretch [start, end), replaced by `replacement`.
	void Add(idx_t start, idx_t end, uint8_t rank, string replacement) {
		spans.push_back({start, end, rank, std::move(replacement)});
	}

	//! Adds each match of `pattern`, replaced by `replace(capture)`.
	template <class REPLACE>
	void AddMatches(const RE2 &pattern, uint8_t rank, const REPLACE &replace) {
		AddMatches(
		    pattern, rank, [](idx_t, idx_t) { return true; }, replace);
	}

	//! Adds each match of `pattern`, replaced by `replacement`.
	void AddMatches(const RE2 &pattern, uint8_t rank, const char *replacement) {
		AddMatches(pattern, rank, [replacement](const string &) { return string(replacement); });
	}

	//! Adds each occurrence of `value`, overlapping ones included, replaced by `replacement`.
	void AddOccurrences(const string &value, uint8_t rank, const string &replacement) {
		for (auto at = text.find(value); at != string::npos; at = text.find(value, at + 1)) {
			spans.push_back({at, at + value.size(), rank, replacement});
		}
	}

	//! Adds the longest end of the text that is the start of `value` and reaches back past the word
	//! the text ends in, replaced by `replacement`. A shorter end lies inside that word, which a cut
	//! message loses anyway.
	void AddEndThatStarts(const string &value, uint8_t rank, const string &replacement) {
		const auto last_word_bytes = text.size() - LastWordStart();
		for (auto length = MinValue(text.size(), value.size()); length > last_word_bytes; length--) {
			if (text.compare(text.size() - length, length, value, 0, length) == 0) {
				spans.push_back({text.size() - length, text.size(), rank, replacement});
				return;
			}
		}
	}

	//! Adds each span of `other`, whose text's byte i is this text's byte `at[i]`. Every span of
	//! `other` holds a byte or more.
	void AddMapped(const Replacements &other, const vector<idx_t> &at) {
		for (const auto &span : other.spans) {
			spans.push_back({at[span.start], at[span.end - 1] + 1, span.rank, span.replacement});
		}
	}

	//! Adds each span of `other` as above, ending each where `end_at` says its last byte's run ends.
	void AddMapped(const Replacements &other, const vector<idx_t> &at, const vector<idx_t> &end_at) {
		for (const auto &span : other.spans) {
			spans.push_back({at[span.start], end_at[span.end - 1], span.rank, span.replacement});
		}
	}

	//! Adds the word the text ends in, after its last white space, replaced by nothing.
	void AddLastWord(uint8_t rank) {
		spans.push_back({LastWordStart(), text.size(), rank, string()});
	}

	//! The text with each merged span replaced, as Emit hands it on, and where its byte `from` lands:
	//! at the start of the replacement of a span that holds it, and at most at the text's end.
	RedactedText Applied(idx_t from) const {
		RedactedText result {string(), 0};
		bool landed = false;
		idx_t cursor = 0;
		for (const auto &span : Merged()) {
			if (!landed && from < span.end) {
				result.from = result.text.size() + MinValue(from, span.start) - cursor;
				landed = true;
			}
			result.text.append(text, cursor, span.start - cursor);
			result.text += ReplacementOf(span);
			cursor = span.end;
		}
		if (!landed) {
			result.from = result.text.size() + MinValue<idx_t>(from, text.size()) - cursor;
		}
		result.text.append(text, cursor, string::npos);
		return result;
	}

	//! Hands each stretch that no span covers to `keep`, and each merged span's replacement to
	//! `replace`, in text order.
	template <class KEEP, class REPLACE>
	void Emit(const KEEP &keep, const REPLACE &replace) const {
		idx_t cursor = 0;
		for (const auto &span : Merged()) {
			keep(text.substr(cursor, span.start - cursor));
			replace(ReplacementOf(span));
			cursor = span.end;
		}
		keep(text.substr(cursor));
	}

private:
	//! What replaces merged `span`. A span that held a line end keeps one after its replacement, so the
	//! first line, the only one a user sees, never takes in text from a later one.
	string ReplacementOf(const Span &span) const {
		const bool held_line_end = std::any_of(text.data() + span.start, text.data() + span.end, EndsLine);
		return held_line_end ? span.replacement + '\n' : span.replacement;
	}

	//! The spans in text order with each overlapping group merged into one. Equal spans keep the
	//! one added first.
	vector<Span> Merged() const {
		auto ordered = spans;
		std::stable_sort(ordered.begin(), ordered.end(), [](const Span &left, const Span &right) {
			return left.start != right.start ? left.start < right.start : left.end > right.end;
		});
		vector<Span> merged;
		for (const auto &span : ordered) {
			if (merged.empty() || span.start >= merged.back().end) {
				merged.push_back(span);
				continue;
			}
			auto &group = merged.back();
			group.end = MaxValue(group.end, span.end);
			if (span.rank > group.rank) {
				group.rank = span.rank;
				group.replacement = span.replacement;
			}
		}
		return merged;
	}

	//! Where the word the text ends in starts: after its last white space.
	idx_t LastWordStart() const {
		const auto last_space = text.find_last_of(" \t\r\n");
		return last_space == string::npos ? 0 : last_space + 1;
	}

	const string &text;
	vector<Span> spans;
};

//! The text with `replacements` applied, and where its byte `from` lands. A message the driver may
//! have cut also loses the word it ends in, and then ends with `...`.
RedactedText Redacted(Replacements replacements, MessageExtent extent, idx_t from) {
	if (extent == MessageExtent::WHOLE) {
		return replacements.Applied(from);
	}
	replacements.AddLastWord(CUT_WORD_RANK);
	auto result = replacements.Applied(from);
	StringUtil::RTrim(result.text);
	result.from = MinValue<idx_t>(result.from, result.text.size());
	result.text += "...";
	return result;
}

//! Whether `c` can end a plain value: a `;` or a line end (ValueEnds says where one does).
bool EndsValue(char c) {
	return c == ';' || EndsLine(c);
}

//! Either marker redaction writes, `<redacted>` or a parameter marker.
constexpr const char *MARKER_FORM = "<(?:redacted|param [0-9]+)>";

//! An IPv4 address, which the sanitizer's address rule and its reading of a name with a port share.
constexpr const char *IPV4_FORM = R"((?:[0-9]{1,3}\.){3}[0-9]{1,3})";

//! The longest IPV4_FORM match, as in `255.255.255.255`.
constexpr idx_t IPV4_MAX_BYTES = 15;

//! A word a process ID follows, with any `_pid` or `_id` after it, which the pid rule and the host
//! rule's pid-word check share.
constexpr const char *PID_KEYWORD_FORM = R"((?:pid|process|backend)(?:_(?:pid|id))?)";

//! A URL: a scheme and `://`, then any character but one of `blanks` (a character class's body), a
//! quote or markup, or a marker redaction wrote for a value inside it. Redaction and the sanitizer's
//! URL rule read this one form.
string UrlForm(const char *blanks) {
	return string(R"((?i)[a-z][a-z0-9+.-]*://(?:[^)") + blanks + R"("'<>]|)" + MARKER_FORM + ")*";
}

//! Where an assignment's keyword, separator and blanks are in a text.
struct AssignmentHead {
	//! Where its replacement starts: at the keyword, or at the opening quote before it.
	idx_t start;
	idx_t separator_end;
	idx_t value_start;
	char separator;
	//! Whether the value runs to the line end, as after a closing quote that is doubled or escaped by
	//! one backslash or more.
	bool value_to_line_end;
	//! The quote just before an unquoted keyword, as a server message quotes the text it echoes
	//! (`"PWD=x"`), or `'\0'`. A value that ends in it, past its closer if it has one, leaves it outside
	//! the replacement.
	char enclosing_quote;
	string keyword;
};

//! Where the value of each assignment, or each SQL credential literal, in a text ends, every end
//! found in linear time in all, however many assignments overlap.
class ValueEnds {
public:
	explicit ValueEnds(const string &text_p)
	    : text(text_p), urls(Urls(text_p)), value_ends(NextValueEnds(text_p, urls)),
	      brace_rest_ends(NextBraceRestEnds(text_p, urls)), line_ends(LineEnds(text_p)), braces(Closers(text_p, '}')),
	      single_quotes(Closers(text_p, '\'')), double_quotes(Closers(text_p, '"')) {
	}

	//! The end of `head`'s assignment, or `string::npos` where it has no value. After a key whose
	//! quote is escaped or doubled, any value runs to the line end.
	idx_t End(const AssignmentHead &head) const {
		const auto separator_end = head.separator_end;
		const auto value_start = head.value_start;
		if (value_start < text.size()) {
			const auto c = text[value_start];
			// A colon right before a colon is a cast in echoed SQL (`pwd::text`), and a keyword's own colon
			// right before `//` ends a URL's scheme (`pwd://h`), which the sanitizer's URL rule reads whole.
			if (head.separator == ':' && value_start == separator_end &&
			    (c == ':' || (ColonFollowsKeyword(separator_end) && text.compare(value_start, 2, "//") == 0))) {
				return string::npos;
			}
			if (head.value_to_line_end && !EndsValue(c)) {
				return line_ends[value_start];
			}
			// A URL can hold a closing brace, so the rest of a braced value may lie inside a URL that
			// started within the braces.
			if (c == '{') {
				const auto closer_end = braces[value_start + 1];
				return EndBeforeEnclosingQuote(head, closer_end, brace_rest_ends[closer_end]);
			}
			if (c == '\'') {
				const auto closer_end = single_quotes[value_start + 1];
				return EndBeforeEnclosingQuote(head, closer_end, value_ends[closer_end]);
			}
			if (c == '"') {
				const auto closer_end = double_quotes[value_start + 1];
				return EndBeforeEnclosingQuote(head, closer_end, value_ends[closer_end]);
			}
			if (!EndsValue(c)) {
				return EndBeforeEnclosingQuote(head, value_start, value_ends[value_start]);
			}
		}
		// Where the blanks run into no value, a plain value starts at them, as in `PWD= ;`. With no
		// blanks, there is no assignment.
		if (value_start == separator_end) {
			return string::npos;
		}
		return head.value_to_line_end ? line_ends[value_start] : value_ends[value_start];
	}

	//! Just past the single quote that closes a SQL literal opening at byte `quote`, or the text's end.
	idx_t LiteralEnd(idx_t quote) const {
		return single_quotes[quote + 1];
	}

private:
	//! `end`, less the value's last byte when it is the quote just before its keyword, so the echo's own
	//! quote stays outside the replacement. Only a byte at or after `rest_start` (past a closer, or a plain
	//! value's first byte, which is no quote) is trimmed, so what a closer keeps inside stays redacted.
	idx_t EndBeforeEnclosingQuote(const AssignmentHead &head, idx_t rest_start, idx_t end) const {
		const auto trims = end > rest_start && head.enclosing_quote != '\0' && text[end - 1] == head.enclosing_quote;
		return trims ? end - 1 : end;
	}

	//! Where each URL (UrlForm) in `text` starts and ends, in text order. A URL runs on here as far as
	//! on the sanitizer's joined line, which leaves out a tab or form feed (SpacedText).
	static vector<std::pair<idx_t, idx_t>> Urls(const string &text) {
		static const RE2 URL(UrlForm(R"( \r\n)"), RE2::Latin1);
		vector<std::pair<idx_t, idx_t>> urls;
		const StringPiece input(text);
		StringPiece match;
		// A URL holds `://`, so every match holds a byte or more and the offset moves on.
		for (idx_t offset = 0; URL.Match(input, offset, text.size(), RE2::UNANCHORED, &match, 1);) {
			const auto start = NumericCast<idx_t>(match.data() - text.data());
			offset = start + match.size();
			urls.emplace_back(start, offset);
		}
		return urls;
	}

	//! Whether the separator that ends at `separator_end` comes straight after a keyword's last letter.
	bool ColonFollowsKeyword(idx_t separator_end) const {
		return StringUtil::CharacterIsAlpha(text[separator_end - 2]);
	}

	//! At each byte, the end of a plain value, or of a quoted value's rest, that runs through it:
	//! the next `;` or line end at or after it, or the text's end, passing over a `;` inside each
	//! URL that starts at or after the byte. A value that ended at a `;` inside a URL it holds would
	//! leave the URL's tail without the scheme the sanitizer's URL rule needs; one that starts inside
	//! a URL leaves that scheme before it. A URL holds no line end.
	static vector<idx_t> NextValueEnds(const string &text, const vector<std::pair<idx_t, idx_t>> &urls) {
		vector<idx_t> ends(text.size() + 1, text.size());
		auto url = urls.rbegin();
		for (idx_t i = text.size(); i-- > 0;) {
			if (url != urls.rend() && url->first == i) {
				ends[i] = ends[url->second];
				++url;
			} else {
				ends[i] = EndsValue(text[i]) ? i : ends[i + 1];
			}
		}
		return ends;
	}

	//! At each byte, the end of a braced value's rest that runs through it: the next `;` or line
	//! end at or after it that no URL holds, or the text's end.
	static vector<idx_t> NextBraceRestEnds(const string &text, const vector<std::pair<idx_t, idx_t>> &urls) {
		vector<idx_t> ends(text.size() + 1, text.size());
		auto url = urls.rbegin();
		for (idx_t i = text.size(); i-- > 0;) {
			while (url != urls.rend() && url->first > i) {
				++url;
			}
			const auto in_url = url != urls.rend() && i < url->second;
			ends[i] = EndsValue(text[i]) && !in_url ? i : ends[i + 1];
		}
		return ends;
	}

	//! At each byte, the next line end at or after it, or the text's end.
	static vector<idx_t> LineEnds(const string &text) {
		vector<idx_t> ends(text.size() + 1, text.size());
		for (idx_t i = text.size(); i-- > 0;) {
			ends[i] = EndsLine(text[i]) ? i : ends[i + 1];
		}
		return ends;
	}

	//! At each byte, just past the `closer` that closes a body starting there, or the text's end
	//! where nothing closes it. A doubled closer escapes it, and so does a backslash before a quote,
	//! read in that order.
	static vector<idx_t> Closers(const string &text, char closer) {
		const auto backslash_escapes = closer != '}';
		vector<idx_t> ends(text.size() + 1, text.size());
		for (idx_t i = text.size(); i-- > 0;) {
			if (text[i] != closer) {
				ends[i] = ends[i + 1];
			} else if (i + 1 < text.size() && text[i + 1] == closer) {
				ends[i] = ends[i + 2];
			} else if (backslash_escapes && i > 0 && text[i - 1] == '\\') {
				ends[i] = ends[i + 1];
			} else {
				ends[i] = i + 1;
			}
		}
		return ends;
	}

	const string &text;
	vector<std::pair<idx_t, idx_t>> urls;
	vector<idx_t> value_ends;
	vector<idx_t> brace_rest_ends;
	vector<idx_t> line_ends;
	vector<idx_t> braces;
	vector<idx_t> single_quotes;
	vector<idx_t> double_quotes;
};

//! Whether the `quote` that closes a keyword at `keyword_start` also opens it, so the replacement takes
//! that opening quote too and keeps one form. Not where a word comes before the opening quote, which
//! the keyword would then run on from.
bool OpensKeyword(const string &text, idx_t keyword_start, const StringPiece &quote) {
	return !quote.empty() && keyword_start >= quote.size() &&
	       StringPiece(text.data() + keyword_start - quote.size(), quote.size()) == quote &&
	       !(keyword_start > quote.size() && IsWordCharacter(text[keyword_start - quote.size() - 1]));
}

//! The assignment heads in `text` whose keyword `runs_on` leaves standing apart and whose value
//! `holds_value` allows, as CredentialForms reads them.
template <class RUNS_ON, class HOLDS_VALUE>
vector<AssignmentHead> AssignmentHeads(const string &text, const RUNS_ON &runs_on, const HOLDS_VALUE &holds_value) {
	// A keyword, a closing quote or none, and `=` or `:` amid the driver's blanks other than a line end,
	// (rsutil.c WHITE_SPACES), case-insensitive as
	// rsutil.c createCaseInsensitiveMap reads them; Latin-1, so U+017F, U+212A never fold to `s`, `k`.
	// They are the driver's ten sensitive keys (rsodbc.h RS_SENSITIVE_CONN_KEYS_LIST) and the secret
	// keys of a credentials file and a COPY or UNLOAD `CREDENTIALS` string. A keyword inside a longer
	// one, as `token` inside `web_identity_token`, runs on from a word, so it never starts a second one.
	static const RE2 HEAD(
	    R"re((?i)(aws_secret_access_key|aws_session_token|AccessKeyID|SecretAccessKey|SessionToken|session_token|Password|)re"
	    R"re(PWD|web_identity_token|token|client_secret|ProxyPwd|https_proxy_password|master_symmetric_key))re"
	    R"re((\\+["']|""|''|["'])?[ \t\f\v]*([=:])[ \t\f\v]*)re",
	    RE2::Latin1);
	vector<AssignmentHead> heads;
	const StringPiece input(text);
	StringPiece match[4];
	for (idx_t offset = 0; HEAD.Match(input, offset, text.size(), RE2::UNANCHORED, match, 4);) {
		const auto keyword_start = NumericCast<idx_t>(match[0].data() - text.data());
		const auto separator_end = NumericCast<idx_t>(match[3].data() - text.data()) + 1;
		if (runs_on(keyword_start) || !holds_value(separator_end)) {
			offset = keyword_start + 1;
			continue;
		}
		const auto value_start = keyword_start + match[0].size();
		offset = value_start;
		const auto &quote = match[2];
		const auto has_opening = OpensKeyword(text, keyword_start, quote);
		const auto head_start = has_opening ? keyword_start - quote.size() : keyword_start;
		const auto before = keyword_start > 0 ? text[keyword_start - 1] : '\0';
		const auto enclosing_quote = quote.empty() && (before == '"' || before == '\'') ? before : '\0';
		heads.push_back({head_start, separator_end, value_start, match[3][0], quote.size() >= 2, enclosing_quote,
		                 match[1].as_string()});
	}
	return heads;
}

//! Where a SQL credential literal's replacement starts, at its keyword or the quote before a keyword in
//! quotes of its own, and the quote that opens the literal.
struct LiteralHead {
	idx_t start;
	idx_t quote;
	string keyword;
};

//! The SQL credential literal heads in `text` whose keyword `runs_on` leaves standing apart.
template <class RUNS_ON>
vector<LiteralHead> LiteralHeads(const string &text, const RUNS_ON &runs_on) {
	// COPY and UNLOAD take a secret key, session token or root key, and CREATE USER and ALTER USER a
	// password, as a literal after SQL white space or none, with no separator; `PASSWORD DISABLE` has
	// none. Backslashes may escape its quote, as SQL inside a string literal writes it. A quote straight
	// after the keyword may close it, as in `'PASSWORD' 'x'`, and a single one may also open the literal:
	// both readings are kept. A double quote opens no SQL literal, so it gets the first reading only.
	static const RE2 HEAD(
	    R"((?i)(SECRET_ACCESS_KEY|SESSION_TOKEN|MASTER_SYMMETRIC_KEY|PASSWORD)(["']?)[ \t\r\n\f\v]*\\*')", RE2::Latin1);
	vector<LiteralHead> heads;
	const StringPiece input(text);
	StringPiece match[3];
	for (idx_t offset = 0; HEAD.Match(input, offset, text.size(), RE2::UNANCHORED, match, 3);) {
		const auto start = NumericCast<idx_t>(match[0].data() - text.data());
		const auto keyword_end = start + match[1].size();
		const auto quote = start + match[0].size() - 1;
		offset = quote + 1;
		if (runs_on(start)) {
			continue;
		}
		const auto head_start = OpensKeyword(text, start, match[2]) ? start - match[2].size() : start;
		heads.push_back({head_start, quote, match[1].as_string()});
		if (match[2] == "'") {
			heads.push_back({head_start, keyword_end, match[1].as_string()});
		}
	}
	return heads;
}

//! The access-key ID, assignment and SQL literal forms in `text`, which both phases replace. A form
//! stands apart from the words beside it, as `\b` reads them, unless `runs_on(at)` says a word runs
//! on across byte `at`; an assignment's value, after its separator ends at byte `at`, must also pass
//! `holds_value(at)`. Each assignment becomes `keyword=<redacted>`, whatever its separator and
//! blanks were, and each literal `keyword '<redacted>'`, so a redacted credential has one form to
//! read and to search for.
template <class RUNS_ON, class HOLDS_VALUE>
Replacements CredentialForms(const string &text, const RUNS_ON &runs_on, const HOLDS_VALUE &holds_value) {
	// A const RE2 is safe to use from several threads at once (re2.h, class RE2).
	static const RE2 ACCESS_KEY(R"((?:AKIA|ASIA)[A-Z0-9]{16})", RE2::Latin1);

	Replacements replacements(text);
	const auto heads = AssignmentHeads(text, runs_on, holds_value);
	const auto literals = LiteralHeads(text, runs_on);
	// Each assignment is added whole, also where it starts inside an earlier one's value: a plain
	// value ends at a `;` it cannot know is inside a later braced or quoted value.
	if (!heads.empty() || !literals.empty()) {
		const ValueEnds ends(text);
		for (const auto &head : heads) {
			const auto end = ends.End(head);
			if (end != string::npos) {
				replacements.Add(head.start, end, ASSIGNMENT_RANK, head.keyword + "=<redacted>");
			}
		}
		for (const auto &literal : literals) {
			replacements.Add(literal.start, ends.LiteralEnd(literal.quote), ASSIGNMENT_RANK,
			                 literal.keyword + " '<redacted>'");
		}
	}
	replacements.AddMatches(
	    ACCESS_KEY, TOKEN_RANK, [&runs_on](idx_t start, idx_t end) { return !runs_on(start) && !runs_on(end); },
	    [](const string &) { return string("<redacted>"); });
	return replacements;
}

//! The access-key ID, assignment and SQL literal forms in a diagnostic, which both phases replace.
Replacements CredentialForms(const string &message) {
	return CredentialForms(
	    message, [&message](idx_t at) { return WordRunsOn(message, at); }, [](idx_t) { return true; });
}

//! The indexes of the bound values long enough to redact, in bind order, the first of equal values
//! only: a later equal value's spans would lose to the first's when merged, at a cost that grows
//! with each copy.
vector<idx_t> RedactedValueIndexes(const vector<string> &bound_values) {
	using Value = std::reference_wrapper<const string>;
	unordered_set<Value, std::hash<string>, std::equal_to<string>> seen;
	vector<idx_t> indexes;
	for (idx_t i = 0; i < bound_values.size(); i++) {
		if (bound_values[i].size() >= MIN_PARAMETER_BYTES && seen.insert(bound_values[i]).second) {
			indexes.push_back(i);
		}
	}
	return indexes;
}

//! The marker that replaces the bound value at `index`, counted from 0.
string ParameterMarker(idx_t index) {
	return "<param " + to_string(index + 1) + ">";
}

//! The inert punctuation a user sees in place of a markup character from server text.
char InertMarkup(char character) {
	switch (character) {
	case '<':
		return '(';
	case '>':
		return ')';
	case '&':
		return '+';
	default:
		return character;
	}
}

//! Whether a user sees a space in place of `codepoint` from server text: a control character (Cc),
//! a line or paragraph separator, which breaks the line a terminal or log shows, a space other
//! than U+0020 (Zs), which no rule reads as one, or one a font may draw as nothing or as a box: a
//! private-use character (Co), U+FFFC or U+FFFD.
bool IsSpacedCharacter(int32_t codepoint) {
	switch (utf8proc_category(codepoint)) {
	case UTF8PROC_CATEGORY_CC:
	case UTF8PROC_CATEGORY_ZL:
	case UTF8PROC_CATEGORY_ZP:
	case UTF8PROC_CATEGORY_CO:
		return true;
	case UTF8PROC_CATEGORY_ZS:
		return codepoint != ' ';
	default:
		return codepoint == 0xFFFC || codepoint == 0xFFFD;
	}
}

//! Whether `codepoint` is one Unicode reserves as default-ignorable while it is unassigned
//! (PropList.txt, Other_Default_Ignorable_Code_Point), which utf8proc marks only once assigned.
bool IsReservedDefaultIgnorable(int32_t codepoint) {
	const auto in = [codepoint](int32_t first, int32_t last) {
		return codepoint >= first && codepoint <= last;
	};
	return codepoint == 0x2065 || in(0xFFF0, 0xFFF8) || codepoint == 0xE0000 || in(0xE0002, 0xE001F) ||
	       in(0xE0080, 0xE00FF) || in(0xE01F0, 0xE0FFF);
}

//! Whether `codepoint` is a noncharacter, which Unicode never assigns.
bool IsNoncharacter(int32_t codepoint) {
	return (codepoint >= 0xFDD0 && codepoint <= 0xFDEF) || (codepoint & 0xFFFE) == 0xFFFE;
}

//! Whether `codepoint` from server text is left out, as one a terminal may show as nothing: a
//! format character (Cf), the bidirectional controls among them, a default-ignorable one, reserved
//! ones included, such as a Hangul filler, a noncharacter, and the empty-looking U+2800 and U+1D159.
bool IsDroppedCharacter(int32_t codepoint) {
	const auto &property = *utf8proc_get_property(codepoint);
	return property.ignorable || property.category == UTF8PROC_CATEGORY_CF || IsReservedDefaultIgnorable(codepoint) ||
	       IsNoncharacter(codepoint) || codepoint == 0x2800 || codepoint == 0x1D159;
}

//! Whether `codepoint` from server text is a combining mark (Mn, Mc or Me), which a user sees on the
//! character before it rather than as nothing.
bool IsCombiningMark(int32_t codepoint) {
	switch (utf8proc_category(codepoint)) {
	case UTF8PROC_CATEGORY_MN:
	case UTF8PROC_CATEGORY_MC:
	case UTF8PROC_CATEGORY_ME:
		return true;
	default:
		return false;
	}
}

//! How many bytes the UTF-8 sequence that `lead` opens would take: 1 for a byte that opens none.
idx_t SequenceBytes(char lead) {
	if ((lead & 0xE0) == 0xC0) {
		return 2;
	}
	if ((lead & 0xF0) == 0xE0) {
		return 3;
	}
	return (lead & 0xF8) == 0xF0 ? 4 : 1;
}

//! Text read from one line of server text, whose byte i was byte `line_at[i]` of the line.
struct LineText {
	string text;
	vector<idx_t> line_at;

	//! Whether bytes `at - 1` and `at` stood side by side in the line, with no character left out
	//! or spaced between them, so that a rule takes its word edges from the line itself.
	bool Touches(idx_t at) const {
		return at > 0 && at < text.size() && line_at[at] == line_at[at - 1] + 1;
	}
};

//! One line of server text as a user sees it (`shown`), and the same without the spaces SpacedText
//! put in place of a character or its combining marks (`joined`), whose byte i is byte `shown_at[i]`
//! of `shown`. Byte i of `joined`, with the marks after it before the next joined byte, ends just
//! before byte `joined_run_end[i]` of the line.
struct SpacedLine {
	LineText shown;
	LineText joined;
	vector<idx_t> shown_at;
	vector<idx_t> joined_run_end;
};

//! Whether a word runs on across byte `at` of `line` in the line itself too.
bool WordRunsOn(const LineText &line, idx_t at) {
	return WordRunsOn(line.text, at) && line.Touches(at);
}

//! Whether the byte before `at` is a letter or digit that stood beside byte `at` in the line.
bool IsAlphaNumericBefore(const LineText &line, idx_t at) {
	return line.Touches(at) && StringUtil::CharacterIsAlphaNumeric(line.text[at - 1]);
}

//! Whether byte `at` is a letter or digit that stood beside the byte before it in the line.
bool IsAlphaNumericAt(const LineText &line, idx_t at) {
	return line.Touches(at) && StringUtil::CharacterIsAlphaNumeric(line.text[at]);
}

//! `line` with each IsDroppedCharacter left out, and each IsSpacedCharacter, and each byte that is
//! no part of a valid UTF-8 character, as one space. Only the bad byte goes, so a broken lead byte
//! cannot take the start of an address.
SpacedLine SpacedText(const string &line) {
	SpacedLine result;
	for (idx_t i = 0; i < line.size();) {
		const auto bytes = SequenceBytes(line[i]);
		const auto *sequence = line.data() + i;
		if (i + bytes > line.size() || Utf8Proc::Analyze(sequence, bytes) == UnicodeType::INVALID) {
			result.shown.text += ' ';
			result.shown.line_at.push_back(i);
			i++;
			continue;
		}
		int length = 0;
		const auto codepoint = Utf8Proc::UTF8ToCodepoint(sequence, length, bytes);
		if (IsSpacedCharacter(codepoint)) {
			result.shown.text += ' ';
			result.shown.line_at.push_back(i);
		} else if (!IsDroppedCharacter(codepoint)) {
			// A mark, or a code point utf8proc does not know, which a newer Unicode version may have made
			// one, stays where the user sees it, but out of the joined line, so that it cannot split a key
			// or an address.
			const auto is_mark_or_unknown =
			    IsCombiningMark(codepoint) || utf8proc_category(codepoint) == UTF8PROC_CATEGORY_CN;
			for (idx_t b = 0; b < bytes; b++) {
				if (!is_mark_or_unknown) {
					result.shown_at.push_back(result.shown.text.size());
					result.joined.line_at.push_back(i + b);
					result.joined.text += sequence[b];
					result.joined_run_end.push_back(i + bytes);
				}
				result.shown.line_at.push_back(i + b);
				result.shown.text += sequence[b];
			}
			if (is_mark_or_unknown && !result.joined_run_end.empty()) {
				result.joined_run_end.back() = i + bytes;
			}
		}
		i += bytes;
	}
	return result;
}

//! `text` with each markup character inert.
string InertText(string text) {
	for (auto &character : text) {
		character = InertMarkup(character);
	}
	return text;
}

//! Text built up to MAX_SERVER_MESSAGE_BYTES, ended with `...` where the next piece did not fit.
class BoundedText {
public:
	//! Appends `piece` whole, or ends the text here when it does not fit.
	void AppendWhole(const string &piece) {
		if (text.size() + piece.size() > MAX_SERVER_MESSAGE_BYTES) {
			cut = true;
		}
		if (!cut) {
			text += piece;
		}
	}

	//! Appends as much of `piece` as fits, never ending inside a UTF-8 sequence.
	void AppendPart(const string &piece) {
		if (cut) {
			return;
		}
		auto room = MAX_SERVER_MESSAGE_BYTES - text.size();
		if (piece.size() <= room) {
			text += piece;
			return;
		}
		while (room > 0 && (static_cast<unsigned char>(piece[room]) & 0xC0) == 0x80) {
			room--;
		}
		text.append(piece, 0, room);
		cut = true;
	}

	string Finish() const {
		return cut ? text + "..." : text;
	}

private:
	string text;
	bool cut = false;
};

//! Whether the colon just before `start` ends a word, as in `ip:` or `IPv6:`, rather than
//! continuing a run of hex groups: the word holds a character that is not a hex digit. `_` runs on
//! inside the word. The word is read as the text shows it, so a character left out inside it
//! cannot make a word of hex digits that keeps the run.
bool FollowsWordColon(const string &text, idx_t start) {
	for (auto at = start - 1; at > 0 && IsWordCharacter(text.at(at - 1)); at--) {
		if (!StringUtil::CharacterIsHex(text.at(at - 1))) {
			return true;
		}
	}
	return false;
}

//! Whether a letter or digit at `end` runs on from the hex run before it. `_` joins nothing.
bool IsJoinedOnRight(const LineText &line, idx_t end) {
	return IsAlphaNumericAt(line, end);
}

//! Whether the hex run at [start, end) runs on into the text beside it: a letter or digit touches
//! it, or, for a run shorter than a full address, it follows a colon after a word of hex digits, as
//! the groups after `deadbeef:` do. `_` joins nothing, so `addr_fe80::1` still holds an address.
bool IsJoinedRun(const LineText &line, idx_t start, idx_t end, bool is_full_length) {
	if (IsAlphaNumericBefore(line, start) || IsJoinedOnRight(line, end)) {
		return true;
	}
	return !is_full_length && line.Touches(start) && line.text[start - 1] == ':' &&
	       IsAlphaNumericBefore(line, start - 1) && !FollowsWordColon(line.text, start);
}

//! Whether the candidate `run` starts as an IPv6 address does: not with a lone colon.
bool StartsAsAddressRun(const string &run) {
	return !(run[0] == ':' && run[1] != ':');
}

//! Whether the hex run at [start, end) is an IPv6 address rather than a time (`12:34:56`) or a
//! cast (`1::dec`): it starts with a group or `::`, nothing joins it (IsJoinedRun), and it has three
//! colons or more, or `::` with a digit after it or after a group that mixes digits and letters, as
//! `fd00::` does.
bool IsAddressRun(const LineText &line, idx_t start, idx_t end) {
	const auto run = line.text.substr(start, end - start);
	const auto colons = NumericCast<idx_t>(std::count(run.begin(), run.end(), ':'));
	if (!StartsAsAddressRun(run) || IsJoinedRun(line, start, end, colons >= 7)) {
		return false;
	}
	if (colons >= 3) {
		return true;
	}
	const auto compressed = run.find("::");
	if (compressed == string::npos) {
		return false;
	}
	// With two colons, the group before `::` is all the run holds before it.
	const auto group = run.substr(0, compressed);
	const auto is_mixed =
	    group.find_first_of("0123456789") != string::npos && group.find_first_not_of("0123456789") != string::npos;
	return is_mixed || run.find_first_of("0123456789", compressed) != string::npos;
}

//! Whether the assignment value at `value_start` in `line` is one, white space alone included, rather
//! than the marker redaction wrote for one. That marker ends its value, so only `;`, the line's end
//! or a cut's closing `...` follows it; any other text means the marker covers part of a value.
bool HoldsValue(const string &line, idx_t value_start) {
	static const string MARKER = "<redacted>";
	static const string CUT = "...";
	const auto value = line.find_first_not_of(" \t", value_start);
	if (value == string::npos || line.compare(value, MARKER.size(), MARKER) != 0) {
		return true;
	}
	const auto after = value + MARKER.size();
	return after < line.size() && line[after] != ';' && line.compare(after, string::npos, CUT) != 0;
}

//! Whether the EC2 short name at `start` starts a word: no letter or digit comes just before it, as
//! one does in `ship-10-0-0-1`. A `_` or `-` may, as in `node_ip-10-0-0-1`.
bool IsEc2NameAlone(const LineText &line, idx_t start) {
	return !IsAlphaNumericBefore(line, start);
}

//! Whether the IPv4 address at [start, end) stands alone: no digit, and no `.` beside a digit, joins
//! it into a longer dotted number such as a five-part version.
bool IsIpv4Alone(const LineText &line, idx_t start, idx_t end) {
	const auto &text = line.text;
	const auto is_digit_before = [&line](idx_t at) {
		return line.Touches(at) && StringUtil::CharacterIsDigit(line.text[at - 1]);
	};
	const auto is_digit_at = [&line](idx_t at) {
		return line.Touches(at) && StringUtil::CharacterIsDigit(line.text[at]);
	};
	const auto joined_left =
	    is_digit_before(start) || (line.Touches(start) && text[start - 1] == '.' && is_digit_before(start - 1));
	const auto joined_right = is_digit_at(end) || (line.Touches(end) && text[end] == '.' && is_digit_at(end + 1));
	return !joined_left && !joined_right;
}

//! A host name: a name of one label or more with any root `.` and a `:port`, a dotted name that ends
//! in an infrastructure suffix, or a dotted name that ends in `internal` after two labels or more.
//! The port comes first, so that it goes with a name that also ends in a suffix. `edge` (`\b`, or
//! empty where the line decides) opens the form and closes the suffix and `internal` alternatives,
//! so that both of the sanitizer's readings share one list.
string HostForm(const string &edge) {
	static const string LABEL = R"([a-z0-9_](?:[a-z0-9_-]{0,61}[a-z0-9_])?)";
	static const string SUFFIX =
	    R"((?:amazonaws\.com(?:\.cn)?|amazon\.com|on\.aws|api\.aws|compute\.[a-z]{2}(?:-[a-z]+)+-[0-9]+))";
	return "(?i)" + edge + "(?:(?:" + LABEL + R"(\.)*)" + LABEL + R"(\.?:[0-9]{1,5}|(?:)" + LABEL + R"(\.)+)" + SUFFIX +
	       edge + "|(?:" + LABEL + R"(\.){2,}internal)" + edge + ")";
}

//! The Verdict on the host match at [start, end), which it replaces if it names a host,
//! so that a time, `01T10:30`, `1.5:30`, an ARN's
//! `us-west-2:123456789012` and `deadbeef:1:2:3` stay.
Verdict HostVerdict(const LineText &line, idx_t start, idx_t end) {
	static const RE2 IPV4_NAME(IPV4_FORM);
	static const RE2 PID_WORD(R"((?i)(?:[a-z0-9_]*_)?)" + string(PID_KEYWORD_FORM));
	const auto &text = line.text;
	const auto colon_in_match = text.substr(start, end - start).find(':');
	if (colon_in_match == string::npos) {
		return ToVerdict(true);
	}
	// A root `.` before the port is no part of the name.
	const auto colon = start + colon_in_match;
	const auto name_end = text[colon - 1] == '.' ? colon - 1 : colon;
	const auto name = text.substr(start, name_end - start);
	if (name.find('.') == string::npos) {
		const auto label_start = name.find_first_not_of('_');
		const auto joins_left =
		    line.Touches(start) &&
		    (text[start - 1] == '-' || (text[start - 1] == ':' && IsAlphaNumericBefore(line, start - 1)));
		const auto joins_right =
		    WordRunsOn(line, end) || (line.Touches(end) && text[end] == ':' && IsAlphaNumericAt(line, end + 1));
		return ToVerdict(label_start != string::npos && StringUtil::CharacterIsAlpha(name[label_start]) &&
		                 !joins_left && !joins_right && !RE2::FullMatch(name, PID_WORD));
	}
	const auto last_underscore = name.rfind('_');
	const auto address_start = last_underscore == string::npos ? 0 : last_underscore + 1;
	if (std::any_of(name.begin(), name.end(), StringUtil::CharacterIsAlpha) ||
	    (RE2::FullMatch(name.substr(address_start), IPV4_NAME) && IsIpv4Alone(line, start + address_start, name_end))) {
		return ToVerdict(true);
	}
	// A HostForm match that starts inside this name ends at the same port and holds no letter either.
	// One that holds the last `_` reads the same address, so only an IPv4 address that ends the name
	// can count; skipping the starts before one keeps a long dotted number linear.
	return {Action::LEAVE, name.size() > IPV4_MAX_BYTES ? name.size() - IPV4_MAX_BYTES : 1};
}

//! Whether the run at [start, end), in brackets with a `:port` after them, is an IPv6 address: it
//! starts as one (StartsAsAddressRun) and holds `::` or three colons or more. The colons rule out a
//! time, and the brackets and the port a cast, so a run of letters such as `face::beef` counts too.
bool IsBracketedAddressRun(const string &text, idx_t start, idx_t end) {
	const auto run = text.substr(start, end - start);
	const auto colons = NumericCast<idx_t>(std::count(run.begin(), run.end(), ':'));
	return StartsAsAddressRun(run) && (colons >= 3 || run.find("::") != string::npos);
}

//! The start of each `process` in `text` that is a verb before a count, as in `could not process 3
//! rows` or `failed to process 12 files`, rather than a noun before a process ID, as in `process 1234
//! terminated`. Only a space parts the words, as the joined line leaves out a tab or other space.
vector<idx_t> ProcessCountStarts(const string &text) {
	static const RE2 PROCESS_COUNT(R"((?i)\b(?:could +not|failed +to) +(process) +[0-9]+ +[a-z])");
	const StringPiece input(text);
	StringPiece match[2];
	vector<idx_t> starts;
	// The next phrase starts after this one's `process`, so a count's word may open one.
	for (idx_t offset = 0; PROCESS_COUNT.Match(input, offset, text.size(), RE2::UNANCHORED, match, 2);) {
		starts.push_back(NumericCast<idx_t>(match[1].data() - text.data()));
		offset = starts.back() + match[1].size();
	}
	return starts;
}

//! The infrastructure details of one line of server text, and the markers redaction wrote in it.
//! A neighbour joins a form only where it stood beside it in the line (LineText::Touches).
Replacements ServerDetailForms(const LineText &line) {
	static const RE2 MARKER("(" + string(MARKER_FORM) + ")");
	// No leading `\b`, so a URL glued to a word, as in `foo_https://`, still goes. A URL runs on
	// through a marker redaction wrote for a value inside it, so the rest of it goes too.
	static const RE2 URL(UrlForm(R"(\s)"));
	// A port after an IPv4 address goes with HOST's name-with-port form, which the address outranks.
	static const RE2 IPV4(IPV4_FORM);
	// Eight groups or more, as one full address, two run together or one with its `:port`, with any
	// longer number after a colon; or a shorter run that ends in a port of five digits or more, `::`
	// or a group, with a `:port` after any `%zone`. Either keeps its `%zone`, as `%en-0` or `%eth0.1`.
	static const RE2 IPV6(R"((?i)(?:[0-9a-f]{1,4}:){7,}[0-9a-f]{1,4}\b(?:%[0-9a-z]+(?:[._-][0-9a-z]+)*)?)"
	                      R"((?::[0-9]+)?|(?:[0-9a-f]{0,4}:)+(?:[0-9]{5,}|:|[0-9a-f]{1,4}))"
	                      R"((?:%[0-9a-z]+(?:[._-][0-9a-z]+)*(?::[0-9]+)?)?)");
	// An IPv6 address in brackets, as a URL writes one, goes with its brackets and `:port`.
	static const RE2 BRACKETED_IPV6(R"((?i)\[[0-9a-f:.]+(?:%[0-9a-z]+(?:[._-][0-9a-z]+)*)?\]:[0-9]+)");
	// A name is a host only when it ends in an infrastructure suffix or a port follows it, so that a
	// qualified name such as "public.users", which the user needs to fix the query, stays.
	// `internal` needs two labels before it, so a table `sales.internal` stays too.
	static const RE2 HOST(HostForm(R"(\b)"));
	// HOST without `\b`, which cannot see a word the line kept apart, so that the line decides both
	// edges.
	static const RE2 HOST_IN_LINE(HostForm(""));
	// An EC2 short name carries the private address it is named after. No `\b`, so one glued to `_`
	// still goes.
	static const RE2 EC2_NAME(R"((?i)ip-[0-9]{1,3}(?:-[0-9]{1,3}){3}(?::[0-9]{1,5})?)");
	// The word may end a longer one joined by `_`, as `backend_pid` and `pg_terminate_backend(` do,
	// and is kept whole, as `process_id` is. The line, not `\b`, decides its edges, and only a letter
	// or digit before it joins it, so that the longer word stays before the marker.
	static const RE2 PID("(?i)(" + string(PID_KEYWORD_FORM) + R"()(?:\s+(?:pid|id))?(?:\(\))?\s*[:=#(-]?\s*[0-9]+)");

	const auto count_starts = ProcessCountStarts(line.text);
	Replacements replacements(line.text);
	replacements.AddMatches(MARKER, MARKER_RANK, [](const string &marker) { return marker; });
	replacements.AddMatches(URL, URL_RANK, "<url>");
	replacements.AddMatches(
	    IPV4, ADDRESS_RANK, [&line](idx_t start, idx_t end) { return IsIpv4Alone(line, start, end); },
	    [](const string &) { return string("<address>"); });
	// Every match inside a run that a word joins on the right runs on to that word too, so the search
	// goes on after the run, rather than reading the run again from each byte.
	replacements.AddMatches(
	    IPV6, ADDRESS_RANK,
	    [&line](idx_t start, idx_t end) {
		    if (IsJoinedOnRight(line, end)) {
			    return Verdict {Action::LEAVE_WHOLE};
		    }
		    return ToVerdict(IsAddressRun(line, start, end));
	    },
	    [](const string &) { return string("<address>"); });
	replacements.AddMatches(
	    BRACKETED_IPV6, ADDRESS_RANK,
	    [&line](idx_t start, idx_t) {
		    return IsBracketedAddressRun(line.text, start + 1, line.text.find_first_of("%]", start));
	    },
	    [](const string &) { return string("<address>"); });
	replacements.AddMatches(
	    HOST, HOST_RANK, [&line](idx_t start, idx_t end) { return HostVerdict(line, start, end); },
	    [](const string &) { return string("<host>"); });
	// Every match inside one a word joins on the right ends where it does, so the search goes on
	// after it; one a word joins on the left goes on one byte in, and HostVerdict says for the rest.
	replacements.AddMatches(
	    HOST_IN_LINE, HOST_RANK,
	    [&line](idx_t start, idx_t end) {
		    if (WordRunsOn(line, end)) {
			    return Verdict {Action::LEAVE_WHOLE};
		    }
		    return WordRunsOn(line, start) ? ToVerdict(false) : HostVerdict(line, start, end);
	    },
	    [](const string &) { return string("<host>"); });
	replacements.AddMatches(
	    EC2_NAME, HOST_RANK, [&line](idx_t start, idx_t) { return IsEc2NameAlone(line, start); },
	    [](const string &) { return string("<host>"); });
	replacements.AddMatches(
	    PID, PID_RANK,
	    [&line, &count_starts](idx_t start, idx_t end) {
		    return !IsAlphaNumericBefore(line, start) && !WordRunsOn(line, end) &&
		           !std::binary_search(count_starts.begin(), count_starts.end(), start);
	    },
	    [](const string &keyword) { return keyword + " <pid>"; });
	return replacements;
}

//! `line` with each credential form its joined reading holds redacted first, as redaction would have
//! had it seen the line joined: a character left out can rejoin a key that redaction saw split. Its
//! markers then read as redaction's own, so a form inside the value goes with it.
string WithJoinedCredentialsRedacted(const string &line) {
	const auto spaced = SpacedText(line);
	const auto &joined = spaced.joined;
	const auto forms = CredentialForms(
	    joined.text, [&joined](idx_t at) { return WordRunsOn(joined, at); },
	    [&joined](idx_t at) { return HoldsValue(joined.text, at); });
	// A mark at a form's end goes with it, as it would in the line whole.
	Replacements redacted(line);
	redacted.AddMapped(forms, joined.line_at, spaced.joined_run_end);
	return redacted.Applied(0).text;
}

} // namespace

RedactedText RedactConnectDiagnostic(const string &message, MessageExtent extent, idx_t from) {
	static const RE2 TOKEN_RUN(R"([A-Za-z0-9+/=_-]{40,})", RE2::Latin1);
	auto replacements = CredentialForms(message);
	replacements.AddMatches(TOKEN_RUN, TOKEN_RANK, "<redacted>");
	return Redacted(std::move(replacements), extent, from);
}

RedactedText RedactStatementDiagnostic(const string &message, const vector<string> &bound_values, MessageExtent extent,
                                       idx_t from) {
	auto replacements = CredentialForms(message);
	const auto indexes = RedactedValueIndexes(bound_values);
	// Whole occurrences go in first, so that an end that is one value whole and the start of another
	// takes the whole value's marker.
	for (const auto i : indexes) {
		replacements.AddOccurrences(bound_values[i], PARAMETER_RANK, ParameterMarker(i));
	}
	// Only a message the driver may have cut can end inside a value it echoed.
	if (extent == MessageExtent::CUT) {
		for (const auto i : indexes) {
			replacements.AddEndThatStarts(bound_values[i], PARAMETER_RANK, ParameterMarker(i));
		}
	}
	return Redacted(std::move(replacements), extent, from);
}

string SanitizeServerMessage(const string &redacted_message) {
	// The rules read the line spaced, and joined again, so that a control character inside a form
	// can neither hide it, as in `pid\x011234`, nor split it, as in `10.0.\x010.1`. A neighbour joins
	// a form only where it stood beside it in the line.
	const auto line =
	    SpacedText(WithJoinedCredentialsRedacted(redacted_message.substr(0, redacted_message.find_first_of("\r\n"))));
	auto forms = ServerDetailForms(line.shown);
	forms.AddMapped(ServerDetailForms(line.joined), line.shown_at);
	BoundedText result;
	forms.Emit([&result](const string &kept) { result.AppendPart(InertText(kept)); },
	           [&result](const string &replacement) { result.AppendWhole(replacement); });
	return result.Finish();
}

} // namespace duckdb
