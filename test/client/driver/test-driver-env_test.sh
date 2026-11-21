#!/usr/bin/env bash
# make test_driver runs the C++ suite through the Makefile's DRIVER_SUITE_ENV, which must leave out the caller's
# LD_LIBRARY_PATH and every DYLD_ variable the macOS loader refuses (PlatformLoaderVariables in
# src/transport/driver/driver_loader_macos.cpp, read from there), so a variable the caller set cannot fail the
# suite's in-process loads. What the suite's environment holds is read with a probe recipe through a second
# makefile, as e2e-selection_test.sh reads a recipe's; and the recipe itself must run the suite through it, with
# RS_REQUIRE_PROBE=1 on the Linux arm only (macOS builds no probe), and the rename tools fetched
# and required there alone; the Windows arm's steps are read too. Where
# the host strips DYLD_ variables before a recipe runs (macOS's System Integrity Protection, in protected
# programs such as /usr/bin/env and /usr/bin/make), the DYLD_ half has nothing to show and reports SKIP.
# Needs no build and no network.
set -euo pipefail

here="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
root="$(cd "$here/../../.." && pwd)"
n=0; failed=0
check() {  # check <name> <command...>
  local name="$1"; shift; n=$((n+1))
  if "$@"; then echo "ok $n - $name"; else echo "not ok $n - $name"; failed=$((failed+1)); fi
}

probe_dir="$(mktemp -d)"
trap 'rm -rf -- "${probe_dir:?}"' EXIT
printf '%s\n' 'probe-suite-env: ; @$(DRIVER_SUITE_ENV) env' 'probe-plain-env: ; @env' > "$probe_dir/probe.mk"
# The names PlatformLoaderVariables returns, from its return statement.
names="$(awk '/^std::vector<std::string> PlatformLoaderVariables\(\) \{/ {f=1} f && /^}/ {exit} f' \
  "$root/src/transport/driver/driver_loader_macos.cpp" | grep -o '"DYLD_[A-Z_]*"' | tr -d '"')" || names=""
set_all=(LD_LIBRARY_PATH=/redshift-planted REDSHIFT_PROBE_CONTROL=seen)
for name in $names; do set_all+=("$name=/redshift-planted"); done
probe() {  # probe <target>: the recipe's environment, with every variable above set by the caller
  (cd "$root" && env "${set_all[@]}" make -s --no-print-directory -f Makefile -f "$probe_dir/probe.mk" "$1" 2>/dev/null)
}
suite_env="$(probe probe-suite-env)" || suite_env=""
plain_env="$(probe probe-plain-env)" || plain_env=""

lists_six() { [[ "$(wc -w <<<"$names")" -eq 6 ]]; }
seen() { grep -qx 'REDSHIFT_PROBE_CONTROL=seen' <<<"$suite_env" && grep -qx 'REDSHIFT_PROBE_CONTROL=seen' <<<"$plain_env"; }
not_set() { ! grep -q "^$1=" <<<"$suite_env"; }
check "the six DYLD_ names are read from PlatformLoaderVariables" lists_six
# The control: both probes see a variable the caller set, so an empty answer, or a SKIP below, cannot pass for
# a probe that did not run.
check "the probe recipes see the caller's environment" seen
check "the suite runs without the caller's LD_LIBRARY_PATH" not_set LD_LIBRARY_PATH
for name in $names; do
  if ! grep -q "^$name=" <<<"$plain_env"; then
    n=$((n+1)); echo "ok $n - the suite runs without $name # SKIP this host strips $name before a recipe runs"
    continue
  fi
  check "the suite runs without the caller's $name" not_set "$name"
done

# The recipe: every line that runs the suite goes through DRIVER_SUITE_ENV (and there are three, one per arm), and
# only the Linux arm's tool tests get RS_REQUIRE_PROBE=1.
recipe="$(awk '/^test_driver:/ {f=1; next} f && /^[^\t]/ {exit} f' "$root/Makefile")"
suite_lines() { grep 'redshift_driver_tests -w NoTests' <<<"$recipe" || true; }
through_env() { [[ "$(suite_lines | wc -l)" -eq 3 ]] && ! suite_lines | grep -vqF '$(DRIVER_SUITE_ENV) ./build/'; }
arm() { awk -v want="$1" 'index($0, want) {f=1} f {print} f && /;; \\$/ {exit}' <<<"$recipe"; }
probe_on_linux_only() {
  arm 'linux_amd64 | linux_arm64)' | grep -q 'RS_REQUIRE_PROBE=1 ' &&
  arm 'osx_amd64 | osx_arm64)' | grep -q 'run-tool-tests.sh' && ! arm 'osx_amd64 | osx_arm64)' | grep -q RS_REQUIRE_PROBE; }
