#!/usr/bin/env bash
# Fixture tests for check-no-network.sh. Run with `make test_graph_check`; make test_client,
# make test_client_tsan and make test_client_e2e run them first. Each case writes a /proc/net/dev
# table and checks the exit status and message.
set -euo pipefail

here="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
tmp="$(mktemp -d)"
trap 'rm -rf -- "${tmp:?}"' EXIT

n=0; failed=0
check() {  # check <name> <expected-exit> <expected-substring> <allow-network> <net-dev-file>
  local name="$1" want="$2" needle="$3" out status
  n=$((n+1))
  out="$(REDSHIFT_ALLOW_NETWORK="$4" bash "$here/check-no-network.sh" "$5" 2>&1)" && status=0 || status=$?
  if [[ "$status" == "$want" && "$out" == *"$needle"* ]]; then
    echo "ok $n - $name"
  else
    echo "not ok $n - $name (exit $status, wanted $want; output: $out)"; failed=$((failed+1))
  fi
}

# net_dev <file> <interface>...; writes the two header lines and one counter line per interface,
# as the kernel lays out /proc/net/dev.
net_dev() {
  local file="$tmp/$1"; shift
  printf 'Inter-|   Receive\n face |bytes    packets\n' > "$file"
  for name in "$@"; do printf '%6s: 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0\n' "$name" >> "$file"; done
  echo "$file"
}
refused="must run without a network"

check "passes when only the loopback interface exists" 0 "" "" "$(net_dev lo lo)"
check "passes when no interface exists at all" 0 "" "" "$(net_dev none)"
check "refuses a network interface, naming what it found" 1 "interfaces: lo eth0" "" \
  "$(net_dev eth lo eth0)"
check "passes when only lo and the kernel's fallback tunnels exist" 0 "" "" \
  "$(net_dev tunnels lo tunl0 sit0 ip6tnl0 gre0 gretap0 erspan0 ip6gre0 ip_vti0 ip6_vti0)"
check "refuses a network interface beside the fallback tunnels" 1 "interfaces: lo sit0 eth0" "" \
  "$(net_dev tunnels-eth lo sit0 eth0)"
check "refuses a numbered tunnel that is not the fallback" 1 "interfaces: lo sit1" "" "$(net_dev sit1 lo sit1)"
check "refuses when it cannot read the table" 1 "cannot read $tmp/missing" "" "$tmp/missing"
check "skips the check, saying so, when REDSHIFT_ALLOW_NETWORK is 1" 0 "skipped" 1 "$(net_dev eth1 lo eth0)"
check "does not skip it for REDSHIFT_ALLOW_NETWORK=0" 1 "$refused" 0 "$(net_dev eth2 lo eth0)"
check "does not skip it for REDSHIFT_ALLOW_NETWORK=true" 1 "$refused" true "$(net_dev eth3 lo eth0)"

# The Makefile wiring: the client targets run the check first, and its recipe hands the check
# the make variable, whatever its value.
root="$here/../../.."
expect() {  # expect <name> <command output> <expected substring>
  n=$((n+1))
  if [[ "$2" == *"$3"* ]]; then echo "ok $n - $1"; else
    echo "not ok $n - $1 (output: $2)"; failed=$((failed+1)); fi
}
expect "make test_client and make test_client_tsan run the check first" \
  "$(grep -E '^test_client test_client_tsan: check_no_network ' "$root/Makefile")" "check_no_network"
expect "make test_client_e2e runs the check first" \
  "$(grep -E '^test_client_e2e: check_no_network ' "$root/Makefile")" "check_no_network"
expect "make check_no_network passes REDSHIFT_ALLOW_NETWORK=1 to the check, which skips" \
  "$(make -s -C "$root" check_no_network REDSHIFT_ALLOW_NETWORK=1 2>&1)" "skipped"
stub="$tmp/stub/"; mkdir -p "${stub}test/client/odbc"; echo 'exit 1' > "${stub}test/client/odbc/check-no-network.sh"
expect "make check_no_network fails when the check fails" \
  "$(make -s -C "$root" check_no_network PROJ_DIR="$stub" >/dev/null 2>&1 && echo passed || echo failed)" "failed"
expect "make check_no_network passes REDSHIFT_ALLOW_NETWORK=0 to the check unchanged" \
  "$(make -n -C "$root" check_no_network REDSHIFT_ALLOW_NETWORK=0 2>&1)" 'REDSHIFT_ALLOW_NETWORK="0" bash'

echo "# $n cases, $((n - failed)) passed"
exit $(( failed > 0 ))
