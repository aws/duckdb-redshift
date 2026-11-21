#!/usr/bin/env bash
# run-e2e.sh <build directory> [stock DuckDB shell]
# On Windows, from Git's bash, it runs the Windows cases described after the build checks.
# The extension end to end through SQL, offline (make test_driver_e2e), on make driver_e2e's test-endpoint build
# with the embedded driver (build/driver-e2e), the one build whose connect takes the end-to-end variables. Each
# SQL case aims both endpoints at closed ports on this host; it must still run where nothing can leave it: a
# container with no network (docker or podman run --network none, as your own uid), or a network namespace that
# keeps the host's root mapped (a root `unshare -n`); not `unshare -rn`, which maps only you, so the system's
# library directories show another owner and the loader refuses them. It checks both. Runs
# the SQL tests in test/client/driver/sql/, each in a clean environment with fake AWS keys, a fresh
# REDSHIFT_DRIVER_DIR and a current directory of its own:
#   no-network        the driver loads and fails the connect itself, refused by the closed ports (twice: a
#                     failure after the load is not stored for the connect), and so do a cluster
#                     endpoint's attach, whose Host, Port and ClusterId the driver takes, and a workgroup
#                     URI's with a port, whose key set with Port the driver takes;
#   refused-variable  with KRB5_CONFIG (a load-phase name) set, the connection is refused before the driver
#                     is loaded or anything is written;
#   odbcini-variable  with ODBCINI naming an odbc.ini that turns the driver's logging on, the driver loads, and
#                     the client's own check refuses the variable before the driver's first environment
#                     handle (a handle-phase name), and the log directory stays empty: neither the load nor
#                     the refusal reads the file;
#   odbc-ini          from a directory that holds an odbc.ini turning the driver's logging on (LogLevel
#                     and a LogPath to an empty directory), the connect goes on to fail the same way, twice,
#                     and the log directory stays empty: every environment handle is allocated with
#                     ODBCINI=/dev/null;
#   group-writable-cwd, writable-cwd  from a directory the group (0770), or every user (1777, as
#                     /tmp), can write, the connect goes on as well;
#   library-path      with a relative LD_LIBRARY_PATH element, the connection is refused before the
#                     driver is loaded or anything is written;
#   absolute-library-path  with LD_LIBRARY_PATH on a directory everyone can write that holds files, which are
#                     no libraries, under the names of the libraries the driver needs (but not those the case's
#                     own programs start with), the connect goes on: those are loaded first from the library
#                     cache's paths.
#   library-path-subdirectories  the same files only in the subdirectories the platform loader tries below the
#                     element (glibc-hwcaps levels, tls, the platform and hardware-capability directories,
#                     `atomics` among them): the connect goes on as well.
# The cases refused before the load also check that the driver directory stayed empty. Then a private copy of
# the shell, which has the extension linked in, runs without REDSHIFT_DRIVER_DIR, so the default bases (the
# directory of the file the extension is mapped from, here the copy's) are used, and the build's shell runs
# tools/extension-coexistence.sh, the coexistence harness (its own rules are extension-coexistence_test.sh's,
# with stand-in CLIs); and, given a stock DuckDB shell, the loadable extension is INSTALLed under a private
# HOME and LOADed, so the base is DuckDB's own extension directory, and the harness runs again with that shell
# and the loadable extension (json and icu are built into it too, so this checks the harness, not the load
# order with separately built extensions). Those shell cases and the harness (which runs its CLI under env -i)
# set no end-to-end variable, so they go to the endpoint the region derives, which with no network does not
# resolve; the two default-base cases check that host.
set -euo pipefail
here="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
root="$(cd "$here/../../.." && pwd)"
build="$(cd "${1:?usage: run-e2e.sh <build directory> [stock DuckDB shell]}" && pwd)"
stock_shell="${2:-}" # a DuckDB shell of the same version without this extension linked in (make stock_shell)
# Git's bash on Windows, whose programs end in .exe; the Windows cases follow the build checks.
windows=false exe=""
case "$(uname -s)" in MINGW* | MSYS*) windows=true exe=".exe" ;; esac
unittest="$build/test/unittest$exe"
loadable="$build/extension/redshift/redshift.duckdb_extension"
shell_binary="$build/duckdb$exe"
fail() { echo "run-e2e: $*" >&2; exit 1; }
if [[ -n "$stock_shell" ]]; then
  [[ -x "$stock_shell" && -f "$stock_shell" ]] || fail "$stock_shell is not an executable DuckDB shell; run make stock_shell"
  stock_shell="$(cd "$(dirname "$stock_shell")" && pwd)/$(basename "$stock_shell")" # absolute: the case runs from another directory
fi
[[ -x "$unittest" && -f "$loadable" ]] || fail "$build has no unittest or loadable extension; run make driver_e2e REDSHIFT_DRIVER=embedded"
grep -qs '^REDSHIFT_CLIENT:STRING=odbc' "$build/CMakeCache.txt" && grep -qs '^REDSHIFT_DRIVER:STRING=embedded' "$build/CMakeCache.txt" &&
  grep -qs '^REDSHIFT_TEST_ENDPOINTS:BOOL=ON' "$build/CMakeCache.txt" ||
  fail "$build is not an embedded ODBC build with the test-endpoint switch; run make driver_e2e REDSHIFT_DRIVER=embedded"
