#!/usr/bin/env bash
# Fixture tests for tsan-own-reports.sh, which decides test_mock_tsan's outcome. Needs no build.
set -euo pipefail
here="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
script="$here/tsan-own-reports.sh"
tmp="$(mktemp -d)"
trap 'rm -rf -- "${tmp:?}"' EXIT
root=/pkg/
n=0
failed=0

passed_output="$tmp/passed.out"
printf 'All tests passed (10 assertions in 2 test cases)\ntsan suite exit status: 0\n' > "$passed_output"

# check <name> <expected exit> <suite output> [<log text>...]: each log text is one log file.
check() {
  local name=$1 expected=$2 output=$3
  shift 3
  local dir="$tmp/case$n"
  mkdir -p "$dir"
  local i=0
  for text in "$@"; do
    printf '%b' "$text" > "$dir/report.$i"
    i=$((i + 1))
  done
  n=$((n + 1))
  local got=0
  bash "$script" "$dir/report" "$root" "$output" > "$dir/stdout" 2>&1 || got=$?
  if [[ $got == "$expected" ]]; then echo "ok $n - $name"; else
    echo "not ok $n - $name (exit $got, expected $expected)"; sed 's/^/#   /' "$dir/stdout"; failed=$((failed + 1)); fi
}

# report <frames...>: one WARNING block with these frames. A -- starts its next stack, an access; a
# --thread starts one that says where thread T1 was created.
report() {
  local text="WARNING: ThreadSanitizer: data race (pid=1)\n  Write of size 8 by thread T1:\n"
  local i=0
  for frame in "$@"; do
    if [[ $frame == -- ]]; then text+="  Previous write of size 8 by thread T2:\n"; i=0; continue; fi
    if [[ $frame == --thread ]]; then
      text+="  Thread T1 (tid=2, running) created by main thread at:\n"
      i=0
      continue
    fi
    text+="    #$i $frame\n"
    i=$((i + 1))
  done
  echo -n "${text}SUMMARY: ThreadSanitizer: data race\n"
}
engine="duckdb::Executor::Reset() ${root}build/tsan/src/parallel/../../../../duckdb/src/parallel/executor.cpp:10 (bin+0x1)"
engine_header="duckdb::TaskScheduler ${root}duckdb/src/include/duckdb/parallel/task.hpp:5 (bin+0x2)"
engine_source="duckdb::VectorOperations::Copy ${root}duckdb/src/common/vector_operations/vector_copy.cpp:120 (bin+0x8)"
package="duckdb::RedshiftConnectionMap::BeginUse ${root}src/storage/redshift_connection_ownership.cpp:140 (bin+0x3)"
duckdb_header="duckdb::shared_ptr<duckdb::Task>::operator= ${root}duckdb/src/include/duckdb/common/shared_ptr_ipp.hpp:155 (bin+0x6)"
test_frame="CATCH_TEST ${root}test/mock/cpp/test_sessions.cpp:500 (bin+0x4)"
catch_header="Catch::compareEqual<int, int> ${root}duckdb/third_party/catch/catch.hpp:1700 (bin+0x7)"
inlined="std::vector<int>::push_back /usr/include/c++/11/bits/stl_vector.h:1198 (bin+0x5)"
interceptor="pthread_mutex_lock <null> (libtsan.so.0+0x506fa)"
summary="ThreadSanitizer: reported 1 warnings\n"

check "a passing suite with no report passes" 0 "$passed_output"
check "an engine report passes, its stack ending in the test" 0 "$passed_output" \
  "$(report "$interceptor" "$engine" "$engine_header" "$test_frame")$summary"
check "an engine report with no frame in the package passes" 0 "$passed_output" \
  "$(report "$interceptor" "$engine" "$engine_header")"
# Pins how far the skip reaches: a DuckDB source frame outside a unity build is the engine's.
check "a DuckDB source frame above a test frame is the innermost under the root, and passes" 0 \
  "$passed_output" "$(report "$interceptor" "$engine_source" "$test_frame")"
check "a report with a frame in the package's src/ fails" 1 "$passed_output" \
  "$(report "$interceptor" "$package" "$engine")"
