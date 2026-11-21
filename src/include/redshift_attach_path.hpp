//===----------------------------------------------------------------------===//
//                         DuckDB - Redshift
//
// redshift_attach_path.hpp
//
// The ATTACH path forms that name a target: a `redshift://` endpoint URI, a long-form Serverless
// workgroup ARN, or a cluster identifier, the aws extension's form. One strict parser; a path either
// names one target or is refused with the reason.
//===----------------------------------------------------------------------===//

#pragma once

#include "duckdb/common/common.hpp"

namespace duckdb {

//! The target an ATTACH path names. The parser keeps no host: the client builds the host from these
//! fields, a workgroup's and a cluster endpoint's alike.
struct RedshiftPathTarget {
	string region;
	//! A Serverless workgroup: its name, with `account_id` (12 digits) or, for a workgroup endpoint
	//! that carries no account, `endpoint_label` (the host's second label).
	string workgroup;
	string account_id;
	//! The host's second label: a workgroup endpoint's that carries no account, or a cluster
	//! endpoint's.
	string endpoint_label;
	//! A provisioned cluster: its identifier, the host's first label.
	string cluster;
	//! The port a URI gives, a workgroup's or a cluster's, or PORT beside a HOST that is a cluster
	//! identifier path's endpoint, from 1 to 65535. With none, a cluster's is Redshift's default, 5439,
	//! and a workgroup's 0, which the client reads as that default.
	int endpoint_port = 0;
	//! The database a URI's path names, or empty.
	string database;
	//! A `redshift://` URI's host, lower-cased and without a root dot, and whether the URI gives a port:
	//! what HOST and PORT beside it may repeat. Empty and false for every other path.
	string host;
	bool port_given = false;
};

//! Whether `path` is a `redshift://` URI (the scheme in any letter case).
bool IsRedshiftEndpointUri(const string &path);

//! Whether `path` is an ARN (`arn:` in any letter case).
bool IsArnPath(const string &path);

//! Parses `redshift://<endpoint>[:<port>][/<database>]`, where the endpoint is a workgroup's
//! (`<workgroup>.<account>.<region>.redshift-serverless.amazonaws.com`, or
//! `<workgroup>.<label>.<region>.redshift-serverless.amazonaws.com` with no account: a second label of
//! digits alone is an account) or a cluster's (`<cluster>.<label>.<region>.redshift.amazonaws.com`). A
//! port, if given, is from 1 to 65535. A workgroup keeps its port. A cluster keeps its label and port, 5439 when none
//! is given, so the client connects to exactly that endpoint: a cluster identifier is unique only within an account and
//! region. Throws InvalidConfigurationException with the reason, never repeating the value.
RedshiftPathTarget ParseRedshiftEndpointUri(const string &uri);

//! Parses a workgroup endpoint's long-form ARN,
//! `arn:aws:redshift-serverless:<region>:<account>:workgroup/<label>/<workgroup>`, whose account is
//! checked but not used: the credentials decide the account. Every other ARN is refused, with the
//! remedy for its form: an ARN that names its target by ID, which needs an AWS lookup, is told the
//! form that names the same target by name (RedshiftClusterFormsHint for a cluster, the workgroup's
//! name for a Serverless workgroup or namespace), and any other ARN RedshiftTargetFormsHint. Throws
//! InvalidConfigurationException with the reason, never repeating the value.
RedshiftPathTarget ParseLongWorkgroupArn(const string &arn);

//! Whether `path` is a Redshift cluster identifier, the aws extension's `ATTACH '<cluster>' (TYPE ...)`:
//! a lower-case letter, then lower-case letters, digits and single '-', not ending in '-', at most 63
//! characters. It names the cluster as CLUSTER does, and the client looks it up.
bool IsClusterIdentifierPath(const string &path);

//! Every form of this extension's own surface that names a target, for messages: a `redshift://` path,
//! a workgroup endpoint's long-form ARN, and the empty path with the target options. A refusal never
//! suggests the aws extension's cluster identifier path, which this extension takes for compatibility
//! only.
string RedshiftTargetFormsHint();

//! The remedy that names a provisioned cluster in this extension's own surface: CLUSTER with the empty
//! path, or the cluster's endpoint as a `redshift://` path.
string RedshiftClusterFormsHint();

//! The endpoint forms a `redshift://` URI takes, for messages.
string RedshiftEndpointFormsHint();

} // namespace duckdb
