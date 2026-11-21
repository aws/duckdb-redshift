//===----------------------------------------------------------------------===//
// redshift_attach_aws_forms.hpp
//
// The ATTACH statements written for the aws extension's Redshift ATTACH, translated into this
// extension's own forms. duckdb/duckdb-aws#258 hands every `TYPE redshift` and Redshift ARN ATTACH
// to this extension unchanged, so a statement a user wrote for the aws extension reaches it as typed:
//
//   ATTACH '<cluster>' [AS <handle>] (TYPE redshift [, SECRET <aws or s3 secret>] [, REGION '<region>']
//          [, DATABASE | DBNAME '<database>'] [, HOST '<the cluster's endpoint>' [, PORT <port>]]
//          [, DURATION_SECONDS <n>] [, <a postgres extension option> ...]);
//
// and, through duckdb/duckdb-aws#259, an ARN that names its target by ID arrives as the target's
// `redshift://<endpoint>:<port>` path with the user's HOST, PORT and DBNAME beside it. This file
// holds everything those statements need that this extension's own forms do not have: the cluster
// identifier as the path, HOST, PORT and DBNAME, the aws extension's secret choice and region
// fallback, and the options that are ignored. The rest of the ATTACH (redshift_attach.cpp) sees only
// this extension's own forms: the empty path with the target options, a `redshift://` path, or the
// long-form workgroup ARN.
//
// The translation never connects to a host it is given: HOST is read as a `redshift://` host and held
// to what else names the target, so it can only name the endpoint the connection goes to, whose name
// the client checks the server's certificate against. Every value it refuses can be given a meaning
// later without failing an ATTACH that works.
//===----------------------------------------------------------------------===//

#pragma once

#include "duckdb/common/types/value.hpp"
#include "duckdb/common/unordered_map.hpp"
#include "redshift_attach_path.hpp"
#include "redshift_config_options.hpp"

namespace duckdb {

class ClientContext;

//! The aws extension's options, as its ParseAttachOptions reads them (duckdb-aws 1b96925,
//! src/redshift/redshift_storage.cpp): the endpoint and its port, and the database under another name.
constexpr const char *AWS_FORM_HOST = "host";
constexpr const char *AWS_FORM_PORT = "port";
constexpr const char *AWS_FORM_DBNAME = "dbname";

//! ATTACH options ignored beside any form, whatever their value, and never read: the postgres
//! extension's, which the aws extension passed on to it with its own (no postgres connection is made),
//! and the aws extension's DURATION_SECONDS, the lifetime of the database credentials it minted. The
//! driver mints those itself at every connect, and an open session outlives them, so no value of it
//! could change what a handle does. The postgres options are duckdb-postgres 8b9d0bbe's ATTACH options
//! (src/postgres_storage.cpp), the revision DuckDB v2.0.1 pins; DURATION_SECONDS is duckdb-aws 1b96925's.
constexpr const char *IGNORED_ATTACH_OPTIONS[] = {
    "schema", "isolation_level", "use_text_protocol", "secret_storage_table", "connect_display", "duration_seconds"};

//! Whether `key`, as DuckDB lowercases it, is one of the options this file takes from an ATTACH: HOST,
//! PORT, DBNAME, or an ignored one. The ATTACH's option-name check takes these too.
bool IsAwsFormOption(const string &key);

//! The aws extension's options an ATTACH gave, each checked on its own value.
struct AwsFormOptions {
	//! HOST, verbatim; empty when not given.
	string host;
	//! PORT, from 1 to 65535; 0 when not given.
	int port = 0;
	//! DBNAME, verbatim; empty when not given, or given as '' beside a cluster identifier, where that means none.
	string dbname;
};

//! Removes HOST, PORT, DBNAME and the ignored options from the options of the ATTACH of `path`, so that
//! only this extension's own options are left, and returns HOST, PORT and DBNAME. Beside a cluster
//! identifier path it also removes a DATABASE of '', which the aws extension reads as none. Each value is
//! checked on its own: HOST and DBNAME strings, HOST not empty, DBNAME not empty except beside a cluster
//! identifier, and PORT a whole number from 1 to 65535, written as a number or quoted, as the aws
//! extension reads it; the ignored options' values are not read. Throws InvalidConfigurationException,
//! naming the option, never repeating the value.
AwsFormOptions TakeAwsFormOptions(const string &path, unordered_map<string, Value> &options);

//! An ATTACH in this extension's own forms, as TranslateAwsForms gives it.
struct AwsFormTranslation {
	//! The path: as given, except a cluster identifier, which becomes the empty path (`options.cluster`
	//! then names it) or, with HOST, that cluster's `redshift://` endpoint.
	string path;
	//! The options, with DBNAME as DATABASE and a cluster identifier's credential source and region. Its
	//! `aws_secret` is the AWS secret chosen for a cluster identifier given neither SECRET nor
	//! CREDENTIAL_PROVIDER, as the aws extension chooses it, and empty otherwise: SECRET is the caller's.
	RedshiftConfigOptions options;
	//! The region to fall back to after the AWS secret's region setting, as the aws extension does, for a
	//! cluster identifier whose region neither REGION nor HOST gives and whose AWS secret is read: null
	//! when there is no such fallback, empty when it falls back to none.
	unique_ptr<string> fallback_region;
};

//! Translates the ATTACH of `path` with `options` and `aws_forms` into this extension's own forms.
//! `secret_name` is SECRET's value, or null. `uri_target` is `path`'s parsed target when `path` is a
//! `redshift://` URI, else null. Beside a cluster identifier path: WORKGROUP, ACCOUNT_ID and CLUSTER are
//! refused; HOST must be that cluster's own endpoint, which becomes the path, with PORT or 5439 as its
//! port, and PORT without HOST is refused; without SECRET and
//! CREDENTIAL_PROVIDER, the secret is chosen as the aws extension's FindAwsSecret chooses it; and the
//! region falls back to s3_region and the environment. Beside a `redshift://` path, HOST and PORT may
//! only repeat it, and are dropped, and DBNAME is refused when it names a database. Beside the empty
//! path and the long-form ARN, HOST and PORT are refused. DBNAME is DATABASE beside every form. Throws
//! InvalidConfigurationException with the reason.
AwsFormTranslation TranslateAwsForms(ClientContext &context, const string &path, const RedshiftPathTarget *uri_target,
                                     const RedshiftConfigOptions &options, optional_ptr<const string> secret_name,
                                     const AwsFormOptions &aws_forms);

//! The refusal of a region missing from every source when it falls back (AwsFormTranslation), naming
//! the sources: REGION, the AWS secret `aws_secret`, s3_region and the environment.
[[noreturn]] void ThrowMissingFallbackRegion(const string &aws_secret);

} // namespace duckdb
