# Testing this extension

SQL tests are [SQLLogicTests](https://duckdb.org/dev/sqllogictest/intro.html): plain SQL with
the expected rows beneath a `----` separator. DuckDB's contribution guide prefers these to
C++ tests, and a failure reports the statement that failed rather than a stack trace. Both
layers also have offline C++ tests (Catch2) for what SQL cannot reach directly: the extension for its
code above the transport interface, such as the conversion layer, the ATTACH configuration and
handle, and the log sink it passes to the client; the client for the code below it. One Catch2
main, `test/catch_main.cpp`, serves every C++ suite.

## Layout

| Directory | Part | Guard | Runs in |
|---|---|---|---|
| `mock/` | the extension (SQL surface) | `mock` | `make test_mock`: builds the mock client; no driver, no network |
| `mock/cpp/` C++ tests | the extension (SQL surface) | none (not `.test` files) | `make test_mock`: the same build, run before the SQL tests |
| `client/sql/` | the ODBC client | `odbc` | `make test_client`: builds the ODBC client with no driver; live tests also `require-env` their credentials |
| `client/{interface,fake_driver,odbc}/` C++ tests (and, in `odbc/`, the offline checks) | the driver interface (`interface/`, shared by the ODBC client and the driver loader), the ODBC client (`fake_driver/`, `odbc/`) | none (not `.test` files) | `make test_client`: ODBC build with `REDSHIFT_DRIVER=none`, no driver download or network (its `[live]` cases always skip here: these builds have no driver; see "The first live query"); `make test_client_tsan`: the same, built with `-fsanitize=thread`; `make test_client_e2e`: the C++ tests alone, in `build/driver-e2e` with the test-endpoint switch; on Linux, `make test_driver_e2e` also runs their offline `[odbc]` cases there with the embedded driver and `REDSHIFT_REQUIRE_DRIVER=1` (see "The output scan and the environment gate") |
| `client/driver/` | the driver loader | none (C++ and tool tests); `odbc` and `DUCKDB_REDSHIFT_TEST_E2E` (`sql/`) | `make test_driver`: ODBC build with the embedded driver; its tool tests also need `openssl` (1.1.0 or later; the macOS tests that sign or verify a package (all of `verify-macos-package_test.sh`) need one whose crypto policy allows SHA-1 signatures, as Amazon Linux 2023's default does, and skip, saying so, where it does not, as on RHEL 9), `python3` (3.8 or later on Linux, for `rename_test.sh`'s pinned-tool cases, which a lock with rename rows requires), `readelf`, `nm`, `objcopy`, `od`, `gcc` with its C++ front end (`g++`) and `curl` on PATH, and a system CA bundle that holds DigiCert Trusted Root G4 (`ca-certificates`, at `/etc/pki/tls/certs/ca-bundle.crt`, `/etc/ssl/certs/ca-certificates.crt` or `/etc/ssl/cert.pem`). These check the Linux and macOS driver packages and the Windows MSI (cached in `build/driver-package-cache/`; offline, the real-MSI case of `verify-windows-msi_test.sh` takes the MSI from `RS_DRIVER_MIRROR` as `<sha256>`). On Linux, for a lock with rename rows, the recipe's `rename-tools.sh` step, not the tool tests, fetches the two pinned rename wheels into `build/rename-tools/` (or `RS_RENAME_TOOLS`) from their pinned URLs, whatever `RS_DRIVER_MIRROR` says; offline, put each there first as `<sha256>.whl` (`third_party/redshift_odbc/tools/lib/rename-requirements.txt`). On a macOS host the tool tests need GNU binutils and coreutils on PATH, after the Command Line Tools of Xcode 15 or later (Apple's `nm` and `otool` read the Mach-O embed, and `nm`'s long options are llvm-nm's from Xcode 15 on), and the cases that build small Linux libraries with `gcc` skip there; the real-driver cases and `make test_driver_e2e` need the host's `libkeyutils.so.1` (`keyutils-libs`), the one library the driver needs outside glibc, and the real-driver cases also run the host's unixODBC and jemalloc, or the ones `REDSHIFT_TEST_UNIXODBC` and `REDSHIFT_TEST_JEMALLOC` name, and warn and stop without them (or fail, with `REDSHIFT_TEST_REQUIRE_HOST_LIBRARIES=1`), and tcmalloc where the host has it or `REDSHIFT_TEST_TCMALLOC` names it (never required); the OpenSSL isolation case needs the host's `libssl.so.3` and warns and stops without it (or fails, with that flag); the trace of the files the driver reads needs `ptrace` (not under another tracer) and warns and stops without it (or fails, with that flag); the Linux build makes `redshift_driver_probe`, which `third_party/redshift_odbc/tools/host-probe.sh` runs; `make test_driver_e2e`: `make driver_e2e`'s test-endpoint build with the embedded driver and a stock DuckDB shell, the `sql/` files, a `LOAD` of the loadable extension and the extension-coexistence harness run by `run-e2e.sh` with no network on Linux |

Every C++ suite is declared the same way: `EXCLUDE_FROM_ALL`, so nothing in a plain `make` or
`make release` compiles them, and linked as DuckDB's `unittest` links — `duckdb_static` plus
`link_extension_libraries`, which brings in this extension, so a test can call the client or
open a database. The client's suites sit behind DuckDB's own gate for a test target as well
(`BUILD_UNITTESTS AND NOT CLANG_TIDY`), which drops them from an extensions-only or
`clang-tidy` configure. Each suite target builds the suites it runs by name first, so a suite
that loses its target fails that target rather than passing as an area without tests. Each
`client/` area lists its own files in its own `sources.cmake`; an ODBC build has
`redshift_client_tests`, and the embedded build also has `redshift_driver_tests`, because
the driver loader's tests link its loader.

`test/client/interface/by_path_driver.cpp` is a `DriverSource` over a driver library a developer
names in `REDSHIFT_TEST_DRIVER_PATH`, pinned by `REDSHIFT_TEST_DRIVER_SHA256`, for offline work with
a real driver, such as loading it and recording traces. No gate target or live test runs through
it. It loads at most once per process, and its refused and overridden variable lists are empty, so
a run through it has no refused-variable protection:
`SSLKEYLOGFILE`, for one, is honoured. The build it is in allocates the environment handle plainly,
so the driver also reads an `odbc.ini` in the current directory.
Both are by design, and the driver loader leaves them so: it is test-only and run by hand. It is
listed in that area's `sources.cmake` alone, so only `redshift_client_tests`, which `make test_client`,
`make test_client_tsan` and `make test_client_e2e` build, links it, and no shipped build contains
it; one of its tests reads the `REDSHIFT_DRIVER=none` build's loadable extension and fails if it
holds any of it. Its own tests need neither the two variables nor a real driver: they load stub
libraries built from `by_path_stub_driver.c`, one exporting every entry point the interface names
and one without `SQLAllocHandle`, plus, on Linux, one with an unresolved symbol, one that interposes
a symbol of the first, and one whose dependency fails to load. All are `EXCLUDE_FROM_ALL`, and
`redshift_client_tests` depends on them, so a plain `make` builds none while the client targets
build them all. The case that loads the developer's own driver is skipped when the two variables are
unset, which is why the targets stay offline. To run it, copy the driver into a directory under the
checkout that only you can write, export
`REDSHIFT_TEST_DRIVER_PATH=<path>` and `REDSHIFT_TEST_DRIVER_SHA256=<hash>`, and run
`REDSHIFT_ALLOW_NETWORK=1 make test_client`; a container that passes neither variable runs without
the case, and `unshare -rn` skips it. The cases copy the stubs into a new directory under
`/tmp`, resolved through any link, and check every directory from `/` down as the driver loader does, so
they need `/` owned by root or the caller. Inside a user namespace, such as `unshare -r`, `/` is
owned by neither, and each such case is reported as skipped; the cases that load a copy are also
skipped when `/tmp` is mounted noexec.

