#!/usr/bin/env bash
# What keeps the end-to-end SQL tests (test/client/driver/sql/) to run-e2e.sh, which runs them with no network.
# There each connection is aimed at closed ports on 127.0.0.1. Every connect of an ODBC build without the
# test-endpoint switch refuses the variable that selects them, and driver_e2e's build, which has it, refuses it
# without both endpoints, so a file selected anywhere else fails closed. These checks
# keep it from being selected at all, so that it skips instead:
#   - the Makefile clears and does not export DUCKDB_REDSHIFT_TEST_E2E (checked with a probe
#     recipe, which make really runs and which only prints its environment, through a second makefile that GNU
#     make 3.81 reads as well);
#   - every end-to-end SQL file that attaches requires DUCKDB_REDSHIFT_TEST_E2E, run-e2e.sh's fake AWS keys and no
#     AWS_SESSION_TOKEN before its first ATTACH (the lint), so a file started by hand with the caller's own
#     credentials skips.
# Needs no build and no network.
set -euo pipefail

here="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
root="$(cd "$here/../../.." && pwd)"
n=0; failed=0
not() { ! "$@"; }
check() {  # check <name> <command...>
  local name="$1"; shift; n=$((n+1))
  if "$@"; then echo "ok $n - $name"; else echo "not ok $n - $name"; failed=$((failed+1)); fi
}

# What a recipe's environment holds is read with a probe recipe; the probe must show the variables the Makefile
# does export, or an empty answer would prove nothing. DUCKDB_REDSHIFT_TEST_E2E is cleared in make as well (a value the caller
# gave is not read by a recipe or a $(shell)), which the probe's last line shows. A second makefile read after the
# project's (make -f twice, which GNU make 3.81 on macOS supports; --eval needs 3.82).
probe_dir="$(mktemp -d)"
lint_dir="$(mktemp -d)"
trap 'rm -rf -- "${probe_dir:?}" "${lint_dir:?}"' EXIT
probe_file="$probe_dir/probe.mk"
printf 'probe-e2e-env: ; @env; echo "make-values:$(DUCKDB_REDSHIFT_TEST_E2E)"\n' > "$probe_file"
probe_env() {  # probe_env <make arguments...>; the environment of a recipe and the variable's value in make
  (cd "$root" && make -s --no-print-directory -f Makefile -f "$probe_file" "$@" 2>/dev/null)
}
env_leak() {  # env_leak exported|command-line -> succeeds when a variable reaches the recipe or the probe is blind
  local seen
  if [[ "$1" == exported ]]; then
    seen="$(export DUCKDB_REDSHIFT_TEST_E2E=odbc-ini; probe_env probe-e2e-env REDSHIFT_CLIENT=odbc)"
  else
    seen="$(probe_env probe-e2e-env REDSHIFT_CLIENT=odbc DUCKDB_REDSHIFT_TEST_E2E=odbc-ini)"
  fi
  grep -q '^REDSHIFT_TEST_CLIENT=odbc$' <<<"$seen" || { echo "the probe does not show REDSHIFT_TEST_CLIENT" >&2; return 0; }
  # Neither in the recipe's environment nor in make's own variables (a recipe or $(shell) would read those).
  grep -q '^DUCKDB_REDSHIFT_TEST_E2E=' <<<"$seen" || ! grep -qx 'make-values:' <<<"$seen"
}
check "no end-to-end variable the caller exported reaches a recipe" not env_leak exported
check "no end-to-end variable on the make command line reaches a recipe" not env_leak command-line

