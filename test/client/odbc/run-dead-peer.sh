#!/usr/bin/env bash
# Runs the dead-peer case (dead_peer_test.cpp) with its database connection through
# a relay container, and cuts the relay off the network once the case has read its first batch, so the
# server's packets stop arriving with no FIN or RST, as from a dead peer. Live: needs the live
# variables, DUCKDB_REDSHIFT_TEST_HOST and DUCKDB_REDSHIFT_TEST_ENDPOINT_URL (a test-endpoint build),
# an env file of AWS credentials, and Docker.
#
#   run-dead-peer.sh <checkout> <build-dir> <credentials-env-file>
#
# Runs both containers in IMAGE, an image with this build's runtime (the one the build ran in), on a
# network of their own; on exit it removes both containers and the network. Fails when the case did
# not run, or when it ran without the cut.
set -euo pipefail

checkout="$(cd "${1:?usage: run-dead-peer.sh <checkout> <build-dir> <credentials-env-file>}" && pwd -P)"
build_dir="${2:?build directory, relative to the checkout}"
[[ "$build_dir" != /* ]] || { echo "run-dead-peer: name the build directory relative to the checkout" >&2; exit 2; }
credentials="${3:?credentials env file}"
host="${DUCKDB_REDSHIFT_TEST_HOST:?the workgroup host the case connects to}"
: "${REDSHIFT_TEST_LIVE_REGION:?}" "${REDSHIFT_TEST_LIVE_WORKGROUP:?}" "${REDSHIFT_TEST_LIVE_ACCOUNT_ID:?}"
: "${DUCKDB_REDSHIFT_TEST_ENDPOINT_URL:?}"
[[ -n "${IMAGE:-}" ]] || { echo "run-dead-peer: set IMAGE to an image with this build's runtime" >&2; exit 2; }

# A random suffix: two runs in different PID namespaces on one Docker daemon can share a PID.
name="redshift-dead-peer-$$-$(od -An -N4 -tx4 /dev/urandom | tr -d ' ')"
signals="$build_dir/${name#redshift-}"
cleanup() {
  docker rm -f "$name-test" "$name" >/dev/null 2>&1 || true
  docker network rm "$name" >/dev/null 2>&1 || true
  rm -rf -- "${checkout:?}/${signals:?}"
}
trap cleanup EXIT
trap 'exit 1' INT TERM
mkdir -p "$checkout/$signals"
docker network create "$name" >/dev/null

# The relay: every connection to port 5439 is copied both ways to the workgroup's, byte for byte, so
# TLS still verifies the workgroup's own name.
relay='
import socket, sys, threading
def pump(source, sink):
    try:
        while True:
            data = source.recv(65536)
            if not data:
                break
            sink.sendall(data)
    finally:
        sink.close()
listener = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
listener.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
listener.bind(("0.0.0.0", 5439))
listener.listen(16)
while True:
    client, _ = listener.accept()
    upstream = socket.create_connection((sys.argv[1], 5439))
    threading.Thread(target=pump, args=(client, upstream), daemon=True).start()
    threading.Thread(target=pump, args=(upstream, client), daemon=True).start()
'
docker run -d --rm --pull never --name "$name" --network "$name" "$IMAGE" python3 -I -c "$relay" "$host" >/dev/null
address="$(docker inspect -f "{{(index .NetworkSettings.Networks \"$name\").IPAddress}}" "$name")"

# Bounded: keepalives end the case about 90 s after the cut; 600 s covers its connect and first batch.
docker run --rm --pull never --name "$name-test" --network "$name" --add-host "$host:$address" \
  -v "$checkout:/work" -u "$(id -u):$(id -g)" -e HOME=/tmp -w /work --env-file "$credentials" \
  -e REDSHIFT_TEST_LIVE_REGION -e REDSHIFT_TEST_LIVE_WORKGROUP -e REDSHIFT_TEST_LIVE_ACCOUNT_ID \
  -e DUCKDB_REDSHIFT_TEST_HOST -e DUCKDB_REDSHIFT_TEST_ENDPOINT_URL -e REDSHIFT_TEST_CLIENT=odbc \
  -e REDSHIFT_TEST_DEAD_PEER_SIGNALS="/work/$signals" "$IMAGE" timeout 600 env -u LD_LIBRARY_PATH \
  "$build_dir/extension/redshift/redshift_client_tests" -w NoTests "Live: a peer gone silent mid-result*" &
tester=$!

# Until the case asks for the cut or ends; its timeout bounds the wait, as a cold connect to the
# relay's one address can take about 135 s over three attempts before the first batch.
while kill -0 "$tester" 2>/dev/null; do
  if [[ -f "$checkout/$signals/cut" ]]; then
    docker network disconnect "$name" "$name"
    touch "$checkout/$signals/cut.done"
    break
  fi
  sleep 1
done
status=0
wait "$tester" || status=$?
[[ -f "$checkout/$signals/cut.done" ]] || { echo "run-dead-peer: the case never asked for the cut" >&2; exit 1; }
exit "$status"
