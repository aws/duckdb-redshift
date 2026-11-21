#!/usr/bin/env bash
# Tool tests for test/client/driver/run-windows-loading.sh and windows-runner-tools.sh. A copy
# runs in a scratch tree beside the real tools and a stand-in windows_modules.py, with stand-ins
# for uname (a Git bash on Windows), cygpath (which marks a path WIN:), msiexec (which writes the
# administrative image from lib/msi.py's reading of the MSI) and powershell (which answers
# STUB_AUTHENTICODE). The build is a windows_amd64 one made at test time: a renamed lock, its MSI
# from make_msi.py, the set, and a loadable extension from make_pe.py. The downloader cases run
# for real, over socket pairs. Run by run-tool-tests.sh.
set -euo pipefail

here="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
repo="$here/../../.."
tmp="$(mktemp -d)"
trap 'rm -rf -- "${tmp:?}"' EXIT

n=0; failed=0
pass() { n=$((n+1)); echo "ok $n - $1"; }
fail() { n=$((n+1)); echo "not ok $n - $1"; failed=$((failed+1)); }
# expect <name> <expected exit> <substring>: checks $status and $out_text from the last run.
expect() {
  if [[ "$status" == "$2" && "$out_text" == *"$3"* ]]; then pass "$1"; else
    fail "$1 (exit $status, wanted $2; output: $out_text)"; fi
}
sha() {
  python3 -I -c 'import hashlib, sys
print(hashlib.sha256(open(sys.argv[1], "rb").read()).hexdigest())' "$1"
}

tree="$tmp/tree"
drv="$tree/test/client/driver"
mkdir -p "$drv" "$tree/third_party/redshift_odbc"
cp "$here/run-windows-loading.sh" "$here/windows-runner-tools.sh" "$here/make_pe.py" "$drv/"
cp -R "$repo/third_party/redshift_odbc/tools" "$tree/third_party/redshift_odbc/"
cat >"$drv/windows_modules.py" <<'EOF'
import sys
sys.exit("self-test: the stand-in failed" if sys.argv[1:] == ["self-test", "fail"] else 0)
EOF
# The copy runs the stand-in's self-test with $STUB_SELF_TEST after it, so fail makes it fail.
sed 's|windows_modules.py" self-test|windows_modules.py" self-test ${STUB_SELF_TEST:-}|' \
  "$here/run-windows-loading.sh" >"$drv/run-windows-loading.sh"

mkdir "$tmp/bin"
printf '#!/bin/sh\necho MINGW64_NT-10.0-20348\n' >"$tmp/bin/uname"
cat >"$tmp/bin/cygpath" <<EOF
#!/bin/sh
# -w -l: the long Windows form, marked WIN: here. -u: C:\\Windows is this run's stand-in
# SystemRoot, as the bare-names step reads it.
if [ "\$1" = -u ] && [ \$# = 2 ]; then
  case "\$2" in
  'C:\\Windows') echo '$tmp/winroot'; exit 0 ;;
  *) echo "cygpath: unexpected \$*" >&2; exit 2 ;;
  esac
fi
[ "\$1" = -w ] && [ "\$2" = -l ] && [ \$# = 3 ] || { echo "cygpath: unexpected \$*" >&2; exit 2; }
echo "WIN:\$3"
EOF
# msiexec /a WIN:<msi> /qn TARGETDIR=WIN:<dir> /l*v WIN:<log>, into an empty directory. Each call
# is logged with the MSYS variables it saw. STUB_MSIEXEC: fail exits 1 after writing a log, change
# alters a file of the image after writing it, and change-source appends a byte to the staged MSI,
# STUB_SOURCE, before reading its own argument, as another process could once the hash is taken.
cat >"$tmp/bin/msiexec" <<'EOF'
#!/usr/bin/env bash
set -euo pipefail
{ printf '%s|' "$@"; echo " conv=${MSYS_NO_PATHCONV:-}${MSYS2_ARG_CONV_EXCL:-}"; } >>"$STUB_LOG"
[[ $# == 6 && "$1" == /a && "$2" == WIN:* && "$3" == /qn && "$4" == TARGETDIR=WIN:* &&
  "$5" == "/l*v" && "$6" == WIN:* ]] || { echo "msiexec: unexpected arguments" >&2; exit 3; }
msi="${2#WIN:}" target="${4#TARGETDIR=WIN:}" log="${6#WIN:}"
[[ -d "$target" && -z "$(ls -A "$target")" ]] ||
  { echo "msiexec: TARGETDIR is not an empty directory" >&2; exit 3; }
# msiexec writes its log in UTF-16, with a byte order mark.
[[ "${STUB_MSIEXEC:-}" != change-source ]] || printf 'x' >>"$STUB_SOURCE"
if [[ "${STUB_MSIEXEC:-}" == fail ]]; then
  python3 -I -c 'import sys
open(sys.argv[1], "wb").write("MSI (s): Product: error 1603\r\n".encode("utf-16"))' "$log"
  exit 1
fi
python3 -I -B - "$STUB_TOOLS/lib/msi.py" "$msi" "$target" <<'PY'
import importlib.util, os, sys
spec = importlib.util.spec_from_file_location("msi", sys.argv[1])
msi = importlib.util.module_from_spec(spec)
spec.loader.exec_module(msi)
db, files = msi.read(sys.argv[2])
bodies = msi.contents(db, files)
for key, _, _, path, _, _, _ in files:
    out = os.path.join(sys.argv[3], *path[2:].replace("%20", " ").split("/"))
    os.makedirs(os.path.dirname(out), exist_ok=True)
    open(out, "wb").write(bodies[key])
PY
if [[ "${STUB_MSIEXEC:-}" == change ]]; then
  printf 'x' >>"$target/PFiles64/Amazon Redshift ODBC Driver (x64)/msvcr100.dll"
fi
EOF
# powershell logs its arguments, and answers the identity query with STUB_ELEVATED (False when
# unset), STUB_SID (STUB_TOKEN_SID by default; none leaves it out) and STUB_GROUPS, the token's
# group SIDs (an interactive logon's by default; none leaves them out), after STUB_PS_STDERR on
# stderr if set, and otherwise Get-AuthenticodeSignature's status; icacls logs its arguments.
cat >"$tmp/bin/powershell" <<'EOF'
#!/bin/sh
case "$*" in
*IsInRole*User.Value*Groups*)
  echo "$*" >>"$STUB_LOG"
  [ -z "${STUB_PS_STDERR:-}" ] || echo "$STUB_PS_STDERR" >&2
  printf '%s\r\n' "${STUB_ELEVATED-False}"
  [ "${STUB_SID:-}" = none ] || printf '%s\r\n' "${STUB_SID:-$STUB_TOKEN_SID}"
  [ "${STUB_GROUPS:-}" = none ] ||
    printf '%s\r\n' "${STUB_GROUPS:-S-1-1-0 S-1-5-32-545 S-1-5-4 S-1-2-1 S-1-5-11 S-1-5-15}"
  exit 0 ;;
esac
echo "$* file=$RS_AUTHENTICODE_FILE" >>"$STUB_LOG"
[ "${STUB_AUTHENTICODE:-Valid}" != fail ] || exit 1
printf '%s\r\n' "${STUB_AUTHENTICODE:-Valid}"
EOF
cat >"$tmp/bin/icacls" <<'EOF'
#!/bin/sh
echo "icacls $*" >>"$STUB_LOG"
[ "${STUB_ICACLS:-}" != fail ] || exit 5
EOF
# The stand-in System32, whose version.dll the bare-names step plants under the original names.
mkdir -p "$tmp/winroot/System32"
printf 'MZ a benign image\n' >"$tmp/winroot/System32/version.dll"
chmod +x "$tmp/bin/"*

