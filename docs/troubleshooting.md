# Troubleshooting

## How errors read

An error from Redshift or the driver carries its SQLSTATE on a line of its own, `SQLSTATE: <code>`:

```text
Binder Error: Redshift refused the statement: ERROR:  syntax error at or near "SELEC" in context "SELEC", at line 1
SQLSTATE: 42601
```

- **A statement that fails** says when it failed: `Redshift refused the statement` while Redshift
  prepared it, before anything ran; `Redshift could not run the statement` while it ran; or
  `Redshift could not return the rest of the result` while rows were arriving. The last two may say
  the statement may have run: check its effects before running it again.
- **Redshift's own message** follows, its first line only, for the classes that describe your
  statement: `42` (syntax and access), `22` (data), `23` (integrity), `0A` (not supported), `XX`
  (internal), `3D` and `3F` (a database or schema that does not exist), `2B`, `25` (transaction
  state), `40`, `55`, `57` and `P0`. The host names, addresses, process IDs, URLs and credential
  forms the extension recognises are replaced in it, as are bound parameter values of 8 bytes or
  more; check an error before you share it. For any other class the message names the class and you
  act on the SQLSTATE.
- **A connect failure** says what was being connected to, why it failed and what to do:

  ```text
  IO Error: Could not connect to Redshift at my-workgroup.111122223333.us-east-1.redshift-serverless.amazonaws.com (region us-east-1, workgroup my-workgroup, account 111122223333, database dev): the AWS identity is not allowed to connect to the workgroup.
  Grant it redshift-serverless:GetCredentials on arn:aws:redshift-serverless:<region>:<account>:workgroup/<workgroup-id>.
  SQLSTATE: 42501
  ```

  At `ATTACH`, where a connect failure usually appears, the message starts `Cannot attach rs: `.
  When the cause is none the extension recognises, the message adds the driver's reason, cleaned the
  same way: `the connection attempt failed. The driver reported "..."`.
- **A configuration error** has no SQLSTATE. `ATTACH` refuses a wrong option or combination at once,
  naming the handle: `Cannot attach rs: Missing required parameter 'CREDENTIAL_PROVIDER': ...`. A
  value, such as the region or a workgroup name, is checked when the `ATTACH` connects: `Cannot
  attach rs: Invalid value for parameter 'REGION': ...`.

