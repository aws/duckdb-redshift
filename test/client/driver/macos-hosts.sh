#!/usr/bin/env bash
# macos-hosts.sh <build directory> <stock DuckDB shell>
# The extension in macOS host applications, run by make test_driver_e2e on osx_amd64 and osx_arm64: the loadable
# extension of make driver_e2e's test-endpoint build (build/driver-e2e) as a user installs it, and what
# weak-definition coalescing reaches between the driver and the other images of a process. Every connect aims
# both endpoints at closed ports on 127.0.0.1, as on Windows: run-e2e.sh's no-network check reads /proc/net/dev,
# which macOS lacks, so nothing here proves the process has no network. Each host runs for the build's
# architecture (an osx_amd64 build's under Rosetta 2 on Apple silicon), with only PATH, fake AWS keys and a private
# HOME in its environment.
#
# The host rows, each skipped when its host is not given, or failed then with REDSHIFT_TEST_REQUIRE_MACOS_HOSTS=1,
# failed when it is given but lipo cannot read it, and skipped when it has no slice of the build's architecture:
#   duckdb-cli       REDSHIFT_TEST_DUCKDB_CLI, the DuckDB CLI release of the pinned version (hardened runtime, with
#                    disable-library-validation)
#   python-org       REDSHIFT_TEST_PYTHON_ORG, python.org Python (hardened, with disable-library-validation); for a
#                    framework unpacked from python.org's installer rather than installed in /Library/Frameworks,
#                    REDSHIFT_TEST_PYTHON_ORG_FRAMEWORKS names the directory that holds its Python.framework,
#                    which the row gives it as DYLD_FALLBACK_FRAMEWORK_PATH (it has
#                    allow-dyld-environment-variables, and the loader refuses only the search variables)
#   homebrew-python  REDSHIFT_TEST_HOMEBREW_PYTHON, Homebrew's Python (not hardened)
#   stock-shell      the argument, the pinned DuckDB's shell built here (make stock_shell; not hardened)
# The Python rows import the duckdb package of the pinned version from REDSHIFT_TEST_DUCKDB_PYTHON, a directory
# that holds it (an unpacked wheel). Each row checks that the host is signed as its row says (codesign), INSTALLs
# a copy of the loadable extension into DuckDB's extension directory under its HOME and LOADs it, unsigned, and
# connects: the connect must fail with the client's connection failure at 127.0.0.1, so the driver was loaded and
# failed the connect itself; the set must be written beside the installed file, and none of its files may carry
# com.apple.quarantine (none of these hosts sets LSFileQuarantineEnabled). Each row runs with
# inputs/weak_slots.c inserted (DYLD_INSERT_LIBRARIES), which reads, as the host exits, where each of the
# driver's weak binds went, and macos_bindings.py lists which of its weak-bound names went outside the driver,
# and to which image; a host that ignores the variable (a hardened one without allow-dyld-environment-variables)
# reads none, and that row skips, saying so, where any other host that reads none fails it.
#
#   quarantine-host  the stock shell again, as a host that opts in to quarantine (below)
#
# Then the weak-bind test host (inputs/weak_host.cpp, built here with the platform's clang++, as are its two
# libraries): a host that defines operator new and delete and counts the driver's calls to them, which loads
# weak_before.c, defining weak one of the driver's weak-bound AWS SDK template instances, then the driver
# (RTLD_NOW | RTLD_LOCAL, as the loader does), then weak_after.c, which weak-binds another of the driver's exported
# weak definitions. It reports which definition each of those binds reached, and fails unless each library's
# bind kept its own, as expected. The same run, under fs_usage where REDSHIFT_TEST_FS_USAGE=sudo
# lets it run fs_usage as root (sudo -n), is the trace of the files the driver opens or probes, which
# macos_config_files.py holds to policy/config-files.txt; without it that case skips, or fails with
# REDSHIFT_TEST_REQUIRE_MACOS_HOSTS=1.
#
# Prints TAP; exits 1 when a case failed, 2 on a usage error.
set -euo pipefail
here="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
root="$(cd "$here/../../.." && pwd)"
usage() { echo "usage: macos-hosts.sh <build directory> <stock DuckDB shell>" >&2; exit 2; }
(($# == 2)) || usage
[[ "$(uname -s)" == Darwin ]] || { echo "macos-hosts.sh: macOS only" >&2; exit 2; }
build="$(cd "$1" && pwd)"
stock_shell="$(cd "$(dirname "$2")" && pwd)/$(basename "$2")"
loadable="$build/extension/redshift/redshift.duckdb_extension"
[[ -f "$loadable" ]] || { echo "macos-hosts.sh: $build has no loadable extension; run make driver_e2e REDSHIFT_DRIVER=embedded" >&2; exit 2; }
grep -qs '^REDSHIFT_CLIENT:STRING=odbc' "$build/CMakeCache.txt" && grep -qs '^REDSHIFT_DRIVER:STRING=embedded' "$build/CMakeCache.txt" &&
  grep -qs '^REDSHIFT_TEST_ENDPOINTS:BOOL=ON' "$build/CMakeCache.txt" ||
  { echo "macos-hosts.sh: $build is not an embedded ODBC build with the test-endpoint switch; run make driver_e2e REDSHIFT_DRIVER=embedded" >&2; exit 2; }
platform="$(sed -n 's/^platform //p' "$build/extension/redshift/redshift_driver_identity.txt")"
case "$platform" in
osx_arm64) arch=arm64 ;;
osx_amd64) arch=x86_64 ;;
*) echo "macos-hosts.sh: $build is a $platform build, not a macOS one" >&2; exit 2 ;;
esac
work="$(mktemp -d)"
work="$(cd "$work" && pwd -P)" # resolved: macOS's /var is a link
trap 'rm -rf -- "${work:?}"' EXIT
chmod 700 "$work"