# The build: a renamed lock of rsodbc.dll and msvcr100.dll from one MSI, and what the build left.
url="https://s3.amazonaws.com/redshift-downloads/drivers/odbc/2.3.0.0"
url+="/AmazonRedshiftODBC64-2.3.0.0.msi"
dir="./PFiles64/Amazon%20Redshift%20ODBC%20Driver%20(x64)"
g_driver=abcdefghijklmnop.dll g_msvcr=qrstuvwxyz234567.dll
python3 -I "$here/make_pe.py" "$tmp/rsodbc.dll" '{"trailing": "driver"}'
python3 -I "$here/make_pe.py" "$tmp/msvcr100.dll" '{"trailing": "msvcr100"}'
file_row() { echo "[\"$1\", \"$1\", \"INSTALLDIR\", \"@$tmp/$1\"]"; }
python3 -I "$here/make_msi.py" "$tmp/good.msi" \
  "{\"files\": [$(file_row rsodbc.dll), $(file_row msvcr100.dll)]}"
msi_sha="$(sha "$tmp/good.msi")"
seed_and_hash="windows_amd64 name-seed $(printf 'b%.0s' {1..64})
windows_amd64 set-hash $(printf 'c%.0s' {1..64})"
renames="windows_amd64 rename rsodbc.dll $g_driver
windows_amd64 rename msvcr100.dll $g_msvcr
$seed_and_hash"
# lock <out> [<msvcr100.dll's sha256>] [<rename rows>]
lock() {
  local d m="${2:-$(sha "$tmp/msvcr100.dll")}"
  d="$(sha "$tmp/rsodbc.dll")"
  { printf '%s\n' "format 2" "version 2.3.0.0" "status development"
    echo "windows_amd64 driver rsodbc.dll $d $d $url $msi_sha $dir/rsodbc.dll"
    echo "windows_amd64 library msvcr100.dll $m $m $url $msi_sha $dir/msvcr100.dll"
    printf '%s\n' "${3:-$renames}"
    grep '^\* ' "$repo/third_party/redshift_odbc/driver.lock"; } >"$1"
}
build="$tmp/build"
ext="$build/extension/redshift"
set_dir="$ext/redshift_driver"
mkdir -p "$set_dir/packages"
lock "$ext/redshift_driver.lock"
cp "$tmp/good.msi" "$set_dir/packages/$msi_sha"
cp "$tmp/rsodbc.dll" "$set_dir/$g_driver"
cp "$tmp/msvcr100.dll" "$set_dir/$g_msvcr"
cp "$repo/third_party/redshift_odbc/"{LICENSE,NOTICE,THIRD_PARTY_LICENSES} "$set_dir/"
# image <out> [<msvcr100.dll's resource file>] [<more resources>]: a loadable extension holding
# the set as RCDATA resources, under the labels the resource script gives them.
image() {
  local r=""
  for pair in "DRIVER $set_dir/$g_driver" "LIBRARY_MSVCR100_DLL ${2:-$set_dir/$g_msvcr}" \
    "NOTICE_LICENSE $set_dir/LICENSE" "NOTICE_NOTICE $set_dir/NOTICE" \
    "NOTICE_THIRD_PARTY_LICENSES $set_dir/THIRD_PARTY_LICENSES"; do
    r+="${r:+, }[10, \"${pair%% *}\", 0, \"@${pair#* }\"]"
  done
  python3 -I "$here/make_pe.py" "$1" \
    "{\"resources\": [$r${3:+, $3}], \"rdata\": {${4:-$entry_export}}, \"trailing\": \"footer\"}"
}
# The one entry point DuckDB's loader looks up in a C++ extension, by the undecorated name the
# extension exports inside extern "C" and the loader passes to GetProcAddress.
entry="redshift_duckdb_cpp_init"
# A Windows build exports DuckDB's own API beside the entry point, so the good image does too.
entry_export="\"export_name\": \"redshift.duckdb_extension\","
entry_export+=" \"exported\": [\"$entry\", \"duckdb_vector_size\"]"
entry_export_with_sql="\"export_name\": \"redshift.duckdb_extension\","
entry_export_with_sql+=" \"exported\": [\"$entry\", \"SQLDriverConnect\"]"
entry_export_with_loader="\"export_name\": \"redshift.duckdb_extension\","
entry_export_with_loader+=" \"exported\": [\"$entry\", \"redshift_LoadEmbeddedDriver\"]"
entry_export_with_set_name="\"export_name\": \"redshift.duckdb_extension\","
entry_export_with_set_name+=" \"exported\": [\"$entry\", \"rsodbc_hook\"]"
image "$ext/redshift.duckdb_extension"

# The client suite's stand-in: the interface suite's import-table case, which reads RS_IMPORTS_DLL.
# It reads the DLL it is handed, as dumpbin would, and fails naming odbc32.dll when the import table
# has one.
# STUB_IMPORTS: pass makes it pass whatever it was handed, silent makes it fail without naming it.
cat >"$ext/redshift_client_tests.exe" <<'EOF'
#!/usr/bin/env bash
dll="${RS_IMPORTS_DLL#WIN:}"
echo "redshift_client_tests $* dll=$dll require_dumpbin=${RS_REQUIRE_DUMPBIN:-}" >>"$STUB_LOG"
[[ -f "$dll" ]] || { echo "the DLL is not readable: $dll"; exit 1; }
[[ "${STUB_IMPORTS:-}" != pass ]] || { echo "All tests passed"; exit 0; }
# The case's own INFO line, which Catch prints with any failure of it, names the DLL's path.
echo "DLL: $dll"
if LC_ALL=C grep -qi 'ODBC32\.dll' "$dll"; then
  # silent: the case fails on another of its checks, not the import one.
  [[ "${STUB_IMPORTS:-}" != silent ]] || { echo "FAILED: REQUIRE( status == 0 )"; exit 1; }
  echo 'FAILED: CHECK( output.find("odbc32.dll") == std::string::npos )'
  exit 1
fi
echo "All tests passed"
EOF
chmod +x "$ext/redshift_client_tests.exe"
# The bare-names tool's stand-in: its own rules are checked below, against the repository's lock.
cat >"$drv/windows_bare_names.py" <<'EOF'
import sys
if sys.argv[1:2] != ["check"] or len(sys.argv) != 5:
    sys.exit("windows_bare_names: the stand-in takes check <lock> <plant directory> <fake file>")
open(sys.argv[2], encoding="ascii").read()
if not __import__("os").path.isdir(sys.argv[3]) or not __import__("os").path.isfile(sys.argv[4]):
    sys.exit("windows_bare_names: the stand-in was not given a folder and a file")
import os
if os.environ.get("STUB_BARE_NAMES") == "fail":
    sys.exit("refused: a load of the string that replaced krbcc64.dll was loaded, not refused")
print("windows_bare_names: the stand-in judged %s with the fake %s" % (sys.argv[3], sys.argv[4]))
EOF

log="$tmp/stub.log"
token_sid=S-1-5-21-1004336348-1177238915-682003330-1001
script="$drv/run-windows-loading.sh"
tool="$drv/windows-runner-tools.sh"
# run <command>...: with the stand-ins first on PATH and the stub log emptied.
run() {
  : >"$log"
  out_text="$(env -u RS_DRIVER_MIRROR -u RS_REQUIRE_MSIEXEC PATH="$tmp/bin:$PATH" STUB_LOG="$log" \
    USERNAME=Administrator STUB_TOKEN_SID="$token_sid" SYSTEMROOT='C:\Windows' \
    STUB_TOOLS="$tree/third_party/redshift_odbc/tools" "$@" 2>&1)" && status=0 || status=$?
}
logged() { grep -qxF -- "$1" "$log"; }
names=("the module-list helper's own cases" "the downloader's bounds and refusals"
  "msiexec /a of the locked MSI, compared with the lock"
  "Get-AuthenticodeSignature on the renamed msvcr100.dll"
  "the set the loadable extension writes is the lock's"
  "the loadable extension's export table"
  "a planted ODBC32.dll import fails the import-table case"
  "a rewritten bare-name string maps nothing and can name no file")

