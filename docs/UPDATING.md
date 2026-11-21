# Moving to a new DuckDB release

This is the maintainers' checklist for moving the extension to a new DuckDB release.

- Move the pins
  - `duckdb.commit` should hold the commit of the latest tagged release (`git rev-parse v1.1.0^{commit}` in a DuckDB clone, for DuckDB `v1.1.0`). Then remove `duckdb/`, or point your link at a tree at the new commit, and run `make duckdb`. Then update the DuckDB version in `README.md`, `docs/install.md` and `docs/platforms.md`, and on the new DuckDB re-check the install statements in `README.md` and `docs/install.md` and the notes on `LOAD ... FROM` and `UPDATE EXTENSIONS` in `docs/install.md`.
  - `third_party/extension-ci-tools/duckdb_extension.Makefile` should be `makefiles/duckdb_extension.Makefile` from the `extension-ci-tools` branch of the new DuckDB release's line, unmodified below its first line, at the commit you record: the branches are named per release line, as `v2.0-cyanoptera` is for DuckDB v2.0. Update the commit in its first line and the commit and SHA-256 in `third_party/extension-ci-tools/README.md`; `make test_clone_duckdb` checks that the first line, the README and the file agree.
  - `.clang-format` and `.clang-tidy` are DuckDB's, copied unchanged at the pinned commit, because clang-format and clang-tidy look for them only in the directories above each source file. Copy `duckdb/.clang-format` and `duckdb/.clang-tidy` over them; `make test_clone_duckdb`, with `duckdb/` at the new pin, checks that the copies equal them byte for byte.
- Refresh the licence texts
  - `THIRD_PARTY_LICENSES` carries DuckDB's licence and those of the libraries DuckDB bundles that the extension contains, each unchanged from DuckDB's source. Copy `duckdb/LICENSE`, and each file of DuckDB's that an entry names, over its copy there.
  - Then compare its list of those libraries with a release build: `nm -C build/release/extension/redshift/redshift.duckdb_extension` shows each one the build contains by its namespace or prefix (`duckdb_re2::`, `duckdb_je_`, `mbedtls_`). Add an entry for a library that appears and remove the entry of one that is gone.

# API changes
DuckDB extensions built with this extension template are built against the internal C++ API of DuckDB. This API is not guaranteed to be stable.
What this means for extension development is that when updating your extensions DuckDB target version using the above steps, you may run into the fact that your extension no longer builds properly.

Currently, DuckDB does not (yet) provide a specific change log for these API changes, but it is generally not too hard to figure out what has changed.

For figuring out how and why the C++ API changed, we recommend using the following resources:
- DuckDB's [Release Notes](https://github.com/duckdb/duckdb/releases)
- DuckDB's history of [Core extension patches](https://github.com/duckdb/duckdb/commits/main/.github/patches/extensions)
- The git history of the relevant C++ Header file of the API that has changed