# Every end-to-end SQL file that attaches (all of them that can reach the driver) selects itself with
# `require-env DUCKDB_REDSHIFT_TEST_E2E <case>`, run-e2e.sh's two fake keys and `require-env-not AWS_SESSION_TOKEN`
# before its first ATTACH; a file without them would run wherever it is started, with the caller's credentials.
selects_before_attaching() {  # selects_before_attaching <file>
  # sqllogictest passes the statement to DuckDB unchanged, so `attach` and an indented ATTACH count.
  awk '!attach && /^require-env DUCKDB_REDSHIFT_TEST_E2E [^ ]+$/ { e2e = 1 }
       !attach && /^require-env AWS_ACCESS_KEY_ID test-not-a-key$/ { key = 1 }
       !attach && /^require-env AWS_SECRET_ACCESS_KEY test-not-a-secret$/ { secret = 1 }
       !attach && /^require-env-not AWS_SESSION_TOKEN$/ { token = 1 }
       tolower($0) ~ /^[ \t]*attach[ \t]/ && !attach { attach = 1 }
       END { exit !(attach && e2e && key && secret && token) }' "$1"
}
every_attaching_file_selects_itself() {  # every_attaching_file_selects_itself [directory of .test files]
  local file found=0
  for file in "${1:-$here/sql}"/*.test; do
    grep -qiE '^[[:space:]]*attach[[:space:]]' "$file" || continue
    found=$((found+1))
    selects_before_attaching "$file" ||
      { echo "$file does not require DUCKDB_REDSHIFT_TEST_E2E, the fake keys and no session token before its first ATTACH" >&2; return 1; }
  done
  ((found >= 1)) || { echo "found no SQL file that attaches" >&2; return 1; }
}
check "every end-to-end file that attaches requires DUCKDB_REDSHIFT_TEST_E2E, the fake keys and no session token first" every_attaching_file_selects_itself
# The lint itself, on files made here: a lower-case or indented attach is found, and passes only with the guards first.
guards='require-env DUCKDB_REDSHIFT_TEST_E2E some-case
require-env AWS_ACCESS_KEY_ID test-not-a-key
require-env AWS_SECRET_ACCESS_KEY test-not-a-secret
require-env-not AWS_SESSION_TOKEN'
lint_file() {  # lint_file <name> <with|without|without-<n>|bare-key|other-key|after-<n>> <attach statement>
  # all guards, none, all but line <n>, the key line with no or another value, or line <n> after the attach
  { printf 'require-env REDSHIFT_TEST_CLIENT odbc\n\n'
    case "$2" in
    with) printf '%s\n\n' "$guards" ;;
    without-* | after-*) awk -v skip="${2#*-}" 'NR != skip' <<<"$guards"; echo ;;
    bare-key) sed 's/^require-env AWS_ACCESS_KEY_ID .*/require-env AWS_ACCESS_KEY_ID/' <<<"$guards"; echo ;;
    other-key) sed 's/^require-env AWS_ACCESS_KEY_ID .*/require-env AWS_ACCESS_KEY_ID AKIAEXAMPLE/' <<<"$guards"; echo ;;
    esac
    printf 'statement ok\n%s\n' "$3"
    [[ "$2" != after-* ]] || { echo; awk -v keep="${2#after-}" 'NR == keep' <<<"$guards"; }; } > "$lint_dir/$1.test"
}
lint_rejects() { rm -f "${lint_dir:?}"/*.test; lint_file "$@"; ! every_attaching_file_selects_itself "$lint_dir" 2>/dev/null; }
lint_accepts() { rm -f "${lint_dir:?}"/*.test; lint_file "$@"; every_attaching_file_selects_itself "$lint_dir" 2>/dev/null; }
check "the lint rejects an upper-case ATTACH without the guards" lint_rejects a without "ATTACH '' AS rs (TYPE redshift)"
check "the lint rejects a lower-case attach without the guards" lint_rejects b without "attach '' AS rs (TYPE redshift)"
check "the lint rejects an indented, mixed-case Attach without the guards" lint_rejects c without "   Attach '' AS rs (TYPE redshift)"
check "the lint accepts a lower-case attach with the guards first" lint_accepts d with "attach '' AS rs (TYPE redshift)"
check "the lint accepts an indented attach with the guards first" lint_accepts e with "    attach '' AS rs (TYPE redshift)"
check "the lint rejects a fake-key line with no value" lint_rejects g bare-key "ATTACH '' AS rs (TYPE redshift)"
check "the lint rejects a key line with another value" lint_rejects k other-key "ATTACH '' AS rs (TYPE redshift)"
for g in 1 2 3 4; do
  check "the lint rejects guard line $g after the ATTACH" lint_rejects "h$g" "after-$g" "ATTACH '' AS rs (TYPE redshift)"
  check "the lint rejects a file missing guard line $g" lint_rejects "f$g" "without-$g" "ATTACH '' AS rs (TYPE redshift)"
done

((failed == 0)) || { echo "$failed of $n failed" >&2; exit 1; }
echo "all $n passed"
