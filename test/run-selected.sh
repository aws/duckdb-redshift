#!/usr/bin/env bash
# Runs DuckDB's unit tester on a test pattern and refuses a run in which no test case
# executed. With every .test file guarded by require-env REDSHIFT_TEST_CLIENT, a run against
# the wrong client, or with the variable unset, skips every file and the tester exits 0.
#
#   run-selected.sh <unittest-binary> <pattern>
#
# A failing test still fails through the tester's own exit status; this only adds the
# "nothing ran" check. It reads the tester's summary line alone, because test output and the
# skip reasons printed after the summary can contain the same words:
#   All tests passed (N assertions in M test cases)        -> M ran
#   All tests passed (K skipped tests, N assertions in M test cases)
#   All tests were skipped (total skipped K)               -> 0 ran
#   No tests ran                                           -> 0 ran
set -euo pipefail

unittest="${1:?usage: run-selected.sh <unittest> <pattern>}"
pattern="${2:?usage: run-selected.sh <unittest> <pattern>}"
[[ -x "$unittest" ]] || { echo "run-selected: $unittest is not executable; build first" >&2; exit 2; }

echo "run-selected: REDSHIFT_TEST_CLIENT=${REDSHIFT_TEST_CLIENT:-<unset>} pattern=$pattern"
output="$(mktemp)"
trap 'rm -f -- "${output:?}"' EXIT
status=0
"$unittest" "$pattern" 2>&1 | tee "$output" || status=$?
(( status == 0 )) || exit "$status"

summary="$(grep -E '^(All tests |No tests ran|test cases:)' "$output" | tail -n 1)" || true
if [[ ! "$summary" =~ ^All\ tests\ passed\ .*\ in\ [1-9][0-9]*\ test\ cases?\)$ ]]; then
  echo "run-selected: no test case ran for '$pattern' with REDSHIFT_TEST_CLIENT=${REDSHIFT_TEST_CLIENT:-<unset>}; refusing to report success" >&2
  exit 1
fi
