# Limitations

- **IAM only.** No database user or password, no driver sign-in plugins and no Kerberos.
- **No NULL parameters.** Write `NULL` in the SQL instead. Write no `?` in the SQL text
  ([Parameters](querying.md#parameters)).
- **No cancellation and no statement timeout.** After Ctrl-C a statement keeps running on Redshift
  until it completes ([Cancelling a statement](querying.md#cancelling-a-statement)).
- **Stopping early.** After a satisfied `LIMIT`, or Ctrl-C, a query can take as long to end as
  Redshift takes to send the rest of the result. While a reachable server stops sending in the
  middle of a result, closing or interrupting the query waits until it resumes; a server that stops
  responding is detected within about 90 seconds (about 2 minutes on Windows).
- **No catalog browsing.** The handle's tables are not visible as a DuckDB catalog, and writes
  through it are refused; use `redshift_query`.
- **One call per handle per DuckDB statement**, one statement per call and one result set per
  statement, and no call in the recursive part of a recursive CTE, as the source of a `PIVOT`
  without an `IN` list, or in the subquery of a `PIVOT`'s `IN (subquery)`.
- **No DuckDB transactions** around `redshift_query`, unless
  `redshift_allow_query_in_duckdb_transaction` is on ([Transactions](querying.md#transactions)).
- **Endpoints only.** No custom domain, VPC endpoint host name, IP address, proxy or tunnel can be
  given as the target.
- **Regions.** Only `aws`-partition regions; a region launched after a release needs a newer
  release.
- **Linux allocators.** Allocators that replace `malloc` are not supported: in a process where
  one does (jemalloc or tcmalloc in `LD_PRELOAD`, or a program that defines `malloc`), connecting
  fails with an error that names the allocator. Start the program without it.
- **Values.** Some values arrive differently from how Redshift shows them, such as `REAL`, computed
  intervals and `CHAR`; [Data types](data-types.md) lists each with its workaround.
- **Platforms.** [Platforms and the embedded driver](platforms.md) lists each platform's limits,
  among them those on Windows.
