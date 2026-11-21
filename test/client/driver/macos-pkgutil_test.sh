#!/usr/bin/env bash
# Cross-check of third_party/redshift_odbc/tools/lib/xar.py with Apple's own reader:
# on macOS, pkgutil --expand-full and the helper take the
# same bytes from the locked package, and both equal the lock. Everywhere else it reports SKIP.
# Run by run-tool-tests.sh.
#
# The package comes from release-cache.sh, as in the other suites: the copy in
# build/driver-package-cache/packages/ when its hash is the lock's, else RS_DRIVER_MIRROR's, else
# one download into that cache through lib/download.sh, which needs only curl, so a macOS host
# with the package in neither place can still download it.
set -euo pipefail

here="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
repo="$(cd "$here/../../.." && pwd)"
tools="$repo/third_party/redshift_odbc/tools"
tmp="$(mktemp -d)"
trap 'rm -rf -- "${tmp:?}"' EXIT
# shellcheck source=release-cache.sh
source "$here/release-cache.sh" # sha, release_file

base_lock="${RS_DRIVER_LOCK:-$repo/third_party/redshift_odbc/driver.lock}"
row="$(bash "$tools/check-lock.sh" "$base_lock" | awk '$1 == "driver" && $2 == "osx_arm64"')"
name="pkgutil --expand-full and lib/xar.py give identical bytes, equal to the lock"
read -r _ _ _ want _ url pkg_sha path <<<"$row"
if [[ "$(uname -s)" != Darwin ]] || ! command -v pkgutil >/dev/null; then
  echo "ok 1 - $name # SKIP needs a macOS runner (pkgutil)"
elif [[ -z "$row" ]]; then
  echo "ok 1 - $name # SKIP the lock under test has no osx_arm64 row"
else
  # lib/download.sh needs only curl, so the package is downloaded where it is in neither place.
  # Any failure, a copy without the lock's SHA-256 included, fails.
  status=0
  pkg="$(release_file "$pkg_sha" "$url" "$pkg_sha")" || status=$?
  if ((status != 0)); then
    echo "not ok 1 - $name (the locked package ${url##*/} could not be copied or downloaded with the lock's SHA-256)"
    exit 1
  else
    pkgutil --expand-full "$pkg" "$tmp/expanded" >/dev/null
    # signer.txt's checksum algorithm, as the fetch reads it (lib/common.sh signer_checksum). Its
    # fail runs inside the substitution, so the reason is what the substitution holds.
    fail() { echo "$*" >&2; exit 1; }
    # shellcheck source=../../../third_party/redshift_odbc/tools/lib/common.sh
    . "$tools/lib/common.sh"
    alg="$(signer_checksum "$repo/third_party/redshift_odbc" 2>&1)" || { echo "not ok 1 - $name ($alg)"; exit 1; }
    python3 -I "$tools/lib/xar.py" extract "$pkg" "$path" "$tmp/helper" "$alg"
    # Compared by hash, not with cmp, which may be missing.
    if [[ "$(sha "$tmp/expanded/${path#./}")" == "$want" && "$(sha "$tmp/helper")" == "$want" ]]; then
      echo "ok 1 - $name"
    else
      echo "not ok 1 - $name (pkgutil: $(sha "$tmp/expanded/${path#./}" 2>&1); helper: $(sha "$tmp/helper"); lock: $want)"
      exit 1
    fi
  fi
fi
echo "# 1 case"