check "a test frame below an inlined DuckDB header frame is the innermost under the root, and fails" 1 \
  "$passed_output" "$(report "$interceptor" "$duckdb_header" "$test_frame" "$engine")"
check "a test frame below an inlined Catch2 header frame is the innermost under the root, and fails" 1 \
  "$passed_output" "$(report "$interceptor" "$catch_header" "$test_frame" "$engine")"
check "a src/ frame below an inlined DuckDB header frame fails" 1 "$passed_output" \
  "$(report "$interceptor" "$duckdb_header" "$package" "$engine")"
check "a test frame below inlined libstdc++ frames is the innermost under the root, and fails" 1 "$passed_output" \
  "$(report "$inlined" "$inlined" "$inlined" "$inlined" "$inlined" "$inlined" "$inlined" "$test_frame" "$engine")"
check "a second stack whose innermost frame under the root is the package's fails" 1 "$passed_output" \
  "$(report "$interceptor" "$engine" "$test_frame" -- "$inlined" "$test_frame" "$engine")"
check "a test frame only where a thread was created leaves an engine report the engine's" 0 "$passed_output" \
  "$(report "$interceptor" "$engine" --thread "$interceptor" "$inlined" "$test_frame" "$engine")"
check "a report with no frame under the package root fails closed" 1 "$passed_output" \
  "$(report "$interceptor" "$inlined" "<null> <null> (bin+0x9)")"
check "a FATAL line outside a report fails" 1 "$passed_output" "FATAL: ThreadSanitizer: unexpected memory mapping\n"
check "a suppressions file it could not read fails" 1 "$passed_output" \
  "ThreadSanitizer: failed to read suppressions file 'x'\n"
check "a deadly signal fails" 1 "$passed_output" "ThreadSanitizer:DEADLYSIGNAL\n==1==ERROR: ThreadSanitizer: SEGV on unknown address\n"
check "a report cut off before its summary fails" 1 "$passed_output" \
  "WARNING: ThreadSanitizer: data race (pid=1)\n  Write of size 8 by thread T1:\n    #0 $engine\n"
check "a report cut off by the next report in the same log fails" 1 "$passed_output" \
  "WARNING: ThreadSanitizer: data race (pid=1)\n  Write of size 8 by thread T1:\n    #0 $engine\n$(report "$interceptor" "$engine")"
# Without the check at a log's first line, the second log's SUMMARY line would close the first log's
# cut-off report as an engine report: the next-report check needs a WARNING line, and neither the
# outside-a-report check nor the end-of-input check would fire. So without that check the script
# exits 0 and this case fails.
check "a report cut off at the end of a log before another fails" 1 "$passed_output" \
  "WARNING: ThreadSanitizer: data race (pid=1)\n  Write of size 8 by thread T1:\n    #0 $engine\n" \
  "SUMMARY: ThreadSanitizer: data race\n"
failed_output="$tmp/failed.out"
printf 'test cases: 3 | 2 passed | 1 failed\n' > "$failed_output"
check "a failed suite fails, even with no report" 1 "$failed_output"
echo "All tests passed (10 assertions in 2 test cases)" > "$tmp/no-status.out"
printf 'test cases: 3 | 2 passed | 1 failed\ntsan suite exit status: 0\n' > "$tmp/status-only.out"
printf 'tsan suite exit status: 0\n' | cat "$passed_output" - > "$tmp/two-status.out"
aborted_output="$tmp/aborted.out"
printf 'All tests passed (10 assertions in 2 test cases)\ntsan suite exit status: 134\n' > "$aborted_output"
check "a suite that aborts after its summary fails" 1 "$aborted_output"
check "a suite output with no status line fails" 1 "$tmp/no-status.out"
check "status 0 without \"All tests passed\" fails" 1 "$tmp/status-only.out"
check "a suite output with two status lines fails" 1 "$tmp/two-status.out"
: > "$tmp/empty.out"
check "a suite that stopped before its summary fails" 1 "$tmp/empty.out" \
  "$(report "$interceptor" "$engine")"

echo "# $n cases, $((n - failed)) passed"
exit $((failed > 0))
