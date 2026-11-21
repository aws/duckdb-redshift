#!/usr/bin/env bash
# The rules of third_party/redshift_odbc/tools/extension-coexistence.sh, with stand-in CLIs, so no build
# and no network: the two orders and the LOADs each gives the CLI, with paths made absolute and quotes
# doubled; the exit status, connect error and driver set an order needs; a detail kept to one field of at
# most 300 bytes; a CLI given by name; exit 2, before any CLI runs, for a CLI that is not an executable
# file, a path whose directory cannot be entered, a name not on PATH, a missing or empty connect.sql and
# where the process has a network (an extension file that is not there is the CLI's to fail, in its order).
# The harness runs from a copy beside a stand-in for test/client/odbc/check-no-network.sh, at the path it
# runs that from, so the result does not depend on this host's network; run-e2e.sh runs the real harness
# with real shells.
set -euo pipefail
here="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
tools="$here/../../../third_party/redshift_odbc/tools"
n=0; failed=0
check() {  # check <name> <command...>
  local name="$1"; shift; n=$((n+1))
  if "$@"; then echo "ok $n - $name"; else echo "not ok $n - $name"; failed=$((failed+1)); fi
}
tmp="$(mktemp -d)"
tmp="$(cd "$tmp" && pwd -P)"
trap 'rm -rf -- "${tmp:?}"' EXIT
chmod 700 "$tmp"

# copy <name> <exit status of the stand-in network check>: a copy of the harness, and its path.
copy() {
  mkdir -p "$tmp/$1/third_party/redshift_odbc/tools" "$tmp/$1/test/client/odbc"
  cp -p "$tools/extension-coexistence.sh" "$tools/connect.sql" "$tmp/$1/third_party/redshift_odbc/tools/"
  echo "exit $2" > "$tmp/$1/test/client/odbc/check-no-network.sh"
  harness="$tmp/$1/third_party/redshift_odbc/tools/extension-coexistence.sh"
}
copy offline 0
offline="$harness"
copy online 1
online="$harness"

# `exits-1` logs the SQL it is given beside itself (the harness runs it under env -i), writes the set's
# directory, prints the connect error and exits 1, as a CLI whose driver loaded would. Each of the others
# breaks one rule: `crashes` is killed by SIGSEGV after the error, `no-set` writes no set, `no-error`
# prints another error, and `noisy` passes with tabs, line ends and 400 more characters in its output.
mkdir -m 700 "$tmp/cli"
cat > "$tmp/cli/exits-1" <<'STAND_IN'
#!/usr/bin/env bash
printf '%s\n' "$3" >> "$0.calls"
[[ "$0" == */no-set ]] || mkdir -p "$REDSHIFT_DRIVER_DIR/.redshift-driver"
if [[ "$0" == */no-error ]]; then echo "Catalog Error: no such function"; else
  echo "Could not connect to Redshift at stand-in (region us-east-1): the connection attempt failed."; fi
[[ "$0" != */noisy ]] || { printf 'a\tb\nc\n'; printf '%0400d\n' 0; }
[[ "$0" != */crashes ]] || { ulimit -c 0; kill -s SEGV $$; }
exit 1
STAND_IN
chmod 700 "$tmp/cli/exits-1"
for other in crashes no-set no-error noisy; do cp -p "$tmp/cli/exits-1" "$tmp/cli/$other"; done
connect="$(< "$tools/connect.sql")"
# calls <stand-in>: the LOADs of each call it logged, one per line, without the connect statements.
calls() {
  local line
  while IFS= read -r line; do
    [[ "$line" == *"$connect" ]] || { echo "a call without connect.sql's statements: $line"; return; }
    printf '%s\n' "${line%"$connect"}"
  done < "$1.calls"
}

out="$(bash "$offline" "$tmp/cli/exits-1" "$tmp/cli/redshift.duckdb_extension" a b)" && status=0 || status=$?
check "two orders, each ok, exit 0" \
  bash -c '[[ $1 == 0 && "$(cut -f1,2 <<<"$2")" == $'"'"'others-first\tok\nredshift-first\tok'"'"' ]]' _ "$status" "$out"
own="LOAD '$tmp/cli/redshift.duckdb_extension'; "
check "the other extensions are LOADed before redshift, then after it, each time with connect.sql's statements" \
  test "$(calls "$tmp/cli/exits-1")" = "LOAD 'a'; LOAD 'b'; $own"$'\n'"${own}LOAD 'a'; LOAD 'b'; "
for broken in crashes no-set no-error; do
  out="$(bash "$offline" "$tmp/cli/$broken" "" a 2>&1)" && status=0 || status=$?
  check "$broken: its one order fails, exit 1" bash -c '[[ $1 == 1 ]] && grep -q "^loaded	fail	" <<<"$2"' _ "$status" "$out"
done

