#!/usr/bin/env bash
# Runs the driver loader's tests (redshift_driver_tests, built by make test_driver) as root in a
# container, with no network. make test_driver runs them as the user, where root's own directories
# come from a user namespace; the few cases that need a second real user (a process that cannot
# write an administrator's root-owned set, a file chown'ed to another user) run only as root and
# warn and stop otherwise. This runs those.
#   test/client/driver/run-loader-tests-as-root.sh [build directory] [Catch2 test spec...]
# The image needs only the C and C++ runtime the tests link (libc, libstdc++, libgcc_s); the
# default is public Ubuntu 24.04, pinned by digest. REDSHIFT_ROOT_TEST_IMAGE names another. The
# image is never pulled here. The tests that load the embedded driver need libkeyutils.so.1 (the
# host prerequisite); an image without it warns and stops them, and the one that is missing it is the
# image the host-prerequisite case runs in. The container's root is disposable, which
# REDSHIFT_TEST_DISPOSABLE_ROOT=1 tells the one case that changes /etc/ld.so.conf.d and runs ldconfig
# (a library planted where the library cache points); no other run sets it. The case that writes next
# to the test executable stops, since that directory is mounted read-only.
set -euo pipefail

default_image="public.ecr.aws/docker/library/ubuntu:24.04@sha256:24737cef17f4e22b1977df7d9a05e77c4e28b7da5b8911f8d8ccda136bf3573c"
build="${1:-build/release}"
shift || true
image="${REDSHIFT_ROOT_TEST_IMAGE:-$default_image}"
[[ -x "$build/extension/redshift/redshift_driver_tests" ]] || {
	echo "run-loader-tests-as-root: no $build/extension/redshift/redshift_driver_tests; run make test_driver first" >&2
	exit 1
}
docker image inspect "$image" >/dev/null 2>&1 || {
	echo "run-loader-tests-as-root: the image $image is not here; fetch it with: docker pull $image" >&2
	exit 1
}
# The build names the tests' directory and the policy files by its own paths, from its CMakeCache: the binary
# directory, which driver_info.h's embedded files follow, and the policy files under the source directory, which
# sources.cmake resolves as the configure saw them. In a tree reached through a symbolic link (a linked home
# directory, say), or built in a container that mounts it at the linked path, those differ from the real paths
# here, and the caller's working directory may spell either. So each directory is mounted at the build's path and
# at its real path. The test inputs are beside the executable.
cached() {  # cached <name>: the build's CMakeCache value of <name>
	local value
	value="$(sed -n "s/^$1:STATIC=//p" "$build/CMakeCache.txt")"
	[[ -n "$value" ]] || {
		echo "run-loader-tests-as-root: $build/CMakeCache.txt names no $1" >&2
		exit 1
	}
	printf '%s\n' "$value"
}
mounts=()
mount_at() {  # mount_at <directory here> <the build's path for it>
	local real
	real="$(realpath "$1")"
	mounts+=(--mount "type=bind,src=$real,dst=$2,readonly")
	[[ "$real" == "$2" ]] || mounts+=(--mount "type=bind,src=$real,dst=$real,readonly")
}
binary_dir="$(cached redshift_BINARY_DIR)"
source_dir="$(cached redshift_SOURCE_DIR)"
mount_at "$build/extension/redshift" "$binary_dir"
mount_at "$(dirname "${BASH_SOURCE[0]}")/../../../third_party/redshift_odbc/policy" "$source_dir/third_party/redshift_odbc/policy"
(($# > 0)) || set -- '~[.]'
# As root whatever USER the image sets. driver_info.h names the embedded files at the build's path,
# which the embed tests read back.
exec docker run --rm --network none --pull=never -u 0:0 -e REDSHIFT_TEST_DISPOSABLE_ROOT=1 "${mounts[@]}" \
	"$image" "$binary_dir/redshift_driver_tests" -w NoTests "$@"