run bash "$script" "$build"
expect "every suite passes on a good build" 0 "run-windows-loading: 8 runner-only suites passed"
missing=""
for name in "${names[@]}"; do grep -qxF "# $name" <<<"$out_text" || missing+=" [$name]"; done
if [[ -z "$missing" ]]; then pass "each named suite runs"; else
  fail "each named suite runs (missing:$missing)"; fi
msiexec_call='^/a\|WIN:/.*/package\.msi\|/qn\|TARGETDIR=WIN:/.*/image\|'
msiexec_call+='/l\*v\|WIN:/.*/msiexec\.log\| conv=1\*$'
if grep -qE "$msiexec_call" "$log"; then
  pass "msiexec gets /a on the hashed copy and a new, empty TARGETDIR, as Windows paths"
else fail "msiexec gets /a on the hashed copy and a new, empty TARGETDIR (log: $(cat "$log"))"; fi
ps="-NoProfile -NonInteractive -Command"
ps+=" (Get-AuthenticodeSignature -LiteralPath \$env:RS_AUTHENTICODE_FILE).Status.ToString()"
if logged "$ps file=WIN:$set_dir/$g_msvcr"; then
  pass "Get-AuthenticodeSignature reads the renamed msvcr100.dll, by its Windows path"
else fail "Get-AuthenticodeSignature reads the renamed msvcr100.dll (log: $(cat "$log"))"; fi
query='$i = [Security.Principal.WindowsIdentity]::GetCurrent(); ([Security.Principal.WindowsPrincipal]'
query+='$i).IsInRole([Security.Principal.WindowsBuiltInRole]::Administrator); $i.User.Value; '
query+="(\$i.Groups | ForEach-Object { \$_.Value }) -join ' '"
if logged "-NoProfile -NonInteractive -Command $query"; then
  pass "the identity query asks for elevation, the user's SID and the space-joined group SIDs"
else fail "the identity query asks for elevation, the SID and the groups (log: $(cat "$log"))"; fi
# The downloader's cases, over this host's sockets.
refused="downloading https://h.example/a.msi failed:"
for line in "within: downloaded 8 bytes with SHA-256 $(printf 12345678 | sha /dev/stdin)" \
  "declared: failed: $refused its declared 11 bytes are over the 10-byte bound" \
  "streamed: failed: $refused the body passed the 10-byte bound and was stopped" \
  "redirect: failed: https://h.example/a.msi answered HTTP 302; redirects are not followed" \
  "stalled: failed: $refused timed out" \
  "http: downloading http://h.example/a.msi failed: only an https:// URL with no user or $(
  )fragment is downloaded"; do
  if grep -qxF -- "$line" <<<"$out_text"; then pass "the downloader suite: ${line%%:*}"; else
    fail "the downloader suite: ${line%%:*} (output: $out_text)"; fi
done

mkdir "$tree/build"; ln -s "$build" "$tree/build/release"
run bash "$script"
expect "with no build dir it checks build/release" 0 \
  "run-windows-loading: 8 runner-only suites passed"
run env PATH="$PATH" bash "$here/run-windows-loading.sh" "$build"
expect "the script in place refuses to run on this host" 1 \
  "runs on Windows, from Git's bash, not on $(uname -s)"

# A failing or missing suite fails the run, naming it.
last() { [[ "$status" == 1 && "${out_text##*$'\n'}" == "run-windows-loading: $1" ]]; }
run env STUB_SELF_TEST=fail bash "$script" "$build"
if last "1 of 8 failed: ${names[0]}" && grep -qxF "# ${names[7]}" <<<"$out_text"; then
  pass "a failing suite fails the run, naming it, after every suite ran"
else fail "a failing suite fails the run, naming it (exit $status; $out_text)"; fi
mv "$tool" "$tmp/tools.sh"
run bash "$script" "$build"
mv "$tmp/tools.sh" "$tool"
if last "7 of 8 failed: ${names[1]}; ${names[2]}; ${names[3]}; ${names[4]}; ${names[5]}; $(
  )${names[6]}; ${names[7]}"; then
  pass "a missing tool script fails each of its suites, by name"
else fail "a missing tool script fails each of its suites, by name (exit $status; $out_text)"; fi
run bash "$tool" admin-image "$build"
# The grant names the token's SID, not USERNAME, which the cases set to another account's name.
grant="/inheritance:r /grant:r \\*$token_sid:\\(OI\\)\\(CI\\)F"
if grep -qE "^icacls WIN:/.*/image $grant\$" "$log" &&
  [[ "$(grep -n -e '^icacls ' -e '^/a|' "$log" | cut -d: -f2 | cut -c1-6 | tr '\n' ' ')" == \
    "icacls /a|WIN " ]]; then
  pass "the image folder gets an owner-only access list before msiexec writes it"
else fail \
  "the image folder gets an owner-only access list before msiexec writes it (log: $(cat "$log"))"; \
  fi
run env STUB_ELEVATED=True bash "$tool" admin-image "$build"
if [[ "$status" == 1 && "$out_text" == \
  *"this account is elevated (an administrator's token is in use); msiexec was not run"* ]] &&
  ! grep -q '^/a' "$log"; then
  pass "an elevated account is refused before msiexec runs"
else fail "an elevated account is refused before msiexec runs (exit $status; $out_text)"; fi

# A well-formed answer for a logon that is not interactive skips the msiexec /a suite, elevated or
# not; an interactive logon with an elevated token, or an answer that is not well formed, fails the
# run.
batch="S-1-1-0 S-1-5-32-545 S-1-5-3 S-1-2-1 S-1-5-11 S-1-5-15 S-1-5-64-10"
elevated_why="this account is elevated (an administrator's token is in use); msiexec was not run"
batch_why="this logon is not interactive: its token lacks INTERACTIVE (S-1-5-4) and holds BATCH"
batch_why+=" (S-1-5-3); msiexec was not run"
service_why="this logon is not interactive: its token lacks INTERACTIVE (S-1-5-4) and holds"
service_why+=" SERVICE (S-1-5-6); msiexec was not run"
by_hand="it runs by hand on each driver update, in an interactive logon as a non-elevated account,"
by_hand+=" with RS_REQUIRE_MSIEXEC=1"
# fails_run <case> <reason> <env>...: the run fails at the msiexec /a suite with the reason, it is
# no SKIP, and msiexec does not run.
fails_run() {
  run env "${@:3}" bash "$script" "$build"
  if last "1 of 8 failed: ${names[2]}" && [[ "$out_text" == *"$2"* &&
    "$out_text" != *"# SKIP"* ]] && ! grep -q '^/a' "$log"; then pass "$1"
  else fail "$1 (exit $status; $out_text)"; fi
}
fails_run "an elevated account fails the run, and msiexec does not run" "$elevated_why" \
  STUB_ELEVATED=True
# With the cross-check required, that skip is a failure, and its reason still names the token.
fails_run "RS_REQUIRE_MSIEXEC=1 makes an elevated batch logon fail the run" \
  "RS_REQUIRE_MSIEXEC=1, so this logon's skip is a failure" \
  STUB_ELEVATED=True STUB_GROUPS="$batch" RS_REQUIRE_MSIEXEC=1
fails_run "an elevation answer that is not True or False fails the run, not a skip" \
  "powershell answered 'garbage', not True or False" STUB_ELEVATED=garbage STUB_GROUPS="$batch"
