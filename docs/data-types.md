# Data types

## Type mapping

Each column of a `redshift_query` result lands as the DuckDB type below. A value that does not fit
its DuckDB type fails the query rather than being rounded or clamped.

| Redshift type | DuckDB type | Notes |
|---|---|---|
| `SMALLINT` | `SMALLINT` | |
| `INTEGER` | `INTEGER` | |
| `BIGINT` | `BIGINT` | |
| `BOOLEAN` | `BOOLEAN` | |
| `REAL` (`FLOAT4`) | `FLOAT` | Sent with 8 significant digits; see [Floating-point values](#floating-point-values) |
| `DOUBLE PRECISION` (`FLOAT8`) | `DOUBLE` | Exact |
| `NUMERIC(p,s)`, `DECIMAL(p,s)` | `DECIMAL(p,s)` | Precision up to 38 |
| `CHAR(n)` | `VARCHAR` | Without its trailing blanks; see [CHAR values](#char-values) |
| `VARCHAR(n)` | `VARCHAR` | Trailing blanks kept |
| `DATE` | `DATE` | |
| `TIME` | `TIME` | |
| `TIMETZ` | `TIME WITH TIME ZONE` | See [Timestamps and times](#timestamps-and-times) |
| `TIMESTAMP` | `TIMESTAMP` | Within DuckDB's range; see [Timestamps and times](#timestamps-and-times) |
| `TIMESTAMPTZ` | `TIMESTAMP WITH TIME ZONE` | An exact instant, within DuckDB's range; see [Timestamps and times](#timestamps-and-times) |
| `VARBYTE` | `BLOB` | |
| `SUPER` | `VARCHAR` | Its JSON text |
| An unqualified interval computed in the query | `VARCHAR` | Text such as `1 day 02:03:04`; see [Intervals](#intervals) |

Any other type is refused before the statement runs, with an error that names the column.
Qualified `INTERVAL YEAR TO MONTH` and `INTERVAL DAY TO SECOND` values are among them, whether
stored in a column or written in the query, such as `INTERVAL '1-2' YEAR TO MONTH`, and so are
`GEOMETRY`, `GEOGRAPHY` and `HLLSKETCH`. Convert such a value in the Redshift SQL, for example with
`ST_AsText(geom)` for a geometry or `col::VARCHAR` for an interval. The system catalog types
`name`, `"char"` and `oid` are not refused: `name` and `"char"` arrive as `VARCHAR`, and `oid` as
`INTEGER`, so an `oid` above 2147483647 fails.

## The session

Every new connection sets the session's time zone to UTC, its date style to `ISO, MDY`, its interval
style to `postgres` and its float output to the most digits Redshift gives, overriding a workgroup's
or user's defaults, so that values arrive in one form. A `SET` or `RESET` of one of them through
`redshift_query` changes how values arrive for the rest of that session, until the next connection
sets them again, so leave them as they are.

## Results use Redshift's types

`redshift_query` returns the types Redshift gives its result, which are not always the ones DuckDB
would give the same expression. For a `NUMERIC(10,2)` column `col`:

| Expression | Type from Redshift | DuckDB's type for it |
|---|---|---|
| Aggregate `avg(col)` | `DECIMAL(38,2)` | `DOUBLE` |
| Window `avg(col) OVER (...)` | `DECIMAL(10,2)`, the column's | `DOUBLE` |
| `col / 3` | `DECIMAL(12,4)` | `DOUBLE` |

All three are truncated: the average of 10, 20 and 20 arrives as `16.66`. A bare `::numeric` is
`NUMERIC(18,0)` in Redshift and `DECIMAL(18,3)` in DuckDB. Cast in the Redshift SQL to choose the
type.

- A window `SUM` over a `DECIMAL` column, `SUM(col) OVER (...)`, fails when a total needs more
  digits than the column's precision: Redshift declares the result with the column's own precision
  and scale. Cast the column, or the result, to a wider `DECIMAL` in the Redshift SQL, such as
  `SUM(col::DECIMAL(38,2)) OVER (...)`.
- A table Redshift creates with `CREATE TABLE AS` can hold a wider `DECIMAL` than the query that
  made it reported, so reading the table back gives the wider type. Declare the table's columns
  instead of deriving them to avoid it.
- A result whose column names repeat, such as `SELECT a.id, b.id`, or with several unnamed
  expressions, which Redshift names `?column?`, lands with DuckDB's suffixes: `id` and `id_1`. Name
  each column with `AS` in the Redshift SQL to choose the names.

## Floating-point values

`REAL` values are sent with 8 significant digits, which identify most but not all 32-bit values: a
`REAL` whose first significant digit is 1 can arrive as the neighbouring `FLOAT`, a relative
difference under 1.2 × 10⁻⁷. That happens to about 5% of the values whose first digit is 1, and
about 1.5% of all `REAL` values. `DOUBLE PRECISION` values arrive exactly. To read a `REAL` exactly,
cast it in the Redshift SQL, `col::DOUBLE PRECISION`; a cast to `VARCHAR` prints the same 8 digits.
A finite value whose printed form overflows fails the query rather than arriving as infinity.

## Timestamps and times

- `TIMESTAMPTZ` values are exact instants. How DuckDB displays them, and computes with them, depends
  on its `TimeZone` setting and on whether its ICU extension is loaded.
- `TIMETZ` values read from a table arrive with offset `+00`, while a constant can keep its offset,
  and DuckDB's equality and `DISTINCT` compare the offset.
- Redshift stores the `TIME` `24:00:00` as `00:00:00`, and rounds fractions beyond 6 digits on
  input, so such values arrive as stored.
- `TIMESTAMP` and `TIMESTAMPTZ` values after `294247-01-10 04:00:54.775806`, the end of DuckDB's
  range, up to Redshift's `294276-12-31`, and a stored `TIMESTAMP` `'infinity'`, fail the query. For
  a write statement, the failure comes after the statement has run: check its effects before you
  run it again.

## Intervals

An interval computed in the query, by subtracting timestamps or times, from an unqualified
`INTERVAL` literal such as `INTERVAL '1 day'`, or from a view built on them, arrives as `VARCHAR` text such as `1 day 02:03:04`, because the
Redshift driver reports it as text. Convert it in DuckDB with `::INTERVAL`, or compute a number in
Redshift, such as `DATEDIFF(second, started, ended)`:

```sql
SELECT elapsed::INTERVAL AS elapsed
FROM redshift_query('rs', $$SELECT '2024-01-02 12:00:00'::timestamp - '2024-01-01 09:58:56'::timestamp AS elapsed$$);
```

Do not change the session's `intervalstyle`, which changes the text.

## CHAR values

A `CHAR` value arrives without its trailing blanks, so DuckDB compares, joins and measures it with
`length` as Redshift does. A trailing tab is kept, as Redshift's own comparisons and `LENGTH` keep
it, although Redshift's cast to `VARCHAR` drops it. Some differences remain, because DuckDB sees the
value without the padding Redshift keeps:

- Pattern matching (`LIKE`, `ILIKE`, `SIMILAR TO` and regular expressions) anchored at the end
  matches in DuckDB where Redshift does not: a `CHAR(25)` holding `'GERMANY'` matches `LIKE '%Y'` in
  DuckDB but not in Redshift.
- Redshift's `octet_length` counts the padding (25 for that value); in DuckDB the stripped value has
  7 bytes.
- A comparison with a literal that has trailing blanks, such as `= 'GERMANY  '`, matches in
  Redshift, which ignores the blanks, but not in DuckDB.

Run such an expression in the Redshift SQL instead. `VARCHAR` values keep their trailing blanks, and
a case-insensitive collation on the Redshift side does not carry over to DuckDB.
