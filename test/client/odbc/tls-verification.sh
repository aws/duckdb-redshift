#!/usr/bin/env bash
# tls-verification.sh <build directory>
# TLS verification on the real driver (tls_verification_test.cpp: the [tls] cases, then the [tls_first]
# case in a process of its own), in make driver_e2e's test-endpoint build with the embedded driver
# (build/driver-e2e). Runs tls_test_servers_test.py, makes a test PKI for this run in a new temporary
# directory, starts tls_test_servers.py on 127.0.0.1 over it, runs the cases with REDSHIFT_TEST_TLS_DIR
# naming that directory, then stops the servers and removes the directory. Needs openssl and python3
# on PATH; run it with no network, as test/README.md shows. On Windows it runs in Git Bash, with
# Windows paths throughout, as the suite and openssl are Windows programs.
set -euo pipefail
windows=0
case "$(uname -s)" in MINGW* | MSYS*) windows=1 ;; esac
if ((windows)); then
  # Git Bash rewrites arguments that look like POSIX paths, such as -subj's "/CN=...", for Windows programs.
  export MSYS_NO_PATHCONV=1
  # The long form (-l), as the Windows scripts in test/client/driver/ give: a temporary
  # directory can hold an 8.3 short name, which run-e2e.sh keeps from Windows programs.
  native() { cygpath -m -l "$1"; }
else
  native() { printf '%s\n' "$1"; }
fi
# Paths are assigned, then converted, so that set -e stops the script on a failed cd or mktemp.
here="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
here="$(native "$here")"
build="$(cd "${1:?usage: tls-verification.sh <build directory>}" && pwd)"
build="$(native "$build")"
suite="$build/extension/redshift/redshift_client_tests"
((windows)) && suite+=".exe"
fail() { echo "tls-verification: $*" >&2; exit 1; }
lines() { if [[ -f "$1" ]]; then wc -l <"$1"; else echo 0; fi; }
[[ -x "$suite" ]] || fail "$build has no redshift_client_tests; build it in make driver_e2e's build"
command -v openssl >/dev/null || fail "needs openssl on PATH"
grep -qs '^REDSHIFT_DRIVER:STRING=embedded' "$build/CMakeCache.txt" &&
  grep -qs '^REDSHIFT_TEST_ENDPOINTS:BOOL=ON' "$build/CMakeCache.txt" ||
  fail "$build is not an embedded build with the test-endpoint switch; run make driver_e2e REDSHIFT_DRIVER=embedded"
# The authentication cases' reply parsing, which the real driver reaches only on some platforms.
python3 -I "$here/tls_test_servers_test.py" || fail "tls_test_servers_test.py failed"

dir="$(mktemp -d "${TMPDIR:-/tmp}/redshift-tls.XXXXXX")"
dir="$(native "$dir")"
servers=""
stop_servers() {
  # A second signal while cleaning up would end bash before the servers and the keys are gone.
  trap '' TERM INT HUP
  if [[ -n "$servers" ]]; then
    # The stop file, which the servers poll: on Windows they run in a native Python, outside Git
    # Bash's POSIX signals. Elsewhere SIGTERM too, which ends them first.
    : >"$dir/stop"
    ((windows)) || kill "$servers" 2>/dev/null || true
    # Bounded: the servers are killed if they have not stopped within 10 seconds.
    for _ in $(seq 100); do kill -0 "$servers" 2>/dev/null || break; sleep 0.1; done
    kill -9 "$servers" 2>/dev/null || true
  fi
  rm -rf -- "${dir:?}"
}
trap stop_servers EXIT
# A signal ends the script through the EXIT trap, so cleanup runs once and finishes.
trap 'exit 143' TERM INT HUP

# Each certificate lives one day, with a key made for this run alone; the CA keys go once used.
cat >"$dir/openssl.cnf" <<'EOF'
[req]
distinguished_name = name
[name]
[ca]
basicConstraints = critical, CA:TRUE
keyUsage = critical, keyCertSign
EOF
new_key() { openssl genpkey -algorithm EC -pkeyopt ec_paramgen_curve:P-256 -out "$dir/$1.key" 2>/dev/null; }
new_ca() {
  new_key "$1"
  openssl req -new -x509 -config "$dir/openssl.cnf" -extensions ca -key "$dir/$1.key" -days 1 \
    -subj "/CN=Redshift TLS test $1" -out "$dir/$1.pem"
}
# leaf <name> <CA> <host>: a server certificate for <host>, signed by <CA>. openssl numbers the
# certificates itself, in <CA>.srl, so no serial passes through the shell.
leaf() {
  new_key "$1"
  openssl req -new -config "$dir/openssl.cnf" -key "$dir/$1.key" -subj "/CN=$3" -out "$dir/$1.csr"
  printf 'basicConstraints = critical, CA:FALSE\nextendedKeyUsage = serverAuth\nsubjectAltName = DNS:%s\n' "$3" \
    >"$dir/$1.ext"
  openssl x509 -req -in "$dir/$1.csr" -CA "$dir/$2.pem" -CAkey "$dir/$2.key" -CAcreateserial \
    -days 1 -extfile "$dir/$1.ext" -out "$dir/$1.pem" 2>/dev/null
}
new_ca trusted-ca
new_ca unrelated-ca
# A CA that signs none of the servers' certificates, for a database hop that trusts no server.
new_ca unused-ca
leaf control trusted-ca localhost
leaf trusted trusted-ca localhost
leaf wrong_host trusted-ca wrong-host.invalid
leaf untrusted unrelated-ca localhost
rm -f -- "$dir"/*-ca.key "$dir"/*-ca.srl "$dir"/*.csr "$dir"/*.ext

python3 -I "$here/tls_test_servers.py" "$dir" &
servers=$!
for _ in $(seq 300); do
  [[ -f "$dir/ports" ]] && break
  kill -0 "$servers" 2>/dev/null || fail "tls_test_servers.py stopped before it was ready"
  sleep 0.1
done
[[ -f "$dir/ports" ]] || fail "tls_test_servers.py was not ready within 30 seconds"

# The Linux loader refuses an LD_LIBRARY_PATH with an empty or relative element (test/README.md).
status=0
REDSHIFT_TEST_TLS_DIR="$dir" env -u LD_LIBRARY_PATH "$suite" -w NoTests "[tls]" || status=$?
# The driver trusts every CA file a connect gave it until the process ends, so the [tls_first]
# case, which needs a database hop without the run's CA, runs in a process of its own.
trusted_before="$(lines "$dir/trusted.connections")"
REDSHIFT_TEST_TLS_DIR="$dir" env -u LD_LIBRARY_PATH "$suite" -w NoTests "[tls_first]" || status=$?
# The driver's text for each connect, for the cases that record rather than pin it.
[[ ! -f "$dir/records" ]] || { echo "tls-verification: the driver's records:"; cat "$dir/records"; }
((status == 0)) || exit "$status"
# A case that skips passes too, so the run passes only if the database cases reached the servers,
# and the authentication cases' lines reached the records printed above.
[[ -s "$dir/untrusted.connections" && -s "$dir/wrong_host.connections" && -s "$dir/trusted.startups" ]] ||
  fail "the [tls] cases did not reach the test servers"
(($(lines "$dir/trusted.connections") > trusted_before)) || fail "the [tls_first] case did not reach the test servers"
grep -q '^server: request=sspi ' "$dir/records" && grep -q '^server: request=gss ' "$dir/records" ||
  fail "the authentication cases recorded no server line"