fails_run "an empty answer fails the run, not a skip" \
  "powershell answered '', not True or False" STUB_ELEVATED= STUB_SID=none STUB_GROUPS=none
fails_run "a groups line that is not SIDs fails the run, not a skip" \
  "groups line for this token, 'System.Object[]', is not a list of SIDs" \
  STUB_GROUPS='System.Object[]'
fails_run "RS_REQUIRE_MSIEXEC=1 makes a batch logon fail the run" \
  "RS_REQUIRE_MSIEXEC=1, so this logon's skip is a failure" RS_REQUIRE_MSIEXEC=1 \
  STUB_GROUPS="$batch"
# skips <case> <reason> <env>...: the run passes, the suite is a SKIP by its name with the reason,
# counted in the summary, and neither icacls nor msiexec runs.
skips() {
  run env "${@:3}" bash "$script" "$build"
  if [[ "$status" == 0 && "${out_text##*$'\n'}" == \
    "run-windows-loading: 7 runner-only suites passed, 1 skipped: ${names[2]}" &&
    "$out_text" == *"# ${names[2]}"$'\n'"# SKIP ${names[2]}: $2; $by_hand"* ]] &&
    ! grep -q -e '^icacls' -e '^/a' "$log"; then pass "$1"
  else fail "$1 (exit $status; $out_text; log: $(cat "$log"))"; fi
}
# A logon that is not interactive skips whatever its token holds: a build host may offer only an
# administrator, and the cross-check runs in an interactive logon anyway.
skips "an elevated account in a batch logon skips, saying both" \
  "this logon is not interactive: its token lacks INTERACTIVE (S-1-5-4) and holds BATCH (S-1-5-3) \
and is elevated; msiexec was not run" STUB_ELEVATED=True STUB_GROUPS="$batch"
skips "a batch logon skips the msiexec /a suite, saying so, and msiexec does not run" \
  "$batch_why" STUB_GROUPS="$batch"
skips "a service logon skips the msiexec /a suite, saying so" "$service_why" \
  STUB_GROUPS="S-1-1-0 S-1-5-32-545 S-1-5-6 S-1-5-11 S-1-5-64-10"
skips "what powershell prints on stderr stays out of the SKIP line" "$batch_why" \
  STUB_PS_STDERR='#< CLIXML <Objs Version="1.1.0.1"><Obj S="progress" RefId="0"/></Objs>' \
  STUB_GROUPS="$batch"
skips "RS_REQUIRE_MSIEXEC=0 lets a batch logon skip" "$batch_why" RS_REQUIRE_MSIEXEC=0 \
  STUB_GROUPS="$batch"
skips "an empty RS_REQUIRE_MSIEXEC lets a batch logon skip" "$batch_why" RS_REQUIRE_MSIEXEC= \
  STUB_GROUPS="$batch"
allowed="it is 1 to require the msiexec /a cross-check, or unset, empty or 0 to let it skip"
for value in yes true 2; do
  run env RS_REQUIRE_MSIEXEC="$value" bash "$script" "$build"
  if [[ "$status" == 1 && "$out_text" == \
    "run-windows-loading: RS_REQUIRE_MSIEXEC is '$value'; $allowed" && ! -s "$log" ]]; then
    pass "RS_REQUIRE_MSIEXEC=$value fails the run before any suite, naming the allowed values"
  else fail "RS_REQUIRE_MSIEXEC=$value fails the run before any suite (exit $status; $out_text)"; fi
done
run env RS_REQUIRE_MSIEXEC=1 bash "$script" "$build"
if [[ "$status" == 0 && "${out_text##*$'\n'}" == "run-windows-loading: 8 runner-only suites passed" ]] &&
  grep -q '^/a' "$log"; then
  pass "an interactive logon as a non-elevated account runs msiexec /a, RS_REQUIRE_MSIEXEC=1 or not"
else fail "an interactive logon runs msiexec /a (exit $status; $out_text)"; fi
run env STUB_MSIEXEC=fail bash "$script" "$build"
if last "1 of 8 failed: ${names[2]}" && [[ "$out_text" == *"msiexec /a exited 1"* ]]; then
  pass "a failing msiexec in an interactive logon fails the run, not a skip"
else fail "a failing msiexec in an interactive logon fails the run (exit $status; $out_text)"; fi
# The skip's reason is the tool's own line, whatever follows it; exit 3 without one is a failure.
# A stand-in tool answers admin-image with STUB_LINES on stderr and exit 3, and the rest as the
# real one does.
cp "$tool" "$drv/real-tools.sh"
cp "$tool" "$tmp/tools.sh"
cat >"$tool" <<'SH'
#!/usr/bin/env bash
[[ "$1" != admin-image ]] || { printf '%s\n' "$STUB_LINES" >&2; exit 3; }
exec bash "$(dirname "${BASH_SOURCE[0]}")/real-tools.sh" "$@"
SH
skips "the skip's reason is the tool's own line, not a line printed after it" "$batch_why" \
  STUB_LINES="windows-runner-tools: admin-image: $batch_why"$'\n'"rm: cannot remove 'x'"
if [[ "$out_text" == *"# SKIP "*$'\n'"rm: cannot remove 'x'"$'\n'* ]]; then
  pass "a skip keeps the tool's other lines, after the SKIP line"
else fail "a skip keeps the tool's other lines, after the SKIP line ($out_text)"; fi
fails_run "exit 3 without the tool's reason fails the run, not a skip" \
  "the tool exited 3 without saying why, so this is a failure" STUB_LINES="rm: cannot remove 'x'"
fails_run "exit 3 with another of the tool's lines fails the run, not a skip" \
  "the tool exited 3 without saying why, so this is a failure" \
  STUB_LINES="windows-runner-tools: admin-image: powershell could not say whether this account is elevated"
mv "$tmp/tools.sh" "$tool"; rm "$drv/real-tools.sh"
# The tool's own answers: exit 3 for a logon that is not interactive, exit 1 for an answer that
# is not well formed.
network="S-1-1-0 S-1-5-32-545 S-1-5-2 S-1-5-11 S-1-5-64-10"
for case in "a network logon|$network|3|lacks INTERACTIVE (S-1-5-4); msiexec was not run" \
  "an interactive logon, INTERACTIVE first|S-1-5-4 S-1-1-0 S-1-5-11|0|its set files are the lock's" \
  "an interactive logon, INTERACTIVE last|S-1-1-0 S-1-5-11 S-1-5-4|0|its set files are the lock's" \
  "a group that only starts S-1-5-4|S-1-1-0 S-1-5-42 S-1-5-3|3|lacks INTERACTIVE (S-1-5-4) and" \
  "a token with no groups|none|1|powershell gave no groups for this token" \
  "a groups line ending in a non-SID|S-1-1-0 S-1-5-4 S-1-5-4x|1|is not a list of SIDs" \
  "a groups line starting with a non-SID|x S-1-1-0 S-1-5-4|1|is not a list of SIDs" \
  "a groups line with two spaces|S-1-1-0  S-1-5-4|1|is not a list of SIDs" \
  "a groups line with a dotted SID|S-1-1-0 S-1-5.4 S-1-5-4|1|is not a list of SIDs"; do
  IFS='|' read -r what groups code want <<<"$case"
  run env STUB_GROUPS="$groups" bash "$tool" admin-image "$build"
  ran=no want_ran=no
  ! grep -q '^/a' "$log" || ran=yes
  [[ "$code" != 0 ]] || want_ran=yes
  if [[ "$status" == "$code" && "$out_text" == *"$want"* && "$ran" == "$want_ran" ]]; then
    pass "$what exits $code, msiexec running only on 0"
  else fail "$what exits $code, msiexec running only on 0 (exit $status; $out_text)"; fi