`test/client/odbc/check-build-graph-offline.sh` fails `make test_client`, `make test_client_tsan`
and `make test_client_e2e` when the `REDSHIFT_DRIVER=none` build's CMake graph has a step that
fetches or embeds the driver, or one that runs curl or wget, names a URL on its command line
or is an ExternalProject, git or pip download; its fixture tests (`make test_graph_check`) run
before the build. It knows the embedded build's fetch by the name `fetch-driver.sh` and its
embed by an assembler object, both the driver loader's. A change to those markers changes the fixture
cases that plant them in `check-build-graph-offline_test.sh` too.
`make test_driver` runs `check-build-graph-offline.sh --expect-embedded build/release`. It fails
unless the embedded graph has both a fetch and an embed step, so a marker that stops matching
the driver loader's steps is noticed. A step that reads its URL from a file, such as `cmake -P` on a script
that calls `file(DOWNLOAD)`, passes the check, and so does a download CMake makes while configuring
(`FetchContent`, `file(DOWNLOAD)`, or `execute_process` running a fetch script), which is not in
the graph at all. Only a run without a network catches those, so `make test_client`,
`make test_client_tsan` and `make test_client_e2e` refuse to run while one is available
(`check-no-network.sh` reads `/proc/net/dev`, the caller's own network namespace).
Run them in a container without a network. Under `unshare -rn` the by-path cases that check
real directories are skipped, so that route proves less than a container. `REDSHIFT_ALLOW_NETWORK=1`
runs them anyway, without that proof; a host without `/proc/net/dev`, such as macOS, needs it.
On a platform other than Linux, macOS and Windows every connect fails on the trust refusal, and so
do the fake-driver cases that connect.
CMake can skip a configure-time download it already made (a hash-pinned `file(DOWNLOAD)`,
`FetchContent`), so the proof holds only for a build directory no networked run configured first:
a run meant as that proof starts from a fresh `build/release`, or `build/tsan` for `make test_client_tsan`,
or `build/driver-e2e` for `make test_client_e2e`.

On Windows, `make test_client` runs the ODBC client's offline cases that need no POSIX facility;
run `make test_client_shape` there with
`REDSHIFT_ALLOW_NETWORK=1`, as the no-network check reads `/proc/net/dev`, and
`RS_REQUIRE_DUMPBIN=1`, which makes the interface's import-table case fail rather than skip when it
finds no `dumpbin`. The fake-driver cases connect there as on Linux, through the fixture's
`SSL_CERT_FILE`, which the Windows trust resolver honours. `load_only_refusal_test.cpp` pins through
the extension the refusals that come before the driver source; it is built only on Windows, and not
in the test-endpoint build, which refuses a test variable with another message. The ODBC client's
`sources.cmake` files, and a `.test` file's `require notwindows`, leave these out on Windows; the
interface's by-path tests are the interface's to leave out, in its own `sources.cmake`:

| Left out on Windows | Why |
|---|---|
| `client/odbc/connection_test.cpp`, `refused_configuration_test.cpp`, `process_state_test.cpp` | they use `SSL_CERT_FILE=/`, `/dev/null` and the POSIX environment's `setenv`; `process_state_test.cpp` is left out whole, the two cases in it that only read variables under the lock included |
| `client/odbc/dead_peer_test.cpp` | it counts the process's threads in `/proc/self/task`, and its harness, `run-dead-peer.sh`, runs Linux containers; its long-stream case goes with it |
| `client/odbc/region_allowlist_test.cpp` | it runs the allowlist generator, CMake and the Makefile as POSIX processes; `region_validation_test.cpp`, which runs everywhere, holds the `ValidateRegion` cases |
| `client/odbc/output_canary_test.cpp` | it runs `scan-output.py` as a POSIX process through `popen`, and uses `trust_store_test_support.hpp` |
| `client/odbc/environment_gate_test.cpp`, `known_environment_live_test.cpp` | seccomp, `/proc` and inotify: their `sources.cmake` lists them on Linux alone, so macOS leaves them out too |
| `client/odbc/trust_store_search_test.cpp` and `trust_store_test_support.hpp`, `trust_store_linux_test.cpp`, `trust_store_macos_test.cpp` | POSIX files and permissions; each platform selects its own resolver's tests |
| `client/sql/trust_unrelated_ca.test` and `cluster_trust_unrelated_ca.test` (`require notwindows`) | on Windows the control plane verifies against the Windows store whatever `SSL_CERT_FILE` says, so the database hop fails instead, with Windows' hint, and these files do not run there |
| `client/driver/environment_allocation_test.cpp`, the driver loader's, which includes the ODBC client's fixture | it allocates through the POSIX loader's `ODBCINI=/dev/null` call; the Windows C++ suite holds the Windows loader's and the resource reader's cases; it stays with the POSIX loaders, as on Windows the allocation is the plain one |

## Selecting tests

The pinned `extension-ci-tools` targets run `unittest "test/*"`, which matches every `.test`
file under `test/`. Rather than change that glob, every `.test` file starts with

```
require-env REDSHIFT_TEST_CLIENT mock      # or odbc
```

and the root `Makefile` sets `REDSHIFT_TEST_CLIENT` to the client the target tests: `mock`
for `make test_mock`, `odbc` for `make test_client`, `make test_client_tsan` and
`make test_driver`, and `REDSHIFT_CLIENT` (default `mock`) for the pinned `make test`. It is
derived, not a knob: a value in the environment is ignored.

The suite targets build their client first. The pinned `test_release`, `test_debug` and
`test_reldebug` do not, because DuckDB's distribution CI builds and tests in separate steps;
they refuse a build configured for another `REDSHIFT_CLIENT` instead. To run the ODBC
client's tests there, build it first: `make release REDSHIFT_CLIENT=odbc` and then
`make test REDSHIFT_CLIENT=odbc`.

`test/lint-test-guards.sh` (`make lint_tests`) fails when a `.test` file lacks that guard
line or names the wrong client for `test/mock/` or `test/client/`. It also rejects a second
`REDSHIFT_TEST_CLIENT` guard later in the file, `require-env` or `require-env-not`, because the
runner evaluates every one it reaches and skips the whole file when one does not hold — in every
suite, while the target still passes on the other files. It rejects any `include` line too: the
lint reads each file on its own and cannot check the guards of a file pulled in. Its idea of a blank line is the pinned parser's:
`\r` stripped, then empty. A line of spaces or tabs is a statement to DuckDB, so the lint
reports it here instead of letting `Tokenize` fail in every build. It runs before every test
target, including the pinned ones. Its own fixture tests are `make test_lint_guards`.

`test/run-selected.sh` runs one directory and refuses a run in which no test case executed,
so a target cannot pass by skipping every file. It reads only the tester's summary line. Its
own fixture tests are `make test_run_selected`.

**`test/client/sql/` must always hold at least one test that runs offline.** The client keeps
that directory as the home of the live SQL tests, which `require-env` their credentials and skip
without them; `make test_client` is an offline target and `run-selected.sh` refuses a run in
which nothing executed. So the directory's last offline test is what keeps the offline target
passing, and a change that removes it breaks `make test_client` with no live test having failed.
`client_selection.test` is that test today.

**Live variables.** Each live test in `test/client/sql/` runs only when every variable it
`require-env`s is set, and it needs a build with a driver, so leave them all unset for
`make test_client`, which builds without one. On a build with the embedded driver,
`make test REDSHIFT_CLIENT=odbc` runs every one whose variables are exported.
`REDSHIFT_TEST_LIVE_REGION` names the region of the live targets, which this process's default AWS
credential chain reaches as the database user `REDSHIFT_TEST_LIVE_DEFAULT_USER`.
`REDSHIFT_TEST_LIVE_PROFILE` names an AWS profile that reaches them as another user,
`REDSHIFT_TEST_LIVE_PROFILE_USER`. `REDSHIFT_TEST_LIVE_WORKGROUP` and
`REDSHIFT_TEST_LIVE_ACCOUNT_ID` name an account-backed Serverless workgroup in that region. The live
tests here other than `credentials_empty_chain.test` attach to that workgroup, except the three
`cluster_*.test` files.
`REDSHIFT_TEST_LIVE_CLUSTER` names a provisioned cluster in that region, which those three files, the
session-ended case in `connection_loss_live_test.cpp` and the cases in
`known_environment_live_test.cpp` connect to; the latter run on each target whose variables are set,
and give the cluster's child processes no test-endpoint variable. A cluster is reached through the
endpoints it derives, so run the three files and the session-ended case with none set. The
other `[live]` C++ cases in `test/client/odbc/` read `REDSHIFT_TEST_LIVE_REGION`,
`REDSHIFT_TEST_LIVE_WORKGROUP` and `REDSHIFT_TEST_LIVE_ACCOUNT_ID` and attach to that workgroup too.
`REDSHIFT_TEST_LIVE_TOKEN_EXPIRY=1` also runs
`credentials_token_expiry.test`, which takes over an hour. `credentials_empty_chain.test` runs
instead in a process whose default chain finds no credentials, with
`REDSHIFT_TEST_LIVE_EMPTY_CHAIN_REGION` naming the region. The chain also reads the container and
instance credentials, so set no other `AWS_*` credential or profile variable, point
`AWS_CONFIG_FILE` and `AWS_SHARED_CREDENTIALS_FILE` at empty files, set
`AWS_EC2_METADATA_DISABLED=true`, and unset `AWS_CONTAINER_CREDENTIALS_RELATIVE_URI` and
`AWS_CONTAINER_CREDENTIALS_FULL_URI`; on EC2, ECS or a CI host with ambient credentials it otherwise
connects. `credentials_wrong_secret.test` runs instead in a process that exports a working,
non-empty `AWS_ACCESS_KEY_ID`, its `AWS_SESSION_TOKEN` if it has one, and `AWS_SECRET_ACCESS_KEY`
set to the placeholder the file names, beside `REDSHIFT_TEST_LIVE_REGION` and the workgroup's two
variables; with any other secret key it skips. Run it alone, since every other live test fails with
that key: `env -u LD_LIBRARY_PATH REDSHIFT_TEST_CLIENT=odbc bash test/run-selected.sh
build/release/test/unittest test/client/sql/credentials_wrong_secret.test`.
`cluster_wrong_secret.test` runs the same way with `REDSHIFT_TEST_LIVE_CLUSTER` in place of the
workgroup's two variables.
`trust_unrelated_ca.test` runs instead in a process whose `SSL_CERT_FILE` names a CA that
signed none of the endpoints, such as a new self-signed one, with
`REDSHIFT_TEST_LIVE_UNRELATED_CA_REGION` naming the live region beside the workgroup's two
variables, and `cluster_trust_unrelated_ca.test` beside `REDSHIFT_TEST_LIVE_CLUSTER`. Each
`*_constants.test` reads only constants cast to a column's type, so a principal that may not write
runs it; the three files beside them, `bulk_fetch.test`, `long_fetch.test` and
`utf8_text.test`, read their values stored, from temporary tables. `describe_no_side_effects.test`
and `shape_changed.test` write one too, since what they check needs a table: that describing an
`INSERT` or a `CREATE TEMP TABLE` runs neither, and that after an `ALTER TABLE` between a
statement's `Describe` and its `Execute`, the `Execute` is refused. `inserted_values.test` writes
10,000 rows it generates and compares what it reads back with them. Each of the six writes only
temporary tables in its own session. On a principal that may not write, run every live file except
those six, each by name through `test/run-selected.sh` as the first-query example below does, since
`make test REDSHIFT_CLIENT=odbc` runs those six too, and leave out the client suite's `[writes]`
cases with `"[live]~[writes]"`.
`utf8_names_windows.test` runs only on Windows (`require windows`): it checks that the Windows
driver's narrow entry points keep non-ASCII names and text as UTF-8.

`REDSHIFT_TEST_LIVE_SECOND_REGION` names another region for `second_region_live_test.cpp`, whose
cases also need the three workgroup variables but attach to no workgroup, and unset the
test-endpoint variables, since the derived endpoints are what they check. The first asks each
target's control plane there, and in `REDSHIFT_TEST_LIVE_REGION`, for a workgroup of
`REDSHIFT_TEST_LIVE_ACCOUNT_ID` and a cluster, named alone and by an endpoint, that do not exist, so
nothing needs to exist in that region. The second also needs
`REDSHIFT_TEST_LIVE_REGION_ONLY_WORKGROUP`, a workgroup of that account in the live region alone: it
must be found there (it opens, refuses the login or does not answer, about 270 s from a host outside
its network when it resolves to three addresses) and not found in the second region.

**The test-hook check.** `check-no-test-hooks.sh` checks that an extension defines no test-only
hook: `make test_client` runs it on its driverless build and `make test_driver` on the shipped one,
`build/release` with the embedded driver. It reads the symbols with `nm` (llvm-nm's long options,
Xcode 15 or later on a Mac), so it runs on Linux and macOS only and says it skips on Windows; its
fixture test needs `c++`, `ar`, `nm` and `strip` and runs only its Makefile cases without them.

**The dead-peer bound.** `test/client/odbc/run-dead-peer.sh <checkout> <build-dir>
<credentials-env-file>` runs `dead_peer_test.cpp`'s `[live]` case with its database connection
through a forwarding container on a Docker network of its own, and disconnects the forwarder once the case
has read its first batch, so the server's packets stop with no FIN or RST. It needs Docker,
`IMAGE`, an image with the build's runtime, the three live variables and both test-endpoint
variables; without its signal
directory the case skips. The forwarder and the network are removed on exit. The file's other
`[live]` case shows the bound is not a time limit: a result still streaming 120 seconds after its
first batch is not cut. It takes about four minutes, so it runs only with
`REDSHIFT_TEST_LIVE_LONG=1` beside the three live variables, on a direct connection.

**The first live query.** The live C++ case, `[live]` in `test/client/odbc/first_query_test.cpp`,
records the embedded driver's calls and result for `SELECT 1` and checks them against the fake's;
`test/client/sql/first_query.test` checks that `SELECT 1` returns `INTEGER 1`. The fake's
`SELECT 1` script is derived from the driver's source, and the `[live]` case checks it against the
embedded driver. On a mismatch, its
warning, headed by the driver's identity and set SHA-256, is the trace to keep beside
`DefaultResult`. The three `[live]` cases in `statement_error_live_test.cpp` run statements the
server refuses: a syntax error, which carries only `sqlstate` and `sqlstates` in `extra_info`; a
class 22 error that echoes an address, shown as `<address>`; and one whose message is longer than
the driver keeps, which the driver must cut where the fake does. All need the workgroup to be
reachable from the host that runs them: publicly accessible, or run on a host inside its VPC.
From anywhere else `SQLDriverConnect` times out with the network-access hint. In a container,
give the default chain `AWS_ACCESS_KEY_ID`, `AWS_SECRET_ACCESS_KEY`, `AWS_SESSION_TOKEN` and
`AWS_REGION`: export them inside it, or `docker run --env-file` a file outside the checkout holding
only those four. Set
`REDSHIFT_TEST_LIVE_REGION`, `REDSHIFT_TEST_LIVE_WORKGROUP` and `REDSHIFT_TEST_LIVE_ACCOUNT_ID`
too. No make target runs the client suite's `[live]` cases: `make test_driver` builds the suite with
the driver in `build/release` for its own checks, and on Linux `make test_driver_e2e` runs only its
offline `[odbc]` cases, in `build/driver-e2e`. So after `make test_driver`, run in the container
(without `LD_LIBRARY_PATH`, which the loader refuses with an empty or relative element, as `make
test_driver` notes):

```bash
cmake --build build/release --target redshift_client_tests
env -u LD_LIBRARY_PATH build/release/extension/redshift/redshift_client_tests -w NoTests "[live]"
env -u LD_LIBRARY_PATH REDSHIFT_TEST_CLIENT=odbc \
  bash test/run-selected.sh build/release/test/unittest test/client/sql/first_query.test
```

**The recorded driver output.** Four of the twelve `[live]` cases in `driver_output_live_test.cpp`
check that the driver still reports what `recorded_driver_output.hpp` holds: each recorded
statement's columns and the text of its row, from the leader and the compute nodes, under the
client's settings and after each recorded caller `SET`, and the text alone through `SQLExecDirect`,
under both, for each statement the client runs that way, whose run through the client also checks
the executed columns. A fifth reads a SUPER object three times for its key order, and the sixth
reads what a caller's `SET` or `RESET` of a pinned setting does to the values the client returns,
against its own expected values, which hold only on a workgroup whose defaults are
`extra_float_digits` 0, time zone UTC, date style `ISO, MDY` and interval style `postgres`. The
first six read literals and system views only, and take about five minutes. The seventh, tagged
`[writes]`, reads the same for values stored in a session temporary table, on the same kind of
workgroup. The eighth and ninth, also tagged `[writes]`, store SUPER numbers in a session temporary
table and read each through `SQLExecDirect` and `SQLExecute`; the ninth's numbers, in exponent
form, arrive rounded to 15 or fewer significant digits on every path. A principal that may not
write leaves these three out with `"[live]~[writes]"`, and the last three below with them. When a
newly pinned driver differs, record its output in `recorded_driver_output.hpp`, and a row that
differs only through `SQLExecDirect` in the test file's `SIMPLE_PROTOCOL_DIFFERENCES`, then re-run
the offline cases in `driver_output_test.cpp`, which replay the recorded output through the fake.
`SIMPLE_PROTOCOL_DIFFERENCES` and `SIMPLE_PROTOCOL_REFUSED`, the caller-settings cells, stored or
not, and the stored-SUPER texts stay in that file, and only the `[live]` run checks them.

`-w NoTests` catches only a filter that matches nothing, and a skipped case still exits 0, so the
`[live]` run's summary counts what was skipped: a skipped case did not run, and each case meant to run
shows passing or failing (a failing first-query case prints its trace warning). "All tests were skipped" means a live variable is unset in this shell or
the build has no driver: export the three variables, and run `make test_driver` first.

**The recorded stored output.** The last three `[live]` cases in `driver_output_live_test.cpp`, also
tagged `[writes]`, store each recorded select's values in a session temporary table, then check that
the driver still sends what `recorded_stored_output.hpp` holds for each table, its columns and its
row through `SQLExecute` and `SQLExecDirect`, under the client's settings and after each recorded
caller `SET`, and that the client returns each stored value as the driver sent it; seven sessions in
all. When a newly pinned driver differs, record its output there and re-run `driver_output_test.cpp`.

The `[live]` cases in `connection_loss_live_test.cpp` touch only their own sessions and write
nothing. One ends its session with `pg_terminate_backend(pg_backend_pid())` on the provisioned
cluster that `REDSHIFT_TEST_LIVE_REGION` and `REDSHIFT_TEST_LIVE_CLUSTER` name: on a Serverless
workgroup the client can wait minutes for the session's end. The others read the same three
variables as above, and fail a statement, abort a transaction and roll it back, `SET` the search
path and the time zone, and describe a `CREATE TEMP TABLE`, which creates nothing.

The `[live]` cases in `prepare_once_live_test.cpp` read the same three variables and check the
statement `Describe` keeps: the driver calls a `redshift_query` scan makes with no parameters, and
with parameters or a DOUBLE column, and what a kept statement does when its table gains a column
while it is kept, through the client or a DuckDB `PREPARE` and `EXECUTE`, and through the client, a
second later. The three that change a table write only a temporary table in their own session and
are tagged `[writes]`, so `'[live]~[writes]'` leaves them out.

The `[detach]` cases in `detach_bystander_live_test.cpp` check what another DuckDB connection's
`DETACH`, `ATTACH OR REPLACE` or rolled-back `ATTACH` does to a bystander's session, each DuckDB
connection on a thread of its own. They read the same three variables, and read
`sys_session_history` and `sys_query_history` through a second handle to see sessions end and
statements run. The nine that write a temporary table in their own session are tagged `[writes]`
too, so `'[live]~[writes]'` leaves them out, as above. `REDSHIFT_TEST_STRESS_ROUNDS` (10) and
`REDSHIFT_TEST_STRESS_SEED` (random, and reported) set the stress case's rounds and pauses. The
stress case's check that no session is left open counts every session of the database user, so it
runs only with `REDSHIFT_TEST_LIVE_EXCLUSIVE=1`, which says nothing else uses that user meanwhile. A
bystander's session that only a `DESCRIBE` opened is found as the one new session of the user, and
its end is checked whenever that search singles it out; when all five tries are ambiguous, the case
fails with the variable set, and otherwise skips that check and says so. To run the cases alone,
give `"[detach]"` in place of `"[live]"` in the `build/release` command above.

To record the trace through the test endpoints instead, run the case in the test-endpoint build
with the driver, in the container as above, with `DUCKDB_REDSHIFT_TEST_ENDPOINT_URL` and
`DUCKDB_REDSHIFT_TEST_HOST` exported beside the three live variables:

```bash
make driver_e2e REDSHIFT_DRIVER=embedded
cmake --build build/driver-e2e --target redshift_client_tests
env -u LD_LIBRARY_PATH build/driver-e2e/extension/redshift/redshift_client_tests -w NoTests "[live]"
```

The warning's provenance then ends ", through the test endpoints". The next
`make test_client_e2e` rebuilds `build/driver-e2e` without a driver.

For the loadable extension, `make stock_shell`, then in `build/stock-shell/duckdb -unsigned`,
`LOAD 'build/release/extension/redshift/redshift.duckdb_extension'`, attach as `first_query.test`
does with the three values, and
`SELECT typeof(c), c FROM redshift_query('rs', 'SELECT 1') AS t(c)` gives `INTEGER` and `1`.

**TLS verification.** `test/client/odbc/tls-verification.sh <build>` runs the `[tls]` cases of
`tls_verification_test.cpp`, then its `[tls_first]` case in a process of its own, on the real
driver, in `make driver_e2e REDSHIFT_DRIVER=embedded`'s `build/driver-e2e`. It makes a test PKI
for the run in a new temporary directory, starts `tls_test_servers.py` (a control plane that
answers `GetCredentials`, over HTTPS and over plain
HTTP, and per case a loopback TCP listener that answers the PostgreSQL `SSLRequest` and serves the
case's certificate with Python's TLS; every listener binds 127.0.0.1), runs the cases with
`REDSHIFT_TEST_TLS_DIR` naming that directory, prints the driver's records the cases wrote there,
then stops the servers and removes it. Without that variable the cases skip, as in `make
test_client`; the script fails a run whose database cases never reached the servers, as skipped
cases would not. The test endpoints aim the connection's host and control plane at the servers, and
the workgroup's port names a case's listener. The cases: a database certificate from a CA outside
the trust source, and one from the trusted CA for another host name, each fail the connect once as a
certificate failure, with one connection to the server; one for the host gets through to the startup
message, through either control plane; `SSL_CERT_FILE` naming an unrelated CA fails the
control-plane call before any database connection; and a connect to a closed port fails once, with
the client's message for a failure of no category. Two more, on the `sspi` and `gss` listeners,
which ask for SSPI or GSS authentication after the startup message and then refuse, check that the
connect fails and that the listener recorded the client's reply in a fixed form (its type and
length, and for a token its mechanism, NTLM or Kerberos, its size and, for those two, its SHA-256,
never its bytes; an NTLM message's hash can match across runs, a Kerberos token's never does, as
each carries a fresh authenticator), which the script prints with the records; a token sent is
recorded, not failed on. The script first runs `tls_test_servers_test.py`, which checks that
recording on replies of every form, and fails a run whose authentication cases left no line in the
records. On Linux each checks `ClassifyConnect`'s category for the driver's records and the records'
text, as measured there; the closed-port case's records need only show the refusal, and the two
authentication cases' records only their first line. Their pinned text is a host's with no Kerberos
ticket: on a host with a default realm and a ticket, the driver's
GSSAPI would ask the realm's KDC for one for `paraccel/localhost` (its default service name), so the
text would change, and with a network the request would leave the host, which the no-network run
below rules out. The script cannot point `KRB5_CONFIG` and `KRB5CCNAME` at empty files instead: the
client refuses a connect with either set, on Linux and Windows
(`third_party/redshift_odbc/policy/refused-environment.txt`). On Windows the script runs in Git
Bash, the database cases call the plain-HTTP control plane, since the driver verifies its control
plane against the Windows store, and they check the category but only record the text, except that
the closed-port case also checks the refusal; the two authentication cases check neither. Two cases
replace the unrelated-CA one there: with `SSL_CERT_FILE` naming its CA, the HTTPS control plane
fails before the database hop with the SDK's text for any request that fails before a response, and
the case checks that text and that it has no category; and, with `SSL_CERT_FILE` unset, the database
hop fails as a certificate failure against the store's export, the content-named file in the
loader's trust directory that the connection string names. Driver 2.3.1.0 keeps each connection's
trust to that connection (2.3.0.0 kept one TLS context per process and added each connect's CA file
to it), so a later connect does not trust a CA an earlier one named, which a `[tls]` case pins on
every platform: after a trusted connect, a connect whose CA file signs no server fails as a
certificate failure. The hop that must fail without the run's CA, against the export on Windows and
a CA that signs no server elsewhere, is still the one `[tls_first]` case, which the script runs in a
process of its own after the `[tls]` run, now only as a precaution; outside Windows that case then
connects with the run's CA, so that hop verifies. Of the live case, the unrelated CA against the real endpoints,
`trust_unrelated_ca.test` and, on a cluster, `cluster_trust_unrelated_ca.test` are the
control-plane half; no live case checks the database connection's half. The script needs OpenSSL's
`openssl` (for the PKI; LibreSSL's is untried), `python3`,
and the real driver's `libkeyutils.so.1`. Run it with no network, after `make
driver_e2e REDSHIFT_DRIVER=embedded` and `cmake --build build/driver-e2e --target
redshift_client_tests`:

```bash
docker run --rm --network none --pull=never -u "$(id -u):$(id -g)" --mount "type=bind,src=$PWD,dst=$PWD" \
  -w "$PWD" <image with the build's tools> timeout 300 bash test/client/odbc/tls-verification.sh build/driver-e2e
```

On Windows, after the same two builds, from Git Bash with Git's `openssl` and a 64-bit Python 3.11
as `python3` on `PATH`:
`timeout 300 bash test/client/odbc/tls-verification.sh build/driver-e2e`.

**The output scan and the environment gate.** `test/client/odbc/scan-output.py [--canary VALUE]...
FILE...` reports each access-key ID, each assignment of a credential keyword whose value is not
a redaction marker, and each canary, by file and line and never the text it found; it exits 1 on a
finding and 2 on a file it cannot read. `output_canary_test.cpp` runs it over what the fake driver's
connect, statement and teardown paths show with canary credentials and a canary parameter, and its
two `[live]` cases do the same on the workgroup the three live variables name. They live in this
directory as cases that must reach the client below ATTACH: a `.test`
file cannot read the output streams or the log sink they scan.
`environment_gate_test.cpp`, on Linux, runs each driver case in a new process of the suite: every
refused variable in turn, under a seccomp filter that kills the process at its first socket, then
logging turned on through each Linux `ini` row of
`third_party/redshift_odbc/policy/config-files.txt`, untrapped, which needs the test-endpoint build
to keep the connect on this host. `known_environment_live_test.cpp`'s two `[live]` cases set each
variable of `policy/known-environment.txt` that has a keyword (but `SSL_CERT_FILE`, the client's own
trust source) or a `claim:` reason to a hostile value in a new process and check that the workgroup
returns what it returns without it; the `DISPLAY` child runs under the process trap, whose filter count
is checked only on Linux 5.9 and later, the first kernels to write one. They do not show
`PGSSLMODE`'s TLS, and on 2.3.0.0 the `PGAPPNAME` row cannot fail. The
gate targets build no driver, so the driver cases skip there. On Linux `make test_driver_e2e` runs the
offline ones after `run-e2e.sh`, with `REDSHIFT_REQUIRE_DRIVER=1`, which fails the environment gate's
driver cases if they would skip (Catch counts a skip as a pass): with it set they fail in a build
without the driver, and the INI case also in one without the test endpoints, such as `build/release`.
Run the `[live]` ones by hand, with the live variables set, in the container after an embedded
test-endpoint build; the command runs every `[odbc][live]` case:

```bash
make driver_e2e REDSHIFT_DRIVER=embedded
cmake --build build/driver-e2e --target redshift_client_tests
env -u LD_LIBRARY_PATH build/driver-e2e/extension/redshift/redshift_client_tests -w NoTests "[odbc][live]"
```

**`make test_client`, `make test_client_tsan`, `make test_client_e2e` and, on Linux, `make test_driver_e2e`
need a `python3` CMake can find.**
`test/client/odbc/region_allowlist_test.cpp` re-runs
`src/transport/odbc/tools/gen-region-allowlist.py` over the endpoint data vendored beside it, so
that a hand edit to the generated region list does not survive a test run. It runs the
interpreter `find_package(Python3)` found, passed to it as `REDSHIFT_PYTHON`, and fails saying so
when CMake found none; `output_canary_test.cpp` and `environment_gate_test.cpp` get it too, for
`canary_output.hpp`, which runs `scan-output.py`. The build itself does not need python3 — DuckDB skips its
own local-extension-repository step without one — so this is the suite's requirement, not the
build's.

**One case also needs a comma-decimal locale, or glibc's `localedef`, and skips without.**
`test/client/odbc/number_text_test.cpp` reads numbers under a `LC_NUMERIC` whose decimal point is a
comma. It uses an installed `de_DE` or `fr_FR` locale, otherwise on Linux builds one with `localedef`
(`glibc-common` on EL, `libc-bin` on Debian and Ubuntu), and skips saying so with neither. Set
`REDSHIFT_REQUIRE_COMMA_LOCALE=1` where the image guarantees one, to fail instead. musl's locales
keep `.` as the decimal point, so the case cannot run there.

## Running

The targets that build compile against the DuckDB tree in `duckdb/`, at the commit `duckdb.commit`
pins, and none fetches it: run `make duckdb` once first, which clones it there, or checks the clone or
link already there.

```bash
make test               # lint, check build/release is the selected client (mock by default), run test/
make test_mock          # lint, build the mock client, run mock/cpp/'s suite, then test/mock/ (test_mock_shape's steps)
make test_mock_shape    # the steps make test_mock runs
make test_mock_tsan     # lint, build the mock client with -fsanitize=thread in build/tsan_mock/, run mock/cpp/'s suite there; fails on a failed or crashed suite, a ThreadSanitizer error, or a report in this extension's code
make test_tsan_own_reports # tsan-own-reports.sh's fixture test, which test_mock_tsan runs first; needs no build
make test_mock_loadable # lint, build the mock client and a DuckDB without it, attach with no LOAD, then run test/mock/ there against the loadable extension
make test_client        # refuse to run with a network, lint, the offline checks' fixtures, build the ODBC client with no driver, check its graph, run its C++ suite and test/client/sql/ (test_client_shape's steps)
make test_client_shape  # the steps make test_client runs
make test_client_tsan   # as `make test_client_shape`, built with -fsanitize=thread in build/tsan/
make test_client_e2e    # refuse to run with a network, lint, run the offline checks' fixtures, build the ODBC client with no driver in build/driver-e2e with the test-endpoint switch, check its graph, run its C++ suite once, without test/client/sql/
make driver_e2e         # build the ODBC client with the driver REDSHIFT_DRIVER names (embedded, fetched, by default) and the test-endpoint switch in build/driver-e2e; never shipped
make test_driver        # lint, build the ODBC client with the embedded driver, run the driver loader's tests
make test_driver_e2e    # lint, build the ODBC client with the embedded driver and a stock shell, run the SQL tests in client/driver/sql/ and a LOAD of the loadable extension (on Linux, only where the connection cannot leave the host), then on Linux the C++ client suite's offline cases there; on macOS, in place of all of these, the host rows of client/driver/macos-hosts.sh
make tidy_top_level     # the pinned clang-tidy check over the files directly in src/, which make tidy-check runs first
make tidy_client        # the pinned clang-tidy check over the ODBC build, which has the client's sources
make tidy_driver        # the same check over src/transport/driver/ in the embedded build, which has the driver loader
make lint_tests         # the guard lint alone
make test_lint_guards   # the lint's fixture tests
make test_run_selected  # run-selected.sh's fixture tests
make test_graph_check   # the offline checks' fixture tests
make test_clone_duckdb  # make duckdb's fixture tests, the build's warning for a duckdb/ off the pin, and the checks that the tree has no submodule, the vendored extension-ci-tools Makefile is unmodified and .clang-format and .clang-tidy equal duckdb/'s when it is at the pin
make test_debug         # as `make test`, debug build
```

`make test_client_tsan` runs the sanitized tests with the pinned engine's own suppression
list, `duckdb/.sanitizer-thread-suppressions.txt`, passed in `TSAN_OPTIONS` exactly as
DuckDB's ThreadSanitizer CI passes it. Without it a run reports the engine's known races —
among them ones inside the jemalloc DuckDB links on Linux — instead of this extension's.

The pinned `format;tidy` check configures from `BUILD_FLAGS`, which carry the default
`REDSHIFT_CLIENT=mock`, and `run-clang-tidy` only checks the files in that compile database.
No client source is in a mock build, so `make tidy_client` runs the same check over the ODBC
build and belongs in the client's validation; the pinned check alone would let the client's findings
accumulate unseen. The driver loader's sources are only in the
embedded build, so `make tidy_driver` checks `src/transport/driver/` there, in `build/tidy_driver/`;
its configure fetches nothing. The pinned check's file regex, `src/.*/`, needs a slash after `src/`,
so it skips the files directly in `src/`, the extension's among them; `make tidy_top_level` checks them,
and `make tidy-check` runs it first. Run `make format-check` and `make tidy-check` before you send a
change.

`make test_driver` must run as a user other than root, outside a user namespace (not under
`unshare -r`), on a host that allows unprivileged user namespaces. Some of the driver loader's cases
check permission refusals that root bypasses, and others create a user namespace of their own to
mount a `tmpfs`, or give a thread its own descriptor table; as root, where user namespaces are
unavailable, or where `unshare` is refused (Docker's default seccomp profile), those cases print a
warning and stop, and the run still passes. The few cases that need a second real user run as
root with `test/client/driver/run-loader-tests-as-root.sh`, which runs the built suite in a
container (Docker, no network). The directory-safety cases also need a private
temporary directory: every directory above `$TMPDIR` (or `/tmp`) owned by the user or root and
not writable by others, apart from a root-owned sticky `/tmp`; elsewhere they warn and stop.

`make test_driver` runs by the platform the build records. On `linux_amd64` and `linux_arm64` it runs
the graph check, the tool tests with `RS_REQUIRE_PROBE=1` and the C++ suite. On `osx_amd64` and
`osx_arm64` it runs the same three, the tool tests without `RS_REQUIRE_PROBE` (macOS builds no probe,
so `host-probe_test.sh` skips its real-probe cases); `make test_driver_e2e` runs the macOS host rows
there (below), not `run-e2e.sh`, which reads `/proc/net/dev` for its no-network check. On `windows_amd64`, from Git's
`bash` in a Visual Studio developer shell, it runs the graph check, the interface's import-table
case against the embedded loadable extension, `embedded_resources.py` on the six images the recipe
lists that link the loader (the loadable extension, `duckdb.exe`, `unittest.exe`, `src/duckdb.dll`,
`redshift_client_tests.exe` and the C++ suite's `redshift_driver_tests.exe`, each holding the set as
`RCDATA` resources equal to `driver_info.h`), then runs that suite, and then
`test/client/driver/run-windows-loading.sh`, the cases
only a Windows host can run, each a named suite: the module-list helper's self-test; `download.py`'s
bounds and refusals over the host's sockets; `msiexec /a /qn` of the locked MSI, run only once its
SHA-256 is the lock's, into a new directory that `msi.py compare` then checks against the lock
(skipped in a logon that is not interactive, whatever its token holds, unless
`RS_REQUIRE_MSIEXEC=1`, as a `# SKIP` line naming the suite and why, counted in the run's summary;
an interactive logon holding an administrator's token is refused, so run it by hand in an
interactive logon as a non-elevated account);
`Get-AuthenticodeSignature` on the renamed `msvcr100.dll`; the loadable extension's resources,
the bytes it writes, against the lock; its export table, which must hold DuckDB's entry point for
this extension and no loader, driver or set name (a Windows build exports DuckDB's own API too,
which its headers export and the loadable links statically); the negative control of the
import-table case, a PE written at test time that
imports `KERNEL32.dll` and `ODBC32.dll`, which the case must fail on that import; and a
`LoadLibraryA` of each string the rename wrote over a bare-name load, which must map nothing
(`ERROR_INVALID_NAME` or `ERROR_MOD_NOT_FOUND`) while a fake planted under the original name beside
it loads, with no file of the rewritten name creatable there (`windows-runner-tools.sh` has their
rules,
`windows_bare_names.py judge` and `names` run the last one's rules anywhere, and
`run-windows-loading_test.sh` runs them all on Linux with stand-ins for the Windows programs). The
C++ suite's 8.3 short-name case needs a volume whose 8.3 name creation is on (some Windows
containers' volumes have it off; `fsutil 8dot3name` turns it on): elsewhere it prints a warning
saying why and skips, and `RS_REQUIRE_SHORT_NAMES=1` makes it fail instead.
The tool tests need POSIX file modes and links, and run on Linux. On any
other platform (musl, FreeBSD) the build has no embed and links the `NOT_BUILT` placeholder: the
target says so, runs the tool tests alone, and skips the graph check and the C++ suite, which has
no case there.
The suite always runs without `LD_LIBRARY_PATH` and the `DYLD_` variables the macOS loader refuses.

