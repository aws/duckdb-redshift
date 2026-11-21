//===----------------------------------------------------------------------===//
// The ATTACH path forms that name a target: the `redshift://` endpoint URI, the long-form
// Serverless workgroup ARN and a cluster identifier (redshift_attach_path.hpp). All are read
// structurally, with no lookup: every value lands in a field that the client checks again before it
// builds a host. A cluster endpoint's label and port are kept, so the client connects to exactly that
// endpoint.
//===----------------------------------------------------------------------===//

#include "redshift_attach_path.hpp"

#include "redshift_attach.hpp"

#include "duckdb/common/exception.hpp"
#include "duckdb/common/string_util.hpp"

namespace duckdb {

namespace {

constexpr const char *URI_SCHEME = "redshift://";
constexpr const char *ARN_PREFIX = "arn:";
constexpr idx_t ACCOUNT_DIGITS = 12;
constexpr idx_t WORKGROUP_MIN = 3;
constexpr idx_t WORKGROUP_MAX = 64;
constexpr idx_t LABEL_MAX = 63;
constexpr idx_t CLUSTER_MAX = 63;
//! A cluster endpoint's port when its URI gives none: Redshift's default.
constexpr int REDSHIFT_DEFAULT_PORT = 5439;

[[noreturn]] void Refuse(const string &what, const string &reason) {
	throw InvalidConfigurationException("%s %s", what, reason);
}

//! Splits at every `separator`, keeping empty fields, also a trailing one, which StringUtil::Split
//! drops: "a:b:" is three fields, so a trailing separator is refused rather than ignored.
vector<string> SplitFields(const string &text, char separator) {
	vector<string> fields;
	idx_t start = 0;
	for (idx_t at = 0; at <= text.size(); at++) {
		if (at == text.size() || text[at] == separator) {
			fields.push_back(text.substr(start, at - start));
			start = at + 1;
		}
	}
	return fields;
}

bool IsLowerAlnum(char c) {
	return (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9');
}

bool AllDigits(const string &text) {
	return !text.empty() && text.find_first_not_of("0123456789") == string::npos;
}

bool IsNameText(const string &text) {
	for (auto c : text) {
		if (!IsLowerAlnum(c) && c != '-') {
			return false;
		}
	}
	return true;
}

//! An AWS region name: two letters, one or more lower-case words, and a number (eu-north-1,
//! us-gov-west-1). The client checks it against its region list afterwards.
bool IsRegion(const string &text) {
	auto parts = SplitFields(text, '-');
	if (parts.size() < 3 || parts[0].size() != 2 || !AllDigits(parts.back())) {
		return false;
	}
	for (idx_t i = 0; i + 1 < parts.size(); i++) {
		if (parts[i].empty() || parts[i].find_first_not_of("abcdefghijklmnopqrstuvwxyz") != string::npos) {
			return false;
		}
	}
	return true;
}

bool IsWorkgroupName(const string &text) {
	return text.size() >= WORKGROUP_MIN && text.size() <= WORKGROUP_MAX && IsNameText(text);
}

//! IsWorkgroupName's rule, for messages, from the same bounds so the two cannot drift apart.
string WorkgroupNameRule() {
	return std::to_string(WORKGROUP_MIN) + " to " + std::to_string(WORKGROUP_MAX) +
	       " lower-case letters, digits and '-'";
}

//! A workgroup endpoint label: a host label, 1 to 63 lower-case letters, digits and hyphens, not starting or
//! ending with a hyphen, and not digits alone, which in the label's place in the host are an account.
bool IsEndpointLabel(const string &text) {
	return !text.empty() && text.size() <= LABEL_MAX && IsNameText(text) && text.front() != '-' && text.back() != '-' &&
	       !AllDigits(text);
}

//! IsEndpointLabel's rule, for messages.
string EndpointLabelRule() {
	return "1 to " + std::to_string(LABEL_MAX) +
	       " lower-case letters, digits and '-', not starting or ending with '-' and not digits alone";
}

//! A cluster endpoint's second label, <cluster>.<label>.<region>.redshift.amazonaws.com: 1 to 63
//! lower-case letters and digits, as Redshift generates it.
bool IsClusterEndpointLabel(const string &text) {
	if (text.empty() || text.size() > LABEL_MAX) {
		return false;
	}
	for (auto c : text) {
		if (!IsLowerAlnum(c)) {
			return false;
		}
	}
	return true;
}

bool IsClusterId(const string &text) {
	return !text.empty() && text.size() <= CLUSTER_MAX && IsNameText(text) && text[0] >= 'a' && text[0] <= 'z' &&
	       text.back() != '-' && text.find("--") == string::npos;
}

string NotAnEndpoint() {
	return "is not a Redshift endpoint. " + RedshiftEndpointFormsHint();
}

//! The target an endpoint host names, and whether it is a workgroup.
RedshiftPathTarget ParseHost(const string &what, const string &given, bool &is_workgroup) {
	if (given.empty()) {
		Refuse(what, "has no host. " + RedshiftEndpointFormsHint());
	}
	if (given.front() == '[') {
		Refuse(what, "names an IP address, not a Redshift endpoint. " + RedshiftEndpointFormsHint());
	}
	auto host = StringUtil::Lower(given);
	if (host.back() == '.') {
		host.pop_back(); // a fully qualified name's root dot
	}
	auto labels = SplitFields(host, '.');
	if (labels.size() != 6 || labels[4] != "amazonaws" || labels[5] != "com") {
		Refuse(what, NotAnEndpoint());
	}
	for (auto &label : labels) {
		if (label.empty()) {
			Refuse(what, NotAnEndpoint());
		}
	}
	RedshiftPathTarget target;
	is_workgroup = true;
	if (labels[3] == "redshift-serverless") {
		// <workgroup>.<account>.<region>.redshift-serverless.amazonaws.com, or a label in the account's place:
		// digits alone are an account, anything else a label.
		if (!IsWorkgroupName(labels[0])) {
			Refuse(what, "has a workgroup name that is not " + WorkgroupNameRule());
		}
		if (AllDigits(labels[1])) {
			if (labels[1].size() != ACCOUNT_DIGITS) {
				Refuse(what, "has no 12-digit account after the workgroup name. " + RedshiftEndpointFormsHint());
			}
			target.account_id = labels[1];
		} else if (IsEndpointLabel(labels[1])) {
			target.endpoint_label = labels[1];
		} else {
			Refuse(what, "has an endpoint label, after the workgroup name, that is not " + EndpointLabelRule() + ". " +
			                 RedshiftEndpointFormsHint());
		}
		target.workgroup = labels[0];
		target.region = labels[2];
	} else if (labels[3] == "redshift") {
		if (!IsClusterId(labels[0])) {
			Refuse(what, "has a cluster identifier that is not a letter followed by lower-case letters, digits and "
			             "single '-', at most 63 characters");
		}
		if (!IsClusterEndpointLabel(labels[1])) {
			Refuse(what, "has a cluster endpoint label, after the cluster identifier, that is not 1 to 63 lower-case "
			             "letters and digits. " +
			                 RedshiftEndpointFormsHint());
		}
		is_workgroup = false;
		target.cluster = labels[0];
		target.endpoint_label = labels[1];
		target.region = labels[2];
	} else {
		Refuse(what, NotAnEndpoint());
	}
	if (!IsRegion(target.region)) {
		Refuse(what, "has no AWS region in its host. " + RedshiftEndpointFormsHint());
	}
	target.host = host;
	return target;
}

//! An ARN split as duckdb-aws reads it: arn:<partition>:<service>:<region>:<account>:<resource>, where
//! the resource is everything after the fifth ':', so that a namespace:<id> resource keeps a further ':'.
struct SplitArn {
	vector<string> fields;
	string resource;
	//! The resource split at '/'.
	vector<string> resource_parts;
};

//! The remedy for an ARN that names a Serverless workgroup or namespace by ID: the workgroup's name.
string WorkgroupNameRemedy() {
	return StringUtil::Format("Give the workgroup's name with its account: ATTACH '' AS <handle> (TYPE %s, WORKGROUP "
	                          "'<workgroup>', ACCOUNT_ID '<account>', REGION '<region>', ...), or its endpoint: "
	                          "ATTACH 'redshift://<workgroup>.<account>.<region>.redshift-serverless.amazonaws.com' "
	                          "AS <handle> (TYPE %s, ...)",
	                          REDSHIFT_ATTACH_TYPE, REDSHIFT_ATTACH_TYPE);
}

//! Refuses every Redshift ARN, and a Serverless ARN that names its workgroup or namespace by ID, with
//! the remedy for its form; returns for any other Serverless ARN, whose fields the caller checks. An
//! ARN that names its target by ID needs an AWS lookup to find the cluster or workgroup it means, a
//! call this extension does not make. The forms are those the duckdb-aws revision DuckDB v2.0.1 pins,
//! 1b96925 (DuckDB's .github/config/extensions/aws.cmake, which attach_path.test checks), resolves in
//! src/redshift/redshift_storage.cpp: a Serverless workgroup/<id> or namespace/<id> and a provisioned resource that
//! starts namespace:<id>, each named by ID with or without a region and an account. No remedy sends the
//! user to the aws extension, which hands every Redshift ATTACH to this one.
void RefuseOtherArnForms(const string &what, const SplitArn &arn, const string &forms) {
	const auto &fields = arn.fields;
	const string by_id = " names its target by ID, and finding the target it means needs an AWS lookup that this "
	                     "extension does not make. ";
	if (fields[2] == "redshift") {
		if (fields.size() == 7 && fields[5] == "cluster") {
			Refuse(what, "is a cluster ARN. " + RedshiftClusterFormsHint());
		}
		// A resource that starts "namespace:" and goes on, namespace:a:b too, as duckdb-aws reads it.
		if (StringUtil::StartsWith(arn.resource, "namespace:") && arn.resource.size() > strlen("namespace:")) {
			Refuse(what, "is a provisioned namespace ARN, which" + by_id + RedshiftClusterFormsHint());
		}
		Refuse(what, "is a Redshift ARN of a resource other than a cluster or a namespace. " + forms);
	}
	if (fields[2] != "redshift-serverless") {
		Refuse(what, "is not a Redshift Serverless workgroup ARN. " + forms);
	}
	// A workgroup/<id> or namespace/<id> resource, with one '/' and an ID after it.
	const auto &resource = arn.resource_parts;
	if (resource.size() == 2 && (resource[0] == "workgroup" || resource[0] == "namespace") && !resource[1].empty()) {
		Refuse(what, "is a Serverless " + resource[0] + " ARN, which" + by_id + WorkgroupNameRemedy());
	}
}

} // namespace

string RedshiftTargetFormsHint() {
	return StringUtil::Format("TYPE %s takes a redshift://<endpoint>[:<port>][/<database>] path; a workgroup "
	                          "endpoint's long-form ARN, "
	                          "arn:aws:redshift-serverless:<region>:<account>:workgroup/<label>/<workgroup>; or the "
	                          "empty path with WORKGROUP and ACCOUNT_ID, or CLUSTER",
	                          REDSHIFT_ATTACH_TYPE);
}

string RedshiftClusterFormsHint() {
	return StringUtil::Format("Give the cluster as CLUSTER: ATTACH '' AS <handle> (TYPE %s, CLUSTER '<cluster>', "
	                          "REGION '<region>', ...), or its endpoint as the path: ATTACH "
	                          "'redshift://<cluster>.<label>.<region>.redshift.amazonaws.com[:<port>]' AS <handle> "
	                          "(TYPE %s, ...)",
	                          REDSHIFT_ATTACH_TYPE, REDSHIFT_ATTACH_TYPE);
}

bool IsClusterIdentifierPath(const string &path) {
	return IsClusterId(path);
}

string RedshiftEndpointFormsHint() {
	return "Give a Serverless workgroup endpoint (<workgroup>.<account>.<region>.redshift-serverless.amazonaws.com, "
	       "or <workgroup>.<label>.<region>.redshift-serverless.amazonaws.com for one without an account) or a "
	       "cluster endpoint (<cluster>.<label>.<region>.redshift.amazonaws.com)";
}

bool IsRedshiftEndpointUri(const string &path) {
	return StringUtil::StartsWith(StringUtil::Lower(path), URI_SCHEME);
}

bool IsArnPath(const string &path) {
	return StringUtil::StartsWith(StringUtil::Lower(path), ARN_PREFIX);
}

RedshiftPathTarget ParseRedshiftEndpointUri(const string &uri) {
	const string what = "The path";
	if (!IsRedshiftEndpointUri(uri)) {
		Refuse(what, "is not a redshift:// URI");
	}
	const auto rest = uri.substr(strlen(URI_SCHEME));
	for (auto c : rest) {
		const auto byte = static_cast<unsigned char>(c);
		if (byte <= 0x20 || byte >= 0x7f) {
			Refuse(what, "holds a space, a control character or a non-ASCII character");
		}
		switch (c) {
		case '@':
			Refuse(what, "holds user information ('@'). The URI carries no credentials");
		case '?':
		case '#':
			Refuse(what, "holds a query or a fragment ('?' or '#'), which a Redshift URI does not take");
		case '%':
			Refuse(what, "holds percent-encoding, which a Redshift URI does not take");
		case '\\':
			Refuse(what, "holds a backslash");
		default:
			break;
		}
	}
	const auto slash = rest.find('/');
	const auto authority = rest.substr(0, slash);
	string database;
	if (slash != string::npos) {
		database = rest.substr(slash + 1);
		if (database.find('/') != string::npos) {
			Refuse(what, "has a path below the database: give redshift://<endpoint>[:<port>][/<database>]");
		}
	}
	auto host = authority;
	int port = 0;
	const auto colon = authority.rfind(':');
	if (colon != string::npos) {
		host = authority.substr(0, colon);
		const auto port_text = authority.substr(colon + 1);
		// At most five digits, so the conversion cannot overflow.
		if (AllDigits(port_text) && port_text.size() <= 5) {
			port = std::stoi(port_text);
		}
		if (port < 1 || port > 65535) {
			Refuse(what, "has a port that is not a number from 1 to 65535");
		}
	}
	bool is_workgroup = false;
	auto target = ParseHost(what, host, is_workgroup);
	// The client connects to exactly this endpoint's port: the one given, a workgroup's or a cluster's.
	// A cluster with none keeps Redshift's default; a workgroup's 0 means the same default below the
	// interface.
	target.endpoint_port = port == 0 && !is_workgroup ? REDSHIFT_DEFAULT_PORT : port;
	target.database = database;
	target.port_given = port != 0;
	return target;
}

RedshiftPathTarget ParseLongWorkgroupArn(const string &arn) {
	const string what = "The path";
	SplitArn split;
	split.fields = SplitFields(arn, ':');
	const auto &fields = split.fields;
	const string forms = RedshiftTargetFormsHint();
	if (fields[0] != "arn") {
		Refuse(what, "is not a Redshift Serverless workgroup ARN: an ARN starts with 'arn:' in lower case. " + forms);
	}
	if (fields.size() < 6) {
		Refuse(what, "is not a Redshift Serverless workgroup ARN. " + forms);
	}
	if (fields[1] != "aws") {
		Refuse(what, "is an ARN outside the aws partition, which this extension does not support. " + forms);
	}
	// The resource is everything after the fifth ':', as duckdb-aws reads it, so that a namespace:<id>
	// resource with further ':' stays whole; for the six-field long form it is the sixth field alone.
	split.resource = StringUtil::Join(vector<string>(fields.begin() + 5, fields.end()), ":");
	split.resource_parts = SplitFields(split.resource, '/');
	RefuseOtherArnForms(what, split, forms);
	const auto &resource = split.resource_parts;
	if (fields.size() != 6 || resource.size() != 3 || resource[0] != "workgroup") {
		Refuse(what, "is not a workgroup endpoint's long-form ARN. " + forms);
	}
	if (!IsRegion(fields[3])) {
		Refuse(what, "is an ARN with no valid AWS region");
	}
	if (fields[4].size() != ACCOUNT_DIGITS || !AllDigits(fields[4])) {
		Refuse(what, "is an ARN with no 12-digit account");
	}
	if (!IsEndpointLabel(resource[1])) {
		Refuse(what, "is a workgroup ARN whose endpoint label is not " + EndpointLabelRule());
	}
	if (!IsWorkgroupName(resource[2])) {
		Refuse(what, "is a workgroup ARN whose workgroup name is not " + WorkgroupNameRule());
	}
	RedshiftPathTarget target;
	target.region = fields[3];
	target.workgroup = resource[2];
	target.endpoint_label = resource[1];
	return target;
}

} // namespace duckdb