done
run bash "$tool" admin-image
expect "a step without its build dir is a usage error" 2 "usage: windows-runner-tools.sh downloads"
run bash "$tool" other "$build"
expect "an unknown step is a usage error" 2 "usage: windows-runner-tools.sh downloads"
mkdir -p "$tmp/linux-build/extension/redshift"
linux_lock="$tmp/linux-build/extension/redshift/redshift_driver.lock"
# The checked-in lock without its windows_amd64 rows, as a Linux-only lock has none.
grep -v '^windows_amd64 ' "$repo/third_party/redshift_odbc/driver.lock" >"$linux_lock"
run bash "$tool" admin-image "$tmp/linux-build"
expect "a lock without a windows_amd64 row fails" 1 \
  "the build's lock has no windows_amd64 driver row"
# Every automated logon takes the skip path, so the lock, copy and hash checks run before the
# logon check: a bad lock, MSI or copy fails a batch logon too, and is never a skip.
run env STUB_GROUPS="$batch" bash "$tool" admin-image "$tmp/linux-build"
expect "in a batch logon, a lock without a windows_amd64 row fails, not a skip" 1 \
  "the build's lock has no windows_amd64 driver row"
printf 'format 2\n' >"$linux_lock"
run bash "$tool" written-bytes "$tmp/linux-build"
expect "a lock that check-lock.sh refuses fails" 1 "redshift_driver.lock fails check-lock.sh"
run env STUB_GROUPS="$batch" bash "$tool" admin-image "$tmp/linux-build"
expect "in a batch logon, a lock that check-lock.sh refuses fails, not a skip" 1 \
  "redshift_driver.lock fails check-lock.sh"

# admin-image: the MSI is checked before msiexec runs, and the image after.
cp "$tmp/good.msi" "$tmp/other.msi"; printf 'x' >>"$tmp/other.msi"
cp "$tmp/other.msi" "$set_dir/packages/$msi_sha"
run bash "$script" "$build"
refusal="packages/$msi_sha has SHA-256 $(sha "$tmp/other.msi"), the lock expects $msi_sha;"
if last "1 of 8 failed: ${names[2]}" && ! grep -q '^/a' "$log" &&
  [[ "$out_text" == *"$refusal msiexec was not run"* ]]; then
  pass "an MSI whose SHA-256 is not the lock's is refused before msiexec runs, failing its suite"
else fail "an MSI whose SHA-256 is not the lock's is refused before msiexec runs ($out_text)"; fi
fails_run "in a batch logon, an MSI whose SHA-256 is not the lock's fails the run, not a skip" \
  "$refusal msiexec was not run" STUB_GROUPS="$batch"
mkdir "$tmp/mirror"; cp "$tmp/good.msi" "$tmp/mirror/$msi_sha"
run env RS_DRIVER_MIRROR="$tmp/mirror" bash "$tool" admin-image "$build"
expect "RS_DRIVER_MIRROR's copy is used before the build's kept one" 0 \
  "its set files are the lock's"
rm "$set_dir/packages/$msi_sha"
run bash "$tool" admin-image "$build"
expect "no copy of the MSI fails" 1 \
  "neither RS_DRIVER_MIRROR nor $set_dir/packages holds the locked MSI $msi_sha"
fails_run "in a batch logon, no copy of the MSI fails the run, not a skip" \
  "neither RS_DRIVER_MIRROR nor $set_dir/packages holds the locked MSI $msi_sha" \
  STUB_GROUPS="$batch"
cp "$tmp/good.msi" "$set_dir/packages/$msi_sha"
run env STUB_MSIEXEC=change bash "$tool" admin-image "$build"
expect "a changed file in the administrative image fails compare" 1 \
  "msi.py compare: files: $dir/msvcr100.dll in the administrative image differs from the cabinet's"
# With no SID line the groups line is read in its place, and named as the answer.
for sid in none Administrator 'S-1-5-21-1;x'; do
  shown="$sid"; [[ "$sid" != none ]] || shown="S-1-1-0 S-1-5-32-545 S-1-5-4 S-1-2-1 S-1-5-11 S-1-5-15"
  no_sid="powershell gave '$shown', not a SID, for this account, so the image folder cannot be made owner-only"
  run env STUB_SID="$sid" bash "$tool" admin-image "$build"
  if [[ "$status" == 1 && "$out_text" == *"$no_sid"* ]] &&
    ! grep -q -e '^icacls' -e '^/a' "$log"; then
    pass "a SID of '$sid' is refused before icacls or msiexec runs"
  else fail "a SID of '$sid' is refused before icacls or msiexec runs (exit $status; $out_text)"; fi
done
run env STUB_ICACLS=fail bash "$tool" admin-image "$build"
if [[ "$status" == 1 && "$out_text" == *"icacls could not give the image folder an owner-only access list; msiexec was not run"* ]] &&
  ! grep -q '^/a' "$log"; then
  pass "a failing icacls is refused before msiexec runs"
else fail "a failing icacls is refused before msiexec runs (exit $status; $out_text)"; fi
cp "$set_dir/packages/$msi_sha" "$tmp/kept.msi"
run env STUB_MSIEXEC=change-source STUB_SOURCE="$set_dir/packages/$msi_sha" bash "$tool" admin-image "$build"
expect "msiexec reads the copy that was hashed, whatever happens to the staged MSI after" 0 \
  "its set files are the lock's"
cp "$tmp/kept.msi" "$set_dir/packages/$msi_sha"
run env STUB_MSIEXEC=fail bash "$tool" admin-image "$build"
expect "a failed msiexec fails, with the end of its log" 1 \
  "msiexec /a exited 1; the end of its log: MSI (s): Product: error 1603"
# A lock whose msvcr100.dll has other bytes than the MSI's.
wrong="$(printf 'b%.0s' {1..64})"
wrong_build="$tmp/wrong-build"
cp -R "$build" "$wrong_build"
lock "$wrong_build/extension/redshift/redshift_driver.lock" "$wrong"
run bash "$tool" admin-image "$wrong_build"
expect "a set file whose SHA-256 in the image is not the lock's fails" 1 \
  "msvcr100.dll at $dir/msvcr100.dll in the administrative image has SHA-256 $(
  sha "$tmp/msvcr100.dll"), the lock expects $wrong"

# authenticode
run env STUB_AUTHENTICODE=NotSigned bash "$tool" authenticode "$build"
expect "an Authenticode status of NotSigned fails" 1 \
  "Get-AuthenticodeSignature reports NotSigned for $set_dir/$g_msvcr, not Valid"
run env STUB_AUTHENTICODE=HashMismatch bash "$tool" authenticode "$build"
expect "an Authenticode status of HashMismatch fails" 1 \
  "Get-AuthenticodeSignature reports HashMismatch for $set_dir/$g_msvcr, not Valid"
run env STUB_AUTHENTICODE=fail bash "$tool" authenticode "$build"
expect "a PowerShell that fails fails" 1 \
  "Get-AuthenticodeSignature could not read $set_dir/$g_msvcr"
run bash "$tool" authenticode "$wrong_build"
expect "a renamed msvcr100.dll that is not the lock's bytes fails" 1 \
  "$g_msvcr has SHA-256 $(sha "$tmp/msvcr100.dll"), the lock expects $wrong"
if [[ ! -s "$log" ]]; then pass "PowerShell does not run for it"; else
  fail "PowerShell does not run for it ($(cat "$log"))"; fi
lock "$wrong_build/extension/redshift/redshift_driver.lock" "" \
  "windows_amd64 rename rsodbc.dll $g_driver