n=0 failed=0
ok() { n=$((n + 1)); echo "ok $n - $1"; }
not_ok() { n=$((n + 1)); failed=$((failed + 1)); echo "not ok $n - $1"; }
skip() { n=$((n + 1)); echo "ok $n - $1 # SKIP $2"; }

connect_sql="$(< "$root/third_party/redshift_odbc/tools/connect.sql")"
connect_failed='Could not connect to Redshift at 127\.0\.0\.1 \(region us-east-1, workgroup [^)]+\): the connection attempt failed\.'
case_env=(DUCKDB_REDSHIFT_TEST_HOST=127.0.0.1 DUCKDB_REDSHIFT_TEST_ENDPOINT_URL=https://127.0.0.1:9
  AWS_ACCESS_KEY_ID=test-not-a-key AWS_SECRET_ACCESS_KEY=test-not-a-secret AWS_REGION=us-east-1
  AWS_EC2_METADATA_DISABLED=true)

# entitlements <binary>: its entitlements, without white space; signing <binary>: `hardened` (the hardened
# runtime, with disable-library-validation), `hardened-validating` (without it) or `not-hardened`. Both from
# codesign, on the slice of the build's architecture, the one the row runs.
entitlements() { codesign -d -a "$arch" --entitlements - --xml "$1" 2>/dev/null | tr -d ' \t\n' || true; }
signing() {
  local flags
  flags="$(codesign -dv -a "$arch" "$1" 2>&1 | sed -n 's/.*flags=0x[0-9a-f]*(\([^)]*\)).*/\1/p' | head -1 || true)"
  if [[ ",$flags," != *,runtime,* ]]; then echo not-hardened
  elif [[ "$(entitlements "$1")" == *'<key>com.apple.security.cs.disable-library-validation</key><true/>'* ]]; then
    echo hardened
  else echo hardened-validating
  fi
}

# The binary a Python runs as: a framework build's bin/python3 starts its Resources/Python.app, which runs on
# its own as well.
python_binary() {
  local real app
  real="$(python3 -I -c 'import os, sys; print(os.path.realpath(sys.argv[1]))' "$1")"
  app="$(dirname "$real")/../Resources/Python.app/Contents/MacOS/Python"
  if [[ -x "$app" ]]; then echo "$app"; else echo "$real"; fi
}

# for_arch <binary>: what to run for the build's architecture: the binary itself where that is what macOS runs
# it as (a thin one, or arm64 on Apple silicon), otherwise a copy thinned to it, which keeps that slice's own
# signature, so that macOS runs it under Rosetta 2. Not /usr/bin/arch: macOS protects it, so it starts without
# the DYLD_ variables it inherits, which the weak-bind test sets, and runs the host without them.
for_arch() {
  local archs copy
  archs="$(lipo -archs "$1")"
  if [[ "$archs" == "$arch" || "$arch" == "$(uname -m)" ]]; then echo "$1"; return; fi
  copy="$(mktemp -d "$work/thin.XXXXXX")/$(basename "$1")"
  lipo -thin "$arch" -output "$copy" "$1" && echo "$copy"
}

