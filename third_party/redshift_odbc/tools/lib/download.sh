#!/usr/bin/env bash
# The one way the tools download a file. Nothing else in tools/ runs curl: fetch-driver.sh uses it
# for the package, verification for the release files, and rename_pin.py fetch-tools for the pinned
# rename wheels.
#
#   download.sh <url> <out-file> <max-bytes>
#
# Writes the URL's body, exactly as the server sent it, to <out-file>. Fails, with one line on
# stderr, unless the answer is 200 and the body is at most <max-bytes> (rounded up to whole KiB).
# Needs no tool a fresh macOS host lacks: curl, bash and the base system's mktemp, tr and rm.
set -euo pipefail
set +o posix # POSIX mode counts ulimit -f in 512-byte blocks

(($# == 3)) || { echo "usage: download.sh <url> <out-file> <max-bytes>" >&2; exit 2; }
url="$1" out="$2" max="$3"
command -v curl >/dev/null || { echo "curl is required and was not found on PATH" >&2; exit 1; }
# -q ignores any .curlrc, which could add --location or --insecure; without --location a redirect
# fails. --path-as-is sends the path unnormalised. curl's own limits bound the time:
# --connect-timeout each connection, --speed-limit and --speed-time a stalled transfer (so --retry
# can act on it), --max-time each attempt, --retry-max-time the retries. --max-filesize refuses a
# declared size over the bound and, from curl 8.4, stops a larger body (exit 63); for older curl,
# such as manylinux_2_28's 7.61, the shell's own ulimit -f (1024-byte units, rounded up) stops it
# with SIGXFSZ (153), and ulimit -c 0 keeps that from leaving a core file. Both are bash builtins.
# A limit that cannot be set exits 120, which curl (0 to 101) never does.
errors="$(mktemp)"
trap 'rm -f -- "$errors"' EXIT
kib=$(((max + 1023) / 1024))
code="$(ulimit -c 0; ulimit -f "$kib" 2>/dev/null || exit 120
  curl -q --proto '=https' --proto-redir '=https' --max-redirs 0 --path-as-is --fail --silent --show-error \
    --retry 3 --retry-max-time 600 --connect-timeout 30 --speed-limit 1024 --speed-time 60 --max-time 600 \
    --max-filesize "$max" --output "$out" --write-out '%{http_code}' -- "$url" 2>"$errors")" && status=0 || status=$?
if ((status != 0)); then
  case "$status" in
  120) why=": cannot set the file-size limit (ulimit -f) to $kib KiB for the $max-byte bound" ;;
  153) why=" (curl exit 153): the body reached the $max-byte size bound (ulimit -f) and was stopped" ;;
  *) why=" (curl exit $status): $(tr '\n' ' ' < "$errors")" ;;
  esac
  echo "downloading $url failed$why" >&2; exit 1
fi
[[ "$code" == 200 ]] || { echo "$url answered HTTP $code; redirects are not followed" >&2; exit 1; }