# Windows (make test_driver_e2e's windows_amd64 branch), from Git's bash with a 64-bit Python 3
# as python3. There is no /proc/net/dev and no unshare, so the run has no no-network proof, and
# prints that it has none. Every case, the shell cases too, aims both endpoints at closed ports on
# 127.0.0.1: the caller may name other closed ports in DUCKDB_REDSHIFT_TEST_HOST and
# DUCKDB_REDSHIFT_TEST_ENDPOINT_URL, and any other host fails the run before any case. The run's
# folder is under the long form of GetTempPathW(), or under REDSHIFT_E2E_RUNNER_DIR, a folder the
# caller names, never Git's /tmp (which can hold an 8.3 name, and which MSYS converts in a
# variable's value but not inside an argument). Each path handed to a Windows program is cygpath's
# long Windows form, and before any case each must be ASCII, have no ~ (an 8.3 name) and fit the
# path limit, so a path that breaks these rules fails as one and not as a loader refusal; the
# loader's own rules are its tests'. Each case runs under env -i with fake keys, SystemRoot, WINDIR
# and TEMP (which Winsock and CNG need) and a private USERPROFILE:
#   no-network, refused-variable  the SQL tests, as on Linux, with REDSHIFT_DRIVER_DIR set (the
#                     second with KRB5_CONFIG, a load-phase name, set before the tester starts);
#   static-shell-default-base  a private copy of the build's shell: the set is written beside it;
#   planted-dlls      the same from a second copy, with a copy of System32's version.dll, a DLL
#                     Windows would load, under each set file's original and generated names and
#                     as krbcc64.dll, leashw64.dll, xpprof64.dll, DbgHelp.dll and Secur32.dll,
#                     beside the copy, in the current folder and in a folder first on PATH: no
#                     module may be at a planted path or have the planted copy's SHA-256;
#   loadable-extension  the stock shell INSTALLs a copy of the loadable extension into a private
#                     extension_directory and LOADs it: the set is written beside the installed
#                     file. It then runs a second time, in a new process with the same profile and
#                     extension directory (loadable-extension-again), which must find the trust
#                     file the first wrote, write no other and leave the set the first wrote as it
#                     was;
#   read-only-admin-tree  with REDSHIFT_DRIVER_DIR naming a read-only tree an administrator
#                     provisioned, which already holds the set and which this account cannot
#                     write, the load uses that set and the trust file goes to the per-user cache's
#                     trust folder. Only a standard account can show that: an administrator's
#                     elevated token can write the tree. The tree is a per-machine fixture, so the
#                     case skips, saying so, unless REDSHIFT_E2E_ADMIN_DIR names it; a host whose
#                     administrator provisioned one names it and sets
#                     REDSHIFT_E2E_REQUIRE_ADMIN_DIR=1, which fails the run before any case when
#                     no tree is named. The run never makes, fills or changes one.
# A shell case must fail the connect with the driver's own diagnostic. windows_modules.py lists the
# shell's modules after it: each set file loaded once, under its generated name (the rename rows of
# the build's lock), from the case's .redshift-driver, and none under an original name; when the
# build's driver_info.h has the loader check DbgHelp.dll, exactly one DbgHelp.dll, System32's. The
# trust file, trust\<its SHA-256>.pem, is beside .redshift-driver, and the case created nothing in
# the per-user cache's trust folder (what it did create is removed, unless a name holds a newline in
# a folder already there).
if $windows; then
  for tool in cygpath python3 sha256sum; do
    command -v "$tool" >/dev/null || fail "$tool is required on Windows and was not found on PATH"
  done
  test_host="${DUCKDB_REDSHIFT_TEST_HOST:-127.0.0.1}"
  test_endpoint_url="${DUCKDB_REDSHIFT_TEST_ENDPOINT_URL:-https://127.0.0.1:9}"
  closed_port="every case must aim at a closed port on 127.0.0.1"
  [[ "$test_host" == 127.0.0.1 ]] || fail "DUCKDB_REDSHIFT_TEST_HOST names $test_host; $closed_port"
  [[ "$test_endpoint_url" =~ ^https://127\.0\.0\.1:[0-9]+$ ]] ||
    fail "DUCKDB_REDSHIFT_TEST_ENDPOINT_URL is $test_endpoint_url; $closed_port"
  system_root="${SYSTEMROOT:-${SystemRoot:-}}"
  windows_dir="${WINDIR:-${windir:-}}"
  [[ -n "$system_root" && -n "$windows_dir" ]] || fail "SystemRoot or WINDIR is not set"
  # The build's own record of whether the loader loads and checks System32's DbgHelp.dll.
  driver_info="$build/extension/redshift/redshift_driver/driver_info.h"
  [[ -f "$driver_info" ]] ||
    fail "the build has no $driver_info; run make driver_e2e REDSHIFT_DRIVER=embedded"
  system_dbghelp=false
  # Python on Windows writes the header and the lock rows with CRLF line ends; each read drops the CR.
  if tr -d '\r' <"$driver_info" | grep -qx '#define REDSHIFT_DRIVER_SYSTEM_DBGHELP 1'; then
    system_dbghelp=true
  fi
  echo "run-e2e: no no-network proof: Windows has no /proc/net/dev or unshare; every case aims" \
    "both endpoints at closed ports on 127.0.0.1 ($test_endpoint_url)"
  # The set's names, from the build's copy of the lock: each file's original and generated name.
  lock_rows="$(bash "$root/third_party/redshift_odbc/tools/check-lock.sh" \
    "$build/extension/redshift/redshift_driver.lock")" ||
    fail "the build's lock fails check-lock.sh"
  lock_rows="${lock_rows//$'\r'/}"
  originals=() generated=()
  while read -r kind platform name embedded_name _; do
    [[ "$platform" == windows_amd64 ]] || continue
    case "$kind" in
    driver | library) originals+=("$name") ;;
    rename) generated+=("$embedded_name") ;;
    esac
  done <<<"$lock_rows"
  ((${#originals[@]} > 0 && ${#generated[@]} == ${#originals[@]})) ||
    fail "the build's lock does not rename each windows_amd64 set file, so no load shows a new name"
  folders="$(python3 -I "$here/windows_modules.py" folders | tr -d '\r')" ||
    fail "windows_modules.py could not read the temporary folder or the per-user cache"
  temp_folder="$(sed -n 1p <<<"$folders")"
  base="${REDSHIFT_E2E_RUNNER_DIR:-$temp_folder}"
  work="$(mktemp -d "$(cygpath -u "$base")/run-e2e.XXXXXXXX")" ||
    fail "cannot make a folder under $base"
  trap 'rm -rf -- "${work:?}"' EXIT
  # win <path>: the existing <path> in long Windows form, as a Windows program is given it.
  win() { cygpath -w -l "$1"; }
  # lower <text>: <text> in lower case. Like the parallel arrays below, it keeps the branch to bash
  # 3.2, which macOS's tool tests run it under through stand-ins.
  lower() { printf '%s' "$1" | tr '[:upper:]' '[:lower:]'; }
  # W <name>: $work/<name> in Windows form, from w_names and w_paths; each is handed to a Windows
  # program, and checked below.
  w_names=() w_paths=()
  W() {
    local i
    for i in "${!w_names[@]}"; do
      [[ "${w_names[i]}" != "$1" ]] || { printf '%s' "${w_paths[i]}"; return; }
    done
    fail "no runner path is named $1"
  }
  for name in driver-no-network driver-refused-variable profile-no-network \
    profile-refused-variable cwd-no-network cwd-refused-variable copy copy-planted \
    cwd-static-shell-default-base cwd-planted-dlls planted-path profile-static-shell-default-base \
    profile-planted-dlls profile-loadable-extension cwd-loadable-extension extensions loadable \
    cwd-admin-tree profile-admin-tree; do
    mkdir "$work/$name"
    # A profile's per-user cache must resolve, as in a real profile: Windows expands
    # %USERPROFILE%\AppData\Local, which SHGetKnownFolderPath refuses when it is missing.
    [[ "$name" != profile-* ]] || mkdir -p "$work/$name/AppData/Local"
    w_names+=("$name") w_paths+=("$(win "$work/$name")")
  done
  cp "$loadable" "$work/loadable/"
  w_names+=(loadable-file work)
  w_paths+=("$(win "$work/loadable/redshift.duckdb_extension")" "$(win "$work")")
  # The administrator-provisioned tree, where the caller names one: the
  # read-only case hands it to the shell, so the rule below checks its path with the run's own.
  # REDSHIFT_E2E_REQUIRE_ADMIN_DIR=1 makes a run that names none fail here, before any case, rather
  # than skip that case; unset, empty or 0 lets it skip.
  case "${REDSHIFT_E2E_REQUIRE_ADMIN_DIR:-}" in
  '' | 0) ;;
  1)
    [[ -n "${REDSHIFT_E2E_ADMIN_DIR:-}" ]] ||
      fail "REDSHIFT_E2E_REQUIRE_ADMIN_DIR=1, but REDSHIFT_E2E_ADMIN_DIR names no" \
        "administrator-provisioned, read-only REDSHIFT_DRIVER_DIR, so read-only-admin-tree" \
        "cannot run"
    ;;
  *)
    fail "REDSHIFT_E2E_REQUIRE_ADMIN_DIR is '$REDSHIFT_E2E_REQUIRE_ADMIN_DIR'; it is 1 to require" \
      "the read-only tree's case, or unset, empty or 0 to let it skip"
    ;;
  esac
  admin_tree=""
  if [[ -n "${REDSHIFT_E2E_ADMIN_DIR:-}" ]]; then
    admin_tree="$(cygpath -u "$REDSHIFT_E2E_ADMIN_DIR")"
    [[ -d "$admin_tree" ]] ||
      fail "REDSHIFT_E2E_ADMIN_DIR names $REDSHIFT_E2E_ADMIN_DIR, which is not a folder"
    w_names+=(admin-tree) w_paths+=("$(win "$admin_tree")")
  fi
  # Leaves room, within 259 characters, for what the run and the loader add below a path: an
  # extension directory's version and platform folders, the set's folder and its longest name.
  runner_path_max=120
  for i in "${!w_names[@]}"; do
    name="${w_names[i]}" path="${w_paths[i]}" rule=""
    if [[ "$path" == *[![:ascii:]]* ]]; then rule="is not ASCII"
    elif [[ "$path" == *'~'* ]]; then rule="has a ~ in a component (an 8.3 short name)"
    elif ((${#path} > runner_path_max)); then rule="is longer than $runner_path_max characters"
    fi
    [[ -z "$rule" ]] || fail "runner path: $path ($name) $rule. Each path the run hands a" \
      "Windows program must be ASCII, with no 8.3 name, and at most $runner_path_max characters," \
      "so that no loader rule decides a case; set REDSHIFT_E2E_RUNNER_DIR to a folder whose" \
      "long form is"
  done
  case_env=(SystemRoot="$system_root" WINDIR="$windows_dir" TEMP="$temp_folder"
    DUCKDB_REDSHIFT_TEST_HOST="$test_host" DUCKDB_REDSHIFT_TEST_ENDPOINT_URL="$test_endpoint_url"
    AWS_ACCESS_KEY_ID=test-not-a-key AWS_SECRET_ACCESS_KEY=test-not-a-secret AWS_REGION=us-east-1
    AWS_EC2_METADATA_DISABLED=true)

  # win_run <case> [VAR=value...]: the SQL tests for one case, as run() does on Linux, from a
  # folder that holds a copy of them (Git's bash copies a link it is asked to make).
  win_run() {
    local case="$1" tester="$work/unittest-$1"
    shift
    echo "run-e2e: $case"
    mkdir -p "$work/cwd-$case/test/client/driver"
    cp -R "$root/test/client/driver/sql" "$work/cwd-$case/test/client/driver/"
    printf '#!/usr/bin/env bash\nexec %q --test-dir %q "$@"\n' "$unittest" "$(W "cwd-$case")" \
      > "$tester"
    chmod 700 "$tester"
    (cd "$work/cwd-$case" && env -i PATH="$PATH" "${case_env[@]}" \
      USERPROFILE="$(W "profile-$case")" REDSHIFT_TEST_CLIENT=odbc \
      DUCKDB_REDSHIFT_TEST_E2E="$case" \
      REDSHIFT_DRIVER_DIR="$(W "driver-$case")" "$@" \
      bash "$root/test/run-selected.sh" "$tester" "test/client/driver/sql/*")
  }
  win_run no-network
  [[ -d "$work/driver-no-network/.redshift-driver" ]] ||
    fail "no-network: the driver set was not written"
  win_run refused-variable KRB5_CONFIG="$(W work)\\user-krb5.conf"
  [[ -z "$(ls -A "$work/driver-refused-variable")" ]] ||
    fail "refused-variable: the driver directory was written to, so the refusal came too late"

  connect_sql="$(< "$root/third_party/redshift_odbc/tools/connect.sql")"
  connect_failed='Could not connect to Redshift at 127\.0\.0\.1 \(region us-east-1, workgroup '
  connect_failed+='[^)]+\): the connection attempt failed\.'
  # comparable <path>: a Windows path as Windows compares it: backslashes, no \\?\, lower case.
  comparable() {
    local path="${1//\//\\}"
    path="${path#\\\\\?\\}"
    lower "$path"
    echo
  }
  # cache_entries <folder>: the folder and what is in it, sorted.
  cache_entries() { { find "$1" -maxdepth 1 2>/dev/null || true; } | LC_ALL=C sort; }
  # shell_case <case> <PATH> <shell> [<argument>...]: runs the shell from cwd-<case> on
  # $work/<case>.sql through windows_modules.py, which lists its modules into `modules`, in the
  # form comparable gives, and their files, in POSIX form, into `module_files`. The folders are
  # $folders_of's, the case's own unless the caller names an earlier case, so a second run can use
  # the profile, extension directory and trust folder the first made.
  modules=() module_files=() folders_of="" expect_cache_trust=false case_cache_entries=""
  case_env_extra=()
  shell_case() {
    local case="$1" path="$2" where="${folders_of:-$1}" cache_trust before created entry status=0
    shift 2
    echo "run-e2e: $case"
    : > "$work/$case.modules"
    : > "$work/$case.out"
    # The per-user cache the loader finds in this case: the same known-folder call, made under the
    # case's own environment, whose private USERPROFILE it can follow.
    cache_trust="$(cd "$work/cwd-$where" && env -i PATH="$path" "${case_env[@]}" \
      "${case_env_extra[@]+"${case_env_extra[@]}"}" \
      USERPROFILE="$(W "profile-$where")" python3 -I "$(win "$here/windows_modules.py")" folders |
      tr -d '\r' | sed -n 2p)" && [[ -n "$cache_trust" ]] ||
      fail "$case: windows_modules.py could not read the case's per-user cache"
    cache_trust="$(cygpath -u "$cache_trust")/trust"
    before="$(cache_entries "$cache_trust")"
    (cd "$work/cwd-$where" && env -i PATH="$path" "${case_env[@]}" \
      "${case_env_extra[@]+"${case_env_extra[@]}"}" \
      USERPROFILE="$(W "profile-$where")" python3 -I "$(win "$here/windows_modules.py")" modules \
      "$(win "$work/$case.modules")" "$(win "$work/$case.out")" "$(win "$work/$case.sql")" \
      "$(win "$1")" "${@:2}") || status=$?
    # Removed even when the run failed, so a file it left cannot hide the same write later.
    created="$(LC_ALL=C comm -13 <(printf '%s\n' "$before") <(cache_entries "$cache_trust"))"
    # The cache's trust folder as this case leaves it, recorded before the removal below, for the
    # one case whose trust file belongs there. The case's profile is its own, so what is there is
    # what it wrote.
    case_cache_entries="$(trust_entries "$(dirname "$cache_trust")")"
    if [[ -n "$created" ]]; then
      # Only paths in the trust folder: a name holding a newline lists as two lines, the second a
      # bare name that rm would take from the working directory. So such a name stays in a trust
      # folder that was already there; a folder the run created lists as itself and goes whole.
      while IFS= read -r entry; do
        [[ "$entry" != "$cache_trust" && "$entry" != "$cache_trust"/* ]] || rm -rf -- "$entry"
      done <<<"$created"
    fi
    ((status == 0)) || fail "$case: the shell's run failed: $(tr '\n' ' ' < "$work/$case.out")"
    # The read-only administrator tree's case is the one that writes there: that tree takes no
    # trust file, so the per-user cache's trust folder is the only place left.
    $expect_cache_trust || [[ -z "$created" ]] ||
      fail "$case: the run wrote into the per-user cache's trust folder" \
        "(removed, unless a name holds a newline in a folder already there): ${created//$'\n'/, }"
    grep -qE "$connect_failed" "$work/$case.out" ||
      fail "$case: the driver did not load and fail the connect: $(tr '\n' ' ' < "$work/$case.out")"
    modules=() module_files=()
    while IFS= read -r module; do
      modules+=("$(comparable "$module")") module_files+=("$(cygpath -u "${module#\\\\\?\\}")")
    done < <(tr -d '\r' < "$work/$case.modules")
  }
  # trust_entries <base>: each file in <base>'s trust folder, by name and SHA-256, sorted.
  trust_entries() {
    local file
    for file in "$1"/trust/*; do
      if [[ -f "$file" ]]; then
        printf '%s %s\n' "${file##*/}" "$(sha256sum < "$file" | cut -d' ' -f1)"
      fi
    done | LC_ALL=C sort
  }
  # driver_root_entries <base>: what <base>'s .redshift-driver holds, two levels down, sorted.
  driver_root_entries() {
    { find "$1/.redshift-driver" -mindepth 1 -maxdepth 2 2>/dev/null || true; } | LC_ALL=C sort
  }
  # set_loaded <case> <base> [<trust base>]: each set file loaded once under its generated name
  # from <base>'s .redshift-driver, none under an original name, and trust\<its SHA-256>.pem in
  # <trust base>, <base> by default. An empty <trust base> leaves the trust file to the caller:
  # the read-only tree's goes to the per-user cache, where shell_case removed what the case wrote,
  # so the caller checks that case's own record of it instead.
  set_loaded() {
    local set_root name module count file trusted=false
    [[ -d "$2/.redshift-driver" ]] || fail "$1: the set was not written to $2"
    set_root="$(comparable "$(win "$2/.redshift-driver")")"
    for name in "${generated[@]}"; do
      count=0
      for module in "${modules[@]}"; do
        # <set root>\<set hash>\<name>
        if [[ "${module##*\\}" == "$(lower "$name")" && "${module%\\*\\*}" == "$set_root" ]]; then
          count=$((count + 1))
        fi
      done
      ((count == 1)) || fail "$1: $name is loaded $count times from a set in $set_root, not once"
    done
    for module in "${modules[@]}"; do
      for name in "${originals[@]}"; do
        [[ "${module##*\\}" != "$(lower "$name")" ]] ||
          fail "$1: $module is loaded under the set's original name"
      done
    done
    if $system_dbghelp; then
      count=0
      for module in "${modules[@]}"; do
        [[ "${module##*\\}" != dbghelp.dll ]] || count=$((count + 1))
      done
      ((count == 1)) || fail "$1: DbgHelp.dll is loaded $count times, not once"
      system_dbghelp_path="$(comparable "$system_root\\System32")\\dbghelp.dll"
      for module in "${modules[@]}"; do
        [[ "${module##*\\}" != dbghelp.dll || "$module" == "$system_dbghelp_path" ]] ||
          fail "$1: $module is loaded, not System32's DbgHelp.dll"
      done
    fi
    if [[ $# -lt 3 || -n "$3" ]]; then
      for file in "${3:-$2}"/trust/*.pem; do
        if [[ -f "$file" && "${file##*/}" == "$(sha256sum < "$file" | cut -d' ' -f1).pem" ]]; then
          trusted=true
        fi
      done
      $trusted || fail "$1: no trust\\<SHA-256>.pem in ${3:-$2}"
    fi
  }
  # quoted <text>: <text> as the inside of an SQL string literal.
  quoted() {
    local quote="'"
    printf '%s' "${1//$quote/$quote$quote}"
  }

  printf '%s\n' "$connect_sql" > "$work/static-shell-default-base.sql"
  cp "$shell_binary" "$work/copy/"
  shell_case static-shell-default-base "$PATH" "$work/copy/duckdb.exe"
  set_loaded static-shell-default-base "$work/copy"

  printf '%s\n' "$connect_sql" > "$work/planted-dlls.sql"
  cp "$shell_binary" "$work/copy-planted/"
  # A file Windows would map, as it does not map a text file: a bare-name load that reached one
  # would load it.
  benign="$(cygpath -u "$system_root\\System32\\version.dll")"
  [[ -f "$benign" ]] || fail "planted-dlls: no $benign to plant"
  benign_sha="$(sha256sum < "$benign" | cut -d' ' -f1)"
  stubs=()
  for dir in copy-planted cwd-planted-dlls planted-path; do
    for name in "${originals[@]}" "${generated[@]}" krbcc64.dll leashw64.dll xpprof64.dll \
      DbgHelp.dll Secur32.dll; do
      cp "$benign" "$work/$dir/$name"
      stubs+=("$(comparable "$(win "$work/$dir/$name")")")
    done
  done
  shell_case planted-dlls "$work/planted-path:$PATH" "$work/copy-planted/duckdb.exe"
  for module in "${modules[@]}"; do
    for stub in "${stubs[@]}"; do
      [[ "$module" != "$stub" ]] || fail "planted-dlls: the shell loaded the planted $module"
    done
  done
  for i in "${!module_files[@]}"; do
    [[ ! -f "${module_files[i]}" ||
      "$(sha256sum < "${module_files[i]}" | cut -d' ' -f1)" != "$benign_sha" ||
      "$(comparable "${modules[i]}")" == "$(comparable "$system_root\\System32\\version.dll")" ]] ||
      fail "planted-dlls: the shell loaded a planted copy as ${modules[i]}"
  done
  set_loaded planted-dlls "$work/copy-planted"

  if [[ -n "$stock_shell" ]]; then
    printf "SET extension_directory = '%s';\nINSTALL '%s';\nLOAD redshift;\n%s\n" \
      "$(quoted "$(W extensions)")" "$(quoted "$(W loadable-file)")" "$connect_sql" \
      > "$work/loadable-extension.sql"
    shell_case loadable-extension "$PATH" "$stock_shell" -unsigned
    installed=("$work/extensions"/*/*/redshift.duckdb_extension)
    [[ ${#installed[@]} == 1 && -f "${installed[0]}" ]] ||
      fail "loadable-extension: INSTALL did not put one copy in the private extension directory"
    [[ ! -e "$work/loadable/.redshift-driver" ]] ||
      fail "loadable-extension: the set was written beside the copy INSTALL read"
    set_loaded loadable-extension "$(dirname "${installed[0]}")"

    # The second-process case: a new process, with the profile, extension directory and trust
    # folder the first case left, finds the trust file the first wrote and writes no other, and
    # uses the set the first wrote as it is: a set it judged invalid would be set aside and written
    # again in the same set-hash folder, which the module list alone cannot tell apart.
    installed_dir="$(dirname "${installed[0]}")"
    trust_before="$(trust_entries "$installed_dir")"
    set_before="$(driver_root_entries "$installed_dir")"
    : > "$work/loadable-extension-again.mark"
    cp "$work/loadable-extension.sql" "$work/loadable-extension-again.sql"
    folders_of=loadable-extension
    shell_case loadable-extension-again "$PATH" "$stock_shell" -unsigned
    folders_of=""
    set_loaded loadable-extension-again "$installed_dir"
    trust_after="$(trust_entries "$installed_dir")"
    [[ "$trust_after" == "$trust_before" ]] ||
      fail "loadable-extension-again: the second process did not reuse the trust file the first" \
        "wrote: the trust folder held ${trust_before//$'\n'/, } and now holds" \
        "${trust_after//$'\n'/, }"
    set_after="$(driver_root_entries "$installed_dir")"
    set_changed="$(find "$installed_dir/.redshift-driver" -mindepth 1 -maxdepth 2 \
      -newer "$work/loadable-extension-again.mark" | LC_ALL=C sort)"
    [[ "$set_after" == "$set_before" && -z "$set_changed" ]] ||
      fail "loadable-extension-again: the second process did not reuse the set the first wrote:" \
        "the driver root held ${set_before//$'\n'/, } and now holds ${set_after//$'\n'/, }," \
        "changed since the first case: ${set_changed:-nothing}"

  else
    echo "run-e2e: loadable-extension # SKIP no stock DuckDB shell given (make stock_shell" \
      "builds one)"
  fi

  # The read-only case: with REDSHIFT_DRIVER_DIR naming a read-only tree an administrator
  # provisioned, which already holds the set, the load uses it and the trust file goes to the
  # per-user cache's trust folder, the only place left. The tree is a per-machine fixture: this run
  # never makes, fills or changes one, and the case runs only where the caller names it
  # (REDSHIFT_E2E_ADMIN_DIR), as its SKIP line says, and only as a standard account, which cannot
  # write the tree.
  if [[ -z "$admin_tree" ]]; then
    echo "run-e2e: read-only-admin-tree # SKIP REDSHIFT_E2E_ADMIN_DIR names no" \
      "administrator-provisioned, read-only REDSHIFT_DRIVER_DIR; a run as a standard account on a" \
      "host whose administrator provisioned one names it, with REDSHIFT_E2E_REQUIRE_ADMIN_DIR=1"
  else
    [[ -d "$admin_tree/.redshift-driver" ]] ||
      fail "read-only-admin-tree: $REDSHIFT_E2E_ADMIN_DIR holds no .redshift-driver, so it is" \
        "not a provisioned tree; its administrator fills it, and this run must not"
    # Read-only for this account, which is what the case is about. The probe's name is new each
    # run, so a file left by an earlier one cannot make a writable tree look read-only, and the
    # probe leaves nothing.
    if probe="$(mktemp "$admin_tree/.redshift-e2e-write-probe.XXXXXX" 2>/dev/null)"; then
      rm -f -- "$probe"
      fail "read-only-admin-tree: this account can write in $REDSHIFT_E2E_ADMIN_DIR, so it is" \
        "not a read-only provisioned tree; run as a standard account, since an administrator's" \
        "elevated token can write it"
    fi
    admin_before="$(driver_root_entries "$admin_tree")"
    : > "$work/read-only-admin-tree.mark"
    printf '%s\n' "$connect_sql" > "$work/read-only-admin-tree.sql"
    folders_of=admin-tree expect_cache_trust=true
    case_env_extra=(REDSHIFT_DRIVER_DIR="$(W admin-tree)")
    shell_case read-only-admin-tree "$PATH" "$work/copy/duckdb.exe"
    folders_of="" expect_cache_trust=false case_env_extra=()
    # The load used the tree as it was: the same names, nothing newer than a mark taken before it.
    admin_after="$(driver_root_entries "$admin_tree")"
    admin_changed="$(find "$admin_tree/.redshift-driver" -mindepth 1 -maxdepth 2 \
      -newer "$work/read-only-admin-tree.mark" | LC_ALL=C sort)"
    [[ "$admin_after" == "$admin_before" && -z "$admin_changed" ]] ||
      fail "read-only-admin-tree: the case changed the provisioned tree: its driver root held" \
        "${admin_before//$'\n'/, } and now holds ${admin_after//$'\n'/, }, changed since the case" \
        "began: ${admin_changed:-nothing}"
    # The trust file is in the per-user cache, named by its own SHA-256, and is the only thing
    # the case wrote there.
    [[ "$(printf '%s\n' "$case_cache_entries" | grep -c .)" == 1 &&
      "${case_cache_entries%% *}" == "${case_cache_entries##* }.pem" ]] ||
      fail "read-only-admin-tree: the case left ${case_cache_entries:-nothing} in the per-user" \
        "cache's trust folder, not one trust\\<SHA-256>.pem"
    set_loaded read-only-admin-tree "$admin_tree" ""
  fi
  echo "run-e2e: passed"
  exit 0
fi
# Offline, shown and not assumed: the driver's connect path makes a credentials call to the endpoint the region
# derives. The check reads this process's own /proc/net/dev and nothing turns it off; every case also runs under
# env -i with fake keys, which each SQL file requires.
env -u REDSHIFT_ALLOW_NETWORK bash "$root/test/client/odbc/check-no-network.sh" /proc/net/dev >/dev/null 2>&1 ||
  fail "this process has a network. Run it in a container with no network (docker or podman run --network none, as your uid), or in a network namespace that keeps the host's root mapped (a root 'unshare -n')"
# The loader refuses the directories of the libraries it loads first, which are the system's, when they
# show an owner other than root, which every case that loads the driver would then meet: a user namespace that does not map the host's
# root makes them show the overflow owner, as `unshare -rn` does. (A rootless container is fine: its image's
# files belong to the container's root.)
overflow_owner="$(cat /proc/sys/kernel/overflowuid 2>/dev/null || echo 65534)"
if [[ "$(stat -c %u /usr 2>/dev/null || true)" == "$overflow_owner" ]]; then
  fail "the system's directories show the overflow owner here, so this process runs in a user namespace that does not map the host's root (as under 'unshare -rn') and the loader would refuse its library directories. Use a container with no network (docker or podman run --network none, as your uid), or a network namespace made by root ('unshare -n')"
fi
work="$(mktemp -d)"
work="$(cd "$work" && pwd -P)" # resolved: REDSHIFT_DRIVER_DIR is refused with a link in its path
cleanup() {
  # Directories only, and find does not follow the links this script plants (GNU chmod has no -h or -P).
  find "${work:?}" -type d -exec chmod u+rwx -- {} + 2>/dev/null || true
  rm -rf -- "${work:?}"
}
trap cleanup EXIT
chmod 700 "$work"

# run <case> [VAR=value...]: the SQL tests for one case. The unit tester runs in a directory of its
# own, made for the case and holding the link to this tree's tests that the tester finds them through;
# `cwd_setup` may add to it, or change its mode, first.
cwd_setup() { :; }
# The test-endpoint build refuses a case variable without both endpoints, so each case
# aims its connection at closed ports on this host: nothing it sends can leave the host, even where a network
# were up, and the driver still loads and fails the connect itself.
test_host=127.0.0.1
test_endpoint_url=https://127.0.0.1:9
run() {
  local case="$1"; shift
  local dir="$work/cwd-$case" tester="$work/unittest-$case"
  echo "run-e2e: $case"
  mkdir -m 700 "$work/driver-$case" "$dir"
  ln -s "$root/test" "$dir/test"
  cwd_setup "$dir"
  printf '#!/bin/sh\nexec "%s" --test-dir "%s" "$@"\n' "$unittest" "$dir" > "$tester"
  chmod 700 "$tester"
  (cd "$root" && env -i PATH="$PATH" HOME="$work" REDSHIFT_TEST_CLIENT=odbc DUCKDB_REDSHIFT_TEST_E2E="$case" \
    DUCKDB_REDSHIFT_TEST_HOST="$test_host" DUCKDB_REDSHIFT_TEST_ENDPOINT_URL="$test_endpoint_url" \
    AWS_ACCESS_KEY_ID=test-not-a-key AWS_SECRET_ACCESS_KEY=test-not-a-secret AWS_REGION=us-east-1 \
    AWS_EC2_METADATA_DISABLED=true REDSHIFT_DRIVER_DIR="$work/driver-$case" "$@" \
    bash "$root/test/run-selected.sh" "$tester" "test/client/driver/sql/*")
  cwd_setup() { :; }
}
# The loader wrote the driver set into the case's driver directory.
written() {
  [[ -d "$work/driver-$1/.redshift-driver" ]] || fail "$1: the driver set was not written"
}
# Nothing was written below the case's driver directory: the refusal came before the bases were chosen.
untouched() {
  [[ -z "$(ls -A "$work/driver-$1")" ]] || fail "$1: the driver directory was written to, so the refusal came too late"
}
# The odbc.ini that would turn the driver's logging on and send the log to an empty directory, which must
# still be empty after: a driver that read the file would create its log there. No log file means the driver did
# not read it, which says something only because the same file, read at the first environment handle, does make
# the driver write a log there: the loader test "The real driver reads an odbc.ini in the current directory at its
# first environment handle, unless it is allocated through AllocateEnvironmentHandle" (driver_loader_test.cpp,
# [driver][loader][real]) is that control, on the same bytes, and runs in `make test_driver`.
# with_odbc_ini <file> <log directory>
with_odbc_ini() {
  printf '[ODBC]\nLogLevel=6\nLogPath=%s\n' "$2" > "$1"
}

run no-network
written no-network
run refused-variable KRB5_CONFIG="$work/user-krb5.conf"
untouched refused-variable
mkdir -m 700 "$work/logs-odbcini-variable"
with_odbc_ini "$work/user-odbc.ini" "$work/logs-odbcini-variable"
run odbcini-variable ODBCINI="$work/user-odbc.ini"
written odbcini-variable
[[ -z "$(ls -A "$work/logs-odbcini-variable")" ]] || fail "odbcini-variable: the driver read \$ODBCINI: $(ls "$work/logs-odbcini-variable" | tr '\n' ' ')"
mkdir -m 700 "$work/logs-odbc-ini"
cwd_setup() { with_odbc_ini "$1/odbc.ini" "$work/logs-odbc-ini"; }
run odbc-ini
written odbc-ini
grep -q '^LogLevel=6$' "$work/cwd-odbc-ini/odbc.ini" || fail "odbc-ini: the odbc.ini does not turn logging on"
grep -qxF "LogPath=$work/logs-odbc-ini" "$work/cwd-odbc-ini/odbc.ini" || fail "odbc-ini: the odbc.ini does not send the log to the empty directory"
[[ -z "$(ls -A "$work/logs-odbc-ini")" ]] || fail "odbc-ini: the driver read the odbc.ini in its current directory: $(ls "$work/logs-odbc-ini" | tr '\n' ' ')"
cwd_setup() { chmod 770 "$1"; }
run group-writable-cwd
written group-writable-cwd
cwd_setup() { chmod 1777 "$1"; }
run writable-cwd
written writable-cwd
run library-path LD_LIBRARY_PATH="relative/lib"
untouched library-path
mkdir -m 777 "$work/planted"
# The names come from the reviewed policy, not a copy of the list: every library the Linux driver may name in
# DT_NEEDED, so a reviewed edit that adds one is planted here too. Not the libraries the case's own programs start
# with, which this LD_LIBRARY_PATH reaches as well: bash and sh (run-selected.sh and the tester's wrapper), the
# programs run-selected.sh runs, and the unit tester. A planted copy of one of those would stop that program before
# or after the SQL ran, and which they are depends on the host: on glibc 2.28 bash needs libdl.so.2, grep (through
# libpcre) libpthread.so.0, and coreutils both and librt.so.1. So they are read here, as the platform loader lists
# them for each program (ldd: its DT_NEEDED entries and theirs; for a script, its interpreter's, as
# coreutils-single's programs are), not copied. Nor either platform's dynamic loader (ld-linux-*), which is never
# looked up by name on a search path, so the check that some name is left cannot pass on the other platform's
# alone. What is left is what the driver needs beyond those, which is what this case is about.
command -v ldd >/dev/null || fail "absolute-library-path: ldd is required and was not found on PATH"
# started_with <program>: the file names of the libraries the platform loader opens to start it, one per line.
started_with() {
  local file first interpreter
  file="$(command -v "$1")" || fail "absolute-library-path: $1 was not found on PATH"
  IFS= read -r first < "$file" || true
  if [[ "$first" == '#!'* ]]; then
    read -r interpreter _ <<< "${first#\#!}"
    file="$interpreter"
  fi
  # A static program needs none: ldd says it is not a dynamic executable and exits 1.
  env -u LD_LIBRARY_PATH ldd "$file" 2>/dev/null |
    awk '$2 == "=>" { print $1; next } $1 ~ /^\// { n = split($1, part, "/"); print part[n] }' || true
}
started=" "
for program in bash /bin/sh mktemp tee grep tail rm "$unittest"; do
  started+="$(started_with "$program" | tr '\n' ' ')"
done
planted=()
while read -r name; do
  case "$name" in '' | \#* | ld-linux-*) continue ;; esac
  [[ "$started" == *" $name "* ]] && continue
  planted+=("$name")
done < "$root/third_party/redshift_odbc/policy/linux/allowed-needed.txt"
((${#planted[@]} > 0)) || fail "absolute-library-path: no library names left to plant from policy/linux/allowed-needed.txt"
for name in "${planted[@]}"; do
  printf 'not a library\n' > "$work/planted/$name"
  chmod 666 "$work/planted/$name"
done
run absolute-library-path LD_LIBRARY_PATH="$work/planted"
written absolute-library-path
# The same names only in the subdirectories the platform loader tries below each LD_LIBRARY_PATH element, which
# its own search list (dlinfo) does not report: glibc-hwcaps levels from glibc 2.33, and the legacy tls, platform
# and hardware-capability directories, as glibc 2.34 tries them on x86_64 (LD_DEBUG=libs) and glibc 2.28 on
# aarch64, `atomics` among them (glibc can load a copy planted only in <dir>/atomics when the driver is opened
# directly). The loader opens each library first by its checked path, so none of these is opened.
mkdir -m 777 "$work/planted-sub"
for sub in glibc-hwcaps/x86-64-v4 glibc-hwcaps/x86-64-v3 glibc-hwcaps/x86-64-v2 tls tls/x86_64 tls/haswell \
  tls/haswell/x86_64 x86_64 haswell haswell/x86_64 tls/aarch64/atomics tls/aarch64 tls/atomics aarch64/atomics \
  aarch64 atomics; do
  mkdir -p "$work/planted-sub/$sub"
  chmod 777 "$work/planted-sub/$sub"
  for name in "${planted[@]}"; do
    printf 'not a library\n' > "$work/planted-sub/$sub/$name"
    chmod 666 "$work/planted-sub/$sub/$name"
  done
done
run library-path-subdirectories LD_LIBRARY_PATH="$work/planted-sub"
written library-path-subdirectories

# The two default-base cases: a connect from a shell, which with no network the driver must fail itself.
# The statements the coexistence harness runs too, so one edit moves both. The host is the Redshift
# Serverless endpoint the client derives, never a local address; the region may come before or after
# redshift-serverless in it, so the pattern takes both.
connect_sql="$(< "$root/third_party/redshift_odbc/tools/connect.sql")"
connect_failed='Could not connect to Redshift at [^ ]+\.redshift-serverless(\.[^ ]+)?\.amazonaws\.com \(region us-east-1, workgroup [^)]+\): the connection attempt failed\.'

# The default bases: a private copy of the shell, which has the extension linked in (a LOAD of a loadable
# file would find it loaded already and open nothing), with no REDSHIFT_DRIVER_DIR. The loader's base is
# the directory of the file the extension is mapped from, here the copy's directory, which the copy owns,
# so the set must be written beside the copy. The loadable extension's own dlopen is the last case's.
# make driver_e2e builds the shell, so a build without it fails here rather than skip the case.
[[ -x "$shell_binary" ]] || fail "static-shell-default-base: $shell_binary is not built; run make driver_e2e REDSHIFT_DRIVER=embedded"
echo "run-e2e: static-shell-default-base"
mkdir -m 700 "$work/copy" "$work/cwd-default" "$work/home"
cp "$shell_binary" "$work/copy/"
out="$work/default-base.out"
# It fails the connect, like no-network: the default bases are what is under test.
status=0
(cd "$work/cwd-default" && env -i PATH="$PATH" HOME="$work/home" AWS_ACCESS_KEY_ID=test-not-a-key \
  AWS_SECRET_ACCESS_KEY=test-not-a-secret AWS_REGION=us-east-1 AWS_EC2_METADATA_DISABLED=true \
  "$work/copy/duckdb" -c "$connect_sql" \
  > "$out" 2>&1) || status=$?
[[ "$status" != 0 ]] || fail "static-shell-default-base: a connect with no network succeeded"
grep -qE "$connect_failed" "$out" ||
  fail "static-shell-default-base: the driver did not load and fail the connect itself: $(tr '\n' ' ' < "$out")"
[[ -d "$work/copy/.redshift-driver" ]] || fail "static-shell-default-base: the set was not written to the extension's directory"
# The harness that loads the extension beside other DuckDB extensions. This checks the harness only: json
# and icu are built into this shell, so their LOADs find them loaded, and an extension that does not load
# fails it.
echo "run-e2e: extension-coexistence"
bash "$root/third_party/redshift_odbc/tools/extension-coexistence.sh" "$shell_binary" "" \
  "$build/extension/json/json.duckdb_extension" "$build/extension/icu/icu.duckdb_extension" ||
  fail "extension-coexistence: the harness did not see the driver load and fail the connect itself"
# Exit 1, an order that failed: exit 2 would be a refusal before any order ran.
status=0
bash "$root/third_party/redshift_odbc/tools/extension-coexistence.sh" "$shell_binary" "" "$work/none.duckdb_extension" \
  >/dev/null 2>&1 || status=$?
[[ "$status" == 1 ]] || fail "extension-coexistence: an extension that does not load gave exit $status, not 1 (a failed order)"

# The loadable extension as a user installs and loads it: a stock shell INSTALLs redshift.duckdb_extension
# from a private copy into DuckDB's own extension directory under HOME, then `LOAD redshift` (unsigned here,
# since a local build is not signed). With no REDSHIFT_DRIVER_DIR the base is that directory, which the
# loadable is mapped from, and the driver is loaded from inside the dlopen'ed extension.
if [[ -n "$stock_shell" ]]; then
  echo "run-e2e: loadable-extension"
  mkdir -m 700 "$work/loadable" "$work/cwd-loadable" "$work/home-loadable"
  cp "$loadable" "$work/loadable/"
  out="$work/loadable.out"
  status=0
  (cd "$work/cwd-loadable" && env -i PATH="$PATH" HOME="$work/home-loadable" AWS_ACCESS_KEY_ID=test-not-a-key \
    AWS_SECRET_ACCESS_KEY=test-not-a-secret AWS_REGION=us-east-1 AWS_EC2_METADATA_DISABLED=true \
    "$stock_shell" -unsigned -c "INSTALL '$work/loadable/redshift.duckdb_extension'; LOAD redshift; $connect_sql" \
    > "$out" 2>&1) || status=$?
  [[ "$status" != 0 ]] || fail "loadable-extension: a connect with no network succeeded"
  grep -qE "$connect_failed" "$out" ||
    fail "loadable-extension: the driver did not load and fail the connect itself: $(tr '\n' ' ' < "$out")"
  installed=("$work/home-loadable"/.duckdb/extensions/*/*/redshift.duckdb_extension)
  [[ ${#installed[@]} == 1 && -f "${installed[0]}" ]] || fail "loadable-extension: INSTALL did not put one copy in DuckDB's extension directory"
  [[ -d "$(dirname "${installed[0]}")/.redshift-driver" ]] ||
    fail "loadable-extension: the set was not written beside the installed extension"
  [[ ! -e "$work/loadable/.redshift-driver" ]] || fail "loadable-extension: the set was written beside the copy INSTALL read"
  # The harness's two-order path, with the loadable extension. json and icu are built into this shell as well,
  # so this checks the harness, not the order with separately built extensions.
  echo "run-e2e: extension-coexistence-orders"
  bash "$root/third_party/redshift_odbc/tools/extension-coexistence.sh" "$stock_shell" "$loadable" json icu ||
    fail "extension-coexistence-orders: the harness did not see the loadable extension load and fail the connect itself"
else
  echo "run-e2e: loadable-extension # SKIP no stock DuckDB shell given (make stock_shell builds one)"
fi
echo "run-e2e: passed"
