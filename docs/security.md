# Security considerations

## Trust model

Anyone who can run SQL in the DuckDB process acts with its credentials: they can attach targets with
the credentials the process can reach, and run any statement on every handle. A handle cannot be
read-only, so what a handle can do is what its database user may do in Redshift. Setting
`enable_external_access` to `false` stops new attaches, but handles attached before keep working.

To run SQL from less-trusted users or input:

- Use a dedicated, least-privilege IAM identity, whose database user has only the grants the work
  needs; for read-only work, only `SELECT`.
- Run that SQL in a DuckDB process of its own, which holds only the credentials and handles that
  user may use.
- Pass values to `redshift_query` in `params`, never by building the SQL text from input, and check
  any identifier you insert against a fixed list.

## TLS

Every connection to Redshift verifies the server's certificate and host name, and no option turns
that off. The trusted CA certificates come from the first of these that exists:

- `SSL_CERT_FILE`, when it names a file this process can read (one it cannot read is skipped; a
  directory or an empty file stops the connect with an error);
- on Linux, the system bundle: `/etc/pki/tls/certs/ca-bundle.crt`,
  `/etc/pki/ca-trust/extracted/pem/tls-ca-bundle.pem`, `/etc/ssl/certs/ca-certificates.crt`,
  `/etc/ssl/ca-bundle.pem` or `/etc/ssl/cert.pem`;
- on macOS, `/etc/ssl/cert.pem` (not the Keychain);
- on Windows, the roots the Windows certificate store trusts ([Windows](platforms.md#where-the-driver-and-its-trust-file-are-written)).

## Credentials

- The extension signs in with IAM only, and Redshift's database credentials are temporary, minted
  for each connection. A `redshift://` path cannot carry credentials. The extension stores none of
  the AWS credentials it reads; the embedded driver keeps a copy in memory while the process runs.
- The driver's and Redshift's messages are cleaned before you see them: the host names, addresses,
  process IDs, URLs and credential forms the extension recognises are replaced, as are bound
  parameter values of 8 bytes or more. The extension's own words are not: its connect errors name
  the target, its endpoint host and region included. A value written in a form it does not
  recognise can still show, so check an error before you share it.
- A key in a `COPY` or `UNLOAD`, or a password in `CREATE USER`, is redacted from the messages the
  extension shows, except where a syntax error points at the key or password itself: Redshift's
  "at or near" part then shows it. The statement still goes to Redshift as its text, and DuckDB's
  own query log, when it is on, records it as written ([Logging](troubleshooting.md#logging)). Use
  `IAM_ROLE` instead of keys where you can.
- A DuckDB secret is kept in memory unless it is created `PERSISTENT`, which DuckDB writes to disk.
  Prefer temporary secrets built from the credential chain.
- While CONNECT-ed, every statement goes to Redshift as text, so `DISCONNECT` before you type a
  credential ([CONNECT and DISCONNECT](querying.md#connect-and-disconnect)).

## Files on disk

On the first connect, the extension writes its embedded driver to disk and loads it from there
([Where the driver is written](platforms.md#where-the-driver-is-written)). It loads the files only
from a directory no other user can change, and checks them against the SHA-256s it was built with.
Some of the safety is the host's:

- Install the extension where only you, or root, can write. A shared or group-writable extension
  directory lets another user change what DuckDB loads.
- With a home directory on a network file system, put DuckDB's extension directory on a local disk
  (DuckDB's `extension_directories` setting) or install the extension owned by root, and set
  `REDSHIFT_DRIVER_DIR` to a local folder: whoever can write the home through the file server could
  otherwise change the driver. On a network file system the owners and modes the checks rely on are
  the ones the server reports.
- On Linux, `LD_LIBRARY_PATH` must not name a directory another user can write, nor one with
  subdirectories they can: the system loader also searches subdirectories such as `tls` and
  `glibc-hwcaps` below each element, and the driver looks up a library there by name when it
  connects. An empty or relative element stops the driver from loading.
- On macOS, a volume that ignores ownership, as external drives do by default, cannot hold the
  driver.

## Environment variables

Some variables would change the driver's connection or security settings, such as its TLS trust, an
AWS endpoint or the ODBC configuration. While one is set in the DuckDB process's environment,
connecting fails with an error that names it; unset it in the environment DuckDB runs in. They
include some libpq `PG*` variables, such as `PGSSLROOTCERT` and `PGOPTIONS`, `SSL_CERT_DIR`,
`SSLKEYLOGFILE`, `AWS_ENDPOINT_URL` and the per-service `AWS_ENDPOINT_URL_*` variables for Redshift,
Redshift Serverless, STS and IAM Identity Center, `ODBCINI`, and the Kerberos variables
(`KRB5_CONFIG`, `KRB5CCNAME` and others).
[refused-environment.txt](../third_party/redshift_odbc/policy/refused-environment.txt) lists every
one. On macOS, the `DYLD_` search variables (`DYLD_LIBRARY_PATH`, `DYLD_FRAMEWORK_PATH` and others)
stop the driver from loading too. Proxy variables are not refused, since TLS is verified end to end.
On Windows, removing a variable only from the Windows environment block, as .NET's
`Environment.SetEnvironmentVariable` does, leaves it in the C runtime's copy the driver reads, so it
is still refused: unset it before the program starts.
