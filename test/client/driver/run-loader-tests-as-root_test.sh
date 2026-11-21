#!/usr/bin/env bash
# The mounts run-loader-tests-as-root.sh gives the root container: the tests' directory and the policy files,
# each at the path the build names it by (its CMakeCache's binary and source directories), which in a tree
# reached through a symbolic link is not its real path, and at its real path, all read-only. Runs the script in a
# copy against a stand-in docker that records its arguments. Needs no build, no docker and no network.
set -euo pipefail

here="$(CDPATH= cd -- "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
n=0; failed=0
not() { ! "$@"; }
check() {  # check <name> <command...>
  local name="$1"; shift; n=$((n+1))
  if "$@"; then echo "ok $n - $name"; else echo "not ok $n - $name"; failed=$((failed+1)); fi
}

tmp="$(mktemp -d)"
trap 'rm -rf -- "${tmp:?}"' EXIT
mkdir "$tmp/bin"
cat > "$tmp/bin/docker" <<'EOF'
#!/usr/bin/env bash
[[ "$1" == image ]] && exit 0
[[ "$1" == run ]] || exit 3
printf '%s\n' "$@" > "$DOCKER_ARGS_LOG"
EOF
chmod +x "$tmp/bin/docker"

suite=build/release/extension/redshift
policy=third_party/redshift_odbc/policy
# tree <dir> <configured-from>: a repository root holding the script, the policy directory and a built suite,
# whose CMakeCache names the source and binary directories under <configured-from>, as a build run from there
# does.
tree() {
  mkdir -p "$1/test/client/driver" "$1/$policy" "$1/$suite"
  cp "$here/run-loader-tests-as-root.sh" "$1/test/client/driver/"
  printf '#!/bin/sh\n' > "$1/$suite/redshift_driver_tests"
  chmod +x "$1/$suite/redshift_driver_tests"
  printf 'redshift_BINARY_DIR:STATIC=%s\nredshift_SOURCE_DIR:STATIC=%s\n' "$2/$suite" "$2" \
    > "$1/build/release/CMakeCache.txt"
}
# run <log> <dir>: the script from <dir>, as `cd <dir>` leaves the working directory.
run() {
  local log="$1" dir="$2"
  (cd "$dir" && PATH="$tmp/bin:$PATH" DOCKER_ARGS_LOG="$log" bash test/client/driver/run-loader-tests-as-root.sh)
}
# mounted <log> <src> <dst>: a read-only bind mount of <src> at <dst>.
mounted() { grep -qxF "type=bind,src=$2,dst=$3,readonly" "$1"; }
mounts() { grep -c '^type=bind,' "$1"; }
# same <log> <log>: the same arguments (cmp may be missing).
same() { [[ "$(cat "$1")" == "$(cat "$2")" ]]; }

mkdir -p "$tmp/real"
ln -s "$tmp/real" "$tmp/link"
real="$(realpath "$tmp/real")/pkg"
linked="$tmp/link/pkg"
tree "$real" "$linked"

run "$tmp/linked.log" "$linked"
check "through a link, the tests' directory is mounted at the path the build names" \
  mounted "$tmp/linked.log" "$real/$suite" "$linked/$suite"
check "through a link, the tests' directory is mounted at its real path too" \
  mounted "$tmp/linked.log" "$real/$suite" "$real/$suite"
check "through a link, the policy files are mounted at the path the build names" \
  mounted "$tmp/linked.log" "$real/$policy" "$linked/$policy"
check "through a link, the policy files are mounted at their real path too" \
  mounted "$tmp/linked.log" "$real/$policy" "$real/$policy"
check "through a link, the suite runs at the path the build names" grep -qxF "$linked/$suite/redshift_driver_tests" "$tmp/linked.log"
check "through a link, four mounts" test "$(mounts "$tmp/linked.log")" -eq 4

# The caller's working directory does not decide the path: the same build, run from its real path.
run "$tmp/from-real.log" "$real"
check "run from the real path, the mounts are the same" same "$tmp/linked.log" "$tmp/from-real.log"

# A build run from the real path: each directory is mounted once, since docker refuses two mounts at one
# destination.
mkdir -p "$tmp/direct"
direct="$(realpath "$tmp/direct")/pkg"
tree "$direct" "$direct"
run "$tmp/direct.log" "$direct"
check "a build run from the real path mounts the tests' directory once" \
  mounted "$tmp/direct.log" "$direct/$suite" "$direct/$suite"
check "a build run from the real path mounts the policy files once" \
  mounted "$tmp/direct.log" "$direct/$policy" "$direct/$policy"
check "a build run from the real path, two mounts" test "$(mounts "$tmp/direct.log")" -eq 2
check "the container has no network" bash -c 'grep -A1 -x -- "--network" "$1" | grep -qx none' _ "$tmp/direct.log"
check "no mount is writable" not grep -q '^type=bind,.*[^y]$' "$tmp/linked.log"

# A CMakeCache that names no binary directory is refused before docker runs.
: > "$direct/build/release/CMakeCache.txt"
rm -f "$tmp/none.log"
run "$tmp/none.log" "$direct" 2> "$tmp/none.err" && status=0 || status=$?
check "a CMakeCache that names no binary directory is refused before docker runs" \
  bash -c '[[ $1 != 0 && ! -e $2 ]] && grep -q "names no redshift_BINARY_DIR" "$3"' _ "$status" "$tmp/none.log" "$tmp/none.err"

((failed == 0)) || { echo "$failed of $n failed" >&2; exit 1; }
echo "all $n passed"
