# Amazon Redshift extension for DuckDB

The Amazon Redshift extension for [DuckDB](https://duckdb.org), named `redshift`, lets you run
Amazon Redshift SQL from a DuckDB session. DuckDB is an in-process analytical database. The
extension sends your SQL statements to Amazon Redshift as written, where they run with Amazon
Redshift SQL syntax and rules. DuckDB receives the results. You can then filter or aggregate them,
join them with local data, write them to Parquet, or store them in a DuckDB table. Use it when a
query on data that Amazon Redshift can read outgrows the machine that runs DuckDB. Amazon Redshift
runs the query on its own compute, and the result comes back to your DuckDB session to join with
your local files.

The extension connects to Amazon Redshift Serverless workgroups and provisioned clusters. You don't
need to install a separate driver or driver manager, or configure an Open Database Connectivity
(ODBC) data source, because the extension includes the official Amazon Redshift ODBC driver. It
signs in with AWS Identity and Access Management (IAM) credentials and verifies server certificates.

This project uses the DuckDB extension API and started from the [DuckDB extension template on
GitHub](https://github.com/duckdb/extension-template). It's a separate project. The DuckDB
Foundation and DuckDB Labs don't maintain it.

## Quick start

### Install the extension

The extension requires DuckDB v2.0.1 on one of the [supported platforms](#platforms). `INSTALL`
downloads the build for the DuckDB version and platform it runs on, and fails with an HTTP error
where the repository has none.

The following SQL statements register the AWS DuckDB extension repository by its host name and
install `redshift`.

```sql
CREATE EXTENSION REPOSITORY aws.amazon.com;
INSTALL redshift FROM aws.amazon.com;
```

DuckDB saves the repository registration and its signing keys for your user, so you don't need to
register it again in each session.

### Connect and query

Configure AWS credentials in the environment running DuckDB. For the required permissions, see
[Authentication and IAM permissions](docs/authentication.md). In each DuckDB session, load the
extension, attach a Serverless workgroup, and run a query. In the following SQL example, replace the
workgroup name, account ID, and AWS Region with your own values.

```sql
LOAD redshift FROM aws.amazon.com;
ATTACH '' AS rs (TYPE redshift, WORKGROUP 'my-workgroup', ACCOUNT_ID '111122223333',
    REGION 'us-east-1', CREDENTIAL_PROVIDER aws_default_chain);
SELECT * FROM redshift_query('rs', 'SELECT current_user, version()');
```

`rs` is the name of the handle that `ATTACH` creates. To run a statement on that Redshift target,
pass the handle name and the SQL statement to `redshift_query`. It returns the results as a DuckDB
table.

This example uses the AWS default credential chain. For other credential sources, see [AWS
credentials and the DuckDB `aws` extension](#aws-credentials-and-the-duckdb-aws-extension).

You can also attach the workgroup by the endpoint the Redshift console shows, as a `redshift://` URI
with an optional port and database. The extension takes the Region from the endpoint.

```sql
ATTACH 'redshift://my-workgroup.111122223333.us-east-1.redshift-serverless.amazonaws.com/dev'
    AS rs (TYPE redshift, CREDENTIAL_PROVIDER aws_default_chain);
```

For provisioned clusters and other ways to specify a target, see [Attaching to
Redshift](docs/attaching.md).

To type Redshift SQL directly, `CONNECT` to the handle. Every statement then goes to Redshift as
written, until `DISCONNECT` returns to DuckDB:

```sql
CONNECT rs;
SELECT current_user, version();
DISCONNECT;
```

For what else to know while you're connected, see [CONNECT and
DISCONNECT](docs/querying.md#connect-and-disconnect).

## Combine Redshift results with local data

This example reads sales from an Apache Iceberg table in an Amazon S3 table bucket and customer data
from the auto-mounted catalog of a Redshift warehouse. Amazon Redshift joins the two tables and
calculates totals by state. DuckDB joins those results with a local CSV file, calculates totals by
region, and writes them to Parquet.

The following SQL statement combines Redshift results with local data. Replace the catalog and table
names with your own.

```sql
COPY (
    SELECT r.region, sum(s.buyers) AS buyers, sum(s.net_paid) AS net_paid
    FROM redshift_query('rs', $$
        SELECT c.state, count(DISTINCT c.customer_id) AS buyers, sum(s.net_paid) AS net_paid
        FROM "amzn-s3-demo-table-bucket@s3tablescatalog".sales.store_sales s
        JOIN "sales_db@my-warehouse".public.customers c ON c.customer_id = s.customer_id
        WHERE s.sale_date BETWEEN '2022-11-24' AND '2022-11-28'
        GROUP BY c.state $$) s
    JOIN read_csv('regions.csv') r USING (state)
    GROUP BY r.region
    ORDER BY net_paid DESC
) TO 'sales_by_region.parquet' (FORMAT parquet);
```

`regions.csv` should contain `state` and `region` columns, with one row for each state. The SQL
inside `redshift_query` runs on Amazon Redshift, so only one row for each state crosses the network.
DuckDB performs the join with the CSV file, calculates the regional totals, and writes the output.
[Running queries](docs/querying.md#three-part-names-and-auto-mounted-catalogs) explains the
three-part table names and auto-mounted catalogs.

## AWS credentials and the DuckDB `aws` extension

The `redshift` extension opens the IAM-authenticated connection through its bundled driver, runs the
SQL passed to `redshift_query`, and converts the results to DuckDB types. Each `ATTACH` selects one
of three AWS credential sources: the default credential chain, a named profile, or a DuckDB secret.
Only the DuckDB-secret option requires the separate DuckDB `aws` extension.

### Default credential chain

Use AWS credentials available through environment variables, files under `~/.aws`, or an instance or
container role:

```sql
ATTACH '' AS rs_chain (TYPE redshift, WORKGROUP 'my-workgroup', ACCOUNT_ID '111122223333',
    REGION 'us-east-1', CREDENTIAL_PROVIDER aws_default_chain);
```

### Named profile

Select a profile from `~/.aws/config` or `~/.aws/credentials`:

```sql
ATTACH '' AS rs_profile (TYPE redshift, WORKGROUP 'my-workgroup', ACCOUNT_ID '111122223333',
    REGION 'us-east-1', CREDENTIAL_PROVIDER profile, PROFILE 'my-profile');
```

### DuckDB secret

Create a secret with the DuckDB `aws` extension and name it in the `ATTACH` statement. `aws` is a
core DuckDB extension, which DuckDB installs and loads itself when `CREATE SECRET` needs it:

```sql
CREATE SECRET my_secret (TYPE aws, PROVIDER credential_chain, REGION 'us-east-1');
ATTACH '' AS rs_secret (TYPE redshift, WORKGROUP 'my-workgroup', ACCOUNT_ID '111122223333',
    REGION 'us-east-1', SECRET my_secret);
```

`CREATE SECRET` can come before or after `LOAD redshift FROM aws.amazon.com`. For more about
credential sources, the IAM actions that each target requires, and the resulting database user, see
[Authentication and IAM permissions](docs/authentication.md).

## Documentation

The following table lists the documentation pages and what each one covers.

| Page | What it covers |
|---|---|
| [Installing the extension](docs/install.md) | Installing from the AWS DuckDB extension repository, loading, versions and new releases, building from source |
| [Authentication and IAM permissions](docs/authentication.md) | Where the AWS credentials come from (`aws_default_chain`, a profile, a DuckDB secret), the IAM actions for each target, the database user |
| [Attaching to Redshift](docs/attaching.md) | The ways to name a workgroup or cluster, the handle, sessions and `DETACH`, network and regions |
| [ATTACH options](docs/attach-options.md) | Every option, the path forms and how options combine |
| [Running queries](docs/querying.md) | `redshift_query`, parameters, row counts, `CONNECT` and `DISCONNECT`, auto-mounted catalogs, transactions, large results |
| [Data types](docs/data-types.md) | How each Redshift type lands in DuckDB, and the differences to know about |
| [Examples](docs/examples.md) | Worked examples: local joins, parameters, copies, Parquet, two targets, writes, session state, `CONNECT` |
| [Security considerations](docs/security.md) | The trust model, TLS, credentials in messages, files on disk, environment variables |
| [Limitations](docs/limitations.md) | What the extension does not do, such as sign-ins other than IAM, cancellation and catalog browsing |
| [Troubleshooting](docs/troubleshooting.md) | How errors read, what to do for each common one, logging |
| [Settings and environment variables](docs/settings.md) | The extension's setting, the DuckDB settings that matter, the environment variables it reads |
| [Platforms and the embedded driver](docs/platforms.md) | Supported platforms and host prerequisites, where the driver is written, Windows |

## Platforms

Builds are available for `linux_amd64`, `linux_arm64`, `osx_arm64`, `osx_amd64`, and
`windows_amd64`, for DuckDB v2.0.1. [Platforms and the embedded driver](docs/platforms.md) lists
what each platform's hosts need.

## Versions

Releases are numbered `vX.Y.Z` and follow [Semantic
Versioning](https://semver.org/spec/v2.0.0.html). The AWS DuckDB extension repository serves each
release under its version, and [CHANGELOG.md](CHANGELOG.md) lists the changes in each one.

## Security

To report a security issue, see [SECURITY.md](SECURITY.md). For how to run the extension safely, see
[Security considerations](docs/security.md).

## License

This project is licensed under the Apache License 2.0; see [LICENSE](LICENSE) and [NOTICE](NOTICE).
[THIRD_PARTY_LICENSES](THIRD_PARTY_LICENSES) lists the third-party software this repository includes
and its binary builds contain, among them DuckDB (MIT) and the Amazon Redshift ODBC driver (Apache
2.0) with the software the driver includes, whose own notices are in
[third_party/redshift_odbc/](third_party/redshift_odbc/).

## Contributing

See [CONTRIBUTING.md](CONTRIBUTING.md). For how to build and test the extension, see [Building from
source](docs/install.md#building-from-source). [UPDATING.md](docs/UPDATING.md) is the maintainers'
checklist for moving to a new DuckDB release.
