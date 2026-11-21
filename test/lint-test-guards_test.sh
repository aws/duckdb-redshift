#!/usr/bin/env bash
# Fixture tests for lint-test-guards.sh. Each case builds a small tree under a temp dir and
# checks the lint's exit status and message. Run with `make test_lint_guards`.
set -euo pipefail

here="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
lint="$here/lint-test-guards.sh"
tmp="$(mktemp -d)"
trap 'rm -rf -- "${tmp:?}"' EXIT

n=0; failed=0
check() {  # check <name> <expected-exit> <expected-substring> <root>
  local name="$1" want="$2" needle="$3" root="$4" out status
  n=$((n+1))
  out="$(bash "$lint" "$root" 2>&1)" && status=0 || status=$?
  if [[ "$status" == "$want" && "$out" == *"$needle"* ]]; then
    echo "ok $n - $name"
  else
    echo "not ok $n - $name (exit $status, wanted $want; output: $out)"; failed=$((failed+1))
  fi
}

fresh() {
  local r="$tmp/$1"; rm -rf -- "${r:?}"; mkdir -p "$r/test/sql" "$r/test/mock" "$r/test/client/sql"
  echo "$r"
}
guarded() { printf '# name: %s\n# group: [x]\n\n%s\n\nstatement ok\nSELECT 1\n' "$1" "require-env REDSHIFT_TEST_CLIENT $2"; }

r="$(fresh base)"
guarded m mock > "$r/test/mock/a.test"; guarded c odbc > "$r/test/client/sql/a.test"
check "accepts a tree where every file is guarded for its directory" 0 "ok" "$r"

r="$(fresh unguarded)"; printf '# name: x\n\nstatement ok\nSELECT 1\n' > "$r/test/mock/u.test"
check "rejects a file whose first statement is not the guard" 1 "test/mock/u.test: first statement must be" "$r"

r="$(fresh comment-only)"; printf '# name: x\n# just comments\n' > "$r/test/mock/e.test"
check "rejects a file with no statement at all" 1 "found: <none>" "$r"

r="$(fresh wrongdir1)"; guarded x odbc > "$r/test/mock/w.test"
check "rejects odbc under test/mock/" 1 "files under test/mock/ must guard on mock, found odbc" "$r"

r="$(fresh wrongdir2)"; guarded x mock > "$r/test/client/sql/w.test"
check "rejects mock under test/client/" 1 "files under test/client/ must guard on odbc, found mock" "$r"

r="$(fresh badvalue)"; guarded x sqlite > "$r/test/mock/v.test"
check "rejects a guard with an unknown client value" 1 "first statement must be" "$r"

r="$(fresh sqlfree)"; guarded x odbc > "$r/test/sql/o.test"; guarded y mock > "$r/test/sql/m.test"
check "places no directory rule on test/sql/" 0 "ok" "$r"

r="$(fresh slow)"; printf 'statement ok\nSELECT 1\n' > "$r/test/mock/s.test_slow"
check "covers .test_slow files too" 1 "test/mock/s.test_slow" "$r"

r="$(fresh sql-unguarded)"; printf '# name: x\n\nstatement ok\nSELECT 1\n' > "$r/test/sql/old.test"
check "rejects an unguarded file under test/sql/ too" 1 "test/sql/old.test: first statement must be" "$r"

r="$(fresh guard-after-comment)"; printf '# name: x\n\n# a comment before the guard is fine\nrequire-env REDSHIFT_TEST_CLIENT mock\n' > "$r/test/mock/g.test"
check "ignores blank and comment lines before the guard" 0 "ok" "$r"

r="$(fresh guard-after-statement)"; printf 'require redshift\nrequire-env REDSHIFT_TEST_CLIENT mock\n' > "$r/test/mock/g.test"
check "rejects a guard that is not the first statement" 1 "found: require redshift" "$r"

# The pinned parser strips \r and then calls a line blank only when it is empty
# (sqllogic_parser.cpp:56, EmptyOrComment at :38-40), so these three cases pin the lint to it.
r="$(fresh whitespace-line)"; printf '# name: x\n   \nrequire-env REDSHIFT_TEST_CLIENT mock\n' > "$r/test/mock/w.test"
check "rejects a whitespace-only line before the guard, as the parser would" 1 "test/mock/w.test: first statement must be" "$r"

r="$(fresh tab-guard)"; printf '# name: x\n\nrequire-env\tREDSHIFT_TEST_CLIENT\tmock\n' > "$r/test/mock/t.test"
check "accepts a guard whose words are separated by tabs" 0 "ok" "$r"

r="$(fresh crlf)"; printf '# name: x\r\n\r\nrequire-env REDSHIFT_TEST_CLIENT mock\r\n' > "$r/test/mock/c.test"
check "accepts a CRLF file, whose \\r the parser strips" 0 "ok" "$r"

# The runner evaluates every require-env and require-env-not it reaches and compares each value,
# so a second guard skips the file in every suite (sqllogic_test_runner.cpp:1276-1334).
r="$(fresh later-guard)"
printf 'require-env REDSHIFT_TEST_CLIENT mock\n\nstatement ok\nSELECT 1\n\nrequire-env REDSHIFT_TEST_CLIENT odbc\n' > "$r/test/mock/l.test"
check "rejects a second REDSHIFT_TEST_CLIENT guard later in the file" 1 "line 6 guards REDSHIFT_TEST_CLIENT again" "$r"

# v2.0's include directive would pull in a file the lint never reads, with guards of its own.
r="$(fresh include)"
printf 'require-env REDSHIFT_TEST_CLIENT mock

include test/mock/other.test
' > "$r/test/mock/i.test"
check "rejects an include" 1 "line 3 includes another file" "$r"

# v2.0 adds require-env-not, which skips the file when the variable has a listed value.
r="$(fresh later-guard-not)"
printf 'require-env REDSHIFT_TEST_CLIENT mock\n\nrequire-env-not REDSHIFT_TEST_CLIENT mock\n' > "$r/test/mock/l.test"
check "rejects a later require-env-not REDSHIFT_TEST_CLIENT" 1 "line 3 guards REDSHIFT_TEST_CLIENT again" "$r"
r="$(fresh guard-not-first)"
printf 'require-env-not REDSHIFT_TEST_CLIENT odbc\n' > "$r/test/mock/n.test"
check "rejects require-env-not as the guard" 1 "first statement must be" "$r"

r="$(fresh later-guard-other-env)"
printf 'require-env REDSHIFT_TEST_CLIENT odbc\n\nrequire-env REDSHIFT_TEST_HOST\n' > "$r/test/client/sql/l.test"
check "allows a later require-env on another variable" 0 "ok" "$r"

r="$(fresh current-require)"; printf 'require-env REDSHIFT_TEST_CLIENT mock\nrequire redshift\n' > "$r/test/mock/g.test"
check "allows require redshift" 0 "ok" "$r"

r="$tmp/no-test-dir"; mkdir -p "$r"
check "fails when it cannot list test/" 1 "cannot list the test files under $r/test" "$r"

r="$tmp/empty-test-dir"; mkdir -p "$r/test/mock"
check "fails when test/ holds no test file" 1 "found no test files under $r/test" "$r"

echo "# $n cases, $((n - failed)) passed"
exit $(( failed > 0 ))