# clean_run <directory> <name=value>... -- <command>...: the command from <directory> with only PATH and the
# variables given. /usr/bin/env is protected too, but the DYLD_ variables it is given as arguments, unlike those
# it inherits, reach the command.
clean_run() {
  local dir="$1"
  shift
  local assignments=()
  while [[ "$1" != -- ]]; do assignments+=("$1"); shift; done
  shift
  (cd "$dir" && exec /usr/bin/env -i PATH="$PATH" ${assignments[@]+"${assignments[@]}"} "$@")
}

# The driver the build embeds (the set fetch-driver.sh wrote), whose weak-bind slots the reader reads, and the
# reader, for the build's architecture.
driver_file="$build/extension/redshift/redshift_driver/librsodbc64.dylib"
python3 -I "$here/macos_bindings.py" slots "$driver_file" "$arch" > "$work/slots" &&
  xcrun clang -arch "$arch" -dynamiclib -O1 -o "$work/weak_slots.dylib" "$here/inputs/weak_slots.c" ||
  { echo "macos-hosts.sh: cannot read the weak-bind slots of $driver_file or build the reader" >&2; exit 1; }

cat > "$work/host.py" <<'PY'
# The Python rows' script: <duckdb package directory> <loadable extension copy> <connect SQL>.
import platform
import sys

sys.path.insert(0, sys.argv[1])
import duckdb

print("version duckdb %s, Python %s from %s, %s" %
      (duckdb.__version__, platform.python_version(), sys.base_prefix, platform.machine()))
con = duckdb.connect(config={"allow_unsigned_extensions": "true"})
con.execute("INSTALL '%s'" % sys.argv[2].replace("'", "''"))
con.execute("LOAD redshift")
for statement in sys.argv[3].split(";"):
    if statement.strip():
        try:
            con.execute(statement)
        except duckdb.Error as error:
            print(error)
PY

