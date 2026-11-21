#!/usr/bin/env bash
# Decides test_mock_tsan's outcome from the suite's output and its ThreadSanitizer logs: the suite
# runs with exitcode=0, which also hides a crash, and its exit status reaches only tee.
#
#   tsan-own-reports.sh <log prefix> <package root> <suite output>
#
# Fails when:
# - the suite output lacks Catch2's "All tests passed" line, or the one status line test_mock_tsan
#   adds after it does not read "tsan suite exit status: 0": a test failed, or the suite crashed,
#   even after its summary;
# - a log holds a ThreadSanitizer line outside a report other than its exit summary, such as a
#   FATAL error, a failed CHECK, a deadly signal or a suppressions file it could not read, or a
#   report that ends before its summary, or before the next report starts;
# - a report is this repository's: one of its frames is in its src/, or, in one of its
#   stacks, the innermost frame with a path under the repository root, DuckDB's headers and its
#   third_party/ (Catch2's catch.hpp) aside, is in its test/; or no frame of the report has a path
#   under the root, so nothing says it is the engine's. The stack that says where a thread was
#   created is not read: a test starts threads that the engine's races run on.
#
# Every other report is the engine's, which it lists and does not fail on. DuckDB's own
# ThreadSanitizer CI runs only some of its tests (`unittest_threadsan` in duckdb/Makefile), so the
# pinned engine's suppression list does not cover the executor paths the mock suite reaches:
# streamed, interrupted and abandoned results. Any src/ frame decides, since DuckDB's header frames
# (shared_ptr_ipp.hpp, vector.hpp) are inlined above this repository's own, as Catch2's are above a
# test's (a CHECK's compareEqual). A test/ frame decides only as the innermost under the root, not
# by its frame number, because every main-thread stack ends in the test that ran, and the
# RelWithDebInfo build prints inlined libstdc++ frames above the test's code.
set -euo pipefail
prefix="$1"
root="$2"
output="$3"
failed=0

if ! grep -q '^All tests passed' "$output"; then
  echo "tsan-own-reports: the suite did not pass, or did not finish: no \"All tests passed\" in $output"
  failed=1
fi
status_lines="$(grep '^tsan suite exit status:' "$output" || true)"
if [[ $status_lines != 'tsan suite exit status: 0' ]]; then
  echo "tsan-own-reports: the suite did not exit with status 0, or its status is not one line: ${status_lines:-no status in $output}"
  failed=1
fi

shopt -s nullglob
logs=("$prefix".*)
if ((${#logs[@]} == 0)); then
  echo "tsan-own-reports: no ThreadSanitizer report"
  exit "$failed"
fi

awk -v root="$root" '
  function under(line, dir) { return index(line, " " root dir) > 0 }
  function under_root(line) { return index(line, " " root) > 0 }
  # A frame in the DuckDB headers or anywhere under duckdb/third_party/ (such as Catch2 catch.hpp),
  # which can sit above this repository's code in a stack and so never decides whose the stack is.
  function skipped_frame(line) {
    return under(line, "duckdb/src/include/") || under(line, "duckdb/third_party/")
  }
  function cut_off() {
    broken++
    print "tsan-own-reports: a report with no summary: " text
    in_report = 0
  }
  # The log before this one ended inside a report.
  FNR == 1 && in_report { cut_off() }
  /^WARNING: ThreadSanitizer:/ {
    # A report cut off by the next one, which a whole log never holds, so it counts as broken.
    if (in_report) cut_off()
    in_report = 1; own = 0; any_root = 0; stack_done = 0; creation = 0; text = $0 "\n"
    next
  }
  in_report {
    text = text $0 "\n"
    # Each stack follows a line that says what it is; "Thread T1 (...) created by ... at:" starts one
    # that says only where a thread was created.
    if ($0 ~ /^  [^ ]/) creation = ($0 ~ /^  Thread T[0-9]+ .* created by .* at:$/)
    if (!creation && $1 ~ /^#[0-9]+$/) {
      if (under($0, "src/")) own = 1
      if (under_root($0)) any_root = 1
      # A stack starts again at #0; for test/, only its innermost frame under the root counts, past
      # the DuckDB and third-party headers inlined into test code as into engine code.
      if ($1 == "#0") stack_done = 0
      if (!stack_done && under_root($0) && !skipped_frame($0)) {
        stack_done = 1
        if (under($0, "test/")) own = 1
      }
    }
    if ($0 ~ /^SUMMARY: ThreadSanitizer/) {
      total++
      if (own || !any_root) {
        owned++
        printf "%s%s\n", (any_root ? "" : "(no frame under the package root, so not known to be the engine'"'"'s)\n"), text
      } else {
        print "engine report: " $0
      }
      in_report = 0
    }
    next
  }
  /ThreadSanitizer/ && $0 !~ /ThreadSanitizer: reported [0-9]+ warnings/ {
    broken++
    print "tsan-own-reports: outside a report: " $0
  }
  END {
    # A report cut off before its summary, as when the process dies while writing it.
    if (in_report) cut_off()
    printf "tsan-own-reports: %d reports, %d of them this package'"'"'s; %d other ThreadSanitizer lines\n", total, owned, broken
    exit (owned > 0 || broken > 0)
  }' "${logs[@]}" || failed=1
exit "$failed"
