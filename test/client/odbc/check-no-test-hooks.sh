#!/usr/bin/env bash
# Fails when the extension a build ships, its loadable file or its static library, defines one of the
# client's test-only hooks: the source hook's setters and the read-path probe, which can switch the
# driver's compression off. The client test binary must define each of them, so a
# hook renamed past this list fails here instead of going unchecked.
#
#   check-no-test-hooks.sh <build-dir>
#
# Reads the symbol tables with nm -C; the release builds keep their local symbols, and a file with
# none fails. On Windows, whose MSVC files nm cannot read, it says so and checks nothing.
set -euo pipefail

build_dir="${1:?usage: check-no-test-hooks.sh <build-dir>}"
extension_dir="$build_dir/extension/redshift"
fail() { echo "check-no-test-hooks: $*" >&2; exit 1; }
case "$(uname -s)" in
MINGW* | MSYS* | CYGWIN*)
  echo "check-no-test-hooks: skipped on Windows: nm cannot read the MSVC build's symbols"
  exit 0 ;;
esac
hooks=('redshift_test::TransferProbe::' 'duckdb::SetDriverSourceForTesting(' 'duckdb::ResetProcessStateForTesting('
  'duckdb::SetConnectClockForTesting(' 'duckdb::SetNumberBindingForTesting(' 'duckdb::SetFetchThreadForTesting(')

# The defined symbols of `file`, demangled. nm's failure, or a file with no symbols, fails the check
# rather than reading as no match. For an archive nm heads each member with a blank line and
# `member.o:`, which are not symbols.
defined_symbols() {
  local symbols
  symbols="$(nm -C --defined-only "$1" 2>/dev/null)" || fail "nm could not read $1"
  symbols="$(sed -e '/^$/d' -e '/:$/d' <<<"$symbols")"
  [[ -n "$symbols" ]] || fail "$1 has no symbols to check; build it unstripped"
  printf '%s\n' "$symbols"
}

for shipped in "$extension_dir/redshift.duckdb_extension" "$extension_dir/libredshift_extension.a"; do
  [[ -f "$shipped" ]] || fail "$shipped is missing; build $build_dir first"
  symbols="$(defined_symbols "$shipped")"
  for hook in "${hooks[@]}"; do
    ! grep -qF "$hook" <<<"$symbols" || fail "$(basename "$shipped") defines the test-only hook $hook"
  done
done
tests="$extension_dir/redshift_client_tests"
[[ -f "$tests" ]] || fail "$tests is missing; build redshift_client_tests in $build_dir first"
symbols="$(defined_symbols "$tests")"
for hook in "${hooks[@]}"; do
  grep -qF "$hook" <<<"$symbols" ||
    fail "redshift_client_tests does not define $hook, so this list no longer names it; update the list"
done
echo "check-no-test-hooks: the shipped extension defines no test-only hook"
