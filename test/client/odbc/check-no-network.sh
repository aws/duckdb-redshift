#!/usr/bin/env bash
# Fails unless this process has no network, so that a passing make test_client shows the
# REDSHIFT_DRIVER=none build needs none, on a build directory no networked run configured first:
# the graph check cannot see a download CMake makes while configuring. It reads /proc/net/dev
# because that file belongs to the caller's own network namespace; /sys/class/net shows the
# namespace that mounted sysfs, so an `unshare -rn` run would look networked there.
# REDSHIFT_ALLOW_NETWORK=1 skips the check and says so.
#
# A host with the ipip, sit, ip6_tunnel, ip_gre, ip6_gre, ip_vti or ip6_vti module loaded gives every
# new namespace that module's fallback tunnel device. A tunnel only carries packets over another
# interface, so those names alone, beside lo, still mean no network.
#
#   check-no-network.sh [<net-dev-file>]    (default /proc/net/dev; its tests pass a fixture)
set -euo pipefail

net_dev="${1:-/proc/net/dev}"
if [[ "${REDSHIFT_ALLOW_NETWORK:-}" == 1 ]]; then
  echo "check-no-network: skipped (REDSHIFT_ALLOW_NETWORK=1); this run does not show that the" \
    "REDSHIFT_DRIVER=none build needs no network"
  exit 0
fi

if [[ -r "$net_dev" ]]; then
  # The first two lines are the table's headers; each later line starts with "<interface>:".
  interfaces="$(awk -F: 'NR > 2 { gsub(/[[:space:]]/, "", $1); printf "%s ", $1 }' "$net_dev")"
  interfaces="${interfaces% }"
  networked=0
  for interface in $interfaces; do
    case "$interface" in
    lo | tunl0 | sit0 | ip6tnl0 | gre0 | gretap0 | erspan0 | ip6gre0 | ip_vti0 | ip6_vti0) ;;
    *) networked=1 ;;
    esac
  done
  [[ "$networked" == 1 ]] || exit 0
  found="interfaces: $interfaces"
else
  found="cannot read $net_dev; a host without it, such as macOS, needs REDSHIFT_ALLOW_NETWORK=1"
fi
echo "check-no-network: make test_client, make test_client_tsan and make test_client_e2e must run" \
  "without a network to show that the REDSHIFT_DRIVER=none build needs none ($found)." \
  "Run them in a container without a network or under unshare -rn, or set" \
  "REDSHIFT_ALLOW_NETWORK=1 to run without that proof." >&2
exit 1
