//===----------------------------------------------------------------------===//
//                         DuckDB - Redshift
//
// redshift_aws_secret.hpp
//
// Reading the AWS credentials of the DuckDB `aws` or `s3` secret an ATTACH's SECRET names.
//===----------------------------------------------------------------------===//

#pragma once

#include "duckdb/common/common.hpp"
#include "redshift_transport.hpp"

namespace duckdb {

class ClientContext;

//! What one read of an AWS secret gives: its credentials (`key_id`, `secret` and `session_token`),
//! which an open passes to one OpenRedshiftConnection call and an ATTACH checks and drops, and its
//! `region` setting, or empty, the fallback region of an ATTACH whose REGION and path give none. That setting is
//! whatever the extension that created the secret stored: a REGION given then, or for a credential_chain secret and the
//! aws extension's default secret, the region it resolved from s3_region, the environment or a profile.
struct RedshiftAwsSecretContents {
	RedshiftAwsCredentials credentials;
	string region;
};

//! The contents of the DuckDB secret `name`, read now through the secret manager. They are what the
//! extension that created the secret stored: the keys given to a `config` secret, or what the aws
//! extension resolved for a `credential_chain` secret when it created it; this reads them and runs no
//! credential chain itself. A secret in the aws extension's default storage, which returns it
//! unresolved by name, is read through the lookup for type `aws`, which makes that extension resolve
//! its chain on the first lookup, once that extension's default credentials are checked to be on, and
//! only if the lookup's match is from that default storage, whose `aws` and `s3` secrets hold one
//! identity.
//!
//! Throws InvalidConfigurationException, naming none of the secret's contents, for a secret
//! that does not exist, has a type other than `aws` or `s3`, or holds no key ID or secret key, and for
//! the default secret while that extension's default credentials are off, its lookup answers nothing,
//! or another aws secret answers its lookup. A secret that does not exist is not named: the name may
//! be a pasted key. Keeps nothing.
RedshiftAwsSecretContents ReadAwsSecret(ClientContext &context, const string &name);

//! Whether the aws extension's default storage answers a lookup, as duckdb-aws DefaultCredentialsEnabled
//! decides: aws_default_credentials is not false (or not registered, as without the aws extension), and
//! external access is on.
bool AwsDefaultCredentialsEnabled(ClientContext &context);

//! The name of the AWS secret the aws extension's Redshift ATTACH would use when it names none
//! (duckdb-aws 1b96925 FindAwsSecret, src/utils/utils.cpp): the only secret of type `aws` the user
//! created, else the only one of type `s3`, across every secret storage but the aws extension's default
//! storage, whatever their scope. Empty when there is neither. Throws InvalidConfigurationException
//! for two or more of the type it would take, asking for SECRET. Lists the secrets; reads none.
string FindOnlyUserAwsSecret(ClientContext &context);

} // namespace duckdb
