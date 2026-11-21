# Changelog

The changes in each release of `redshift`, the Amazon Redshift extension for DuckDB, newest
first. The format is based on [Keep a Changelog](https://keepachangelog.com/en/1.1.0/), and versions
follow [Semantic Versioning](https://semver.org/spec/v2.0.0.html). Each release lists its breaking
changes, features and fixes.

## v1.0.0 - 2026-11-21

### Features

- First release of `redshift`, the Amazon Redshift extension for DuckDB: run Amazon Redshift SQL from a DuckDB session. The extension sends your SQL statements to Amazon Redshift as written, where they run with Amazon Redshift SQL syntax and rules. DuckDB receives the results. You can then filter or aggregate them, join them with local data, write them to Parquet, or store them in a DuckDB table. Use it when a query on data that Amazon Redshift can read outgrows the machine that runs DuckDB. Amazon Redshift runs the query on its own compute, and the result comes back to your DuckDB session to join with your local files.
- Attach Amazon Redshift Serverless workgroups and provisioned clusters with `ATTACH ... (TYPE redshift)`, by the endpoint the Redshift console shows (`redshift://<endpoint>`, with an optional port and database) or by options (`WORKGROUP` and `ACCOUNT_ID`, or `CLUSTER`, each with `REGION`). `ATTACH` statements written for DuckDB's `aws` extension are also accepted, and with that extension loaded the namespace and workgroup ARNs the AWS console shows attach without `TYPE`.
- Sign in with IAM: each `ATTACH` takes AWS credentials from the default credential chain (`CREDENTIAL_PROVIDER aws_default_chain`), a named profile (`CREDENTIAL_PROVIDER profile, PROFILE '<name>'`) or a DuckDB `aws` or `s3` secret (`SECRET <name>`), gets temporary database credentials for that identity from Redshift (`redshift-serverless:GetCredentials` or `redshift:GetClusterCredentialsWithIAM`), and verifies the server's TLS certificate.
- Run Redshift SQL with `redshift_query('<handle>', '<sql>')`, which returns the result as a DuckDB table and binds parameter values given in `params := [...]`, or `CONNECT` to a handle to send each statement to Redshift as written until `DISCONNECT`.
- Results arrive as DuckDB types: integers, `DECIMAL` up to precision 38, floating point, `BOOLEAN`, `VARCHAR`, `DATE`, `TIME`, `TIMESTAMP` and their time-zone forms, `VARBYTE` as `BLOB` and `SUPER` as its JSON text; a value that does not fit its DuckDB type fails the query rather than being rounded, and other types, such as `GEOMETRY`, are refused with an error that names the column.
- Each build embeds the official Amazon Redshift ODBC driver, version 2.3.1.0, so no separate driver, driver manager or ODBC data source is needed.
- Builds for DuckDB v2.0.1 on `linux_amd64`, `linux_arm64`, `osx_arm64`, `osx_amd64` and `windows_amd64` (tested on Windows Server 2022; Windows 10 and 11 are not supported), from the AWS DuckDB extension repository: register it once with `CREATE EXTENSION REPOSITORY aws.amazon.com`, install with `INSTALL redshift FROM aws.amazon.com`, then load it in each session with `LOAD redshift FROM aws.amazon.com`.
