//===----------------------------------------------------------------------===//
//                         DuckDB - Redshift
//
// redshift_attach.hpp
//
// Registering `ATTACH '' AS <handle> (TYPE <REDSHIFT_ATTACH_TYPE> [, SECRET <aws or s3 secret>]
// [, <option> '<value>' ...])` and `ATTACH '<redshift:// URI or ARN>' [AS <handle>] (TYPE
// <REDSHIFT_ATTACH_TYPE> ...)`, and resolving their configuration. The ATTACH statements written for the
// aws extension are translated into these forms first (redshift_attach_aws_forms.hpp).
//===----------------------------------------------------------------------===//

#pragma once

#include "duckdb/common/common.hpp"
#include "duckdb/common/optional_ptr.hpp"
#include "duckdb/common/types/value.hpp"
#include "duckdb/common/unordered_map.hpp"
#include "redshift_attach_aws_forms.hpp"
#include "redshift_config_options.hpp"
#include "redshift_transport.hpp"

namespace duckdb {

class ClientContext;
class ExtensionLoader;

//! The storage type name, as written in `ATTACH ... (TYPE <name>)`. It is also the extension's name,
//! so DuckDB loads the extension for an ATTACH that names the type. duckdb-aws 1b96925 registers a
//! stand-in of the same name only while nothing serves the type, and its stand-in loads this
//! extension and hands it the ATTACH, so either load order ends with this extension's type.
constexpr const char *REDSHIFT_ATTACH_TYPE = "redshift";

//! Registers the REDSHIFT_ATTACH_TYPE storage extension, so that `ATTACH '' AS <handle> (TYPE <that
//! name>, ...)` and `ATTACH '<redshift:// URI or ARN>' [AS <handle>] (TYPE <that name>, ...)` create
//! an attach handle: a non-browsable catalog holding the resolved configuration, and so do the aws
//! extension's statements that redshift_attach_aws_forms.hpp translates. ATTACH opens the
//! attaching DuckDB connection's connection once every check has passed (RedshiftCatalog::
//! OpenAttachingConnection), and fails with the open's error. The extension options are SECRET, the configuration
//! options of redshift_config_options.hpp, each a string, and the aws extension's (IsAwsFormOption); any other is
//! refused. So is the handle name `memory`, in any letter case, which DuckDB gives an ATTACH of the empty path without
//! AS, and a read-only handle, because redshift_query cannot keep one from writing: READ_ONLY, READ_WRITE false, or the
//! read-only default of a read-only DuckDB, which READ_WRITE overrides.
void RegisterRedshiftAttach(ExtensionLoader &loader);

//! The configuration parameters among the extension options of the ATTACH of `path`, as DuckDB delivered
//! them; a field is empty when its option is not given. The aws extension's options are taken first, into
//! `aws_forms` (TakeAwsFormOptions, redshift_attach_aws_forms.hpp). Ignores every other option. Throws an
//! InvalidConfigurationException, naming the parameter and `handle`, for a value that is not a string, which is never
//! cast, and for an empty one, and for what TakeAwsFormOptions refuses. An empty `handle` stands for a name DuckDB
//! derived from the path, which a refusal does not repeat; it says an AS name would be shown.
RedshiftConfigOptions ReadRedshiftAttachOptions(const string &handle, const string &path,
                                                const unordered_map<string, Value> &options, AwsFormOptions &aws_forms);

//! Resolves the configuration of the ATTACH of `handle` from its options and its path alone.
//! `secret_name` is SECRET's value, the name of the DuckDB `aws` or `s3` secret whose AWS credentials
//! the connection uses, or null when the ATTACH has no SECRET; a given but empty name is refused.
//! `path` is empty, or a `redshift://` endpoint or a workgroup endpoint's long-form ARN that names the
//! target (redshift_attach_path.hpp), the target's one source, or a cluster identifier. `aws_forms` holds
//! the aws extension's options, which ReadRedshiftAttachOptions took from the ATTACH's; TranslateAwsForms turns the
//! ATTACH into this extension's own forms first (redshift_attach_aws_forms.hpp). Opens no connection to Redshift.
//!
//! The configuration passes ValidateRedshiftOptions (redshift_config_options.hpp): one credential source,
//! SECRET or CREDENTIAL_PROVIDER, and the target rules. It must name a target. With SECRET, the AWS
//! secret is then read once (redshift_aws_secret.hpp): its credentials are checked and dropped, and
//! its region is the fallback for REGION after the path, and the last one unless the translation
//! falls back further. The secret is looked up through DuckDB's secret manager,
//! across every storage it has registered, so the call must run inside an active transaction, as
//! DuckDB's attach callback does. No secret SECRET does not name is ever used, except the one the
//! translation chooses for a cluster identifier.
//!
//! Throws an InvalidConfigurationException that names `handle` for a path of another form, a path
//! beside target options, an empty SECRET, a configuration ValidateRedshiftOptions refuses, one
//! without a target, a region that neither REGION, the path nor the AWS secret gives, and an AWS
//! secret that is missing, of another type or holds no key ID or secret key, and for what
//! TranslateAwsForms refuses. The aws extension's
//! default secret is also refused while its default credentials are off, and when its lookup answers
//! nothing or another aws secret answers it. An empty `handle`, a name DuckDB derived from the path,
//! is not named: the refusal says an AS name would be shown.
RedshiftConfig ResolveRedshiftAttachConfig(ClientContext &context, const string &handle, const string &path,
                                           const RedshiftConfigOptions &attach_options,
                                           optional_ptr<const string> secret_name, const AwsFormOptions &aws_forms);

} // namespace duckdb
