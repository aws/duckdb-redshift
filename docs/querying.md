# Running queries

## redshift_query

```sql
redshift_query(handle, sql [, params := [...]])
```

`redshift_query` runs one Redshift SQL statement on the handle an `ATTACH` created and returns its
result as a DuckDB table. The extension passes the SQL on unchanged, so it is Redshift SQL, not
DuckDB SQL; the embedded driver can rewrite a `?` in it ([Parameters](#parameters)):

```sql
SELECT * FROM redshift_query('rs', 'SELECT venueid, venuename FROM venue ORDER BY venueid LIMIT 10');
```

The result can be used like any table: joined with local data, filtered, or stored.

```sql
CREATE TABLE venues AS SELECT * FROM redshift_query('rs', 'SELECT * FROM venue');
COPY (SELECT * FROM redshift_query('rs', 'SELECT * FROM sales')) TO 'sales.parquet' (FORMAT parquet);
```

DuckDB cannot change the SQL text, so a filter, join or aggregate written in DuckDB around
`redshift_query` runs locally, on every row Redshift returns. Write it in the Redshift SQL to have
Redshift do it.

### Quoting

The SQL is a DuckDB string, so its single quotes are doubled. A dollar-quoted string avoids that
(but write no `?` in it; see [Parameters](#parameters)):

```sql
SELECT * FROM redshift_query('rs', $$SELECT * FROM users WHERE city = 'Seattle'$$);
```

### Parameters

`params` binds a list of strings to the statement's markers, `$1`, `$2` and so on, in order. The
values are sent separately from the SQL text, never substituted into it. Each is a string; cast it
in the Redshift SQL where another type is needed:

```sql
SELECT * FROM redshift_query('rs', 'SELECT * FROM users WHERE city = $1 AND userid > $2::int',
    params := ['Seattle', '100']);
```

Write no `?` in the SQL text, with or without `params`: the embedded driver rewrites each `?` it
finds into a marker before Redshift sees the statement, inside a dollar-quoted string too, so
`SELECT $$colou?r$$` returns `colou$1r`. Write a literal question mark as `chr(63)`. Pass untrusted
values in `params`, never in the SQL text.

NULL parameters are not supported: write `NULL` in the SQL instead.

### Statements that return no rows

A statement with no result set, such as DML or DDL, returns one row with one `BIGINT` column,
`rowcount`: the number of rows the statement changed, 0 for DDL, or NULL when Redshift reports none.
A query whose result has no rows returns an empty table with its columns instead.

```sql
SELECT * FROM redshift_query('rs', 'CREATE TEMP TABLE scores (name VARCHAR(20), score INT)');
SELECT * FROM redshift_query('rs', $$INSERT INTO scores VALUES ('a', 1), ('b', 2), ('c', 3)$$);
SELECT * FROM redshift_query('rs', 'UPDATE scores SET score = score + 10 WHERE score > 1');
```

The `INSERT` returns `rowcount` 3 and the `UPDATE` 2.

### One statement per handle at a time

Each handle has one Redshift session per DuckDB connection, which runs one statement at a time. So
one DuckDB statement can use a handle in only one `redshift_query` call; two calls on the same
handle in one statement are refused before anything runs. To combine two Redshift results, attach a
second handle, or store one result in a DuckDB temporary table first:

```sql
CREATE TEMP TABLE first_result AS SELECT * FROM redshift_query('rs', 'SELECT 1 AS id');
SELECT * FROM first_result, redshift_query('rs', 'SELECT 2 AS id');
```

For the same reason, `redshift_query` is refused in the recursive part of a recursive CTE, as the
source of a `PIVOT` without an `IN` list, and in the subquery of a `PIVOT`'s `IN (subquery)`. DuckDB
runs the recursive part more than once, and the other two before the `PIVOT`, as statements of
their own.

## CONNECT and DISCONNECT

`CONNECT` sends every following statement to Redshift as it is typed, until `DISCONNECT`:

```sql
CONNECT rs;
SELECT current_database(), current_user;
SHOW search_path;
DISCONNECT;
```

Each statement typed while CONNECT-ed runs as a `redshift_query` call on the handle, so everything
on this page applies to it. `CONNECT` is for plain SQL:

- While CONNECT-ed, only `DISCONNECT` is DuckDB's, and it always returns to DuckDB. Every other
  statement goes to Redshift as written, DuckDB's own too: `USE memory`, a query of
  `duckdb_logs()`, `BEGIN`, `COMMIT`, `ROLLBACK`, `SET`, `ATTACH`, `PREPARE` and a second
  `CONNECT`, and the transaction calls of DuckDB's client APIs.
- If the handle's connection cannot open, each statement while CONNECT-ed tries to open it again
  and fails, and the DuckDB CLI's default prompt, whose own query to Redshift fails too, shows
  `#ERROR`: run `DISCONNECT`, fix the cause, then `CONNECT rs` again.
- A credential typed while CONNECT-ed, in a `CREATE SECRET`, a `SET` or an `ATTACH` with a password,
  goes to Redshift as statement text, where Redshift may record it in its query history. Run
  `DISCONNECT` before typing one.
- A statement a client API prepared, before `CONNECT` or while CONNECT-ed, runs on Redshift as its
  text each time it is executed while CONNECT-ed. One with parameters is refused when it is executed
  while CONNECT-ed, so a client that binds parameters, such as a JDBC `PreparedStatement`, is not
  supported while CONNECT-ed; use `redshift_query` with `params` instead.
- `CONNECT` inside a DuckDB transaction is refused. Once a client turns autocommit off while
  CONNECT-ed, each statement is refused and the `CONNECT` stays, so none meant for Redshift runs in
  DuckDB: run `DISCONNECT`, then `COMMIT` or `ROLLBACK`, then `CONNECT` again with autocommit on.
  `redshift_allow_query_in_duckdb_transaction` does not change that. Only
  `CONNECT <handle>` is supported, not the string form `CONNECT '<string>'`.
- The DuckDB CLI's default prompt sends a query of its own to Redshift after each statement, which a
  prompt without `current_database_and_schema` avoids, and its dot-commands that run SQL, such as
  `.tables` and `.schema`, ask Redshift while CONNECT-ed.

## Databases, schemas and tables

The handle is not a browsable DuckDB catalog. List what Redshift holds through Redshift's own
commands and system views:

```sql
SELECT * FROM redshift_query('rs', 'SHOW DATABASES');
SELECT * FROM redshift_query('rs', 'SHOW SCHEMAS FROM DATABASE dev');
SELECT * FROM redshift_query('rs', 'SHOW TABLES FROM SCHEMA dev.public');
```

### Three-part names and auto-mounted catalogs

A query on one database can read another through Redshift's three-part names,
`"<database>"."<schema>"."<table>"`. Where the workgroup or cluster auto-mounts the AWS Glue Data
Catalog, `SHOW DATABASES` lists its catalogs as `auto mounted catalog`, and they are read the same
way:

| Catalog | Three-part name |
|---|---|
| The AWS Glue Data Catalog | `awsdatacatalog."<glue database>"."<table>"` |
| An Amazon S3 table bucket | `"<table bucket>@s3tablescatalog"."<namespace>"."<table>"` |
| A database of another Redshift warehouse registered in the Data Catalog | `"<database>@<catalog>"."<schema>"."<table>"` |

```sql
SELECT * FROM redshift_query('rs',
    'SELECT count(*) FROM "amzn-s3-demo-table-bucket@s3tablescatalog".my_namespace.my_table');
SELECT * FROM redshift_query('rs', 'SHOW TABLES FROM SCHEMA "amzn-s3-demo-table-bucket@s3tablescatalog".my_namespace');
```

Connect to a local database, such as `dev`, and use these names. `DATABASE` cannot name a catalog
that `SHOW DATABASES` lists with `allow_connections` false, such as `awsdatacatalog`: connecting
fails with SQLSTATE `55000`. A catalog `SHOW DATABASES` does not list fails with SQLSTATE `3D000`;
reading one needs the grants Redshift and AWS Lake Formation ask for, as Redshift's documentation
describes.

## Transactions

Redshift runs each statement in its own transaction (autocommit) unless you open one. The extension
never begins, commits or rolls back a transaction while the session is open; closing a session
([DETACH](attaching.md#sessions-and-detach)) rolls back one it left open.

### A Redshift transaction

Open and end it with statements through `redshift_query`, or typed while CONNECT-ed. It stays open
in the handle's session between queries:

```sql
SELECT * FROM redshift_query('rs', 'BEGIN');
SELECT * FROM redshift_query('rs', $$INSERT INTO scores VALUES ('d', 4)$$);
SELECT * FROM redshift_query('rs', 'COMMIT');
```

After an error inside a Redshift transaction, Redshift refuses every statement with SQLSTATE `25P02`
until the transaction ends, and the error says so. Run `ROLLBACK` to end it; the work done in it is
lost, and `COMMIT` would roll it back too:

```sql
SELECT * FROM redshift_query('rs', 'ROLLBACK');
```

A lost connection, or the error
`Redshift session error on attach handle "rs": the session cannot be used any more`, ends the
session instead and rolls back its open transaction, so there is no `25P02`: the statements after it
run in a new session, each committing on its own
([Troubleshooting](troubleshooting.md#running-statements)).

### DuckDB transactions

DuckDB's `COMMIT` and `ROLLBACK` do not control Redshift, so `redshift_query` is refused inside an
explicit DuckDB transaction, one that `BEGIN` opened or that a client opened by turning autocommit
off. The error says what state it left the DuckDB transaction in. Run `redshift_query` outside the
transaction.

To run it inside one anyway, turn on `redshift_allow_query_in_duckdb_transaction`. A DuckDB
`ROLLBACK` then does not undo what the Redshift statements did:

```sql
SET redshift_allow_query_in_duckdb_transaction = true;
BEGIN;
SELECT * FROM redshift_query('rs', 'SELECT 1');
COMMIT;
```

Even with the setting, a call is refused on a handle the open DuckDB transaction attached, and in a
DuckDB transaction an earlier error aborted.

## Cancelling a statement

A statement cannot be cancelled from DuckDB, and the extension sets no statement timeout: after
Ctrl-C, the statement keeps running on Redshift until it completes. The DuckDB connection running it
waits for it, so cancel it from somewhere else: another DuckDB process or connection with a handle
of its own, or any Redshift client, with the process ID that `pg_backend_pid()` gave on the same
handle before the long statement:

```sql
-- Before the long statement, on the handle that will run it:
SELECT * FROM redshift_query('rs', 'SELECT pg_backend_pid()');
-- From another DuckDB connection or process:
SELECT * FROM redshift_query('rs_other', 'SELECT pg_cancel_backend(<pid>)');
```

Redshift's own `statement_timeout`, set with `SET` through `redshift_query`, applies to the rest of
that session.

## Large results

- Rows stream from Redshift in batches while DuckDB processes them, fetched up to four batches
  ahead, with fetch buffers of about 16 MiB. DuckDB holds only what the query keeps.
- Do the filtering and aggregation in the Redshift SQL, so that only the rows you need cross the
  network.
- To keep a large result, write it once: `CREATE TABLE ... AS SELECT * FROM redshift_query(...)`, or
  `COPY (SELECT * FROM redshift_query(...)) TO 'file.parquet'`.
- A call without `params` whose result has no `REAL`, `DOUBLE PRECISION` or interval columns is
  prepared once and read on a faster path.
- One call reads its result on one connection. To load a large table faster, split it into disjoint
  ranges of a key and read each range with its own `redshift_query`, each on its own DuckDB
  connection running at the same time, into one table you create first:

  ```sql
  -- On each DuckDB connection, with its own range:
  INSERT INTO sales_copy
      SELECT * FROM redshift_query('rs', 'SELECT * FROM sales WHERE salesid >= 0 AND salesid < 50000000');
  ```

  This needs a key to split on. Each slice pays the per-query overhead and reads its own snapshot of
  the table. `ORDER BY` and `LIMIT` do not split. Use no more slices than the workgroup or cluster
  can run at once. Do not combine the slices with `UNION ALL` in one statement, even on separate
  handles: DuckDB runs its branches one after another.
