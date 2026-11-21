# Attaching to Redshift

An `ATTACH` of `TYPE redshift` creates a *handle*: a name that [`redshift_query`](querying.md) and
`CONNECT` send Redshift SQL through. The target, a Serverless workgroup or a provisioned cluster, is
named either by its endpoint in the path or by options on an empty path. Each `ATTACH` names exactly
one source of AWS credentials ([Authentication](authentication.md)). `ATTACH` statements written for
DuckDB's `aws` extension are also accepted, for compatibility ([ATTACH
options](attach-options.md#attach-statements-written-for-the-aws-extension)). [ATTACH
options](attach-options.md) lists every option and how they combine.

## By endpoint

Give the endpoint the Redshift console shows after `redshift://`, with an optional port and
database:

```sql
-- A Serverless workgroup
ATTACH 'redshift://my-workgroup.111122223333.us-east-1.redshift-serverless.amazonaws.com/dev'
    AS rs (TYPE redshift, CREDENTIAL_PROVIDER aws_default_chain);

-- A provisioned cluster, with its port
ATTACH 'redshift://my-cluster.abc123xyz.us-east-1.redshift.amazonaws.com:5439/dev'
    AS rs_cluster (TYPE redshift, CREDENTIAL_PROVIDER aws_default_chain);
```

The region comes from the endpoint, and the port is 5439 unless the path gives another. A cluster
named by its endpoint is connected to at exactly that endpoint and port, with no lookup. A
Serverless workgroup endpoint that carries a label instead of an account,
`<workgroup>.<label>.<region>.redshift-serverless.amazonaws.com`, is attached the same way, or by
its long-form ARN, `arn:aws:redshift-serverless:<region>:<account>:workgroup/<label>/<workgroup>`.

## By options

On an empty path, name the target with options instead:

```sql
-- A Serverless workgroup: its name and the account that owns it
ATTACH '' AS rs (TYPE redshift, WORKGROUP 'my-workgroup', ACCOUNT_ID '111122223333',
    REGION 'us-east-1', DATABASE 'dev', CREDENTIAL_PROVIDER aws_default_chain);

-- A provisioned cluster: its identifier
ATTACH '' AS rs_cluster (TYPE redshift, CLUSTER 'my-cluster', REGION 'us-east-1',
    DATABASE 'dev', CREDENTIAL_PROVIDER aws_default_chain);
```

`CLUSTER` alone connects to the endpoint the cluster reports. A paused cluster is not resumed by a
connect: resume it first. A labelled workgroup endpoint has no option form: name it by its path.

## By ARN, through the aws extension

A namespace or workgroup ARN that names its target by its ID is refused by `TYPE redshift`, since
this extension cannot look a target up by its ID. DuckDB's `aws` extension can look it up: with it
loaded, give the ARN without `TYPE`, and the `aws` extension finds the target's endpoint and hands
the `ATTACH` to this extension.

```sql
LOAD aws;

-- A provisioned cluster, by its namespace ARN
ATTACH 'arn:aws:redshift:us-east-1:111122223333:namespace:<namespace-id>' AS rs_cluster (SECRET my_secret);

-- A Serverless workgroup, by its workgroup ARN
ATTACH 'arn:aws:redshift-serverless:us-east-1:111122223333:workgroup/<workgroup-id>' AS rs (SECRET my_secret);

-- A Serverless workgroup, by its namespace ARN
ATTACH 'arn:aws:redshift-serverless:us-east-1:111122223333:namespace/<namespace-id>' AS rs (SECRET my_secret);
```

- `SECRET` names an `aws` or `s3` secret. With at most one such secret it can be left out: the
  `aws` extension uses that secret, or, with none, the AWS default credential chain.
- Without `AS`, the handle's name comes from the ARN: for a cluster's namespace ARN, the ARN without
  its `arn:`; for a Serverless ARN, the workgroup's or namespace's ID. Give `AS` to choose a name you
  can type.
