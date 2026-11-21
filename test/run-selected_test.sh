#!/usr/bin/env bash
# Fixture tests for run-selected.sh. Each case runs it against a stand-in tester that prints a
# canned output and exits with a given status; a case that names the tester's arguments makes it
# exit 9 instead when they differ. Run with `make test_run_selected`.
set -euo pipefail

here="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
tmp="$(mktemp -d)"
trap 'rm -rf -- "${tmp:?}"' EXIT
tester="$tmp/unittest"
pattern='test/x/*'
printf '#!/usr/bin/env bash\n[[ -z "${CANNED_ARGS:-}" || "$*" == "$CANNED_ARGS" ]] || exit 9\nprintf "%%b" "$CANNED_OUTPUT"\nexit "$CANNED_STATUS"\n' > "$tester"
chmod +x "$tester"

n=0; failed=0
check() {  # check <name> <expected-exit> <tester-output> [tester-exit] [tester-args]
  local name="$1" want="$2" status
  n=$((n+1))
  CANNED_OUTPUT="$3" CANNED_STATUS="${4:-0}" CANNED_ARGS="${5:-}" bash "$here/run-selected.sh" "$tester" "$pattern" >/dev/null 2>&1 \
    && status=0 || status=$?
  if [[ "$status" == "$want" ]]; then
    echo "ok $n - $name"
  else
    echo "not ok $n - $name (exit $status, wanted $want)"; failed=$((failed+1))
  fi
}

check "accepts a run in which test cases ran" 0 'All tests passed (9 assertions in 2 test cases)\n'
check "accepts a run in which some files were skipped" 0 \
  'All tests passed (1 skipped test, 4 assertions in 1 test case)\n\nSkipped tests for the following reasons:\nrequire-env REDSHIFT_TEST_CLIENT odbc: 1\n'
check "refuses a run in which every file was skipped" 1 'All tests were skipped (total skipped 2)\n'
check "refuses a run in which no test ran" 1 'No tests ran\n'
check "reads only the summary line, not test output that looks like one" 1 \
  'printed (5 assertions in 3 test cases)\nAll tests were skipped (total skipped 1)\n'
check "uses the last summary line, not an earlier output line shaped like one" 1 \
  'All tests passed (5 assertions in 3 test cases)\nAll tests were skipped (total skipped 1)\n'
check "propagates the tester's own failure status" 3 'test cases: 2 | 1 passed | 1 failed\n' 3
check "passes its pattern to the tester as the tester's only argument" 0 \
  'All tests passed (1 assertion in 1 test case)\n' 0 "$pattern"

echo "# $n cases, $((n - failed)) passed"
exit $(( failed > 0 ))