# A detail stays one field of at most 300 bytes.
out="$(bash "$offline" "$tmp/cli/noisy" "" a)" && status=0 || status=$?
check "a detail with tabs and line ends is one field, cut to 300 bytes" \
  bash -c '[[ $1 == 0 && "$(wc -l <<<"$2")" -eq 1 ]] && [[ "$(cut -f1,2 <<<"$2")" == $'"'"'loaded\tok'"'"' ]] &&
    detail="$(cut -f3- <<<"$2")" && [[ "$detail" != *$'"'"'\t'"'"'* && ${#detail} == 300 && "$detail" == *"failed. a b c 000"* ]]' \
  _ "$status" "$out"

# Paths are made absolute, since each order runs in a directory of its own, and a quote in one is doubled;
# a name is LOADed as it is. As DuckDB's LOAD has it, an argument with a '.' or a '\' is a path too.
mkdir -p "$tmp/caller/it's here"
rm -f "$tmp/cli/exits-1.calls"
out="$(cd "$tmp/caller" && bash "$offline" "$tmp/cli/exits-1" "" "it's here/x.duckdb_extension" y.duckdb_extension \
  z.so 'ext\x' json)" && status=0 || status=$?
check "relative paths are made absolute with their quotes doubled, and a name is kept" \
  bash -c '[[ $1 == 0 && "$2" == "$3" ]]' _ "$status" "$(calls "$tmp/cli/exits-1")" \
  "LOAD '$tmp/caller/it''s here/x.duckdb_extension'; LOAD '$tmp/caller/y.duckdb_extension'; LOAD '$tmp/caller/z.so'; LOAD '$tmp/caller/ext\x'; LOAD 'json'; "

# A path whose directory cannot be entered is refused, exit 2, before any CLI runs; so is a CLI path.
rm -f "$tmp/cli/exits-1.calls"
status=0; bash "$offline" "$tmp/cli/exits-1" "" "$tmp/none/x.duckdb_extension" > "$tmp/bad.out" 2>&1 || status=$?
check "an extension in a directory that cannot be entered: exit 2, no CLI run" \
  bash -c '[[ $1 == 2 && ! -e "$2" ]] && grep -qF "cannot enter the directory of $3" "$4"' _ \
  "$status" "$tmp/cli/exits-1.calls" "$tmp/none/x.duckdb_extension" "$tmp/bad.out"
status=0; bash "$offline" "$tmp/cli/exits-1" "$tmp/none/redshift.duckdb_extension" a >/dev/null 2>&1 || status=$?
check "a redshift extension in a directory that cannot be entered: exit 2, no CLI run" \
  bash -c '[[ $1 == 2 && ! -e "$2" ]]' _ "$status" "$tmp/cli/exits-1.calls"
status=0; bash "$offline" "$tmp/none/duckdb" "" a >/dev/null 2>&1 || status=$?
check "a CLI in a directory that cannot be entered: exit 2" test "$status" = 2
# A CLI path that is not an executable file (not there, not executable, a directory) is refused too.
cp -p "$tmp/cli/exits-1" "$tmp/cli/not-executable"; chmod 600 "$tmp/cli/not-executable"
for bad in "$tmp/cli/not-there" "$tmp/cli/not-executable" "$tmp/cli"; do
  status=0; bash "$offline" "$bad" "" a > "$tmp/bad-cli.out" 2>&1 || status=$?
  check "a CLI at ${bad#"$tmp/"}: exit 2, naming it, no order run" \
    bash -c '[[ $1 == 2 && ! -e "$2" ]] && grep -qF "the CLI $3 is not an executable file" "$4" && ! grep -q "	" "$4"' _ \
    "$status" "$bad.calls" "$bad" "$tmp/bad-cli.out"
done

# A CLI given by name is the one on PATH; one that is not there is refused, exit 2.
out="$(PATH="$tmp/cli:$PATH" bash "$offline" exits-1 "" a)" && status=0 || status=$?
check "a CLI given by name is found on PATH" bash -c '[[ $1 == 0 && "$(cut -f1,2 <<<"$2")" == $'"'"'loaded\tok'"'"' ]]' \
  _ "$status" "$out"
status=0; (cd "$tmp/cli" && bash "$offline" redshift-cli-not-on-path "" a > "$tmp/name.out" 2>&1) || status=$?
check "a CLI name not on PATH: exit 2, naming it" \
  bash -c '[[ $1 == 2 ]] && grep -qF "no program named redshift-cli-not-on-path on PATH" "$2"' _ "$status" "$tmp/name.out"

# With a network the harness refuses, exit 2, before any CLI runs.
rm -f "$tmp/cli/exits-1.calls"
status=0; bash "$online" "$tmp/cli/exits-1" "" a > "$tmp/online.out" 2>&1 || status=$?
check "with a network: exit 2 before any CLI runs" \
  bash -c '[[ $1 == 2 && ! -e "$2" ]] && grep -qF "this process has a network" "$3"' _ "$status" "$tmp/cli/exits-1.calls" \
  "$tmp/online.out"
# Without connect.sql beside it, or with an empty one, the harness refuses, exit 2, rather than run orders
# that cannot pass.
copy no-connect 0
rm "$tmp/no-connect/third_party/redshift_odbc/tools/connect.sql"
rm -f "$tmp/cli/exits-1.calls"
status=0; bash "$harness" "$tmp/cli/exits-1" "" a > "$tmp/no-connect.out" 2>&1 || status=$?
check "without connect.sql: exit 2, no CLI run" \
  bash -c '[[ $1 == 2 && ! -e "$2" ]] && grep -qF "no connect.sql, or an empty one, beside this script" "$3"' _ "$status" "$tmp/cli/exits-1.calls" \
  "$tmp/no-connect.out"
: > "$tmp/no-connect/third_party/redshift_odbc/tools/connect.sql"
status=0; bash "$harness" "$tmp/cli/exits-1" "" a > "$tmp/no-connect.out" 2>&1 || status=$?
check "with an empty connect.sql: exit 2, no CLI run" \
  bash -c '[[ $1 == 2 && ! -e "$2" ]] && grep -qF "no connect.sql, or an empty one, beside this script" "$3"' _ "$status" "$tmp/cli/exits-1.calls" \
  "$tmp/no-connect.out"
check "too few arguments: exit 2" bash -c 'status=0; bash "$1" "$2" "" >/dev/null 2>&1 || status=$?; [[ $status == 2 ]]' \
  _ "$offline" "$tmp/cli/exits-1"
echo "# $n cases, $((n - failed)) passed"
((failed == 0))