- Without `DATABASE`, the connection uses the target's own default database.
- The lookup needs, besides the permissions connecting needs ([IAM
  permissions](authentication.md#iam-permissions)): `redshift:DescribeClusters` for a cluster's
  namespace ARN; `redshift-serverless:ListWorkgroups` and `redshift-serverless:GetNamespace` for a
  workgroup ARN; `redshift-serverless:ListNamespaces` and `redshift-serverless:ListWorkgroups` for a
  Serverless namespace ARN.

## When errors appear

`ATTACH` first checks the options, how they combine and the shape of the path, and refuses what is
wrong at once, naming the handle. With `SECRET`, it also reads the secret: one that
does not exist, is not of type `aws` or `s3`, or holds no key ID or secret key is refused at
`ATTACH`. So is a missing region, when neither `REGION`, the path nor the secret gives one.

It then connects, opening the Redshift session of the DuckDB connection that ran it. So the values
themselves, such as the region, the database, and a workgroup or cluster name given as an option, are
checked at `ATTACH` too, and so are the network, whether the credentials are accepted, and the
permissions: a target, database or credentials it cannot connect with fail the `ATTACH`, as
`Cannot attach rs: ...`, and leave no handle. An `ATTACH OR REPLACE` that cannot connect keeps the
handle it would have replaced.

## The handle

- Name it with `AS`. An empty path needs `AS`; a path without `AS` gets the name DuckDB derives from
  it. The name `memory` is DuckDB's default database and is refused.
- The handle cannot be read-only: `redshift_query` sends its SQL to Redshift unchanged, so nothing
  could keep it from writing. `READ_ONLY` is refused; in a DuckDB opened read-only, add `READ_WRITE`
  to the `ATTACH`. For read-only access, use credentials whose database user has only `SELECT`
  grants.
- While `enable_external_access` is `false`, no new handle can be attached. Handles attached before
  keep working.
- Its tables are not browsable from DuckDB, so `SHOW TABLES FROM rs` lists nothing and `SELECT *
  FROM rs.public.t` is refused. [Running queries](querying.md#databases-schemas-and-tables) shows
  how to list them through `redshift_query`.
- `duckdb_databases()` lists the handles:

  ```sql
  SELECT database_name, type FROM duckdb_databases() WHERE type = 'redshift';
  ```

## Sessions and DETACH

Each DuckDB connection gets its own Redshift session on a handle: the one `ATTACH` opened for the
DuckDB connection that ran it, and for every other DuckDB connection one that its first query opens.
A session is kept until the DuckDB connection closes or the handle is detached. Temporary tables, `SET` values and an
open Redshift transaction stay in that session between queries. If its connection is lost, or a
statement fails with
`Redshift session error on attach handle "rs": the session cannot be used any more`, the next
statement opens a new session without them
([Troubleshooting](troubleshooting.md#running-statements)).

```sql
DETACH rs;
```

`DETACH` closes the handle's sessions. A Redshift transaction still open in one is rolled back.

## Network and regions

- The endpoint must be reachable from the host DuckDB runs on. A private workgroup or cluster is
  reachable only from inside its network. A custom domain name, a VPC endpoint's host name, an IP
  address, a proxy or a tunnel cannot be given.
- Region names are those of the `aws` partition. A region the extension accepts does not mean the
  target is available there; that is checked when it connects, and the error then has a hint. A
  newly launched AWS region needs a new release of the extension.
- A Serverless workgroup that is resuming from idle can take some time to answer the `ATTACH` or a
  DuckDB connection's first query.
  The extension waits up to 45 seconds for each of the endpoint's addresses on each connect attempt
  and makes up to three attempts, starting the last within 180 seconds of the first. A resume can
  outlast that wait; run the `ATTACH` or the query again. A workgroup whose network drops this host's connections,
  such as a private one, fails only after the same wait, about 4.5 minutes for the usual three
  addresses, while one whose network refuses them fails at once. While a connect waits, the
  process's other Redshift connects wait behind it, and Ctrl-C takes effect only when the wait ends.
  A cluster's connect is not retried.
