# unixODBC headers

`sql.h`, `sqlext.h`, `sqltypes.h`, `sqlucode.h` and `COPYING` are copied unmodified from
unixODBC 2.3.12. `unixodbc.h` is not vendored; the driver interface header defines the one size
it would supply. The driver's own files live in `third_party/redshift_odbc/`, not here.

- Source release: https://www.unixodbc.org/unixODBC-2.3.12.tar.gz
- Version: 2.3.12
- Tarball SHA-256: `f210501445ce21bf607ba51ef8c125e10e22dffdffec377646462df5f01915ec`
- Files: the four headers from the release's `include/`, and `COPYING` from its top level,
  unmodified.

SHA-256 of each file:

```
277553e376e776bce7b59f88db7384072f93828ee9233a8910c04cb47a026862  sql.h
f98157841290dbada1cdfc5f067261c92e21afa53ee4b59c736b7fcafc91f2cc  sqlext.h
fb09191fb4f109f234f0b3ed5c5754283d010cffba5c9648bfac9d9517656762  sqltypes.h
ca0a55f11da964dce09e22e89e690fe0d89fe7dbe6739e6c2976eefce5efe128  sqlucode.h
a6b9aa1e9bfade121164c06fd975e9b913a8e38bf33d25ff967dfa3ed07d12f8  COPYING
```

To check: unpack the release and `cmp` each file, or run `sha256sum -c` on the list above in
this directory.