$seed_and_hash"
run bash "$tool" authenticode "$wrong_build"
expect "a lock that renames no msvcr100.dll fails" 1 "the build's lock renames no msvcr100.dll"
rm "$wrong_build/extension/redshift/redshift_driver/$g_msvcr"
lock "$wrong_build/extension/redshift/redshift_driver.lock"
run bash "$tool" authenticode "$wrong_build"
expect "a missing renamed msvcr100.dll fails" 1 "$g_msvcr, the renamed msvcr100.dll, is missing"
# Windows ignores case, so the lock may name it MSVCR100.dll.
sed 's/ msvcr100\.dll / MSVCR100.dll /' "$ext/redshift_driver.lock" \
  >"$wrong_build/extension/redshift/redshift_driver.lock"
cp "$tmp/msvcr100.dll" "$wrong_build/extension/redshift/redshift_driver/$g_msvcr"
run bash "$tool" authenticode "$wrong_build"
expect "a lock that names it MSVCR100.dll passes" 0 "$g_msvcr, the renamed MSVCR100.dll, is Valid"

# written-bytes
run bash "$tool" written-bytes "$build"
expect "the loadable extension holding the lock's set passes" 0 \
  "holds the 5 files of the lock's set"
image "$ext/redshift.duckdb_extension" "$tmp/rsodbc.dll"
run bash "$tool" written-bytes "$build"
expect "a resource that is not the lock's bytes fails, naming the file" 1 \
  "$g_msvcr (RCDATA \"LIBRARY_MSVCR100_DLL\") is ['$(sha "$tmp/rsodbc.dll")'], the lock $(
  )expects $(sha "$tmp/msvcr100.dll") once"
image "$ext/redshift.duckdb_extension" "" "[10, \"EXTRA\", 0, \"x\"]"
run bash "$tool" written-bytes "$build"
expect "a resource that is no file of the set fails" 1 \
  "RCDATA \"EXTRA\" is not a file of the lock's set"
image "$ext/redshift.duckdb_extension" "" \
  "[10, \"NOTICE_LICENSE\", 1033, \"@$set_dir/LICENSE\"]"
run bash "$tool" written-bytes "$build"
expect "a set file held twice fails" 1 "LICENSE (RCDATA \"NOTICE_LICENSE\") is ['"
python3 -I "$here/make_pe.py" "$ext/redshift.duckdb_extension" '{"trailing": "no resources"}'
run bash "$tool" written-bytes "$build"
expect "an extension without the set fails" 1 "LICENSE (RCDATA \"NOTICE_LICENSE\") is missing"
rm "$ext/redshift.duckdb_extension"
run bash "$tool" written-bytes "$build"
expect "a missing extension fails" 1 "cannot read $ext/redshift.duckdb_extension"

# The downloader suite fails on a downloader that breaks a rule, naming the case.
downloader="$tree/third_party/redshift_odbc/tools/lib/download.py"
cp "$downloader" "$tmp/download.py"
# broken <name> <sed script> <expected failure>: the suite on download.py changed so.
broken() {
  sed "$2" "$tmp/download.py" >"$downloader"
  run bash "$tool" downloads
  expect "$1" 1 \
    "windows-runner-tools: downloads: $3"
}
broken "a downloader that takes a redirect's answer as the body fails the suite" \
  's/if response.status != 200:/if response.status not in (200, 302):/' \
  "redirect: downloaded 0 bytes"
broken "a downloader that reads a body declared over the bound fails the suite" \
  's/^        if declared is not None and .*:$/        if False:/' \
  "declared: failed: $refused the body passed the 10-byte bound and was stopped, wanted failed:"
broken "a downloader that leaves a partial body fails the suite" \
  's/^        os.unlink(out)$/        pass/' \
  "streamed: failed: $refused the body passed the 10-byte bound and was stopped, and left"
broken "a downloader that exits 0 on an http:// URL fails the suite" \
  's/^        return 1$/        return 0/' \
  "http: exit 0, downloading http://h.example/a.msi failed: only an https:// URL"
broken "a downloader that leaves a file on an http:// URL fails the suite" \
  $'s/^    if parts.scheme != "https"/    if parts.scheme == "http": open(out, "w").close()\\\n&/' \
  "http: exit 1, downloading http://h.example/a.msi failed: only an https:// URL"
broken "a downloader that refuses an http:// URL with another message fails the suite" \
  's|only an https:// URL with no user|only a secure URL with no user|' \
  "http: exit 1, downloading http://h.example/a.msi failed: only a secure URL"
broken "a downloader that keeps a body short of its declared length fails the suite" \
  's/^                    if declared is not None and size != declared:$/                    if False:/' \
  "short: downloaded 2 bytes"
cp "$tmp/download.py" "$downloader"

# The export table. A step run on its own, so each rule names its own image, and the suite's own
# name is in the run above.
exports_says() { # exports_says <name> <substring> [<rdata spec>]
  local image="$ext/redshift.duckdb_extension"
  image "$image" "" "" "${3:-}"
  run bash "$tool" exports "$build"
  expect "$1" 1 "$2"
  image "$image"  # the good image again, for the steps after this one
}
exports_says "an extension that exports nothing fails the export check" \
  "its export table does not hold DuckDB's entry point $entry (it holds 0 names)" \
  '"functions": 0'
exports_says "an extension that exports DuckDB's API but not the entry point fails" \
  "does not hold DuckDB's entry point $entry (it holds 1 name)" \
  "\"export_name\": \"redshift.duckdb_extension\", \"exported\": [\"duckdb_vector_size\"]"
exports_says "an extension that exports a driver entry point fails, naming it" \
  "SQLDriverConnect is exported, which is the loader's, the driver's or a set file's" \
  "$entry_export_with_sql"
exports_says "an extension that exports a loader name fails, naming it" \
  "redshift_LoadEmbeddedDriver is exported, which is the loader's" \
  "$entry_export_with_loader"
exports_says "an extension that exports a set file's name fails, naming it" \
  "rsodbc_hook is exported, which is the loader's" "$entry_export_with_set_name"
exports_says "an extension that forwards an export fails" "an export is forwarded to" \
  "$entry_export, \"forwarders\": [\"other.f\"]"
exports_says "an extension that exports an unnamed function fails" \
  "@3 is exported with no name, which GetProcAddress can still reach" \
  "$entry_export, \"functions\": 1"
# Each other word the rule refuses, inside a longer name as an export would hold it, so a word
# dropped from the list or misspelt there fails here.
for word in redshift_driver LoadEmbeddedDriver AllocateEnvironmentHandle DriverLoader \
  LoadDriverFileSet windows_driver rsodbc krb5_64 gssapi64 k5sprt64 comerr64 wshelp64 msvcr100 ssl; do
  exports_says "an extension that exports a name holding $word fails, naming it" \
    "x_${word}_y is exported, which is the loader's, the driver's or a set file's" \
    "\"export_name\": \"redshift.duckdb_extension\", \"exported\": [\"$entry\", \"x_${word}_y\"]"
done
run bash "$tool" exports "$build"
expect "the good extension's export table holds the entry point and DuckDB's own names" 0 \
  "exports 2 names, DuckDB's own with the entry point $entry"

# The negative control of the interface suite's import-table case.
run bash "$tool" imports-negative "$build"
expect "the planted-import step fails the case and says so" 0 \
  "the import-table case fails a PE that imports ODBC32.dll, on that import"
if grep -q 'redshift_client_tests .*dll=.*planted-import\.dll require_dumpbin=1' "$log"; then
  pass "the case is given the planted DLL and RS_REQUIRE_DUMPBIN=1, so a missing dumpbin fails it"
else fail "the case's arguments (log: $(cat "$log"))"; fi
run env STUB_IMPORTS=pass bash "$tool" imports-negative "$build"
expect "a case that passes on a planted ODBC32.dll import fails the step" 1 \
  "the import-table case passed on a PE that imports ODBC32.dll"
