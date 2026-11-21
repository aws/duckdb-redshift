#!/usr/bin/env bash
# rename-tools.sh <lock>: the Linux make test_driver recipe's step before run-tool-tests.sh. When
# the lock has a rename row, rename_test.sh's pinned-tool cases must run, so this fetches delvewheel
# and pefile into RS_RENAME_TOOLS (rename_pin.py fetch-tools: from their pinned URLs by SHA-256,
# never from RS_DRIVER_MIRROR) and prints 1, the recipe's RS_REQUIRE_RENAME_TOOLS. A lock without a
# rename row fetches and prints nothing; a lock that cannot be read fails.
set -euo pipefail

(($# == 1)) || { echo "usage: rename-tools.sh <lock>" >&2; exit 2; }
here="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
[[ -f "$1" && -r "$1" ]] || { echo "rename-tools: cannot read the lock $1" >&2; exit 1; }
awk '$1 !~ /^#/ && $2 == "rename" { found = 1 } END { exit !found }' "$1" || exit 0
python3 -I "$here/../../../third_party/redshift_odbc/tools/lib/rename_pin.py" fetch-tools >&2 ||
  { echo "rename-tools: cannot fetch the pinned rename tools for $1; offline, put delvewheel's and" \
    "pefile's wheels in ${RS_RENAME_TOOLS:-build/rename-tools}/ as <sha256>.whl (rename-requirements.txt)" >&2
    exit 1; }
echo 1
