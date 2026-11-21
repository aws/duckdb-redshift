//===----------------------------------------------------------------------===//
//                         DuckDB - Redshift
//
// src/transport/odbc/target_hints.hpp
//
// What a failed connect says that depends on its target. Its own header, so the error path takes
// it without the connection-settings API. No ODBC header: tested offline.
//===----------------------------------------------------------------------===//

#pragma once

#include "duckdb/common/string.hpp"

namespace duckdb {

//! What a connect failure says that depends on its target.
struct TargetHints {
	//! What a failure calls the target: "workgroup" or "cluster".
	string noun;
	//! Whether a connect timeout may be the target resuming from idle suspension, so it carries the
	//! resume text and is eligible for the resume retry: Serverless only.
	bool resumes_when_idle = false;
	//! A denied connect's hint: the actions and the resource to grant.
	string permission;
	//! The hint for a target the connect cannot find or reach.
	string availability;
	//! The hint for a login the server refused (SQLSTATE class 28, or the server's refused-password text). A cluster
	//! endpoint sets it because credentials of another account that has a cluster of the same name get database
	//! credentials for that cluster, which the endpoint's cluster refuses; a workgroup sets it because its login uses a
	//! password GetCredentials has just issued. Where it is empty, a refused login gets a fixed hint to check that the
	//! identity may sign in to the target's database.
	string login_refused;
	//! The AWS secret the credentials came from (RedshiftConfig::aws_secret), named by a credential
	//! failure instead of the credential chain. Empty when the driver resolved the credentials.
	string aws_secret;
	//! Whether the connect goes to a workgroup's host on 5439 because no port was given: a workgroup by name
	//! with its account, or a workgroup endpoint path without a port. A failure to reach it then says how to
	//! give another port. False for every cluster (its identifier's port is looked up, and its endpoint's
	//! always comes with one), for a workgroup endpoint with its port, and for a workgroup without an account.
	bool default_port = false;
	//! Whether the driver resolves the credentials through the default AWS credential chain (no PROFILE and no
	//! SECRET) while AWS_PROFILE is set, so that a failure to find any adds the hint for a role profile, which that
	//! chain does not read.
	bool aws_profile_variable = false;
};

} // namespace duckdb
