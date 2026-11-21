# Installing the extension

## Installing

Signed builds of the extension are published to the AWS DuckDB extension repository. Register the
repository once, by its host name, then install from it:

```sql
CREATE EXTENSION REPOSITORY aws.amazon.com;
INSTALL redshift FROM aws.amazon.com;
```

Registering it is one-time setup for each user on a machine.

When you register it, DuckDB reads the repository's address from `aws.amazon.com`, then the signing
keys the repository publishes, both over HTTPS. It keeps the keys with the repository for your
user, and checks the signature of every extension installed from it against them, so registering
trusts the keys the repository serves at that time. To see the fingerprints of the keys DuckDB
pinned:

```sql
SELECT repository_name, key_fingerprints FROM duckdb_extension_repositories();
```

Registering the repository, and installing, updating and loading from it, need no setting: DuckDB's
default for `allow_extension_repositories`, `'hosts'`, allows a repository registered by its host
name. `'forbidden'` stops all four.

## Loading

Load the extension in each session before you attach a target:

```sql
LOAD redshift FROM aws.amazon.com;
```

An `ATTACH` of a Serverless workgroup's long-form ARN ([Attaching to Redshift](attaching.md)) needs no
`LOAD`: DuckDB's `aws` extension loads `redshift` from `aws.amazon.com` for it. Every other form
needs the `LOAD` first.

A plain `LOAD redshift` does not find an extension installed from an added repository: the `LOAD`
must name the repository.

Do not set `allow_unsigned_extensions`. DuckDB checks every build from the AWS DuckDB extension
repository against the repository's key without it, and the fix for a bad release is a newer release
(see [Taking a new release](#taking-a-new-release)).

Each binary contains the official Amazon Redshift ODBC driver, which the extension writes to disk
and loads on the first connect. [Platforms and the embedded driver](platforms.md) lists what each
platform's hosts need and where the driver is written.

## Versions

Releases are numbered `vX.Y.Z` and follow [Semantic
Versioning](https://semver.org/spec/v2.0.0.html). [CHANGELOG.md](../CHANGELOG.md) lists the changes
in each release.

This release is built for DuckDB v2.0.1. An extension binary loads
only in the DuckDB version it was built for, because DuckDB's extension interface is not stable
between versions, and the address `INSTALL` downloads from names that exact version. After you
upgrade DuckDB, install the extension again from the new version.

The repository serves each release under its version, `vX.Y.Z`, which is also the version DuckDB
shows for the loaded extension:

```sql
LOAD redshift FROM aws.amazon.com;
SELECT extension_version FROM duckdb_extensions() WHERE extension_name = 'redshift';
```

## Taking a new release

Update the extension, then restart DuckDB: a loaded extension, and the driver it loaded, stay loaded
until the process ends.

```sql
UPDATE EXTENSIONS;
```

`UPDATE EXTENSIONS` updates every installed extension, this one from the repository it was
installed from. Give it no list: `UPDATE EXTENSIONS (redshift)` looks only at extensions that were
not installed from a named repository. To take a new release of this extension alone, install it
again instead:

```sql
FORCE INSTALL redshift FROM aws.amazon.com;
```

There is no route back to an earlier release from a file: DuckDB checks a binary installed from a
path against its own keys only, so a build the AWS DuckDB extension repository signed does not load
that way.

## Building from source

To build from a checkout of this repository, you need git, make, curl, CMake 3.19 or later (3.21
or later to build `windows_amd64`), a C++17 compiler and Python 3. The build compiles against
DuckDB at the commit `duckdb.commit` pins, which `make duckdb` clones into `duckdb/`. In the
checkout:

```
make duckdb
make release REDSHIFT_CLIENT=odbc   # the extension, with the embedded Amazon Redshift ODBC driver
make test REDSHIFT_CLIENT=odbc      # an offline check; test/README.md has the other suites
```

The build writes the extension to `build/release/extension/redshift/redshift.duckdb_extension`, and
a DuckDB shell with the extension built in to `build/release/duckdb`. It downloads the driver
release that `third_party/redshift_odbc/driver.lock` names and checks it before embedding it;
[third_party/redshift_odbc/README.md](../third_party/redshift_odbc/README.md) describes the lock and
the checks. A plain `make` builds the extension with an in-memory stand-in for Redshift instead,
which the tests that need no driver run against. [CONTRIBUTING.md](../CONTRIBUTING.md) has the other
build, test and format commands, and [test/README.md](../test/README.md) describes every test suite.

A build you make yourself is not signed. Try it in the shell the build writes,
`build/release/duckdb`, which has the extension built in; to load the extension file itself, start
that shell with `-unsigned`. Use the signed releases elsewhere.

The one Makefile the build takes from DuckDB's extension CI tools is vendored in
`third_party/extension-ci-tools/`, whose README records where it came from.
[UPDATING.md](UPDATING.md) says how to move to a new DuckDB version.
