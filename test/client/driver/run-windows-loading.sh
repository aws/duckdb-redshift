#!/usr/bin/env bash
# Runs the driver's cases that only a Windows host can run, from one command: make test_driver's
# windows_amd64 branch runs it after the C++ suite, so every such run repeats each, except
# msiexec_suite's case in a logon that is not interactive, which skips (see below).
#
#   bash run-windows-loading.sh [<build dir>]
#
# The build dir, build/release by default, is the windows_amd64 embedded build the recipe made.
# The tool cases are windows-runner-tools.sh's steps, whose comment gives each one's rules. Each
# suite is named below and fails the run when it fails, a missing one included, so the run cannot
# pass by losing one; only msiexec_suite's case may skip, and its SKIP line names it, with the
# reason, and the summary counts it. A change that adds a Windows-only suite adds it to the list.
# Runs from Git's bash, with a 64-bit Python 3 on PATH as python3.
# RS_REQUIRE_MSIEXEC=1 makes that skip a failure; unset, empty or 0 lets it skip; any other value
# fails the run.
set -euo pipefail

here="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
case "$(uname -s)" in
MINGW* | MSYS*) ;;
*) echo "run-windows-loading: runs on Windows, from Git's bash, not on $(uname -s)" >&2; exit 1 ;;
esac
case "${RS_REQUIRE_MSIEXEC:-}" in
'' | 0 | 1) ;;
*)
  echo "run-windows-loading: RS_REQUIRE_MSIEXEC is '$RS_REQUIRE_MSIEXEC'; it is 1 to require" \
    "the msiexec /a cross-check, or unset, empty or 0 to let it skip" >&2
  exit 1
  ;;
esac
build="${1:-$here/../../../build/release}"
tool="$here/windows-runner-tools.sh"

failed=() skipped=() count=0
suite() { # suite <name> <command>...
  count=$((count + 1))
  echo "# $1"
  "${@:2}" || failed+=("$1")
}
# msiexec /a is refused in a batch logon, and likely in any that is not interactive; the tool then
# exits 3, saying why on its own line. fetch-driver.sh already checked these files against the lock,
# so such a logon skips this cross-check. RS_REQUIRE_MSIEXEC=1, for a run in an interactive logon,
# makes it fail; so does exit 3 with no such line, since only the tool's own refusal may skip.
msiexec_suite() { # msiexec_suite <name> <command>...
  count=$((count + 1))
  echo "# $1"
  local status=0 why line reason="" skip=""
  { why="$("${@:2}" 2>&1 >&3)" || status=$?; } 3>&1
  while IFS= read -r line; do
    [[ "$line" != "windows-runner-tools: admin-image: this logon is not interactive: "* ]] ||
      reason="${line#windows-runner-tools: admin-image: }"
  done <<<"$why"
  if ((status == 3)) && [[ -n "$reason" && "${RS_REQUIRE_MSIEXEC:-}" != 1 ]]; then
    echo "# SKIP $1: $reason; it runs by hand on each driver update, in an interactive logon as a" \
      "non-elevated account, with RS_REQUIRE_MSIEXEC=1"
    skipped+=("$1") skip=1
  fi
  # The tool's stderr follows the SKIP line, so a skip keeps everything else the tool said.
  [[ -z "$why" ]] || printf '%s\n' "$why" >&2
  [[ -z "$skip" ]] || return 0
  if ((status == 3)) && [[ -z "$reason" ]]; then
    echo "run-windows-loading: the tool exited 3 without saying why, so this is a failure" >&2
  elif ((status == 3)); then
    echo "run-windows-loading: RS_REQUIRE_MSIEXEC=1, so this logon's skip is a failure" >&2
  fi
  ((status == 0)) || failed+=("$1")
}
suite "the module-list helper's own cases" python3 -I "$here/windows_modules.py" self-test
suite "the downloader's bounds and refusals" bash "$tool" downloads
msiexec_suite "msiexec /a of the locked MSI, compared with the lock" \
  bash "$tool" admin-image "$build"
suite "Get-AuthenticodeSignature on the renamed msvcr100.dll" bash "$tool" authenticode "$build"
suite "the set the loadable extension writes is the lock's" bash "$tool" written-bytes "$build"
suite "the loadable extension's export table" bash "$tool" exports "$build"
suite "a planted ODBC32.dll import fails the import-table case" \
  bash "$tool" imports-negative "$build"
suite "a rewritten bare-name string maps nothing and can name no file" \
  bash "$tool" bare-names "$build"

if ((${#failed[@]} > 0)); then
  printf -v names '%s; ' "${failed[@]}"
  echo "run-windows-loading: ${#failed[@]} of $count failed: ${names%; }" >&2
  exit 1
fi
if ((${#skipped[@]} > 0)); then
  printf -v names '%s; ' "${skipped[@]}"
  echo "run-windows-loading: $((count - ${#skipped[@]})) runner-only suites passed," \
    "${#skipped[@]} skipped: ${names%; }"
  exit 0
fi
echo "run-windows-loading: $count runner-only suites passed"