To run a C++ suite on its own, build it by name in a build of its client and run it
(`redshift_mock_tests` needs a mock build, `make release REDSHIFT_CLIENT=mock`; `make test_client`
leaves build/release an ODBC build). `-w NoTests` makes Catch2 fail rather than pass when no case
ran:

```bash
cmake --build build/release --target redshift_mock_tests
./build/release/extension/redshift/redshift_mock_tests -w NoTests
```

The ODBC build's `OpenRedshiftConnection` obtains the driver from the driver source when a
connection opens. `make test_client` builds with `REDSHIFT_DRIVER=none`, so a connection there
reports the `NOT_BUILT` placeholder's failure, which `client/odbc/shipped_driver_source_test.cpp`
checks in that build alone. The C++ tests in `client/odbc/` that open a connection install the fake
driver instead, through the test-only setter in `client/odbc/driver_source_hook.cpp`.
`REDSHIFT_DRIVER` (`embedded` or `none`) chooses the driver an ODBC build carries; a mock build
carries none either way.

`make test_driver_e2e` runs the embedded driver end to end through SQL, in `make driver_e2e`'s test-endpoint
`build/driver-e2e`, the one build that takes the end-to-end variables, then on Linux the C++ client suite's
offline cases there (the environment gate above). Each SQL case aims its connection at
closed ports on the host itself; run it all the same where nothing can leave the host: a container with no network, as your own
uid, or a network namespace that keeps the host's root mapped, which only root's `unshare -n` makes; not
`unshare -rn`, whose namespace does not map the host's root, so the loader refuses the system's library
directories. On Linux `run-e2e.sh` checks both, reading its own `/proc/net/dev`, and nothing turns the network check
off (so it does not run in a container that has a network). In a container,
mount the tree at the path it was built at, since the unit tester's run path is absolute, and run only the
script, for example `docker run --rm --network none --pull=never -u "$(id -u):$(id -g)" --mount
"type=bind,src=$PWD,dst=$PWD" -w "$PWD" <image> bash test/client/driver/run-e2e.sh build/driver-e2e
build/stock-shell/duckdb` after `make driver_e2e REDSHIFT_DRIVER=embedded` and `make stock_shell` (a stock
DuckDB shell of the pinned version, without this extension; with no second argument the `LOAD` case is
skipped). The driver loads and then fails the connect, refused by the closed ports (the two shell cases, which
set no end-to-end variable, by the endpoint the region derives, which does not resolve). It runs a set
load-phase variable (refused before the load), a set `ODBCINI` (a handle-phase variable, refused after the
load and before the driver's first environment handle), a current directory holding an `odbc.ini` (which
must not be read) or writable by the group or by everyone (which must connect), a relative and an absolute
`LD_LIBRARY_PATH` (and one whose only copies are in the subdirectories the platform loader tries below it), a
copy of the shell with the default bases, and the stock shell `INSTALL`ing a copy of
`redshift.duckdb_extension` and `LOAD`ing it. On Linux the recipe then runs the client suite in the same
container: `cmake --build build/driver-e2e --config Release --target redshift_client_tests`, then,
without `LD_LIBRARY_PATH` and the macOS loader's `DYLD_` variables (the Makefile's `DRIVER_SUITE_ENV`),
`REDSHIFT_REQUIRE_DRIVER=1 build/driver-e2e/extension/redshift/redshift_client_tests -w NoTests
'[odbc]~[live]'`. `run-e2e.sh` alone sets `DUCKDB_REDSHIFT_TEST_E2E`, on the unit
tester's command line; the Makefile clears and does not export it, so a value left in a shell cannot select
one of those files in `make test REDSHIFT_CLIENT=odbc`, and each of them selects itself with `require-env
DUCKDB_REDSHIFT_TEST_E2E`, `run-e2e.sh`'s fake AWS keys and `require-env-not AWS_SESSION_TOKEN` before its first
`ATTACH`, so a run by hand with real credentials skips them (a tool test, `e2e-selection_test.sh`, shows
both). `client/driver/environment_allocation_test.cpp`, in `make test_driver`, shows that the ODBC client's connection
allocates its environment handle through the loader, and that the ODBC client refuses a set variable in the loader's
words.

