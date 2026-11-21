# ATTACH options

## Options

| Option | Value | Default | Notes |
|---|---|---|---|
| `TYPE` | `redshift` | | Required. Without it, `ATTACH ''` attaches an empty in-memory DuckDB database of that name and ignores the other options |
| `WORKGROUP` | A Serverless workgroup's name | | With `ACCOUNT_ID`, on an empty path. A name, not an ARN |
| `ACCOUNT_ID` | The 12-digit ID of the account that owns the workgroup | | Only with `WORKGROUP`. Quote it: `ACCOUNT_ID '111122223333'` |
| `CLUSTER` | A provisioned cluster's identifier | | On an empty path. Not with `WORKGROUP` |
| `DATABASE` | The database to connect to | `dev` | Not when the path names one |
| `REGION` | An AWS region, such as `us-east-1` | The path's, else the secret's | Required on an empty path, unless the `SECRET` holds a region. With a path, it must be the path's |
| `CREDENTIAL_PROVIDER` | `aws_default_chain` or `profile` | | One credential source: this or `SECRET` |
| `PROFILE` | An AWS profile name | | Required with `CREDENTIAL_PROVIDER profile`, and only with it |
| `SECRET` | The name of a DuckDB `aws` or `s3` secret | | One credential source: this or `CREDENTIAL_PROVIDER` |
| `READ_WRITE` | DuckDB's own option | | Needed only in a DuckDB opened read-only; `READ_ONLY` is refused |

The options above take strings. Quote them; a bare word such as `aws_default_chain` reads as a
string too, but a bare number, such as an unquoted `ACCOUNT_ID`, is refused.
[Authentication](authentication.md) describes `CREDENTIAL_PROVIDER`, `PROFILE` and `SECRET`. There
is no configuration secret: the target, region and database are given on each `ATTACH`. An option
the table does not list is refused by name, and so is one that asks for a sign-in other than IAM,
such as a user name or password. The exceptions are the `aws` extension's options
([below](#attach-statements-written-for-the-aws-extension)).

## Path forms

| Target | Path |
|---|---|
| Given by options | `''` |
| Serverless workgroup | `redshift://<workgroup>.<account>.<region>.redshift-serverless.amazonaws.com[:<port>][/<database>]` |
| Serverless workgroup endpoint with a label instead of an account | `redshift://<workgroup>.<label>.<region>.redshift-serverless.amazonaws.com[:<port>][/<database>]` |
| The same labelled endpoint, by its long-form ARN | `arn:aws:redshift-serverless:<region>:<account>:workgroup/<label>/<workgroup>` |
| Provisioned cluster | `redshift://<cluster>.<label>.<region>.redshift.amazonaws.com[:<port>][/<database>]` |

- The `redshift://` scheme is matched in any letter case.
- The port is 5439 unless the path gives another, from 1 to 65535.
- A `redshift://` path holds no user information (`@`), query or fragment (`?`, `#`),
  percent-encoding, backslash, space, control or non-ASCII character, and no path below the
  database.
- Any other ARN is refused, such as one that names a workgroup or namespace by its ID, or a cluster
  ARN. Give a workgroup by its endpoint or as `WORKGROUP` with `ACCOUNT_ID`, and a cluster by its
  endpoint or as `CLUSTER`, or attach the ARN without `TYPE` through the `aws` extension ([By
  ARN](attaching.md#by-arn-through-the-aws-extension)).

## Values

`ATTACH` checks the account ID, the host and port a path gives, and, with `SECRET`, the secret
itself: its name, that it exists and is of type `aws` or `s3`, and that it holds a key ID and secret
key. The rest, a path's database and the region list included, is checked when the `ATTACH`
connects.

| Value | Rule |
|---|---|
| Workgroup name | 3 to 64 lower-case letters, digits and `-` |
| Account ID | Exactly 12 digits, as a quoted string |
| Cluster identifier | 1 to 63 lower-case letters, digits and `-`, starting with a letter, with no two `-` in a row and none at the end |
| Database name | At most 127 bytes, with no leading or trailing space and no control character or `}` |
| Profile name | At most 255 bytes, with the same character rules |
| Secret name | At most 255 bytes, with the same character rules |
| Region | A lower-case `aws`-partition region built into the release. The China, AWS GovCloud (US), isolated and European Sovereign Cloud partitions are not supported |

## How options combine

- A path names the target, so `WORKGROUP`, `ACCOUNT_ID` and `CLUSTER` are refused beside it, and so
  is `DATABASE` when the path names a database.
- A `REGION` that names another region than the path is refused.
- `WORKGROUP` and `CLUSTER` together are refused: give one target.
- `WORKGROUP` without `ACCOUNT_ID`, and `ACCOUNT_ID` without `WORKGROUP`, are refused.
- An empty path that names no target is refused.
- Exactly one credential source: `SECRET`, or `CREDENTIAL_PROVIDER` (with `PROFILE` exactly when it
  is `profile`). None, or both, is refused.

## ATTACH statements written for the aws extension

`ATTACH` statements written for DuckDB's `aws` extension are also accepted, for compatibility; write
new statements in the forms on this page. Such a statement that names a cluster by its identifier
and gives no `DATABASE` connects to `dev`, not to the cluster's own default database: give
`DATABASE` on a cluster whose default is another one. Without `SECRET` or `CREDENTIAL_PROVIDER`,
such a statement uses your only `aws` secret, else your only `s3` secret, whatever its `SCOPE`,
persistent ones included; with neither secret, it uses the AWS default credential chain. Two
secrets of that type are refused: give `SECRET` to choose.