Programs that read an error's extra information, as some DuckDB clients expose it, find the SQLSTATE
the extension acted on under `sqlstate`, every record's SQLSTATE under `sqlstates`, and, on a
connect failure after the driver has loaded, its version under `redshift_driver`. The debug log
names the driver too ([Logging](#logging)).

## What you see, and what to do

### Installing and loading

| What you see | What to do |
|---|---|
| `INSTALL` fails with an HTTP error | Check that DuckDB is the version [Installing](install.md#versions) names, on one of the [supported platforms](platforms.md): the address `INSTALL` downloads from names the DuckDB version and platform |
| `LOAD redshift` says the extension is not found | Load it from the repository: `LOAD redshift FROM aws.amazon.com` |

### Attaching

| What you see | What to do |
|---|---|
| `Cannot attach ...: Missing required parameter 'CREDENTIAL_PROVIDER'` | Name one credential source: `CREDENTIAL_PROVIDER aws_default_chain`, `CREDENTIAL_PROVIDER profile, PROFILE '...'` or `SECRET ...` ([Authentication](authentication.md)) |
| `Invalid value for parameter 'ACCOUNT_ID'` | Quote it, as 12 digits: `ACCOUNT_ID '111122223333'` |
| `the path must be empty, a redshift:// endpoint or a workgroup endpoint's ARN` | Start the endpoint with `redshift://`, or give the target as options on an empty path ([Attaching](attaching.md)) |
| `Cannot attach ...: Missing target` | Name a workgroup (`WORKGROUP` with `ACCOUNT_ID`) or a cluster (`CLUSTER`), or give the endpoint as a `redshift://` path |
| `names its target by ID, and finding the target it means needs an AWS lookup that this extension does not make` | Give the workgroup by its endpoint or `WORKGROUP` with `ACCOUNT_ID`, or the cluster by its endpoint or `CLUSTER` ([Path forms](attach-options.md#path-forms)) |
| `only IAM authentication is supported` | Remove the user, password or sign-in plugin option, and use IAM credentials ([Authentication](authentication.md#what-is-not-supported)) |
| `Cannot attach ...: a Redshift handle cannot be read-only` | Remove `READ_ONLY`; in a read-only DuckDB, add `READ_WRITE` ([The handle](attaching.md#the-handle)) |

### Connecting

| What you see | What to do |
|---|---|
| `Invalid value for parameter 'REGION': it is not an aws-partition region name`, at `ATTACH` | Use a lower-case `aws`-partition region, such as `us-east-1`; a region newer than the release needs a newer release |
| `no usable AWS credentials were found through the AWS credential chain` | Make credentials reachable for this process: environment variables, the profile `PROFILE` names, or the instance's role. For IAM Identity Center, run `aws sso login` and use a secret ([IAM Identity Center](authentication.md#iam-identity-center)). Name a profile that assumes a role with `PROFILE` ([Profiles](authentication.md#profiles)) |
| `the AWS credentials found through the AWS credential chain were refused or have expired`, or `the AWS credentials from AWS secret '...' were refused or have expired` | Refresh the credentials. A `SECRET`'s are never refreshed by the extension: create the secret again |
| `the AWS identity is not allowed to connect to the workgroup` (or cluster), SQLSTATE `42501` | Grant the IAM actions the hint names ([IAM permissions](authentication.md#iam-permissions)) |
| `the Redshift workgroup was not found` (or cluster) | Check the endpoint's workgroup or cluster name, account and region, that the credentials are for that account, and, for a cluster, whether it is paused |
| A connect failure whose reason names `FIPS and custom endpoint are not supported` or `Dualstack and custom endpoint are not supported` | `AWS_USE_FIPS_ENDPOINT` or `AWS_USE_DUALSTACK_ENDPOINT` is `true` in the environment, or `use_fips_endpoint` or `use_dualstack_endpoint` is in the AWS profile: the extension names the Redshift endpoint itself, which the AWS SDK does not combine with either setting. Unset them, or set them to `false`, for the DuckDB process |
| `the Redshift workgroup's host name could not be resolved` | Check the endpoint's spelling, and that this host can resolve public names |
| `the Redshift Serverless workgroup did not answer within ... seconds over ... connect attempts` | The workgroup may still be resuming from idle: run the `ATTACH` or the query again. If it keeps happening, the workgroup is not reachable from this host's network, as a private workgroup is not |
| `the Redshift cluster did not answer` | The cluster is not reachable from this host's network: check its public access, security group and route |
| Each new connection waits about 12 seconds before it connects, on a host where the instance metadata service (169.254.169.254) does not answer, as behind some VPNs | The driver's AWS SDK looks a region up there, although the extension gives the driver the target's region. Set `AWS_REGION` for the DuckDB process (for example `export AWS_REGION=us-east-1`) to skip that lookup, or, on a host that is not an Amazon EC2 instance, `AWS_EC2_METADATA_DISABLED=true` |
| `the database was not found on the Redshift workgroup`, SQLSTATE `3D000`, at connect | Check `DATABASE` or the database in the path. To read an auto-mounted catalog, connect to `dev` and use a [three-part name](querying.md#three-part-names-and-auto-mounted-catalogs) |
| `Cannot connect to shared database`, SQLSTATE `55000`, at connect | `DATABASE` names a catalog the target takes no connections to, such as `awsdatacatalog`, which `SHOW DATABASES` lists with `allow_connections` false: connect to `dev` and use a [three-part name](querying.md#three-part-names-and-auto-mounted-catalogs). The message's region and network hint does not apply |
| `the Redshift workgroup refused the login` (or cluster) | Check that the identity may sign in to the target's database. For a cluster attached by its endpoint, the credentials may be for another account with a cluster of the same name |
| `the server's TLS certificate could not be verified` | Check the host's CA certificates, `SSL_CERT_FILE` if set, and that nothing on the network path replaces Redshift's certificate. On Windows, the root store must hold Amazon's root ([Windows](platforms.md#where-the-driver-and-its-trust-file-are-written)). Verification cannot be turned off |
| `No CA certificate bundle was found` | Install the operating system's CA certificates, or set `SSL_CERT_FILE` to a file of PEM CA certificates |
| `<NAME> is set; the Redshift driver would read it` | Unset that variable for the DuckDB process ([Environment variables](security.md#environment-variables)) |
| `the Redshift ODBC driver could not be loaded` | The message names what is missing, such as `libkeyutils.so.1` or a newer glibc on Linux, or the Visual C++ runtime on Windows ([Platforms](platforms.md)) |
| `no usable directory to install the Redshift ODBC driver in` | The message names each directory tried and the rule it broke: set `REDSHIFT_DRIVER_DIR` to a directory only you can write ([Where the driver is written](platforms.md#where-the-driver-is-written)) |

### Running statements

| What you see | What to do |
|---|---|
| `"<name>" does not exist`, SQLSTATE `3D000`, in a statement | The database or catalog in a three-part name is not one `SHOW DATABASES` lists, or the user may not read it |
| `current transaction is aborted`, SQLSTATE `25P02` | An earlier statement failed inside a Redshift transaction: run `SELECT * FROM redshift_query('rs', 'ROLLBACK')` ([Transactions](querying.md#transactions)) |
| `redshift_query cannot run while a DuckDB transaction is open` | Run it outside the DuckDB transaction; the message says whether to `ROLLBACK` first ([Transactions](querying.md#duckdb-transactions)) |
| `redshift_query cannot use attach handle "rs" more than once in the same DuckDB statement` | Store one result in a DuckDB temporary table first, or attach a second handle |
| A statement holding `?` returns or fails oddly | Write no `?` in the SQL text; use `$1`, `$2`, ... for parameters and `chr(63)` for a literal question mark ([Parameters](querying.md#parameters)) |
| `has a type this extension does not support` | Cast the column in the Redshift SQL ([Data types](data-types.md)) |
| `the statement's result shape changed on the server after it was checked` | Redshift gave a column another type when the statement ran than when it was checked, as it can for an expression over a system view, such as `queue_time / 1e6` on `SYS_QUERY_HISTORY`. Cast the column in the Redshift SQL, such as `(queue_time / 1e6)::DOUBLE PRECISION`. The message says the statement may have run: check a write's effects first |
| `Redshift handle "rs" cannot be browsed as a catalog` | Query through `redshift_query`; list tables with `SHOW TABLES FROM SCHEMA ...` ([Databases, schemas and tables](querying.md#databases-schemas-and-tables)) |
| `the Redshift connection of attach handle "rs" was lost` | The statement may already have taken effect: the message says when it may have, so check before you run a write again. The next statement opens a new session; temporary tables, `SET` values and an open transaction of the lost session are gone, and the message says so when the session held any |
| `Redshift session error on attach handle "rs": the session cannot be used any more` | Redshift's answer (`Internal Service Error`, or `Existing writable session ... is still open`, SQLSTATE `XX000`) left the session unusable, so the extension discarded it. The next statement on the handle opens a new session; temporary tables, `SET` values and an open transaction of the old one are gone, and the message says so when the session held any. The message says whether the statement may have run: check before you run a write again. A script that must stop at this error should run with the DuckDB shell's `-bail`, so that it stops there instead of running its later statements in the new session, outside the old one's transaction |
| `the server gave no message` | If the message says the statement may have run, check its effects first. If it fails the same way again, report it with the extension and driver versions |
| After Ctrl-C, the statement keeps running on Redshift | Expected: there is no cancellation from DuckDB. Stop it from another session ([Cancelling a statement](querying.md#cancelling-a-statement)), or set Redshift's `statement_timeout` |

## Logging

The extension writes to DuckDB's log: the driver it loaded, waits for a workgroup to resume, and the
warnings the driver returns. Redshift's notices, such as
`INFO:  Table "t" does not exist and will be skipped`, are not in the log: the driver prints them
to the DuckDB process's standard error. Turn on debug logging before the `ATTACH`, which connects, in
memory so that the DuckDB CLI can query it, and read it. The driver line is written once per process,
at its first connect:

```sql
CALL enable_logging(level = 'debug', storage = 'memory');
ATTACH 'redshift://my-workgroup.111122223333.us-east-1.redshift-serverless.amazonaws.com/dev'
    AS rs (TYPE redshift, CREDENTIAL_PROVIDER aws_default_chain);
SELECT * FROM redshift_query('rs', 'SELECT 1');
SELECT log_level, message FROM duckdb_logs WHERE message LIKE '%Redshift%';
```

Run them after `DISCONNECT`: while CONNECT-ed, they go to Redshift.

Parameter values of 8 bytes or more are redacted from the statement warnings it records. DuckDB's
own query log, in the same table, records each statement as written, its `params` and any key in the
SQL included: check the log before you share it. Each connection also gives Redshift an application
name, `duckdb_redshift/<extension version> duckdb/<DuckDB version> <platform>`, which Redshift's
connection log (`SYS_CONNECTION_LOG`) records.

The limits that are not errors are in [Limitations](limitations.md).