run env STUB_IMPORTS=silent bash "$tool" imports-negative "$build"
expect "a case that fails on another of its checks fails the step" 1 \
  "the case failed, but not on the ODBC32.dll import"
# The planted file's name must hold no odbc32: the case prints the path with every failure, so a
# name with it in would pass this step on any failure at all.
if ! grep -qi 'dll=.*odbc32' "$log"; then
  pass "the planted file's name holds no odbc32, so the case's own INFO line cannot stand in"
else fail "the planted file's name holds odbc32 (log: $(cat "$log"))"; fi
mv "$ext/redshift_client_tests.exe" "$tmp/suite.exe"
run bash "$tool" imports-negative "$build"
expect "a build with no client suite fails the step" 1 "run make test_driver"
mv "$tmp/suite.exe" "$ext/redshift_client_tests.exe"

# The step greps the interface case's own printed expression (test/client/interface/
# windows_imports_test.cpp): this pins it here, so a rewrite of that CHECK fails on Linux too, not
# only on Windows.
imports_case="$repo/test/client/interface/windows_imports_test.cpp"
if grep -qF 'output.find("odbc32.dll")' "$imports_case" &&
  grep -qF 'The loadable extension imports no ODBC driver manager' "$imports_case"; then
  pass "the import-table case still holds the expression and name the step greps for"
else fail "the import-table case's expression or name changed ($imports_case)"; fi

# The rewritten bare-name strings: the step, with the tool stood in.
run bash "$tool" bare-names "$build"
expect "the bare-names step runs the tool on the build's lock and a fake to plant" 0 \
  "the stand-in judged"
run env STUB_BARE_NAMES=fail bash "$tool" bare-names "$build"
expect "a rewritten string that mapped something fails the step" 1 \
  "a rewritten bare-name string mapped something, or could name a file"
mv "$tmp/winroot/System32/version.dll" "$tmp/winroot/System32/kept"
run bash "$tool" bare-names "$build"
expect "no System32 version.dll to plant fails the step" 1 "to plant under the original names"
mv "$tmp/winroot/System32/kept" "$tmp/winroot/System32/version.dll"
run env SYSTEMROOT= bash "$tool" bare-names "$build"
expect "an empty SystemRoot fails the step" 1 "SystemRoot is not set"

# windows_bare_names.py's own rules, against the lock this repository ships, so they are checked
# against the strings the rename really wrote (the step above runs on Windows only).
bare="$here/windows_bare_names.py"
real_lock="$repo/third_party/redshift_odbc/driver.lock"
hex_of() { printf '%s' "$1" | od -An -tx1 | tr -d ' \n'; }
listed="$(python3 -I "$bare" names "$real_lock")" && status=0 || status=$?
if ((status == 0)) && [[ "$(grep -c . <<<"$listed")" -ge 1 ]] &&
  grep -q '^krbcc64\.dll 7c7c' <<<"$listed" && grep -q '^leashw64\.dll 7c7c' <<<"$listed"; then
  pass "the lock's rewritten strings are the ones that replaced krbcc64.dll and leashw64.dll"
else fail "the lock's rewritten strings ($status: $listed)"; fi
results="$tmp/bare-results"
write_results() { # write_results [<outcome for the first rewritten load>] [<its name outcome>]
  : >"$results"
  while read -r name rewritten; do
    printf '%s original loaded C:\\plant\\%s\n' "$(hex_of "$name")" "$name" >>"$results"
    printf '%s rewritten %s\n' "$rewritten" "${1:-invalid-name}" >>"$results"
    printf '%s name %s\n' "$rewritten" "${2:-refused}" >>"$results"
    set --  # only the first string takes the caller's outcomes
  done <<<"$listed"
}
judge() { python3 -I "$bare" judge "$real_lock" "$results" 'C:\plant'; }
write_results
if out_text="$(judge 2>&1)" && [[ "$out_text" == *"maps nothing and can name no file"* ]]; then
  pass "a run where every rewritten string mapped nothing and each fake loaded passes"
else fail "a good results file ($out_text)"; fi
write_results 'loaded C:\plant\krbcc64.dll'
if ! out_text="$(judge 2>&1)" && [[ "$out_text" == refused:*"it must map nothing"* ]]; then
  pass "a rewritten string that loaded a planted file fails, naming the rule"
else fail "a loaded rewritten string ($out_text)"; fi
# Windows Server 2022 answers such a load with ERROR_MOD_NOT_FOUND (126), which the row takes:
# nothing was mapped, and the name case below shows no file of that name can exist.
write_results 'error 126'
if out_text="$(judge 2>&1)" && [[ "$out_text" == *"maps nothing and can name no file"* ]]; then
  pass "a rewritten string that found no file (ERROR_MOD_NOT_FOUND) passes, as nothing was mapped"
else fail "ERROR_MOD_NOT_FOUND ($out_text)"; fi
write_results 'error 5'
if ! out_text="$(judge 2>&1)" && [[ "$out_text" == refused:*"error 5"* ]]; then
  pass "a rewritten string refused with another error fails: it may have reached a file"
else fail "another error ($out_text)"; fi
write_results invalid-name created
if ! out_text="$(judge 2>&1)" && [[ "$out_text" == name:*"could be created"* ]]; then
  pass "a rewritten string a file could be named after fails, whatever the load did"
else fail "a name that can be created ($out_text)"; fi
write_results
sed -i.bak 's/ original loaded .*/ original error 126/' "$results"
if ! out_text="$(judge 2>&1)" && [[ "$out_text" == control:*"proves nothing"* ]]; then
  pass "a control where no planted fake loaded fails: a refusal then proves nothing"
else fail "a missing control ($out_text)"; fi
write_results
sed -i.bak '1s|C:\\plant|C:\\elsewhere|' "$results"
if ! out_text="$(judge 2>&1)" && [[ "$out_text" == control:*"not from C:\\plant"* ]]; then
  pass "a control whose fake loaded from another folder fails"
else fail "a control from elsewhere ($out_text)"; fi
write_results
cp "$results" "$tmp/results-kept" && head -1 "$tmp/results-kept" >>"$results"
if ! out_text="$(judge 2>&1)" && [[ "$out_text" == names:*"twice"* ]]; then
  pass "a results file with a string's load twice fails, so a doubled line cannot hide one"
else fail "a doubled results line ($out_text)"; fi
cp "$tmp/results-kept" "$results"
printf '%s rewritten invalid-name\n' "$(hex_of other.dll)" >>"$results"
if ! out_text="$(judge 2>&1)" && [[ "$out_text" == names:*"the lock does not record"* ]]; then
  pass "a results file with a load of a string the lock does not record fails"
else fail "an unknown results line ($out_text)"; fi
write_results
head -2 "$results" >"$tmp/short" && mv "$tmp/short" "$results"
if ! out_text="$(judge 2>&1)" && [[ "$out_text" == names:*"the results hold no line for"* ]]; then
  pass "a results file that misses a string's load fails"
else fail "a short results file ($out_text)"; fi
write_results invalid-name exists
if ! out_text="$(judge 2>&1)" && [[ "$out_text" == name:*"(exists)"* ]]; then
  pass "a rewritten string that names a file already in the folder fails"
else fail "a name that exists ($out_text)"; fi
# The control is judged first, so a run whose fakes did not load says so, whatever else it shows.
write_results invalid-name created
sed -i.bak 's/ original loaded .*/ original error 126/' "$results"
if ! out_text="$(judge 2>&1)" && [[ "$out_text" == control:* ]]; then
  pass "a run whose controls failed reports the control first, before a name that could be created"
