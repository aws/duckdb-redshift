# DuckDB extension CI tools: the extension Makefile

`duckdb_extension.Makefile` is `makefiles/duckdb_extension.Makefile` from DuckDB's extension CI
tools, the one file of that repository the build uses: the root `Makefile` includes it. Its first
line names where it came from; the rest is the upstream file, unmodified.

- Repository: https://github.com/duckdb/extension-ci-tools
- Commit: `e38c2e18085bb960555ed74f1d9695de8e67b108`, on the `v2.0-cyanoptera` branch (DuckDB v2.0)
- Licence: none stated. The repository has no licence file, at this commit or on its `main`
  branch, and neither its README nor the Makefile itself names one. DuckDB and its extension
  template, whose Makefile includes this file, are released under the MIT licence
  (`third_party/duckdb-extension-template/LICENSE`).

SHA-256 of the upstream file, which is this directory's `duckdb_extension.Makefile` from its second
line on:

```
2fa13c6a78e75dff2f4d3119f63e4d6923f946fd78e080135acaf353d8c8e7b5  makefiles/duckdb_extension.Makefile
```

To check: `tail -n +2 duckdb_extension.Makefile | sha256sum` in this directory prints that hash, and
so does `sha256sum makefiles/duckdb_extension.Makefile` in a clone of the repository at the commit.
`make test_clone_duckdb` checks the first. `docs/UPDATING.md` says how to move the pin.
