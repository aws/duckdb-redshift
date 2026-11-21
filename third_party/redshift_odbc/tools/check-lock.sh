#!/usr/bin/env bash
# Checks a driver lock against its rules (README.md, "The lock") and prints it in normalised form.
# fetch-driver.sh reads a lock only through this output, once per fetch.
#
#   check-lock.sh [<lock>]
#
# Without an argument it checks the lock named by RS_DRIVER_LOCK when that is set, and otherwise
# the checked-in driver.lock. An empty RS_DRIVER_LOCK is an error rather than a silent fallback to
# the checked-in lock. The output format is described in lib/lock.py.
set -euo pipefail

here="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
if (($# > 1)); then
  echo "usage: check-lock.sh [<lock>]" >&2
  exit 2
elif (($# == 1)); then
  lock="$1"
elif [[ -n "${RS_DRIVER_LOCK+set}" ]]; then
  [[ -n "$RS_DRIVER_LOCK" ]] || { echo "check-lock: RS_DRIVER_LOCK is set but empty" >&2; exit 1; }
  lock="$RS_DRIVER_LOCK"
else
  lock="$here/../driver.lock"
fi
command -v python3 >/dev/null || { echo "check-lock: python3 is required and was not found" >&2; exit 1; }
# -I: ignore PYTHON* variables and the user's site directory, so they cannot change the checker's
# imports. PATH still chooses the interpreter, as it chooses every tool the build runs.
exec python3 -I "$here/lib/lock.py" "$lock"
