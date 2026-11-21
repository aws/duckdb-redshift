#!/usr/bin/env bash
# Every .test file under test/ must start with a require-env guard naming the client it is
# written for, so `unittest "test/*"` (what `make test` runs) skips the tests for the other
# client instead of running them against the wrong build. See test/README.md,
# "Selecting tests".
#
# Rules, applied to the first line that is neither blank nor a `#` comment:
#   - it must be `require-env REDSHIFT_TEST_CLIENT mock` or `require-env REDSHIFT_TEST_CLIENT odbc`,
#     with any spaces or tabs between the words;
#   - under test/mock/ the value must be mock; under test/client/ it must be odbc;
#   - no later line may guard on REDSHIFT_TEST_CLIENT again, with require-env or (since v2.0)
#     require-env-not. The runner evaluates every one it reaches and compares each value
#     (duckdb/test/sqlite/sqllogic_test_runner.cpp:1276-1334), so a second, contradicting guard
#     skips the file in every suite while its target still passes on the other files;
#   - no line may `include` another file (a v2.0 directive): the lint reads each file alone, so an
#     included file's guards would go unchecked.
#
# "Blank" is the pinned parser's definition, not a looser one: it strips `\r` and then treats a
# line as blank only when it is empty (sqllogic_parser.cpp:56, and EmptyOrComment at :38-40).
# A line of spaces or tabs is a statement to DuckDB, whose Tokenize then fails with
# "Empty line!?", so this lint must see it as the first statement and reject it here rather
# than skip it and let every build fail later.
#
# Files elsewhere under test/ may guard on either client.
#
# Usage: lint-test-guards.sh [package-root]   (the argument exists for the fixture tests)
# Exit status: 0 when every file passes, 1 otherwise; each problem is one line on stderr.
set -euo pipefail

root="${1:-$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)}"
cd "$root"

# Listed up front rather than read through a process substitution, whose exit status set -e
# never sees: a find failure (no test/ directory, an unreadable subdirectory) must fail the
# lint, not leave files unchecked.
files="$(find test -type f \( -name '*.test' -o -name '*.test_slow' -o -name '*.test_coverage' \) | LC_ALL=C sort)" ||
  { echo "lint-test-guards: cannot list the test files under $root/test" >&2; exit 1; }
# A tree with no test file at all is a broken layout, not a clean one: report it rather than
# pass having checked nothing.
[[ -n "$files" ]] || { echo "lint-test-guards: found no test files under $root/test" >&2; exit 1; }

status=0
problem() { echo "lint-test-guards: $*" >&2; status=1; }

while IFS= read -r f; do
  [[ -n "$f" ]] || continue
  guard=""; later=""; later_line=0; n=0
  while IFS= read -r line || [[ -n "$line" ]]; do
    n=$((n + 1))
    line="${line//$'\r'/}"
    if [[ -z "$line" || "$line" == \#* ]]; then continue; fi
    if [[ -z "$guard" ]]; then
      guard="$line"
    elif [[ -z "$later" && "$line" =~ ^require-env(-not)?[[:space:]]+REDSHIFT_TEST_CLIENT([[:space:]]|$) ]]; then
      later="$line"; later_line=$n
    fi
    if [[ "$line" =~ ^include([[:space:]]|$) ]]; then
      problem "$f: line $n includes another file, whose guards this lint cannot check; inline it instead"
    fi
  done < "$f"

  if [[ ! "$guard" =~ ^require-env[[:space:]]+REDSHIFT_TEST_CLIENT[[:space:]]+(mock|odbc)[[:space:]]*$ ]]; then
    problem "$f: first statement must be 'require-env REDSHIFT_TEST_CLIENT mock' or '... odbc', found: ${guard:-<none>}"
    continue
  fi
  client="${BASH_REMATCH[1]}"
  if [[ -n "$later" ]]; then
    problem "$f: line $later_line guards REDSHIFT_TEST_CLIENT again, which skips the whole file in every suite, found: $later"
  fi
  case "$f" in
    test/mock/*)   [[ "$client" == mock ]] || problem "$f: files under test/mock/ must guard on mock, found $client" ;;
    test/client/*) [[ "$client" == odbc ]] || problem "$f: files under test/client/ must guard on odbc, found $client" ;;
  esac
done <<< "$files"

if (( status == 0 )); then echo "lint-test-guards: ok"; fi
exit $status
