#!/usr/bin/env bash
# Runs every tool test under test/client/driver/ (each *_test.sh) and fails if any fails, or if a
# suite in the required list below is missing, so the target cannot pass by losing its tests. make
# test_driver calls it; it also runs on its own and needs no build (host-probe_test.sh skips its real-probe
# cases without the probe, unless RS_REQUIRE_PROBE=1, as make test_driver sets on an embedded Linux build).
# It fetches no rename tool: on Linux, make test_driver fetches the pinned rename tools first and sets
# RS_REQUIRE_RENAME_TOOLS=1 (rename-tools.sh). A change that adds a suite adds it to the list.
set -euo pipefail

here="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
shopt -s nullglob
tests=("$here"/*_test.sh)
for required in check-lock_test.sh driver-info_test.sh cpio_test.sh common_test.sh fetch-driver_test.sh \
  embedded-build_test.sh embed-build-graph_test.sh verify-signer_test.sh verify-release_test.sh \
  verify-extract_test.sh verify-elf_test.sh verify-env_test.sh xar_test.sh macos-pkgutil_test.sh \
  verify-macos-package_test.sh verify-macos-library_test.sh verify-macos-policy_test.sh \
  only-embedded_test.sh e2e-selection_test.sh run-e2e_test.sh host-probe_test.sh extension-coexistence_test.sh \
  embedded-macho_test.sh test-driver-env_test.sh run-loader-tests-as-root_test.sh \
  pe_test.sh windows-modules_test.sh verify-windows-signer_test.sh verify-windows-msi_test.sh \
  rename_test.sh embedded-resources_test.sh windows-stage1-check_test.sh \
  verify-windows-policy_test.sh run-windows-loading_test.sh windows-loader-source_test.sh macos-hosts_test.sh; do
  [[ -f "$here/$required" ]] || { echo "run-tool-tests: the required suite $required is missing" >&2; exit 1; }
done

failed=()
for t in "${tests[@]}"; do
  echo "# $(basename "$t")"
  bash "$t" || failed+=("$(basename "$t")")
done
((${#failed[@]} == 0)) || { echo "run-tool-tests: failed: ${failed[*]}" >&2; exit 1; }
echo "run-tool-tests: ${#tests[@]} tool test files passed"
