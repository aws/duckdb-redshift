#!/usr/bin/env bash
# Fixture tests for check-no-test-hooks.sh. Run with `make test_graph_check`; make test_client and
# make test_client_tsan run them first. Each case compiles small objects into a build directory's
# extension files and checks the exit status and message.
set -euo pipefail

here="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
n=0; failed=0

# The Makefile wiring, which needs no tools: the recipe test_client_shape and test_client_tsan share
# runs the check on its build directory, and test_driver on the shipped build, both after the client
# tests are built.
makefile="$here/../../../Makefile"
# wired <name> <makefile> <recipe's first line> <the build line> <the check's line> [no]: whether the
# recipe runs the check after it builds redshift_client_tests in the same build directory, or with
# `no`, that it does not. Both are whole recipe lines, so a longer line that holds one, such as
# `<check> || true`, does not count.
wired() {
  local recipe found=yes
  recipe="$(sed -n "/^$3\$/,/^\$/p" "$2")"
  n=$((n+1))
  grep -xF -A1000 -- "$4" <<<"$recipe" | grep -xqF -- "$5" || found=no
  if [[ "$found" == "${6:-yes}" ]]; then
    echo "ok $n - $1"
  else
    echo "not ok $n - $1"; failed=$((failed+1))
  fi
}
build_line=$'\t$(call build_suite,$(CLIENT_BUILD),$(CLIENT_CONFIG),redshift_client_tests)'
check_line=$'\tbash $(PROJ_DIR)test/client/odbc/check-no-test-hooks.sh build/$(CLIENT_BUILD)'
wired "test_client and test_client_tsan run the check on their build" "$makefile" \
  'test_client_shape test_client_tsan:' "$build_line" "$check_line"
wired "test_driver runs the check on the shipped build" "$makefile" 'test_driver: lint_tests' \
  $'\t$(call build_suite,release,Release,redshift_client_tests)' \
  $'\tbash $(PROJ_DIR)test/client/odbc/check-no-test-hooks.sh build/release'
# A recipe whose build or check line carries more, such as `|| true`, or whose check comes before
# the build, is not wired.
fixture="$(mktemp)"
printf 'r:\n%s || true\n%s\n\n' "$build_line" "$check_line" > "$fixture"
wired "a build line with more on it does not count" "$fixture" 'r:' "$build_line" "$check_line" no
printf 'r:\n%s\n%s || true\n\n' "$build_line" "$check_line" > "$fixture"
wired "a check line with more on it does not count" "$fixture" 'r:' "$build_line" "$check_line" no
printf 'r:\n%s\n%s\n\n' "$check_line" "$build_line" > "$fixture"
wired "a check before the build does not count" "$fixture" 'r:' "$build_line" "$check_line" no
rm -f -- "$fixture"

# The other cases compile objects and expect the check to run, so Windows, where the check skips,
# and a host without the tools run none of them.
skip() { echo "# $1"; echo "# $n cases, $((n - failed)) passed"; exit $(( failed > 0 )); }
case "$(uname -s)" in
MINGW* | MSYS* | CYGWIN*) skip "the rest skipped on Windows, where the check skips too" ;;
esac
for tool in c++ ar nm strip; do
  command -v "$tool" >/dev/null || skip "the rest skipped: no $tool on PATH"
done
tmp="$(mktemp -d)"
trap 'rm -rf -- "${tmp:?}"' EXIT
check() {  # check <name> <expected-exit> <expected-substring> <build-dir>
  local name="$1" want="$2" needle="$3" out status
  n=$((n+1))
  out="$(bash "$here/check-no-test-hooks.sh" "$4" 2>&1)" && status=0 || status=$?
  if [[ "$status" == "$want" && "$out" == *"$needle"* ]]; then
    echo "ok $n - $name"
  else
    echo "not ok $n - $name (exit $status, wanted $want; output: $out)"; failed=$((failed+1))
  fi
}

