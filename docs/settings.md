# Settings and environment variables

The target and the credentials are given in `ATTACH`, not as settings: see
[ATTACH options](attach-options.md).

## The extension's setting

| Name | Type | Default | Description |
|---|---|---|---|
| `redshift_allow_query_in_duckdb_transaction` | `BOOLEAN` | `false` | Lets `redshift_query` run inside an explicit DuckDB transaction, where DuckDB `COMMIT` and `ROLLBACK` do not control its Redshift operations: a `ROLLBACK` does not undo them. Still refused on a handle the open transaction attached, in a transaction an earlier error aborted, and for statements sent while CONNECT-ed. See [Transactions](querying.md#transactions) |

It is set per session, with `SET` and `RESET`; `SET GLOBAL` works too. It is a debug setting, an
escape hatch for workloads that cannot leave a DuckDB transaction, so `duckdb_settings()` lists it
only when asked for debug settings; `current_setting()` reads it:

```sql
SELECT current_setting('redshift_allow_query_in_duckdb_transaction');
SELECT name, value FROM duckdb_settings(debug := true) WHERE name LIKE 'redshift%';
```

Setting it to `true` logs a warning, and so does the first call it lets run in a DuckDB transaction.

## DuckDB settings that matter

| Setting | Effect on this extension |
|---|---|
| `allow_extension_repositories` | Needs no change: the default, `'hosts'`, allows the AWS DuckDB extension repository, registered by its host name. `'forbidden'` stops registering it and installing, updating and loading from it ([Installing](install.md)) |
| `allow_unsigned_extensions` | Leave it off: builds from the AWS DuckDB extension repository are signed |
| `enable_external_access` | While `false`, no new handle can be attached; handles attached before keep working |
| `extension_directories` | Where DuckDB keeps installed extensions; see [Files on disk](security.md#files-on-disk) for where to put it |

## Environment variables

These are read by the extension or the driver it embeds:

| Variable | Effect |
|---|---|
| `AWS_PROFILE`, `AWS_CONFIG_FILE`, `AWS_SHARED_CREDENTIALS_FILE`, `AWS_ACCESS_KEY_ID`, `AWS_SECRET_ACCESS_KEY`, `AWS_SESSION_TOKEN` and the other variables of the AWS credential chain | Where `aws_default_chain` and `profile` find credentials ([Authentication](authentication.md)) |
| `AWS_REGION` | Read by the driver's AWS SDK: when set, it skips the SDK's region lookup in the instance metadata service, which makes each new connection wait about 12 seconds where that service does not answer ([Troubleshooting](troubleshooting.md#connecting)). It does not override the region an `ATTACH` gives |
| `AWS_EC2_METADATA_DISABLED` | `true` skips that lookup too; set it only on a host that is not an Amazon EC2 instance |
| `REDSHIFT_DRIVER_DIR` | The one directory the embedded driver is written to or found in, instead of the defaults ([Platforms](platforms.md#where-the-driver-is-written)) |
| `SSL_CERT_FILE` | A file of PEM CA certificates to verify Redshift's certificate against, instead of the system's. Read at each connect ([TLS](security.md#tls)) |
| `XDG_CACHE_HOME` and `HOME` (Linux), `HOME` (macOS) | Where the per-user cache for the driver is ([Platforms](platforms.md#where-the-driver-is-written)). On Windows the cache is in the user's local application data folder, which the extension asks Windows for rather than reading `LOCALAPPDATA` |
| `LD_LIBRARY_PATH` (Linux) | Searched by the system loader when the driver connects; see [Files on disk](security.md#files-on-disk) |

Some variables stop a connect while they are set, such as some libpq `PG*` variables,
`SSL_CERT_DIR`, `AWS_ENDPOINT_URL` and `ODBCINI`: [Environment
variables](security.md#environment-variables) lists them. Certificate verification cannot be turned
off.