On `osx_amd64` and `osx_arm64`, `make test_driver_e2e` runs `client/driver/macos-hosts.sh` on
`build/driver-e2e` and `build/stock-shell/duckdb` (built for the platform's architecture, `OSX_BUILD_ARCH`):
the loadable extension in host applications, each run for the build's architecture (an `osx_amd64`
build's under Rosetta 2: a universal host thinned to its `x86_64` slice, python.org's Python through its
`-intel64` launcher) with only `PATH`, fake AWS keys and a private `HOME`, and every connect aimed at
closed ports on `127.0.0.1`, as on Windows; macOS has no `/proc/net/dev`, so nothing proves the process
has no network. Each host `INSTALL`s a copy of the extension, `LOAD`s it unsigned and connects; the
driver must load and fail the connect itself, the set must be beside the installed file, and none of it
may carry `com.apple.quarantine`. The hosts are the DuckDB CLI release of the pinned version
(`REDSHIFT_TEST_DUCKDB_CLI`), python.org Python (`REDSHIFT_TEST_PYTHON_ORG`) and Homebrew's Python
(`REDSHIFT_TEST_HOMEBREW_PYTHON`), each Python importing the pinned `duckdb` package from
`REDSHIFT_TEST_DUCKDB_PYTHON` (an unpacked wheel), and the stock shell; a host that is not given skips,
or fails with `REDSHIFT_TEST_REQUIRE_MACOS_HOSTS=1`, one that is given but cannot be read fails, and each
must be signed as its row says (the hardened runtime with `disable-library-validation` for the first two,
none for the others). Then the
stock shell as a host that opts in to quarantine (`client/driver/inputs/quarantine_self.c`, inserted with
`DYLD_INSERT_LIBRARIES`): the set's files must carry the attribute, and the driver must still load. Last,
a test host built there with Xcode's `clang++` (`client/driver/inputs/weak_host.cpp`, with
`weak_before.c` and `weak_after.c`) shows what weak-definition coalescing reaches between the driver and
the process's other images, as each host row does for that host: `client/driver/inputs/weak_slots.c`,
inserted with `DYLD_INSERT_LIBRARIES`, reads at exit where each of the driver's weak binds went
(`client/driver/macos_bindings.py` gives the slots and sums up), and the test host's case fails unless
each library's weak bind kept its own definition and the driver's operator `new` and `delete` binds all went
to the test host's; with `REDSHIFT_TEST_FS_USAGE=sudo` the same run is traced
with `fs_usage` under `sudo -n` (it needs root), and `client/driver/macos_config_files.py` holds every path
the process opened or probed to `policy/config-files.txt`; without it that case skips, or fails with
`REDSHIFT_TEST_REQUIRE_MACOS_HOSTS=1`.