# The Linux arm fetches the pinned rename tools for its lock before the tool tests, which get the
# step's output as RS_REQUIRE_RENAME_TOOLS; the macOS arm fetches none and sets no switch, so those
# cases skip there.
rename_tools_on_linux_only() {
  local a fetch_at tests_at
  local fetch='rename_tools="$$(bash $(PROJ_DIR)test/client/driver/rename-tools.sh "$$lock")" &&'
  a="$(arm 'linux_amd64 | linux_arm64)')"
  fetch_at="$(grep -nF -- "$fetch" <<<"$a" | cut -d: -f1)"
  tests_at="$(grep -n 'run-tool-tests.sh' <<<"$a" | cut -d: -f1)"
  [[ -n "$fetch_at" && -n "$tests_at" && "$fetch_at" -lt "$tests_at" ]] &&
    grep -qF 'RS_DRIVER_LOCK="$$lock"' <<<"$a" &&
    grep -qF 'RS_REQUIRE_RENAME_TOOLS="$$rename_tools"' <<<"$a" &&
    ! arm 'osx_amd64 | osx_arm64)' | grep -q 'rename-tools.sh\|RS_REQUIRE_RENAME_TOOLS'
}
# The Windows arm runs on a Windows host only, so its steps are read here: the graph check, the interface's
# import-table case on the embedded build, the resource check of the six images it lists, and the
# Windows-only cases (run-windows-loading.sh), with no tool tests, which need POSIX file modes and links.
windows_steps() {
  local a; a="$(arm 'windows_amd64)')"
  for step in '--expect-embedded build/release' '$(call build_suite,release,Release,redshift_client_tests)' \
    'RS_REQUIRE_DUMPBIN=1 ./build/release/extension/$(EXT_NAME)/redshift_client_tests -w NoTests' \
    "'The loadable extension imports no ODBC driver manager'" \
    'python3 -I $(PROJ_DIR)test/client/driver/embedded_resources.py build/release/extension/$(EXT_NAME)/redshift_driver/driver_info.h' \
    '$(EXT_NAME).duckdb_extension build/release/duckdb.exe build/release/test/unittest.exe' \
    'build/release/src/duckdb.dll build/release/extension/$(EXT_NAME)/redshift_client_tests.exe' \
    'redshift_driver_tests.exe' 'bash $(PROJ_DIR)test/client/driver/run-windows-loading.sh ;;'; do
    grep -qF -- "$step" <<<"$a" || return 1
  done
  # After the dumpbin guard, each step runs only once the one before passed: no line ends in "; \".
  ! grep -vF 'command -v dumpbin' <<<"$a" | grep -qE '[^;]; \\$' && ! grep -q run-tool-tests.sh <<<"$a"
}
# The Windows-only cases come last, on the line after the suite's, and only once the suite has passed.
windows_cases_after_suite() {
  local a want
  a="$(arm 'windows_amd64)')"
  want="\$(DRIVER_SUITE_ENV)./build/release/extension/\$(EXT_NAME)/redshift_driver_tests-wNoTests"
  want+="'~[.]'&&\\|bash\$(PROJ_DIR)test/client/driver/run-windows-loading.sh;;\\|"
  grep -A1 -F 'redshift_driver_tests -w NoTests' <<<"$a" | tr -d '\t ' | tr '\n' '|' | grep -qxF "$want"
}
check "test_driver runs the suite through DRIVER_SUITE_ENV on the three embedded arms" through_env
check "test_driver passes RS_REQUIRE_PROBE=1 to the tool tests on the Linux arm only" probe_on_linux_only
check "test_driver fetches the pinned rename tools and requires them on the Linux arm only" \
  rename_tools_on_linux_only
check "test_driver's Windows arm checks the graph, the imports and six images' resources, then the runner's cases" \
  windows_steps
check "test_driver's Windows arm runs run-windows-loading.sh after the suite passes" \
  windows_cases_after_suite

((failed == 0)) || { echo "$failed of $n failed" >&2; exit 1; }
echo "all $n passed"