else fail "the control first ($out_text)"; fi
# create_outcome itself, which check runs on Windows: a name a file is already there under is
# "exists", not "refused", and a name it can create is "created" and leaves nothing.
mkdir "$tmp/create" && : >"$tmp/create/existing"
outcomes="$(cd "$tmp/create" && python3 -I -B -c 'import sys
sys.path.insert(0, sys.argv[1])
import windows_bare_names as w
print(w.create_outcome(b"existing"), w.create_outcome(b"fresh"), w.create_outcome(b"a\0b"))' "$here")"
if [[ "$outcomes" == "exists created refused" && ! -e "$tmp/create/fresh" ]]; then
  pass "a file already under the name is 'exists', a new one 'created' and removed, a NUL 'refused'"
else fail "create_outcome ($outcomes; $(ls "$tmp/create"))"; fi
# names' own rules, on locks written here with hex_of: both row orders, one row per distinct
# string, and a row whose original is not a plain file name.
bars() { printf '7c%.0s' $(seq 1 "$1"); }
patch_row() { # patch_row <original name> <offset> [<new hex>]: a row in check-lock.sh's order
  printf 'patch windows_amd64 krb5_64.dll %s %s %s\n' "$2" "$(hex_of "$1")" "${3:-$(bars "${#1}")}"
}
{ echo "format 2"; patch_row krbcc64.dll 10; } >"$tmp/order-lock"
listed_order="$(python3 -I "$bare" names "$tmp/order-lock")" && status=0 || status=$?
if ((status == 0)) && [[ "$listed_order" == "krbcc64.dll $(bars 11)" ]]; then
  pass "a lock in check-lock.sh's row order (kind first) is read, as the tool is given it"
else fail "check-lock.sh's row order ($status: $listed_order)"; fi
# Two names of one length, so the rename wrote the same string over both: one load, one row.
{ echo "format 2"; patch_row aaaacc64.dll 10; patch_row bbbbcc64.dll 40; } >"$tmp/same-lock"
listed_same="$(python3 -I "$bare" names "$tmp/same-lock")"
if [[ "$(grep -c . <<<"$listed_same")" == 1 ]]; then
  pass "two names the rename wrote one string over give one row, since the string is what loads"
else fail "one row per distinct string ($listed_same)"; fi
# A row whose original is not a plain file name: check plants a fake under it, so each form
# Windows would read as a path fails, not only the one with a forward slash.
for name in "sub/krbcc64.dll" "sub\\krbcc64.dll" "C:krbcc64.dll" "C:\\krbcc64.dll"; do
  { echo "format 2"; patch_row "$name" 10; } >"$tmp/path-lock"
  if ! out_text="$(python3 -I "$bare" names "$tmp/path-lock" 2>&1)" &&
    [[ "$out_text" == names:*"which is not a plain file name"* ]]; then
    pass "the rewritten name $name fails, since a fake is planted under it"
  else fail "a name that is not a plain file name: $name ($out_text)"; fi
done
# A set member's own name, rewritten in place: it names a file of the set, not a bare-name load,
# so it is no rewritten string and the suite must not try to load it.
{ echo "format 2"; echo "library windows_amd64 krb5_64.dll $(printf 'a%.0s' {1..64}) $(
  )$(printf 'b%.0s' {1..64})"; patch_row krb5_64.dll 10 "$(hex_of oddrzrn.dll)"; } >"$tmp/member-lock"
if ! out_text="$(python3 -I "$bare" names "$tmp/member-lock" 2>&1)" &&
  [[ "$out_text" == names:*"records no patch row"* ]]; then
  pass "a patch row that rewrote a set member's own name is no rewritten bare name"
else fail "a set member's name ($out_text)"; fi
# A string rewritten to a length other than its name's is no rename's: each is as long as the name.
{ echo "format 2"; patch_row krbcc64.dll 10 7c7c; } >"$tmp/short-lock"
if ! out_text="$(python3 -I "$bare" names "$tmp/short-lock" 2>&1)" &&
  [[ "$out_text" == names:*"is 2 bytes long, and it must be as long as the name, 11 bytes"* ]]; then
  pass "a rewritten string shorter than the name it replaced fails"
else fail "a rewritten string of another length ($out_text)"; fi
# The rename writes '|' over a bare name without .dll, and over a UTF-16LE one, too; this reader
# loads neither, so such a row fails rather than dropping out of the suite unloaded.
# A UTF-16LE name as its patch row holds it: every byte but the last NUL, which did not change.
utf16_hex() { local h; h="$(hex_of "$1" | sed 's/../&00/g')"; printf '%s' "${h%00}"; }
utf16_bars() { local h; h="$(printf '7c00%.0s' $(seq 1 "$1"))"; printf '%s' "${h%00}"; }
{ echo "format 2"; patch_row krbcc64.dll 10; patch_row xpprof64 40; } >"$tmp/bare-lock"
{ echo "format 2"; patch_row krbcc64.dll 10
  printf 'patch windows_amd64 krb5_64.dll 40 %s %s\n' "$(utf16_hex leashw64.dll)" "$(utf16_bars 12)"
} >"$tmp/utf16-lock"
for form in bare utf16; do
  if ! out_text="$(python3 -I "$bare" names "$tmp/$form-lock" 2>&1)" &&
    [[ "$out_text" == names:*"wrote '|' over"*"which this reader does not load"* ]]; then
    pass "a '|' rewrite the reader does not load fails ($form)"
  else fail "a '|' rewrite the reader does not load ($form: $out_text)"; fi
done
# The pin step joins changes a few bytes apart into one row, so a rewritten name can share its row
# with a changed byte beside it: the row's bytes are then no name, and its '|' run fails rather than
# dropping out unloaded. A '|' run the row leaves as it was is no rewrite.
{ echo "format 2"; patch_row krbcc64.dll 10
  printf 'patch windows_amd64 krb5_64.dll 40 %s0001 %s0002\n' "$(hex_of leashw64.dll)" "$(bars 12)"
} >"$tmp/joined-lock"
if ! out_text="$(python3 -I "$bare" names "$tmp/joined-lock" 2>&1)" &&
  [[ "$out_text" == names:*"wrote '|' over"*"which this reader does not load"* ]]; then
  pass "a '|' rewrite that shares its patch row with other changed bytes fails"
else fail "a '|' rewrite joined to other changes ($out_text)"; fi
{ echo "format 2"; patch_row krbcc64.dll 10; patch_row 'a|||b' 40 "$(hex_of 'c|||b')"; } >"$tmp/kept-lock"
if listed_kept="$(python3 -I "$bare" names "$tmp/kept-lock" 2>&1)" &&
  [[ "$listed_kept" == "krbcc64.dll $(bars 11)" ]]; then
  pass "a patch row that leaves a '|' run as it was is no rewritten name"
else fail "an unchanged '|' run ($listed_kept)"; fi
# A changed header byte or address of one or two bytes can hold 0x7c by chance: no name.
{ echo "format 2"; patch_row krbcc64.dll 10; patch_row x 1 7c; patch_row xy 2 7c7c; } >"$tmp/rva-lock"
if listed_rva="$(python3 -I "$bare" names "$tmp/rva-lock" 2>&1)" &&
  [[ "$listed_rva" == "krbcc64.dll $(bars 11)" ]]; then
  pass "a one- or two-byte patch row that writes 0x7c is no rewritten name"
else fail "short patch rows of 0x7c ($listed_rva)"; fi
printf 'format 2\nversion 2.3.0.0\n' >"$tmp/no-patch-lock"
if ! out_text="$(python3 -I "$bare" names "$tmp/no-patch-lock" 2>&1)" &&
  [[ "$out_text" == names:*"records no patch row"* ]]; then
  pass "a lock with no rewritten bare name fails rather than passing with nothing to load"
else fail "a lock with no patch row ($out_text)"; fi

echo "# $n cases, $((n - failed)) passed"
((failed == 0))