On `windows_amd64`, from Git's `bash` with a 64-bit Python 3 as `python3`, `make test_driver_e2e`
runs `run-e2e.sh`'s Windows branch with `build/stock-shell/duckdb.exe`. Windows has no
`/proc/net/dev` and no `unshare`, so the run prints that it has no no-network proof, and every case,
the shell cases too, aims both endpoints at closed ports on `127.0.0.1` (`DUCKDB_REDSHIFT_TEST_HOST`
and `DUCKDB_REDSHIFT_TEST_ENDPOINT_URL` may name other closed ports there, and nothing else). Its
folder is under the long form of `GetTempPathW()`, or under `REDSHIFT_E2E_RUNNER_DIR`, and every
path it hands a Windows program is `cygpath -w -l`'s; each must be ASCII, have no `~` and be at
most 120 characters, and a path outside those rules fails the run before any case, saying which. It
runs `no-network` and
`refused-variable` (`KRB5_CONFIG` set before the tester starts), a copy of the build's shell with
the default bases, `planted-dlls` (copies of System32's `version.dll`, a real PE, since Windows
maps no other file, under the set's original and generated names and as `krbcc64.dll`,
`leashw64.dll`, `xpprof64.dll`, `DbgHelp.dll` and `Secur32.dll`, beside the copy, in the current
folder and first on `PATH`; no module may be at a planted path or have the copy's SHA-256, other
than System32's `version.dll` itself), and the stock shell `INSTALL`ing and `LOAD`ing the loadable
extension into a private `extension_directory`, twice: the second run, in a new process with the
same profile and extension directory, must find the `trust\<sha256>.pem` the first wrote, write no
other and leave the set the first wrote as it was. With `REDSHIFT_E2E_ADMIN_DIR` naming a read-only
`REDSHIFT_DRIVER_DIR` an administrator provisioned, which already holds the set, one more case
loads from it and must put its trust file in the per-user cache's `trust` folder instead. Only a
standard account can run it, since an administrator's elevated token can write the tree. Without
that variable the case skips, saying so; `REDSHIFT_E2E_REQUIRE_ADMIN_DIR=1`, which a host whose
administrator provisioned such a tree sets, fails the run before any case instead. No run makes,
fills or changes such a tree. In each shell
case `windows_modules.py modules`
lists the shell's modules: each set file is loaded under its generated name from the case's
`.redshift-driver`, none under an original name and no planted file, `trust\<sha256>.pem` is beside
`.redshift-driver`, and
the case writes nothing into the `trust` folder of the per-user cache its private profile resolves,
except that read-only tree's case, whose trust file belongs there and which the run then removes.
`run-e2e_test.sh` runs the branch on Linux with stand-ins for Git's tools and the shells.