# host_row <row> <cli|python> <host> <signing it must have>
host_row() {
  local row="$1" kind="$2" host="$3" want="$4" binary archs sig dir out installed set_dir file quarantined version bindings
  if [[ -z "$host" ]]; then
    if [[ "${REDSHIFT_TEST_REQUIRE_MACOS_HOSTS:-}" == 1 ]]; then not_ok "$row: no host given (REDSHIFT_TEST_REQUIRE_MACOS_HOSTS=1)"
    else skip "$row" "no host given"; fi
    return
  fi
  if [[ "$kind" == python ]]; then binary="$(python_binary "$host")"; else binary="$host"; fi
  # A host that is given but cannot be read fails, whatever REDSHIFT_TEST_REQUIRE_MACOS_HOSTS says.
  archs="$(lipo -archs "$binary" 2>&1)" || { not_ok "$row: cannot read $binary: $archs"; return; }
  if [[ " $archs " != *" $arch "* ]]; then
    skip "$row" "$binary has no $arch slice"
    return
  fi
  sig="$(signing "$binary")"
  [[ "$sig" == "$want" ]] || { not_ok "$row: $binary is $sig, not $want"; return; }
  dir="$work/$row"
  mkdir -m 700 "$dir" "$dir/home" "$dir/cwd" "$dir/loadable"
  cp "$loadable" "$dir/loadable/"
  local copy="$dir/loadable/redshift.duckdb_extension" status=0 run
  run="$(for_arch "$binary")" || { not_ok "$row: $binary could not be thinned to $arch"; return; }
  # python.org's own way to run its universal Python as x86_64 on Apple silicon: its -intel64 launcher.
  local intel="$(dirname "$(python3 -I -c 'import os, sys; print(os.path.realpath(sys.argv[1]))' "$host")")"
  intel="$(ls "$intel"/python3.*-intel64 2>/dev/null | head -1 || true)"
  [[ "$kind" != python || "$arch" == "$(uname -m)" || -z "$intel" ]] || run="$intel"
  local command=("$run" -unsigned -c "INSTALL '$copy'; LOAD redshift; $connect_sql")
  [[ "$kind" == cli ]] || command=("$run" -I "$work/host.py" "${REDSHIFT_TEST_DUCKDB_PYTHON:-}" "$copy" "$connect_sql")
  local extra=(DYLD_INSERT_LIBRARIES="$work/weak_slots.dylib" REDSHIFT_WEAK_SLOTS="$work/slots"
    REDSHIFT_WEAK_SLOTS_OUT="$dir/reached")
  [[ "$row" != python-org || -z "${REDSHIFT_TEST_PYTHON_ORG_FRAMEWORKS:-}" ]] ||
    extra+=(DYLD_FALLBACK_FRAMEWORK_PATH="$REDSHIFT_TEST_PYTHON_ORG_FRAMEWORKS")
  clean_run "$dir/cwd" HOME="$dir/home" "${case_env[@]}" "${extra[@]}" -- "${command[@]}" \
    > "$dir/out" 2> "$dir/err" || status=$?
  out="$(cat "$dir/out" "$dir/err" | tr '\n' ' ' | cut -c1-600 || true)"
  ((status < 128)) || { not_ok "$row: the host was killed (exit $status): $out"; return; }
  if ! grep -qE "$connect_failed" "$dir/out" "$dir/err"; then
    not_ok "$row: the driver did not load and fail the connect itself (exit $status): $out"
    return
  fi
  installed=("$dir/home"/.duckdb/extensions/*/*/redshift.duckdb_extension)
  set_dir="$(dirname "${installed[0]}")/.redshift-driver"
  set_dir="$(ls -d "$set_dir"/*/ 2>/dev/null | head -1 || true)"
  if [[ ${#installed[@]} != 1 || ! -f "${installed[0]}" || -z "$set_dir" ]]; then
    not_ok "$row: the set was not written beside the installed extension"
    return
  fi
  quarantined=""
  for file in "$set_dir" "$set_dir"*; do
    xattr -p com.apple.quarantine "$file" >/dev/null 2>&1 && quarantined+=" $file"
  done
  [[ -z "$quarantined" ]] || { not_ok "$row: written files carry com.apple.quarantine:$quarantined"; return; }
  if [[ "$kind" == cli ]]; then
    # Under Rosetta 2 the shell prints a coloured warning first.
    version="DuckDB $("$run" --version 2>/dev/null | grep -o 'v[0-9][0-9.]* .*' | tail -1 || true)"
  else
    version="$(sed -n 's/^version //p' "$dir/out" | head -1 || true)"
  fi
  ok "$row: $version, $binary ($sig): the driver loaded from ${set_dir%/}, unquarantined, and failed the connect itself"
  if bindings="$(python3 -I "$here/macos_bindings.py" summary "$driver_file" "$arch" "$dir/reached" 2>&1)"; then
    ok "$row: weak binds: $bindings"
  elif [[ "$sig" != not-hardened &&
    "$(entitlements "$binary")" != *'<key>com.apple.security.cs.allow-dyld-environment-variables</key><true/>'* ]]; then
    skip "$row: weak binds" "$bindings"
  else
    not_ok "$row: weak binds: $bindings"
  fi
}

host_row duckdb-cli cli "${REDSHIFT_TEST_DUCKDB_CLI:-}" hardened
host_row python-org python "${REDSHIFT_TEST_PYTHON_ORG:-}" hardened
host_row homebrew-python python "${REDSHIFT_TEST_HOMEBREW_PYTHON:-}" not-hardened
host_row stock-shell cli "$stock_shell" not-hardened

# quarantine-host: the stock shell as a host that opts in to quarantine, its start given the quarantine an app
# that sets LSFileQuarantineEnabled runs with (inputs/quarantine_self.c, in DYLD_INSERT_LIBRARIES): the extension
# is installed by a run without it, then loaded and connected with it, so the loader writes the set in a process
# whose new files are quarantined. Each file of the set must carry com.apple.quarantine, and the driver must
# still load and fail the connect itself.
quarantine_host() {
  local dir="$work/quarantine" status=0 file missing="" value="" set_dir installed run
  mkdir -m 700 "$dir" "$dir/home" "$dir/cwd" "$dir/loadable"
  cp "$loadable" "$dir/loadable/"
  run="$(for_arch "$stock_shell")" &&
    xcrun clang -arch "$arch" -dynamiclib -O1 -o "$dir/quarantine_self.dylib" "$here/inputs/quarantine_self.c" ||
    { not_ok "quarantine-host: its library did not build"; return; }
  clean_run "$dir/cwd" HOME="$dir/home" -- "$run" -unsigned \
    -c "INSTALL '$dir/loadable/redshift.duckdb_extension'" > "$dir/install.out" 2>&1 ||
    { not_ok "quarantine-host: the INSTALL failed: $(tr '\n' ' ' < "$dir/install.out" | cut -c1-400)"; return; }
  clean_run "$dir/cwd" HOME="$dir/home" "${case_env[@]}" DYLD_INSERT_LIBRARIES="$dir/quarantine_self.dylib" -- \
    "$run" -unsigned -c "LOAD redshift; $connect_sql" > "$dir/out" 2>&1 || status=$?
  ((status < 128)) || { not_ok "quarantine-host: the host was killed (exit $status): $(tr '\n' ' ' < "$dir/out" | cut -c1-400)"; return; }
  installed=("$dir/home"/.duckdb/extensions/*/*/redshift.duckdb_extension)
  set_dir="$(ls -d "$(dirname "${installed[0]}")"/.redshift-driver/*/ 2>/dev/null | head -1 || true)"
  [[ -n "$set_dir" ]] ||
    { not_ok "quarantine-host: no set was written (exit $status): $(tr '\n' ' ' < "$dir/out" | cut -c1-400)"; return; }
  for file in "$set_dir"*; do
    value="$(xattr -p com.apple.quarantine "$file" 2>/dev/null)" || missing+=" ${file##*/}"
  done
  [[ -z "$missing" ]] ||
    { not_ok "quarantine-host: the host left the files it wrote unquarantined:$missing, so it opted in to nothing"; return; }
  if grep -qE "$connect_failed" "$dir/out"; then
    ok "quarantine-host: the set's files carry com.apple.quarantine ($value), and the driver still loaded and failed the connect itself"
  else
    not_ok "quarantine-host: the set is quarantined ($value), and the driver did not load and fail the connect itself (exit $status): $(tr '\n' ' ' < "$dir/out" | cut -c1-600)"
  fi
}
quarantine_host

# The weak-bind test host, on a copy of the build's driver ($driver_file). The two names are AWS SDK template instances
# the driver both weak-binds and exports, which neither the smoke below nor a password connect reach: taken from
# the driver's export list (the policy's rows, each a name without the Mach-O leading underscore). The same run is
# the file trace: with REDSHIFT_TEST_FS_USAGE=sudo, fs_usage, which needs root, runs under sudo -n and records the
# paths the host names, which macos_config_files.py holds to policy/config-files.txt; otherwise that case skips.
weak_study() {
  local exports="$root/third_party/redshift_odbc/policy/macos/expected-exports.txt" before after driver dir status=0
  local tracer="" port=9 home="$work/weak-home" cwd="$work/weak-cwd" ca="$work/weak-ca.pem" files bindings
  before="$(grep -m1 -F GetCredentialsRequest19SetCustomDomainNameIPKc "$exports" | awk '{print $1}' || true)"
  after="$(grep -m1 -F GetCredentialsRequest16SetWorkgroupNameIRK "$exports" | awk '{print $1}' || true)"
  [[ -n "$before" && -n "$after" ]] || { not_ok "weak study: the export list has neither name"; return; }
  # A copy of the driver on a short path, so that fs_usage, which shortens a long path from its start, shows
  # each path whole. The host's own files (the driver's directory, the host, its libraries, the reader and its
  # slots) are in one directory, which the trace does not count; HOME and the current directory are beside it,
  # as on Linux, so a path under either that no row lists is flagged.
  dir="$work/weak"
  driver="$dir/d/librsodbc64.dylib"
  mkdir -m 700 "$dir" "$dir/d" "$home" "$cwd"
  cp "$driver_file" "$driver"
  # The CA file the smoke names, as the ODBC client always names one: a real certificate, which the driver loads
  # before it connects, copied before the trace starts and outside the host's own directory, so the trace counts
  # it only as the connection's own file.
  cp "$root/third_party/redshift_odbc/signing/linux/DigiCertTrustedG4CodeSigningRSA4096SHA3842021CA1.pem" "$ca" ||
    { not_ok "weak study: the CA file for the smoke could not be copied"; return; }
  cp "$work/slots" "$work/weak_slots.dylib" "$dir/"
  xcrun clang -arch "$arch" -dynamiclib -O1 -o "$dir/weak_before.dylib" "$here/inputs/weak_before.c" \
    -DREDSHIFT_WEAK_SYMBOL="\"_$before\"" &&
    xcrun clang -arch "$arch" -dynamiclib -O1 -o "$dir/weak_after.dylib" "$here/inputs/weak_after.c" \
      -DREDSHIFT_WEAK_SYMBOL="\"_$after\"" -DREDSHIFT_WEAK_NAME="\"$after\"" &&
    xcrun clang++ -arch "$arch" -std=c++17 -O1 -o "$dir/weak_host" "$here/inputs/weak_host.cpp" ||
    { not_ok "weak study: the test host or its libraries did not build"; return; }
  if [[ "${REDSHIFT_TEST_FS_USAGE:-}" == sudo ]]; then
    # fs_usage records from when it starts, for at most -t seconds; the host takes a few.
    sudo -n /usr/bin/fs_usage -w -f pathname -t 30 weak_host > "$dir/trace" 2>&1 &
    tracer=$!
    sleep 3
  fi
  clean_run "$cwd" HOME="$home" DYLD_INSERT_LIBRARIES="$dir/weak_slots.dylib" REDSHIFT_WEAK_SLOTS="$dir/slots" \
    REDSHIFT_WEAK_SLOTS_OUT="$dir/reached" -- "$dir/weak_host" "$driver" "$dir/weak_before.dylib" \
    "$dir/weak_after.dylib" "$port" "$home/not-a-configuration-file" "$ca" > "$dir/out" 2> "$dir/err" || status=$?
  [[ -z "$tracer" ]] || wait "$tracer" || true
  if ((status != 0)) || ! grep -qx 'before_own_call own' "$dir/out" ||
    ! grep -q '^smoke HY000 .*\[Redshift\]\[ODBC Driver\].*Connection refused' "$dir/out"; then
    not_ok "weak study: the test host failed (exit $status): $(tr '\n' ' ' < "$dir/out" | cut -c1-300)"
    return
  fi
  # What is expected, which a change in coalescing would move: the later library's weak bind reaches
  # its own definition, the driver's bind of the earlier library's name keeps the driver's, and every slot of the
  # driver's operator new and delete binds the test host's (a split, some slots to libc++abi, fails).
  if grep -qx 'after_bind own' "$dir/out"; then
    ok "weak study: $(grep -E '^(before_own_call|driver_operator_new|driver_operator_delete|after_bind) ' "$dir/out" | tr '\n' ';')"
  else
    not_ok "weak study: the later library's weak bind did not reach its own definition: $(grep '^after_bind ' "$dir/out" || true)"
  fi
  if bindings="$(python3 -I "$here/macos_bindings.py" summary "$driver" "$arch" "$dir/reached" "_$before" "_$after" \
    __Znwm __ZdlPv 2>&1)" && grep -qxF -- "_$before -> librsodbc64.dylib" <<<"$bindings" &&
    grep -qxF -- "__Znwm -> weak_host" <<<"$bindings" && grep -qxF -- "__ZdlPv -> weak_host" <<<"$bindings"; then
    ok "weak study: the driver's binds in the test host: $(tr '\n' ';' <<<"$bindings")"
  else
    not_ok "weak study: the driver's bind of _$before must keep the driver's definition, and its operator new and delete binds go to the test host's: $(tr '\n' ';' <<<"$bindings")"
  fi
  if [[ -z "$tracer" && "${REDSHIFT_TEST_REQUIRE_MACOS_HOSTS:-}" == 1 ]]; then
    not_ok "config files: the closed-port smoke under fs_usage: REDSHIFT_TEST_FS_USAGE=sudo is not set (REDSHIFT_TEST_REQUIRE_MACOS_HOSTS=1)"
  elif [[ -z "$tracer" ]]; then
    skip "config files: the closed-port smoke under fs_usage" "fs_usage needs root; REDSHIFT_TEST_FS_USAGE=sudo runs it under sudo -n"
  elif files="$(python3 -I "$here/macos_config_files.py" "$dir/trace" "$root/third_party/redshift_odbc/policy/config-files.txt" \
    "$driver" "$home" "$cwd" weak_host "$home/not-a-configuration-file" "$ca" 2>&1)"; then
    ok "config files: the closed-port smoke under fs_usage: $(tr '\n' ';' <<<"$files")"
  else
    not_ok "config files: the closed-port smoke under fs_usage: $(tr '\n' ';' <<<"$files" | cut -c1-2000)"
  fi
}
weak_study

echo "# $n cases, $failed failed"
((failed == 0))
