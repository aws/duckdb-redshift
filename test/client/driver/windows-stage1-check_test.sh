#!/usr/bin/env bash
# Tool tests for test/client/driver/windows-stage1-check.sh. A copy runs in a scratch tree beside
# the real pe.py and a stand-in windows_modules.py, which records each call and fails the command
# STAGE1_FAIL names, with a stand-in shell that INSTALLs as DuckDB's does, into
# <extension directory>/<version>/<platform>/. The resource step reads images make_pe.py builds.
# The script in place runs too, and stops at the self-test, which needs Windows. Run by
# run-tool-tests.sh.
set -euo pipefail

here="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
repo="$here/../../.."
tmp="$(mktemp -d)"
trap 'rm -rf -- "${tmp:?}"' EXIT

n=0; failed=0
pass() { n=$((n+1)); echo "ok $n - $1"; }
fail() { n=$((n+1)); echo "not ok $n - $1"; failed=$((failed+1)); }
# shellcheck source=bounded.sh
source "$here/bounded.sh" # bounded <command>...: at most $BOUNDED_SECONDS, its process group stopped

tree="$tmp/tree"
mkdir -p "$tree/test/client/driver" "$tree/third_party/redshift_odbc/tools/lib"
cp "$here/windows-stage1-check.sh" "$tree/test/client/driver/"
cp "$repo/third_party/redshift_odbc/tools/lib/pe.py" "$tree/third_party/redshift_odbc/tools/lib/"
cp "$repo/third_party/redshift_odbc/tools/connect.sql" "$tree/third_party/redshift_odbc/tools/"
cat >"$tree/test/client/driver/windows_modules.py" <<'EOF'
import os, sys
with open(os.environ["STAGE1_LOG"], "a") as log:
    log.write(" ".join(sys.argv[1:]) + "\n")
    if sys.argv[1] == "check":
        log.write(open(sys.argv[3]).read())
if sys.argv[1] == os.environ.get("STAGE1_FAIL"):
    sys.exit("%s: the stand-in failed" % sys.argv[1])
EOF
# The stand-in shell takes -unsigned -c "SET extension_directory = '<dir>'; INSTALL '<file>'", as the
# script runs it. STAGE1_SHELL: ok installs one copy, twice two of different versions, none none,
# and refuse fails as a refused INSTALL does.
cat >"$tmp/duckdb" <<'EOF'
#!/usr/bin/env bash
[[ "$1" == -unsigned && "$2" == -c ]] || exit 3
mode="${STAGE1_SHELL:-ok}"
[[ "$mode" != refuse ]] || { echo "IO Error: the extension could not be installed" >&2; exit 1; }
q="'"
dir="$(sed -n "s/^SET extension_directory = '\(.*\)'; INSTALL '.*'\$/\1/p" <<<"$3")"
file="$(sed -n "s/^SET extension_directory = '.*'; INSTALL '\(.*\)'\$/\1/p" <<<"$3")"
# A quote not doubled ends the literal, which DuckDB's parser would refuse.
[[ "${dir//$q$q/}" != *"$q"* && "${file//$q$q/}" != *"$q"* ]] || { echo "Parser Error: unterminated quote" >&2; exit 1; }
dir="${dir//$q$q/$q}"; file="${file//$q$q/$q}"
[[ "$mode" != none ]] || exit 0
versions=(v2.0.0)
[[ "$mode" != twice ]] || versions+=(v1.5.0)
for version in "${versions[@]}"; do
  mkdir -p "$dir/$version/windows_amd64" && cp "$file" "$dir/$version/windows_amd64/"
done
EOF
chmod +x "$tmp/duckdb"

mkdir -p "$tmp/copy" "$tmp/empty" "$tmp/it's"
python3 -I "$here/make_pe.py" "$tmp/copy/redshift.duckdb_extension" '{"trailing": "a metadata footer"}'
cp "$tmp/copy/redshift.duckdb_extension" "$tmp/it's/"
# stage1 <copy dir> [<shell>]: runs the scratch copy from $tmp with its work directory under $runs,
# leaving its whole output in $out, its last line in $last and its exit status in $status.
runs="$tmp/runs"
stage1() {
  : >"$tmp/log"
  mkdir -p "$runs"
  out="$(cd "$tmp" && STAGE1_LOG="$tmp/log" TMPDIR="$runs" bounded bash "$tree/test/client/driver/windows-stage1-check.sh" \
    "$@" 2>&1)" && status=0 || status=$?
  last="${out##*$'\n'}"
}
# expect <name> <expected-exit> <expected last line>
expect() {
  if [[ "$status" == "$2" && "$last" == "$3" ]]; then pass "$1"
  else fail "$1 (exit $status, wanted $2; output: $out)"; fi
}
says="windows-stage1-check:"

stage1 "$tmp/copy"
expect "prints its usage without a shell" 2 "usage: windows-stage1-check.sh <copy dir> <shell>"
stage1 "$tmp/empty" "$tmp/duckdb"
expect "fails, not skips, a copy that lacks the extension" 1 \
  "$says resource: $tmp/empty/redshift.duckdb_extension is missing; give the directory the bundle was copied to"
stage1 "$tmp/copy" "$tmp/no-such-shell"
expect "fails a shell that does not exist" 1 "$says shell: $tmp/no-such-shell is not an executable DuckDB shell"
printf '#!/bin/sh\n' >"$tmp/not-executable"
stage1 "$tmp/copy" "$tmp/not-executable"
expect "fails a shell that is a file but not executable" 1 \
  "$says shell: $tmp/not-executable is not an executable DuckDB shell"