clean='namespace duckdb { int Shipped() { return 1; } }'
probe='namespace redshift_test { struct TransferProbe { static int Start(); }; int TransferProbe::Start() { return 2; } }'
# Each hook the check lists, and a source that defines it alone, in the same order.
hook_names=('redshift_test::TransferProbe::' 'duckdb::SetDriverSourceForTesting(' 'duckdb::ResetProcessStateForTesting('
  'duckdb::SetConnectClockForTesting(' 'duckdb::SetNumberBindingForTesting(' 'duckdb::SetFetchThreadForTesting(')
hook_sources=("$probe" 'namespace duckdb { void SetDriverSourceForTesting(int) {} }'
  'namespace duckdb { void ResetProcessStateForTesting() {} }' 'namespace duckdb { void SetConnectClockForTesting(int) {} }'
  'namespace duckdb { void SetNumberBindingForTesting(int) {} }' 'namespace duckdb { void SetFetchThreadForTesting(int) {} }')
hooks="$(printf '%s\n' "${hook_sources[@]}")"

# object <name> <source>: a compiled object file.
object() {
  printf '%s\n' "$2" > "$tmp/$1.cpp"
  c++ -c -o "$tmp/$1.o" "$tmp/$1.cpp"
  echo "$tmp/$1.o"
}
# symbolless <name>: an object with no symbols at all. An empty source is not enough on macOS,
# where the assembler gives each section a local label (ltmp0), so the object is stripped whole:
# plain strip, as Apple's strip -x on an object relinks it with ld -r -x, which renames local labels
# rather than removing them. A strip that refuses stops the run (set -e) at the cp or ar that reads
# its answer.
symbolless() {
  local file
  file="$(object "$1" '')"
  strip "$file" && echo "$file"
}
# build <name> <loadable source> <library source> <tests source>: a build directory whose three
# files are objects of those sources, the library an archive; an empty source leaves the file out.
build() {
  local dir="$tmp/$1/extension/redshift"
  mkdir -p "$dir"
  [[ -z "$2" ]] || cp "$(object "$1-loadable" "$2")" "$dir/redshift.duckdb_extension"
  [[ -z "$3" ]] || ar rc "$dir/libredshift_extension.a" "$(object "$1-library" "$3")"
  [[ -z "$4" ]] || cp "$(object "$1-tests" "$4")" "$dir/redshift_client_tests"
  echo "$tmp/$1"
}

check "passes when only the tests define the hooks" 0 "defines no test-only hook" \
  "$(build clean "$clean" "$clean" "$hooks")"
check "refuses the probe in the loadable extension" 1 \
  "redshift.duckdb_extension defines the test-only hook redshift_test::TransferProbe::" \
  "$(build loadable "$probe" "$clean" "$hooks")"
# One case a hook, so a hook dropped from the check's list fails its own case.
for i in "${!hook_names[@]}"; do
  check "refuses ${hook_names[$i]} in the static library" 1 \
    "libredshift_extension.a defines the test-only hook ${hook_names[$i]}" \
    "$(build "library-$i" "$clean" "${hook_sources[$i]}" "$hooks")"
done
check "refuses tests that lack one hook, so a renamed one is not left unchecked" 1 \
  "does not define duckdb::SetDriverSourceForTesting(" "$(build stale "$clean" "$clean" "$probe")"
stripped="$(build stripped "$clean" "$clean" "$hooks")"
cp "$(symbolless empty)" "$stripped/extension/redshift/redshift.duckdb_extension"
check "refuses a shipped file with no symbols, which it could not check" 1 "has no symbols to check" "$stripped"
# nm heads an archive's member with its name, which is not a symbol.
headed="$(build headed "$clean" "" "$hooks")"
ar rc "$headed/extension/redshift/libredshift_extension.a" "$(symbolless headed-empty)"
check "refuses an archive whose member has no symbols" 1 \
  "libredshift_extension.a has no symbols to check" "$headed"
check "refuses a build without its loadable extension" 1 "is missing" "$(build missing "" "$clean" "$hooks")"
check "refuses a build without the client tests" 1 "redshift_client_tests is missing" \
  "$(build untested "$clean" "$clean" "")"

echo "# $n cases, $((n - failed)) passed"
exit $(( failed > 0 ))
