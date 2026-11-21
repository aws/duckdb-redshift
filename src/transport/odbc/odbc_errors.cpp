#include "odbc/odbc_errors.hpp"

#include "duckdb/common/exception.hpp"
#include "duckdb/common/exception/binder_exception.hpp"
#include "duckdb/common/string_util.hpp"
#include "duckdb/common/to_string.hpp"
#include "duckdb/common/unordered_map.hpp"
#include "duckdb/common/unordered_set.hpp"
#include "duckdb/common/vector.hpp"
#include "odbc/diagnostic_redaction.hpp"
#include "odbc/odbc_message_framing.hpp"

#include <cstring>

namespace duckdb {

namespace {

//! ODBC's general-error SQLSTATE, which the driver also writes when it has nothing more precise.
constexpr const char *GENERIC_SQLSTATE = "HY000";

//! What the client says a connect failure was, and what the user should do. Neither repeats
//! anything the driver wrote.
struct CategoryText {
	const char *cause;
	const char *hint;
};

//! How a connect record shows its category: by its SQLSTATE, or by a lower-case phrase in its
//! message, since the IAM failures measured on the driver all arrive as HY000 and differ in text.
//! A one-word phrase is an AWS error code, matched only as a whole word, and the first code in a
//! record decides among codes (FirstErrorCodeCategory).
struct CategoryRule {
	ConnectCategory category;
	vector<string> sqlstates;
	vector<string> phrases;
	CategoryText text;
};

//! The AWS code for credentials the service does not recognise. On driver 2.3.0.0 an opt-in region
//! answered with it too (other times InternalFailure), so its record cannot tell a bad key from a
//! region the account has not enabled or the session token does not cover.
constexpr const char *UNRECOGNIZED_CLIENT_CODE = "unrecognizedclientexception";

//! The credentials failures whose record shows the service refused credentials it was given, rather than
//! the chain finding none: a wrong secret key's signature errors, an invalid or expired session token, and
//! an unrecognised key. Each is matched as CategoryRules matches it, a code as a whole word.
const vector<string> &RefusedCredentialPhrases() {
	static const vector<string> PHRASES = {"invalidsignatureexception",
	                                       "signaturedoesnotmatch",
	                                       "request signature we calculated does not match",
	                                       "security token included in the request is invalid",
	                                       "expiredtokenexception",
	                                       "security token included in the request is expired",
	                                       UNRECOGNIZED_CLIENT_CODE};
	return PHRASES;
}

//! What a credentials failure says when the service refused the credentials the chain gave.
constexpr const char *CREDENTIALS_REFUSED_CAUSE =
    "the AWS credentials found through the AWS credential chain were refused or have expired";

//! What libpq writes before the resolver's answer for a host name it could not resolve.
constexpr const char *RESOLVER_PHRASE = "to address:";

//! What a credentials hint adds when the default chain found no credentials while AWS_PROFILE is set: driver
//! 2.3.0.0's chain does not read a profile that assumes a role, whose source then never runs (measured with a
//! source_profile over a credential_process and over static keys), while the driver's own profile reader, which
//! PROFILE selects, does.
constexpr const char *ROLE_PROFILE_HINT =
    " If AWS_PROFILE names a profile that assumes a role (role_arn with source_profile), name it with "
    "CREDENTIAL_PROVIDER profile, PROFILE '<name>' instead: the default AWS credential chain does not read such a "
    "profile.";

//! What a credentials hint adds for UNRECOGNIZED_CLIENT_CODE.
constexpr const char *REGION_NOT_ENABLED_HINT =
    " If these credentials work in other regions, the region may be an opt-in region the account has "
    "not enabled or their session token does not cover; check it.";

//! Where a connect failure's text names its target, which TargetHints' noun replaces.
constexpr const char *TARGET_NOUN = "{target}";

//! What an OTHER connect failure says.
constexpr CategoryText OTHER_TEXT = {"the connection attempt failed",
                                     "Check the region and that this host can reach the {target}, then retry."};

//! What a refused login says. Its hint is the target's own (TargetHints' `login_refused`), which
//! CategoryHint gives, so it has none here.
constexpr CategoryText LOGIN_REFUSED_TEXT = {"the Redshift {target} refused the login", ""};

//! What a connect timeout says on a target that does not resume from idle: no resume text and no
//! attempt count, as it gets no retry.
constexpr CategoryText NETWORK_TIMEOUT_TEXT = {
    "the Redshift {target} did not answer",
    "The {target} may not be publicly reachable from this host's network; check its network access, then retry."};

//! The certificate failure's hint. Windows trusts its root store, which may lack the roots the
//! endpoints chain to while automatic root update is off or has not run.
#ifdef _WIN32
constexpr const char *CERTIFICATE_HINT =
    "Check the trusted CA certificates, from SSL_CERT_FILE or the Windows root store, and the network path to the "
    "Redshift host. A host whose automatic root update is off, or has not run yet, may lack the roots Redshift's "
    "certificates chain to: turn it on, or let it run, then connect again; or set SSL_CERT_FILE. Verification "
    "cannot be turned off.";
#else
constexpr const char *CERTIFICATE_HINT =
    "Check the trusted CA certificates, from SSL_CERT_FILE or the system store, and the network path to the "
    "Redshift host. Verification cannot be turned off.";
#endif

//! Redshift's state, through libpq, for a database the target does not have (invalid_catalog_name).
constexpr const char *DATABASE_NOT_FOUND_SQLSTATE = "3D000";

//! The rules in the order ClassifyConnect tries them.
const vector<CategoryRule> &CategoryRules() {
	static const vector<CategoryRule> RULES = {
	    // The first three, the two expired-token phrases and a wrong secret key's
	    // InvalidSignatureException (a workgroup's) and SignatureDoesNotMatch (a cluster's) with their
	    // signature text were measured on driver 2.3.0.0; "no credential provider" is unmeasured. The
	    // chained-profile texts are the driver's (IAMProfileCredentialsProvider.cpp), the loop's
	    // measured on 2.3.0.0.
	    {ConnectCategory::CREDENTIALS,
	     {},
	     {"no aws profile found", "missingauthenticationtoken", "missingauthenticationtokenexception",
	      "security token included in the request is invalid", "expiredtokenexception",
	      "security token included in the request is expired", "no credential provider", "invalidsignatureexception",
	      "signaturedoesnotmatch", "request signature we calculated does not match", "chained profiles loop",
	      "maximum chained profiles reached"},
	     {"no usable AWS credentials were found through the AWS credential chain",
	      "Check the credentials this process can reach: the default AWS credential chain, or the profile PROFILE "
	      "names."}},
	    // Measured on driver 2.3.0.0: curl's error 77 and 60 texts, and libpq's chain failure (AWS-LC's
	    // reason text, matched with libpq's lead-in so that a name spelling it alone does not match)
	    // and host-name texts (tls_verification_test.cpp). OpenSSL's reason text, from driver releases
	    // before AWS-LC, and libpq's root-file text are unmeasured. The Windows control plane has no
	    // phrase: its SDK gives every request that fails before a response the same text.
	    {ConnectCategory::CERTIFICATE,
	     {},
	     {"problem with the ssl ca cert", "ssl peer certificate or ssh remote key was not ok",
	      "ssl error: certificate_verify_failed", "certificate verify failed", "does not match host name",
	      "root certificate file"},
	     {"the server's TLS certificate could not be verified", CERTIFICATE_HINT}},
	    // AccessDeniedException was measured on an earlier driver release, not on 2.3.0.0, and 42501
	    // in an earlier measurement; "insufficient privilege"
	    // is unmeasured. The query APIs' AccessDenied is what a cluster's DescribeClusters denial
	    // is expected to give, unmeasured. Its hint is the target's, from ConnectFailureContext,
	    // since each target has its own actions.
	    {ConnectCategory::PERMISSION,
	     {"42501"},
	     {"accessdenied", "accessdeniedexception", "insufficient privilege"},
	     {"the AWS identity is not allowed to connect to the {target}", ""}},
	    // GetCredentials' answer for a workgroup name the region and account do not have, and libpq's
	    // with glibc's text for a host name that does not exist, both measured on 2.3.0.0. glibc gives
	    // that text for an unreachable resolver too where nsswitch's hosts line has myhostname, as
	    // AL2023's, RHEL's and Fedora's do; elsewhere that outage stays OTHER. macOS's text for the same
	    // answer (the system's gai_strerror, which the driver's libpq uses there), and Windows' (the
	    // driver's libpq builds its own getaddrinfo there, whose pg_gai_strerror says "Unknown host"),
	    // read from the driver's source, unmeasured. For a cluster, GetClusterCredentialsWithIAM's
	    // ClusterNotFound, measured on 2.3.0.0. The hint is the target's own (CategoryHint).
	    {ConnectCategory::TARGET_NOT_FOUND,
	     {},
	     {"resourcenotfoundexception", "clusternotfound", "to address: name or service not known",
	      "to address: nodename nor servname provided", "to address: unknown host"},
	     {"the Redshift {target} was not found", ""}},
	    // Redshift's answer, through libpq, for a database the target does not have: the SQL standard's
	    // invalid_catalog_name, seen on driver 2.3.0.0 against a cluster and a workgroup. The target
	    // was found and the credentials worked, so the database alone is wrong. Matched by its state,
	    // never its text, which repeats the name.
	    {ConnectCategory::DATABASE_NOT_FOUND,
	     {DATABASE_NOT_FOUND_SQLSTATE},
	     {},
	     {"the database was not found on the Redshift {target}",
	      // No word on the database used when none is given: dev for this extension's own forms, but the
	      // target's default for an ARN the aws extension resolves, which the client cannot tell apart.
	      "Check DATABASE, or the database in the path, against the databases the {target} has."}},
	    // ODBC's two timeout states, and libpq's timeout text (fe-misc.c, pqWaitTimed), unmeasured.
	    {ConnectCategory::RESUME_TIMEOUT,
	     {"HYT00", "HYT01"},
	     {"timeout expired"},
	     {"the Redshift Serverless workgroup did not answer",
	      "It may still be resuming from idle suspension, which can take a few minutes; retry. If it "
	      "keeps timing out, the workgroup may not be publicly reachable from this host's network."}}};
	return RULES;
}

//! The class of `sqlstate`, which the SQL standard makes its first two characters.
string SqlStateClass(const string &sqlstate) {
	return sqlstate.substr(0, 2);
}

//! The SQLSTATE a failure acts on for `record`: HY000 for an unreadable or carried-over state and
//! for a success or warning state (class 00 or 01), and otherwise the record's own.
string FailureSqlState(const DiagnosticRecord &record) {
	const auto sqlstate_class = SqlStateClass(record.SqlState());
	if (!record.HasReadableSqlState() || sqlstate_class == "00" || sqlstate_class == "01" ||
	    record.StateIsCarriedOver()) {
		return GENERIC_SQLSTATE;
	}
	return record.SqlState();
}

//! Whether `shown`, a server message as ShownText gives it, says nothing once its severity word is
//! gone, as the server's "ERROR:" with no text after it.
bool IsEmptyServerText(const string &shown) {
	bool had_severity;
	return WithoutSeverity(shown, had_severity).empty();
}

//! Whether `shown`, a first line as SanitizeServerMessage gives it, is a server severity word and nothing
//! else, as the "ERROR:" of an internal error whose primary message is empty.
bool IsSeverityAlone(const string &shown) {
	bool had_severity;
	return WithoutSeverity(shown, had_severity).empty() && had_severity;
}

//! The reason on the "error:" line of the box Redshift sends as the DETAIL of a compute node's error,
//! in `redacted_message`, or "" when the message does not have that box's shape. Measured on a Redshift
//! Serverless workgroup for a cast: a DETAIL line with nothing after its label, a rule of dashes, then
//! "  error:  Invalid digit, Value 'a', Pos 0, Type: Integer" and its code, context, query, location and
//! process lines. Only the error line is read, and only when it comes right after the rule that follows
//! the DETAIL line, so no other diagnostic line is taken for it; the lines after it, which describe the
//! server and the data, stay out.
string BoxedErrorLine(const string &redacted_message) {
	static const string DETAIL_LABEL = "DETAIL:";
	static const string ERROR_LABEL = "error:";
	enum class Expect { DETAIL, RULE, ERROR } expect = Expect::DETAIL;
	auto end = redacted_message.find_first_of("\r\n");
	while (end != string::npos) {
		const auto begin = end + 1;
		end = redacted_message.find_first_of("\r\n", begin);
		auto line = redacted_message.substr(begin, end == string::npos ? string::npos : end - begin);
		StringUtil::Trim(line);
		if (line.empty()) {
			continue;
		}
		switch (expect) {
		case Expect::DETAIL:
			if (line != DETAIL_LABEL) {
				return string();
			}
			expect = Expect::RULE;
			break;
		case Expect::RULE:
			if (line.size() < 5 || line.find_first_not_of('-') != string::npos) {
				return string();
			}
			expect = Expect::ERROR;
			break;
		case Expect::ERROR:
			if (!StringUtil::StartsWith(line, ERROR_LABEL)) {
				return string();
			}
			line = line.substr(ERROR_LABEL.size());
			StringUtil::Trim(line);
			return line;
		}
	}
	return string();
}

//! `record`'s text as a user may see it, sanitized, without the driver's framing, which repeats the
//! SQLSTATE and spends the sanitizer's 256 bytes: its first line, or, when that line is a severity word
//! alone, that word and the reason on the error line of Redshift's DETAIL box (BoxedErrorLine).
string ShownText(const DiagnosticRecord &record) {
	const auto message = record.UnframedMessage();
	auto shown = SanitizeServerMessage(message);
	if (!IsSeverityAlone(shown)) {
		return shown;
	}
	const auto reason = BoxedErrorLine(message);
	if (reason.empty()) {
		return shown;
	}
	StringUtil::Trim(shown);
	return SanitizeServerMessage(shown + "  " + reason);
}

//! Whether `c` can be part of a name a user chose, such as a workgroup's or a profile's.
bool IsNameByte(char c) {
	return StringUtil::CharacterIsAlphaNumeric(c) || c == '-' || c == '_';
}

//! Whether `phrase` is one word, which makes it an AWS error code.
bool IsErrorCode(const string &phrase) {
	return phrase.find(' ') == string::npos;
}

//! Whether `message[at]`, beside a word, joins it to `message[beyond]`'s name: a name byte, or a '.'
//! between name bytes, as in a dotted host name. A '.' that ends a sentence joins nothing.
bool JoinsName(const string &message, idx_t at, idx_t beyond) {
	return IsNameByte(message[at]) || (message[at] == '.' && beyond < message.size() && IsNameByte(message[beyond]));
}

//! Where the lower-cased `message` first holds the error code `code` as a whole word, or npos, so
//! neither a workgroup named accessdenied-lab nor the host accessdenied.<account>... holds
//! accessdenied.
idx_t ErrorCodeAt(const string &message, const string &code) {
	for (auto at = message.find(code); at != string::npos; at = message.find(code, at + 1)) {
		const auto end = at + code.size();
		const bool joined_before = at > 0 && JoinsName(message, at - 1, at >= 2 ? at - 2 : message.size());
		const bool joined_after = end < message.size() && JoinsName(message, end, end + 1);
		if (!joined_before && !joined_after) {
			return at;
		}
	}
	return string::npos;
}

//! Whether the lower-cased `message` holds `phrase` as CategoryRules reads one: an error code (one word) as a
//! whole word, any other phrase anywhere.
bool HoldsPhrase(const string &message, const string &phrase) {
	return IsErrorCode(phrase) ? ErrorCodeAt(message, phrase) != string::npos : message.find(phrase) != string::npos;
}

//! The category of the error code that comes first in the lower-cased `message`, or OTHER when it
//! holds none. The driver's IAM layout writes the AWS code before any name the user chose, so a
//! name spelled like a code decides only in a record with no code before it.
ConnectCategory FirstErrorCodeCategory(const string &message) {
	auto category = ConnectCategory::OTHER;
	auto first = string::npos;
	for (const auto &rule : CategoryRules()) {
		for (const auto &phrase : rule.phrases) {
			const auto at = IsErrorCode(phrase) ? ErrorCodeAt(message, phrase) : string::npos;
			if (at < first) {
				first = at;
				category = rule.category;
			}
		}
	}
	return category;
}

//! Whether `record` has one of `rule`'s states, holds one of its phrases of several words, or
//! holds one of its error codes before any other rule's. A record of SQLSTATE 3D000 is decided by
//! that state alone: its text repeats the database name the user chose, which may spell another
//! rule's code (a database named accessdenied), so no other rule reads it.
bool Matches(const CategoryRule &rule, const DiagnosticRecord &record) {
	if (FailureSqlState(record) == DATABASE_NOT_FOUND_SQLSTATE) {
		return rule.category == ConnectCategory::DATABASE_NOT_FOUND;
	}
	for (const auto &sqlstate : rule.sqlstates) {
		if (FailureSqlState(record) == sqlstate) {
			return true;
		}
	}
	const auto message = StringUtil::Lower(record.Message());
	for (const auto &phrase : rule.phrases) {
		// Codes are matched below, in the order FirstErrorCodeCategory gives them.
		if (!IsErrorCode(phrase) && HoldsPhrase(message, phrase)) {
			return true;
		}
	}
	return FirstErrorCodeCategory(message) == rule.category;
}

//! The text of `category`'s rule, or OTHER_TEXT for a category no rule has.
CategoryText TextFor(ConnectCategory category) {
	for (const auto &rule : CategoryRules()) {
		if (rule.category == category) {
			return rule.text;
		}
	}
	return OTHER_TEXT;
}

//! `count` and `noun`, the noun plural unless the count is 1.
string Counted(idx_t count, const string &noun) {
	return to_string(count) + " " + noun + (count == 1 ? "" : "s");
}

//! The text of `category` on a target with `hints`.
CategoryText TextFor(ConnectCategory category, const TargetHints &hints) {
	if (category == ConnectCategory::LOGIN_REFUSED) {
		return LOGIN_REFUSED_TEXT;
	}
	const bool network_timeout = category == ConnectCategory::RESUME_TIMEOUT && !hints.resumes_when_idle;
	return network_timeout ? NETWORK_TIMEOUT_TEXT : TextFor(category);
}

//! Whether a credentials failure's record shows that the service refused credentials the chain found, rather
//! than the chain finding none.
bool CredentialsRefused(const ConnectClassification &classification) {
	if (classification.category != ConnectCategory::CREDENTIALS || !classification.acted_on) {
		return false;
	}
	const auto message = StringUtil::Lower(classification.acted_on->Message());
	for (const auto &phrase : RefusedCredentialPhrases()) {
		if (HoldsPhrase(message, phrase)) {
			return true;
		}
	}
	return false;
}

//! What the client says caused a connect failure, repeating nothing the driver wrote. A resume
//! timeout's cause goes on to say how long the attempts waited, because the cold start of an idle
//! workgroup can outlast them.
//! A target that does not resume gets none of that.
string ConnectCause(const ConnectClassification &classification, const ConnectFailureContext &context) {
	const auto category = classification.category;
	if (category == ConnectCategory::CREDENTIALS && !context.hints.aws_secret.empty()) {
		// The driver was given the AWS secret's credentials and resolved none itself.
		return "the AWS credentials from AWS secret '" + context.hints.aws_secret + "' were refused or have expired";
	}
	if (CredentialsRefused(classification)) {
		// The chain found credentials and the service refused them: saying none were found sends the user
		// looking for a missing profile.
		return CREDENTIALS_REFUSED_CAUSE;
	}
	if (category == ConnectCategory::TARGET_NOT_FOUND && classification.acted_on &&
	    StringUtil::Lower(classification.acted_on->Message()).find(RESOLVER_PHRASE) != string::npos) {
		// The host's resolver answered: the name did not resolve, which is what it says, rather than that
		// the target is missing.
		return "the Redshift {target}'s host name could not be resolved";
	}
	string cause = TextFor(category, context.hints).cause;
	if (!IsResumeTimeout(category, context.hints)) {
		return cause;
	}
	return cause + " within " + Counted(context.elapsed_seconds, "second") + " over " +
	       Counted(context.attempts, "connect attempt");
}

//! The hint of `category`: the target's own for a denied connect, a target not found or a refused
//! login, otherwise the category's, for this target.
string CategoryHint(ConnectCategory category, const ConnectFailureContext &context) {
	if (category == ConnectCategory::PERMISSION) {
		return context.hints.permission;
	}
	if (category == ConnectCategory::TARGET_NOT_FOUND) {
		return context.hints.availability;
	}
	if (category == ConnectCategory::LOGIN_REFUSED) {
		return context.hints.login_refused.empty()
		           ? "Check that this AWS identity may sign in to the {target}'s database, then retry."
		           : context.hints.login_refused;
	}
	if (category == ConnectCategory::CREDENTIALS && !context.hints.aws_secret.empty()) {
		return "This extension does not refresh an AWS secret's credentials: check the secret's keys, or recreate the "
		       "secret to get new temporary credentials (for the aws extension's default secret, reopen the "
		       "database), then retry.";
	}
	return TextFor(category, context.hints).hint;
}

//! What a failure to reach a target on 5439, for want of a port, adds (TargetHints' `default_port`): a
//! workgroup or cluster endpoint can listen on another port, which only the path can give.
constexpr const char *DEFAULT_PORT_HINT =
    " With no port in the path, the connection uses 5439; for an endpoint on another port, give the endpoint "
    "with its port as the path, redshift://<endpoint>:<port>.";

//! Whether the driver tagged the failure an IAM one (`[860:8:IAMConnectionError]`): a credentials call such as
//! GetCredentials, made over HTTPS before any connect to the target's host and port.
bool IsIamFailure(const ConnectClassification &classification) {
	// As a whole word, so a host named after a workgroup such as iamconnectionerror-lab does not match.
	return classification.acted_on && ErrorCodeAt(StringUtil::Lower(classification.acted_on->UnframedMessage()),
	                                              "iamconnectionerror") != string::npos;
}

//! The hint after a connect failure's cause: CategoryHint; for a failure to reach a target on the default
//! port, other than an IAM call's, DEFAULT_PORT_HINT; for a credentials failure whose record
//! `acted_on` holds UNRECOGNIZED_CLIENT_CODE, the opt-in region it may be; and for one where the default chain
//! found no credentials while AWS_PROFILE is set, ROLE_PROFILE_HINT.
string ConnectHint(const ConnectClassification &classification, const ConnectFailureContext &context) {
	auto hint = CategoryHint(classification.category, context);
	if (context.hints.default_port && !IsIamFailure(classification) &&
	    (classification.category == ConnectCategory::OTHER ||
	     classification.category == ConnectCategory::RESUME_TIMEOUT)) {
		hint += DEFAULT_PORT_HINT;
	}
	if (classification.category == ConnectCategory::CREDENTIALS && context.hints.aws_secret.empty() &&
	    context.hints.aws_profile_variable && !CredentialsRefused(classification)) {
		hint += ROLE_PROFILE_HINT;
	}
	const bool unrecognized =
	    classification.category == ConnectCategory::CREDENTIALS && classification.acted_on &&
	    ErrorCodeAt(StringUtil::Lower(classification.acted_on->Message()), UNRECOGNIZED_CLIENT_CODE) != string::npos;
	return unrecognized ? hint + REGION_NOT_ENABLED_HINT : hint;
}

//! Whether a statement record's sanitized server message may be shown: a class on this list, whose
//! first line says what failed in the caller's request. 42 (syntax or access), 22 (data), 23 (integrity),
//! 0A (not supported) and XX (internal, many of them fixable statement problems); 3D (a database that does
//! not exist, an auto-mounted catalog among them), 3F (a schema), 2B (objects that depend on one), 25 (an
//! aborted or read-only transaction), 40 (a serialization failure or deadlock), 55 (an object in use), 57
//! (a cancelled statement) and P0 (a stored procedure's own RAISE). Any other class keeps the client's
//! message, so the driver's own (HY), and 08, 53 and 58, whose lines describe the server's connections,
//! resources and system, where a host name the sanitizer's forms do not take can stand, show no text.
//! SanitizeServerMessage keeps only the first line, after redaction, and replaces the hosts, addresses,
//! process IDs and URLs it knows, so a later line that describes the server, as an XX error's does,
//! stays out; the one exception is the error line of Redshift's DETAIL box in place of a first line
//! that is only a severity word (ShownText).
bool ShowsServerMessage(const string &sqlstate) {
	static const unordered_set<string> SHOWN = {"42", "22", "23", "0A", "XX", "3D", "3F",
	                                            "2B", "25", "40", "55", "57", "P0"};
	return SHOWN.count(SqlStateClass(sqlstate)) > 0;
}

//! The SQLSTATE class of `sqlstate` for a user, with its name where the client knows one.
string ClassDescription(const string &sqlstate) {
	static const unordered_map<string, string> NAMES = {
	    {"08", "connection exception"},   {"25", "invalid transaction state"},
	    {"53", "insufficient resources"}, {"57", "operator intervention"},
	    {"58", "system error"},           {"HY", "driver error"},
	    {"XX", "internal error"}};
	const auto sqlstate_class = SqlStateClass(sqlstate);
	const auto name = NAMES.find(sqlstate_class);
	return "SQLSTATE class " + sqlstate_class + (name == NAMES.end() ? "" : ", " + name->second);
}

//! What the client was doing when a statement failed, in its own words: the lead that opens its
//! message, the activity a failure of the driver's own (class HY) names, and whether the statement
//! may have run by then.
struct PhaseText {
	string lead;
	string activity;
	bool may_have_run;
};

PhaseText TextOf(StatementPhase phase) {
	switch (phase) {
	case StatementPhase::ALLOCATE:
		return {"the Redshift driver could not allocate a statement handle", "allocating a statement handle", false};
	case StatementPhase::PREPARE:
		return {"Redshift refused the statement", "preparing the statement", false};
	case StatementPhase::PARAMETERS:
		return {"the Redshift driver did not report the statement's parameter markers",
		        "reading the statement's parameter markers", false};
	case StatementPhase::DESCRIBE:
		return {"Redshift did not describe the statement's result columns", "describing the statement's result columns",
		        false};
	case StatementPhase::BIND:
		return {"the Redshift driver could not bind the statement's parameters", "binding the statement's parameters",
		        false};
	case StatementPhase::EXECUTE:
		return {"Redshift could not run the statement", "running the statement", true};
	case StatementPhase::FETCH:
		return {"Redshift could not return the rest of the result", "returning the rest of the result", true};
	}
	throw InternalException("unknown Redshift statement phase");
}

//! What a user does after a failure that says nothing they can act on: nothing has run before the
//! statement runs, so a retry is safe; from then on a write may have taken effect.
const char *NextStep(bool may_have_run) {
	return may_have_run ? "The statement may have run; check before running it again." : "Retry the statement.";
}

//! What the client says about a failure that left no record.
string NoRecordText(const DiagnosticSet &records) {
	return records.is_incomplete ? "the driver's diagnostic records could not be read"
	                             : "the driver returned no diagnostic record";
}

//! The `extra_info` of a failure acted on through `acted_on`: `sqlstate` and `sqlstates`, or
//! nothing when there is no record.
unordered_map<string, string> SqlStateInfo(const DiagnosticSet &records,
                                           optional_ptr<const DiagnosticRecord> acted_on) {
	unordered_map<string, string> info;
	if (acted_on) {
		info["sqlstate"] = FailureSqlState(*acted_on);
		info["sqlstates"] = records.SqlStates();
	}
	return info;
}

//! `message` with the `SQLSTATE: <code>` line (docs/troubleshooting.md), when there is a SQLSTATE to report.
string WithSqlStateLine(const string &message, optional_ptr<const DiagnosticRecord> acted_on) {
	return acted_on ? message + "\nSQLSTATE: " + FailureSqlState(*acted_on) : message;
}

//! What a failure's records say about its cause, to follow a clause: the SQLSTATE class of the
//! record acted on, or why there is none (none returned, or the records could not be read).
string RecordCause(const DiagnosticSet &records, optional_ptr<const DiagnosticRecord> acted_on) {
	return acted_on ? " (" + ClassDescription(FailureSqlState(*acted_on)) + ")" : ", and " + NoRecordText(records);
}

//! What a lost connection's records say about why, to follow "was lost": RecordCause, except that
//! Redshift's own end of the session (57P01 to 57P03, an administrator, a shutdown or a restart) in any
//! record says so, since libpq's own failure can come first under another state (such as 08006), and a
//! loss whose record holds one of libpq's dropped-connection texts under the driver's state
//! (class HY, for a closed socket, a TLS connection that ended or a reset) names Redshift or the
//! network, not the driver, which only passed on what libpq saw. Another class HY record, such as a
//! failed allocation, keeps "driver error". Fixed text only: the record's own is libpq's or the server's, and stays
//! out.
string LossCause(const DiagnosticSet &records, optional_ptr<const DiagnosticRecord> acted_on) {
	if (!acted_on) {
		return RecordCause(records, acted_on);
	}
	for (const auto &record : records.records) {
		const auto state = FailureSqlState(record);
		if (state == "57P01" || state == "57P02" || state == "57P03") {
			return " (Redshift ended the session)";
		}
	}
	if (SqlStateClass(FailureSqlState(*acted_on)) == "HY" && acted_on->ShowsConnectionLoss()) {
		return " (Redshift, or the network between this host and Redshift, closed it)";
	}
	return RecordCause(records, acted_on);
}

//! A statement failure's message: the server's sanitized primary message for a statement-phase class
//! it may show, the driver's own failure for class HY, whose text is the driver's or libpq's, and
//! otherwise the class alone.
string StatementMessage(const DiagnosticSet &records, optional_ptr<const DiagnosticRecord> acted_on,
                        const PhaseText &text) {
	if (!acted_on) {
		return text.lead + ", and " + NoRecordText(records) + ". " + NextStep(text.may_have_run);
	}
	const auto sqlstate = FailureSqlState(*acted_on);
	if (records.phase == DiagnosticPhase::STATEMENT && ShowsServerMessage(sqlstate)) {
		const auto shown = ShownText(*acted_on);
		if (IsEmptyServerText(shown)) {
			return text.lead + " (" + ClassDescription(sqlstate) + "), and the server gave no message. " +
			       NextStep(text.may_have_run) +
			       " If it fails the same way, report it with the extension and driver versions.";
		}
		return text.lead + ": " + shown;
	}
	if (SqlStateClass(sqlstate) == "HY") {
		return "the Redshift driver failed while " + text.activity + " (" + ClassDescription(sqlstate) +
		       "), and its message is not shown. " + NextStep(text.may_have_run) +
		       " If it fails the same way, report it with the extension and driver versions.";
	}
	return text.lead + " (" + ClassDescription(sqlstate) +
	       "). The server's message for this class is not shown; act on the SQLSTATE.";
}

//! The record whose SQLSTATE a statement or transport failure reports, and an OTHER connect
//! failure's: the first whose FailureSqlState is not HY000, else the first, else null.
optional_ptr<const DiagnosticRecord> ActedOnRecord(const DiagnosticSet &records) {
	for (const auto &record : records.records) {
		if (FailureSqlState(record) != GENERIC_SQLSTATE) {
			return &record;
		}
	}
	return records.records.empty() ? nullptr : &records.records.front();
}

//! IOException with the records' SQLSTATEs and the driver's identity, `message` followed by the
//! SQLSTATE line of the record acted on.
[[noreturn]] void ThrowDriverIOException(const DiagnosticSet &records, optional_ptr<const DiagnosticRecord> acted_on,
                                         const redshift_driver::DriverIdentity &identity, const string &message) {
	auto info = SqlStateInfo(records, acted_on);
	const auto driver = DriverInfo(identity);
	info.insert(driver.begin(), driver.end());
	throw IOException(info, WithSqlStateLine(message, acted_on));
}

//! A session setup failure's message, from its lead clause and its records, whose cause is LossCause's
//! when the connection was `lost`, and RecordCause's otherwise.
[[noreturn]] void ThrowSetupIOException(const DiagnosticSet &records, const redshift_driver::DriverIdentity &identity,
                                        const string &lead, const string &next, bool lost) {
	const auto acted_on = ActedOnRecord(records);
	const auto cause = lost ? LossCause(records, acted_on) : RecordCause(records, acted_on);
	ThrowDriverIOException(records, acted_on, identity, lead + cause + ". " + next);
}

} // namespace

ConnectClassification ClassifyConnect(const DiagnosticSet &records) {
	for (const auto &rule : CategoryRules()) {
		for (const auto &record : records.records) {
			if (Matches(rule, record)) {
				return {rule.category, &record};
			}
		}
	}
	return {ConnectCategory::OTHER, ActedOnRecord(records)};
}

namespace {

//! The server's text for a refused password, which names the database user ("... for user \"IAM:u\"").
constexpr const char *PASSWORD_REFUSED_PHRASE = "password authentication failed";

//! Whether `record` shows an authentication refusal its state does not: a class 28 state the driver
//! carried over (counted as HY000), or the server's refused-password text under any state.
bool ShowsRefusalBesideState(const DiagnosticRecord &record) {
	return SqlStateClass(record.SqlState()) == "28" ||
	       StringUtil::Lower(record.Message()).find(PASSWORD_REFUSED_PHRASE) != string::npos;
}

//! ClassifyConnect's answer, unless it names no category: then the first record in SQLSTATE class 28,
//! or, with none, the first that ShowsRefusalBesideState, makes it LOGIN_REFUSED, on every target, so an
//! authentication refusal never shows its record's text, which names the database user, as an
//! unclassified failure's reason would. A refusal in neither form, such as a text with no class 28 state,
//! is not recognised. Unmeasured, from the driver's source: a failed connect's record takes libpq's last
//! SQLSTATE, the server's (public source v2.3.0, rslibpq.c:535 and rsutil.c:516-520).
ConnectClassification ClassifyConnectFor(const DiagnosticSet &records) {
	const auto classification = ClassifyConnect(records);
	if (classification.category != ConnectCategory::OTHER) {
		return classification;
	}
	for (const auto &record : records.records) {
		if (SqlStateClass(FailureSqlState(record)) == "28") {
			return {ConnectCategory::LOGIN_REFUSED, &record};
		}
	}
	for (const auto &record : records.records) {
		if (ShowsRefusalBesideState(record)) {
			return {ConnectCategory::LOGIN_REFUSED, &record};
		}
	}
	return classification;
}

} // namespace

namespace {

//! What a connect failure no category names adds after its cause: the reason in the record it acted on,
//! as its own sentence, `. The driver reported "<reason>"`, so "the connection attempt failed." reads as
//! before for whatever matches it, or nothing when that record has no text. Without it such a
//! failure says only that the attempt failed, and a refused login, a throttled or failed credentials
//! request, a TLS failure and a network failure all read alike. The record's text is connect-phase
//! redacted when it is read (access-key IDs, credential assignments and values, and every run of 40 or
//! more token characters, the form a secret key, session token or database password takes), and
//! SanitizeServerMessage then keeps its first line and replaces the host names, addresses, process IDs
//! and URLs in it. The sanitizer leaves out characters that show nothing, which can join two runs that
//! redaction saw apart, so the reason is redacted again after it. It carries no credential the
//! redaction recognises and none of the infrastructure forms the sanitizer replaces; what the
//! sanitizer does not take, such as a one-label host name, stays, as in shown server text. A category
//! with fixed text gets none: its cause and hint already say what to do.
string ConnectReason(const ConnectClassification &classification) {
	if (classification.category != ConnectCategory::OTHER || !classification.acted_on) {
		return string();
	}
	auto reason = RedactConnectDiagnostic(SanitizeServerMessage(classification.acted_on->UnframedMessage()),
	                                      MessageExtent::WHOLE, 0)
	                  .text;
	// The driver tags an IAM failure `[860:8:IAMConnectionError]:` ahead of its text; the tag names no
	// cause, so the reason starts after it.
	if (StringUtil::StartsWith(reason, "[")) {
		const auto close = reason.find(']');
		if (close != string::npos && reason.substr(0, close).find(':') != string::npos) {
			reason = reason.substr(close + 1);
			while (!reason.empty() && (reason.front() == ':' || reason.front() == ' ')) {
				reason.erase(0, 1);
			}
		}
	}
	StringUtil::Trim(reason);
	if (IsEmptyServerText(reason)) {
		return string();
	}
	return ". The driver reported \"" + reason + "\"";
}

} // namespace

bool IsResumeTimeout(ConnectCategory category, const TargetHints &hints) {
	return category == ConnectCategory::RESUME_TIMEOUT && hints.resumes_when_idle;
}

bool RecordsShowConnectionLoss(const DiagnosticSet &records) {
	for (const auto &record : records.records) {
		if (record.ShowsConnectionLoss()) {
			return true;
		}
	}
	return false;
}

bool RecordsShowUnusableSession(const DiagnosticSet &records) {
	for (const auto &record : records.records) {
		if (record.ShowsUnusableSession()) {
			return true;
		}
	}
	return false;
}

void ThrowConnectFailure(const DiagnosticSet &records, const ConnectFailureContext &context) {
	const auto classification = ClassifyConnectFor(records);
	auto cause = ConnectCause(classification, context);
	if (!classification.acted_on && records.is_incomplete) {
		// Said as the statement path says it, so the hint is not mistaken for a diagnosis.
		cause += ", and " + NoRecordText(records);
	}
	// Only the client's own words get the noun, so a configured value holding TARGET_NOUN stays as it is,
	// and so does the driver's reason, added after the replacement.
	const auto explanation = StringUtil::Replace(cause, TARGET_NOUN, context.hints.noun) +
	                         ConnectReason(classification) + ".\n" +
	                         StringUtil::Replace(ConnectHint(classification, context), TARGET_NOUN, context.hints.noun);
	ThrowDriverIOException(records, classification.acted_on, context.identity,
	                       "Could not connect to Redshift at " + context.target + ": " + explanation);
}

unordered_map<string, string> DriverInfo(const redshift_driver::DriverIdentity &identity) {
	return {{"redshift_driver", identity.Describe()}};
}

void ThrowPreConnectFailure(const DiagnosticSet &records, const redshift_driver::DriverIdentity &identity,
                            const string &lead, const string &next) {
	const auto acted_on = ActedOnRecord(records);
	const auto unread = !acted_on && records.is_incomplete ? ", and " + NoRecordText(records) : string();
	ThrowDriverIOException(records, acted_on, identity, lead + unread + "; " + next);
}

namespace {

//! A statement failure's `extra_info`: SqlStateInfo's, and `redshift_error` = `session_unusable` when the records
//! show a session Redshift's answer left unusable.
unordered_map<string, string> StatementFailureInfo(const DiagnosticSet &records,
                                                   optional_ptr<const DiagnosticRecord> acted_on) {
	auto info = SqlStateInfo(records, acted_on);
	if (RecordsShowUnusableSession(records)) {
		info["redshift_error"] = "session_unusable";
	}
	return info;
}

} // namespace

void ThrowStatementFailure(const DiagnosticSet &records, StatementPhase phase) {
	const auto acted_on = ActedOnRecord(records);
	const auto info = StatementFailureInfo(records, acted_on);
	const auto message = WithSqlStateLine(StatementMessage(records, acted_on, TextOf(phase)), acted_on);
	if (phase == StatementPhase::PREPARE || phase == StatementPhase::PARAMETERS || phase == StatementPhase::DESCRIBE) {
		throw BinderException(info, message);
	}
	throw Exception(info, ExceptionType::EXECUTOR, message);
}

void ThrowCellReadFailure(const DiagnosticSet &records, idx_t column) {
	const auto acted_on = ActedOnRecord(records);
	const auto cell = "column " + std::to_string(column) + " of the current row";
	const PhaseText text {"Redshift could not return " + cell, "returning " + cell, true};
	throw Exception(StatementFailureInfo(records, acted_on), ExceptionType::EXECUTOR,
	                WithSqlStateLine(StatementMessage(records, acted_on, text), acted_on));
}

void ThrowCutValue(const DiagnosticSet &records, const string &lead) {
	const auto acted_on = ActedOnRecord(records);
	throw Exception(SqlStateInfo(records, acted_on), ExceptionType::EXECUTOR,
	                WithSqlStateLine(lead + ", so the batch was not returned rather than a value cut short; report "
	                                        "it with the extension and driver versions",
	                                 acted_on));
}

void ThrowMultipleResultSets() {
	throw Exception(ExceptionType::EXECUTOR,
	                "the Redshift statement returned more than one result set, and this "
	                "extension reads only one; run a statement that returns a single result set");
}

void ThrowTransportFailure(const DiagnosticSet &records, const string &target, StatementPhase phase) {
	const auto acted_on = ActedOnRecord(records);
	const auto outcome = TextOf(phase).may_have_run ? ". The statement was not replayed, because a write it carried "
	                                                  "may already have taken effect; check before running it again."
	                                                : ". The statement was not replayed.";
	const auto message =
	    "The connection to Redshift at " + target + " was lost" + LossCause(records, acted_on) + outcome;
	throw IOException(SqlStateInfo(records, acted_on), WithSqlStateLine(message, acted_on));
}

void ThrowSessionSetupFailure(const DiagnosticSet &records, const string &target,
                              const redshift_driver::DriverIdentity &identity, const string &sql) {
	ThrowSetupIOException(records, identity,
	                      "Could not set up the Redshift session at " + target + ": the setup statement \"" + sql +
	                          "\" failed",
	                      "The connection is closed, and no statement of yours was sent. Run the statement again, "
	                      "and if it fails the same way, report it with the extension and driver versions.",
	                      false);
}

void ThrowSessionSetupLoss(const DiagnosticSet &records, const string &target,
                           const redshift_driver::DriverIdentity &identity) {
	ThrowSetupIOException(records, identity,
	                      "Could not set up the Redshift session at " + target +
	                          ": the connection was lost during the setup",
	                      "No statement of yours was sent; run the statement again.", true);
}

void LogInformationalDiagnostics(RedshiftLogSink &sink, const DiagnosticSet &records) {
	for (const auto &record : records.records) {
		auto line = "Redshift driver diagnostic, SQLSTATE " + record.SqlState();
		if (records.phase == DiagnosticPhase::STATEMENT && ShowsServerMessage(FailureSqlState(record))) {
			line += ": " + ShownText(record);
		}
		sink.Debug(line);
	}
}

} // namespace duckdb