mkdir -p "$tmp/embedded"
python3 -I "$here/make_pe.py" "$tmp/embedded/redshift.duckdb_extension" \
  '{"resources": [[10, 101, 0, "driver bytes"], [10, "DRIVER_SET", 1033, "set"]]}'
stage1 "$tmp/embedded" "$tmp/duckdb"
expect "fails an extension that holds RCDATA resources, naming each" 1 \
  "$says resource: $tmp/embedded/redshift.duckdb_extension holds RCDATA resources: 101 0 12; \"DRIVER_SET\" 1033 3"
printf '\177ELF\002\001\001' >"$tmp/embedded/redshift.duckdb_extension"
stage1 "$tmp/embedded" "$tmp/duckdb"
expect "fails an extension pe.py cannot read, with its reason" 1 \
  "$says resource: pe.py could not read $tmp/embedded/redshift.duckdb_extension: image: not a PE image (no MZ header)"

STAGE1_FAIL=self-test stage1 "$tmp/copy" "$tmp/duckdb"
expect "fails when the self-test fails" 1 "$says self-test: windows_modules.py could not list this process's modules"
STAGE1_SHELL=refuse stage1 "$tmp/copy" "$tmp/duckdb"
expect "fails a refused INSTALL" 1 "$says install: $tmp/duckdb could not INSTALL $tmp/copy/redshift.duckdb_extension"
STAGE1_SHELL=none stage1 "$tmp/copy" "$tmp/duckdb"
expect "fails an INSTALL that puts no copy in the new extension directory" 1 \
  "$says install: INSTALL did not put one copy in the new extension directory"
STAGE1_SHELL=twice stage1 "$tmp/copy" "$tmp/duckdb"
expect "fails an INSTALL that leaves two copies in the new extension directory" 1 \
  "$says install: INSTALL did not put one copy in the new extension directory"
STAGE1_FAIL=check stage1 "$tmp/copy" "$tmp/duckdb"
expect "fails when the check fails" 1 "$says check: the shell's run failed the rule named above"
if [[ -z "$(ls -A "$runs")" ]]; then pass "removes its work directory when a step fails"
else fail "removes its work directory when a step fails ($(ls -A "$runs"))"; fi

stage1 "$tmp/copy" "$tmp/duckdb"
expect "passes a copy whose installed extension passes the check" 0 "$says passed for $tmp/duckdb"
check_line="$(grep '^check ' "$tmp/log" || true)"
work="${check_line#check }"; work="${work%%/extensions/*}"
sql="$(sed -n '/^check /,$p' "$tmp/log" | tail -n +2)"
# connect.sql's attach and query, the form the extension takes (an ATTACH's options, no secret).
expected_sql="SET extension_directory = '$work/extensions';
LOAD redshift;
$(< "$repo/third_party/redshift_odbc/tools/connect.sql")"
if [[ "$(head -n 1 "$tmp/log")" == self-test && "$work" == "$runs/"* &&
  "$check_line" == "check $work/extensions/v2.0.0/windows_amd64/redshift.duckdb_extension $work/stage1.sql $tmp/duckdb -unsigned" &&
  "$sql" == "$expected_sql" ]]; then
  pass "runs the self-test, then checks the installed copy with the SQL, the shell and -unsigned"
else fail "runs the self-test, then checks the installed copy with the SQL, the shell and -unsigned ($(cat "$tmp/log"))"; fi
if [[ -z "$(ls -A "$runs")" ]]; then pass "removes its work directory when it passes"
else fail "removes its work directory when it passes ($(ls -A "$runs"))"; fi
mv "$tree/third_party/redshift_odbc/tools/connect.sql" "$tmp/connect.sql"
stage1 "$tmp/copy" "$tmp/duckdb"
expect "fails, before the check, without connect.sql" 1 \
  "$says check: no connect.sql, or an empty one, at \
$tree/test/client/driver/../../../third_party/redshift_odbc/tools/connect.sql"
mv "$tmp/connect.sql" "$tree/third_party/redshift_odbc/tools/connect.sql"

runs="$tmp/run's"
stage1 "$tmp/it's" "$tmp/duckdb"
expect "quotes a quote in the copy's and the extension directory's paths" 0 "$says passed for $tmp/duckdb"
if grep -qF "SET extension_directory = '$tmp/run''s/" "$tmp/log"; then pass "doubles a quote in the SQL's extension directory"
else fail "doubles a quote in the SQL's extension directory ($(cat "$tmp/log"))"; fi
runs="$tmp/runs"

# The script in place, with the real windows_modules.py, which needs Windows for its self-test.
out="$(bounded bash "$here/windows-stage1-check.sh" "$tmp/copy" "$tmp/duckdb" 2>&1)" && status=0 || status=$?
last="${out##*$'\n'}"
if [[ "$status" == 1 && "$out" == *"windows: this command runs on Windows only"* &&
  "$last" == "$says self-test: windows_modules.py could not list this process's modules" ]]; then
  pass "the script in place reads the real extension's resources, then stops at the self-test off Windows"
else fail "the script in place reads the real extension's resources, then stops at the self-test off Windows (exit $status; $out)"; fi

echo "# $n cases, $((n - failed)) passed"
exit $(( failed > 0 ))
