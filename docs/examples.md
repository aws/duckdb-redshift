# Examples

The examples attach a Serverless workgroup as `rs`, with the credentials of the AWS default chain,
and read tables named `venue` and `sales`. Replace the target and the table names with your own:

```sql
LOAD redshift FROM aws.amazon.com;
ATTACH '' AS rs (TYPE redshift, WORKGROUP 'my-workgroup', ACCOUNT_ID '111122223333',
    REGION 'us-east-1', CREDENTIAL_PROVIDER aws_default_chain);
```

## Join a Redshift aggregate with local data

Let Redshift reduce the data, then join the small result with a local DuckDB table:

```sql
CREATE TEMP TABLE targets AS
    SELECT * FROM (VALUES (1, 100), (2, 50)) t(venueid, target);

SELECT r.venueid, r.tickets, t.target
FROM redshift_query('rs', 'SELECT venueid, sum(qtysold) AS tickets FROM sales GROUP BY venueid') r
JOIN targets t USING (venueid)
ORDER BY r.venueid;
```

## Bind parameters

```sql
SELECT * FROM redshift_query('rs',
    'SELECT salesid, qtysold FROM sales WHERE venueid = $1::int AND qtysold >= $2::int ORDER BY salesid',
    params := ['1', '2']);
```

## Keep a local copy, or write Parquet

```sql
CREATE TABLE venue_copy AS SELECT * FROM redshift_query('rs', 'SELECT * FROM venue');

COPY (SELECT * FROM redshift_query('rs', 'SELECT * FROM sales')) TO 'sales.parquet' (FORMAT parquet);
```

## Combine two targets

Attach each target as a handle of its own, then join their results in DuckDB:

```sql
ATTACH '' AS rs_cluster (TYPE redshift, CLUSTER 'my-cluster', REGION 'us-east-1',
    CREDENTIAL_PROVIDER aws_default_chain);

SELECT v.venuename, s.tickets
FROM redshift_query('rs', 'SELECT venueid, venuename FROM venue') v
JOIN redshift_query('rs_cluster', 'SELECT venueid, sum(qtysold) AS tickets FROM sales GROUP BY venueid') s
    USING (venueid)
ORDER BY v.venuename;
```

A DuckDB statement can use a handle in only one `redshift_query` call
([One statement per handle at a time](querying.md#one-statement-per-handle-at-a-time)). To read one
target twice, stage one result in a DuckDB temporary table first:

```sql
CREATE TEMP TABLE busy_venues AS
    SELECT * FROM redshift_query('rs', 'SELECT venueid FROM sales GROUP BY venueid HAVING sum(qtysold) > 2');

SELECT v.venuename
FROM redshift_query('rs', 'SELECT venueid, venuename FROM venue') v
JOIN busy_venues b USING (venueid);
```

## Write to Redshift

DML and DDL run through `redshift_query`. A statement with no result set returns a `rowcount`:

```sql
SELECT * FROM redshift_query('rs', 'CREATE TEMP TABLE audit (salesid BIGINT, note VARCHAR(256))');
SELECT * FROM redshift_query('rs', $$INSERT INTO audit SELECT salesid, 'checked' FROM sales$$);
```

A write runs on Redshift as it is: a failed one may still have taken effect, so check the target
before you run it again. To load many rows from DuckDB into Redshift, use multi-row `INSERT`
statements, each within Redshift's statement size limit, or stage the data in Amazon S3 and run
Redshift's `COPY`.

## Session state

The handle keeps one Redshift session per DuckDB connection, so `SET` values and temporary tables
last between calls:

```sql
SELECT * FROM redshift_query('rs', $$SET query_group TO 'nightly_report'$$);
SELECT * FROM redshift_query('rs', 'CREATE TEMP TABLE recent AS SELECT * FROM sales WHERE salesid > 1');
SELECT * FROM redshift_query('rs', 'SELECT count(*) AS n FROM recent');
```

Another DuckDB connection gets a session of its own and does not see them. Whether a client API's
new cursor is a DuckDB connection of its own depends on the API.

## Type Redshift SQL with CONNECT

`CONNECT` sends each statement to Redshift as written, until `DISCONNECT`. The statements run in the
same Redshift session as `redshift_query` on this DuckDB connection, so a temporary table created
while CONNECT-ed is still there afterwards:

```sql
CONNECT rs;
CREATE TEMP TABLE top_venues AS
    SELECT venueid, sum(qtysold) AS tickets FROM sales GROUP BY venueid ORDER BY tickets DESC LIMIT 10;
SELECT * FROM top_venues;
DISCONNECT;

SELECT v.venueid, v.tickets, t.target
FROM redshift_query('rs', 'SELECT venueid, tickets FROM top_venues') v
JOIN targets t USING (venueid);
```

`DISCONNECT` always returns to DuckDB; the join with `targets`, the local table of the first
example, runs in DuckDB. [CONNECT and DISCONNECT](querying.md#connect-and-disconnect) lists what
goes to Redshift while CONNECT-ed and what is not supported.
