#!/usr/bin/env bash
# host-probe.sh
# The host probe: prints the host's facts, then runs redshift_driver_probe, which loads the
# embedded driver as the extension does and runs the closed-port smoke, and adds each directory's
# file system type. One line per check, `<check>\t<ok|fail|skip>\t<detail>`. It takes no
# arguments and makes no connection to a Redshift endpoint: the smoke dials a local port nothing
# listens on, and a live query is the extension's own LOAD and query, run separately. RS_PROBE
# names the probe (default: the embedded build's). Exits with the probe's status: 0 when no check
# failed, 1 when one did, 2 on a usage error or without a probe, and 128 plus the signal, after a
# `probe fail` line, when it was killed.
set -euo pipefail

# The probe makes no live connect (the ODBC client's tests make those), so the usage line says where
# a live query runs.
[[ $# -eq 0 ]] ||
  { echo "usage: host-probe.sh (no arguments; a row's live query is the extension's own LOAD and query, run beside this tool)" >&2; exit 2; }
here="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
probe="${RS_PROBE:-$here/../../../build/release/extension/redshift/redshift_driver_probe}"
[[ -x "$probe" ]] || { printf 'probe\tfail\tno probe at %s (make test_driver builds it)\n' "$probe"; exit 2; }

# A value is one field: a tab, newline or carriage return in it (REDSHIFT_DRIVER_DIR's, say) becomes a space.
fact() { printf 'host\tok\t%s=%s\n' "$1" "$(printf '%s' "$2" | tr '\t\n\r' '   ')"; }
fact kernel "$(uname -sr)"
fact arch "$(uname -m)"
# RS_OS_RELEASE names another os-release file, for the tests.
fact os "$( (. "${RS_OS_RELEASE:-/etc/os-release}" 2>/dev/null && echo "${PRETTY_NAME:-unknown}") || echo unknown)"
fact glibc "$(getconf GNU_LIBC_VERSION 2>/dev/null || echo unknown)"
# Whether the library cache lists libkeyutils.so.1, the one library the driver needs beyond a base system.
# The cache only: the loader also looks in glibc's system directories, and the probe's load line below is
# the answer for the host. ldconfig is not on every user's PATH; its output is taken whole before it is
# searched.
ldcache="$( (ldconfig -p || /sbin/ldconfig -p || /usr/sbin/ldconfig -p) 2>/dev/null || true)"
case "$ldcache" in
  *'libkeyutils.so.1 '*) keyutils=present ;;
  '') keyutils=unknown ;;
  *) keyutils=absent ;;
esac
fact libkeyutils_in_cache "$keyutils"
# Without /proc (macOS) the mount table and the user namespace's uid_map cannot be read here, so each fact is
# unknown, not empty.
fact tmp_mount "$(awk '$2 == "/tmp" {o = $4} END {print o ? o : "not a separate mount"}' /proc/self/mounts 2>/dev/null || echo unknown)"
fact uid_map "$(awk 'NR == 1 {print $1 ":" $2 ":" $3}' /proc/self/uid_map 2>/dev/null || echo unknown)"
fact driver_dir "${REDSHIFT_DRIVER_DIR-unset}"

work="$(mktemp -d)"
trap 'rm -rf -- "${work:?}"' EXIT
status=0
"$probe" > "$work/probe.out" || status=$?
# Each line as the probe printed it; after the directory the driver was loaded from and each base
# (`<role>: <directory>[: <why it is skipped>]`), that directory's file system type (stat -f).
filesystem() {
  [[ "$1" == /* ]] || return 0
  # GNU stat's output only: BSD stat reads -f as its format option, prints something else and fails.
  local type
  type="$(stat -f -c %T -- "$1" 2>/dev/null)" || type=unknown
  printf 'filesystem\tok\t%s %s\n' "$1" "$type"
}
while IFS=$'\t' read -r check outcome detail; do
  printf '%s\t%s\t%s\n' "$check" "$outcome" "$detail"
  case "$check/$outcome" in
    directory/ok) filesystem "$detail" ;;
    base/ok) filesystem "${detail#*: }" ;;
    base/skip) dir="${detail#*: }"; filesystem "${dir%%: *}" ;;
  esac
done < "$work/probe.out"
((status <= 128)) || printf 'probe\tfail\tthe probe was killed by signal %d after the lines above\n' "$((status - 128))"
exit "$status"
