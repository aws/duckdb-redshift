#!/usr/bin/env bash
# Build-level tests of the embedded driver's graph. A scratch CMake project
# includes src/transport/driver/sources.cmake, with stand-in targets named as the real ones, and is
# built with Unix Makefiles, which compares only time stamps, and with Ninja when it is on PATH:
# every target that reads the set depends on the one that owns the fetch, so a parallel build
# fetches once; an unchanged rebuild, and a configure again with the same lock, fetch and assemble
# nothing; a configure the build starts by itself keeps the configured lock; a deleted set file is
# fetched again from the kept copy; a build embeds the lock as the last configure read it, and
# switching to an older lock file, or copying one over the lock in use, fetches again at the next
# configure; a lock edited in place configures again and is checked; the identity file names the
# lock. A refused RS_DRIVER_LOCK leaves the cached lock and its copy as
# they were, a lock restored after a refused configure builds again, setting RS_DRIVER_LOCK to the
# checked-in lock goes back to it, and a cached lock that is gone is named with that way back. Then
# configuration refuses a bad RS_DRIVER_LOCK, a lock without the platform, and a compiler for
# another architecture, without glibc or without 64-bit pointers. The windows_amd64 branch
# configures with a stand-in resource compiler, its object linked where the set must reach. With
# REDSHIFT_DRIVER=none, sources.cmake adds only the placeholder, with no fetch, embed or identity
# file. Run by run-tool-tests.sh; needs no build of the extension. It skips without CMake 3.19 or a
# 64-bit glibc C compiler for the host's architecture, on a host that is not linux_amd64 or
# linux_arm64.
#
# The packages are stand-ins, as fetch-driver_test.sh's ELF cases use: a minimal RPM whose gzip
# payload holds a newc archive with a small ELF header for the host's architecture at the lock's
# path, served through RS_DRIVER_MIRROR, in locks that differ from the checked-in one only in the
# host platform's hashes. So the suite runs offline, and a stand-in curl fails any download.
set -euo pipefail

here="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
repo="$(cd "$here/../../.." && pwd)"

skip() { echo "ok 1 - embedded build graph # SKIP $1"; exit 0; }
command -v cmake >/dev/null || skip "cmake is not on PATH"
cmake_version="$(cmake --version | sed -n 's/^cmake version \([0-9]*\)\.\([0-9]*\).*/\1 \2/p')"
read -r cmake_major cmake_minor <<<"$cmake_version"
((${cmake_major:-0} > 3 || (${cmake_major:-0} == 3 && ${cmake_minor:-0} >= 19))) ||
  skip "the embed needs CMake 3.19 or newer (cmake_language DEFER), and cmake is ${cmake_major:-?}.${cmake_minor:-?}"
case "$(uname -s)/$(uname -m)" in
Linux/x86_64) platform=linux_amd64 machine=62 other=linux_arm64 other_processor=aarch64 arch=__x86_64__ ;;
Linux/aarch64) platform=linux_arm64 machine=183 other=linux_amd64 other_processor=x86_64 arch=__aarch64__ ;;
*) skip "only a linux_amd64 or linux_arm64 host builds the ELF embed" ;;
esac
# The compiler CMake takes, CC or the first of cc, gcc and clang, must target 64-bit glibc on the
# host's architecture, as sources.cmake's compile check requires; a musl host has no embed.
cc_cmd="${CC:-}"
if [[ -z "$cc_cmd" ]]; then
  for c in cc gcc clang; do if command -v "$c" >/dev/null; then cc_cmd="$c"; break; fi; done
fi
[[ -n "$cc_cmd" ]] && command -v "${cc_cmd%% *}" >/dev/null || skip "no C compiler is on PATH"
macros="$(printf '#include <features.h>\n' | $cc_cmd -dM -E -x c - 2>/dev/null)" || macros=""
for m in __GLIBC__ __LP64__ "$arch"; do
  grep -q "^#define $m " <<<"$macros" || skip "the C compiler ($cc_cmd) does not define $m, so the host has no ELF embed"
done

tmp="$(mktemp -d)"
trap 'rm -rf -- "${tmp:?}"' EXIT
tmp="$(cd "$tmp" && pwd -P)"
n=0; failed=0
pass() { n=$((n+1)); echo "ok $n - $1"; }
fail() { n=$((n+1)); echo "not ok $n - $1"; failed=$((failed+1)); }
sha() { python3 -I -c 'import hashlib,sys; print(hashlib.sha256(open(sys.argv[1],"rb").read()).hexdigest())' "$1"; }

# A stand-in curl records that it was called and fails: nothing here downloads.
mkdir -p "$tmp/bin" "$tmp/mirror"
printf '%s\n' '#!/usr/bin/env bash' "touch '$tmp/curl-called'" 'exit 99' > "$tmp/bin/curl"
chmod +x "$tmp/bin/curl"

base_lock="$repo/third_party/redshift_odbc/driver.lock"
row="$(bash "$repo/third_party/redshift_odbc/tools/check-lock.sh" "$base_lock" | awk -v p="$platform" '$1 == "driver" && $2 == p')"
read -r _ _ _ drv_sha _ _ pkg_sha path_in_pkg <<<"$row"
# stand_in <name>: lock $tmp/<name>.lock for a stand-in driver whose bytes name <name>; sets
# <name>_sha to the driver's SHA-256.
stand_in() {
  python3 -I -c 'import sys
sys.stdout.buffer.write(b"\x7fELF\x02\x01\x01" + bytes(9) + bytes([3, 0, int(sys.argv[1]), 0]) + sys.argv[2].encode() * 64)' \
    "$machine" "$1" > "$tmp/$1.so"
  python3 -I "$here/make_cpio.py" "$tmp/$1.cpio" "$path_in_pkg:file:1:$tmp/$1.so"
  python3 -I "$here/make_rpm.py" "$tmp/$1.rpm" AmazonRedshiftODBC-64-bit 2.3.0 0 "$(uname -m)" < "$tmp/$1.cpio"
  cp "$tmp/$1.rpm" "$tmp/mirror/$(sha "$tmp/$1.rpm")"
  sed "s/$drv_sha/$(sha "$tmp/$1.so")/; s/$pkg_sha/$(sha "$tmp/$1.rpm")/" "$base_lock" > "$tmp/$1.lock"
  printf -v "$1_sha" '%s' "$(sha "$tmp/$1.so")"
}
stand_in first
stand_in second
stand_in third
# Older than anything a build writes, so only the configure's copy of the lock, rewritten when its
# content changes, can start the fetch.
touch -d '2001-01-01 00:00:00' "$tmp/second.lock" "$tmp/third.lock"

# The scratch project: sources.cmake reads src/ and third_party/ from CMAKE_CURRENT_SOURCE_DIR, as
# in the extension's own directory. The stand-ins read the set as the real targets do: both
# extensions assemble the embed, and the test executable includes driver_info.h. It links nothing,
# so only sources.cmake's deferred dependency orders it after the fetch.
proj="$tmp/project"
mkdir -p "$proj"
ln -s "$repo/src" "$proj/src"
ln -s "$repo/third_party" "$proj/third_party"
cat > "$proj/CMakeLists.txt" <<'EOF'
cmake_minimum_required(VERSION 3.19)
project(embed_build_graph C)
set(REDSHIFT_DRIVER embedded)
set(EXTENSION_NAME redshift_extension)
set(LOADABLE_EXTENSION_NAME redshift_loadable_extension)
include(src/transport/driver/sources.cmake)
if(REDSHIFT_DRIVER_GEN_DIR)
  list(FILTER REDSHIFT_DRIVER_SOURCES INCLUDE REGEX "[.]S$")
  add_library(${EXTENSION_NAME} STATIC ${REDSHIFT_DRIVER_SOURCES})
  add_library(${LOADABLE_EXTENSION_NAME} STATIC ${REDSHIFT_DRIVER_SOURCES})
  add_executable(redshift_driver_tests tests.c)
  set_source_files_properties(tests.c PROPERTIES OBJECT_DEPENDS "${REDSHIFT_DRIVER_GEN_DIR}/driver_info.h"
                                                 INCLUDE_DIRECTORIES "${REDSHIFT_DRIVER_GEN_DIR}")
endif()
EOF
cat > "$proj/tests.c" <<'EOF'
#include "driver_info.h"
#define COUNT(name, size, sha256, stem) +1
int main(void) { return (0 REDSHIFT_DRIVER_FILES(COUNT)) == 4 ? 0 : 1; }
EOF

# Every run gets only the variables named here, so a caller's lock, mirror or make flags (make
# test_driver's jobserver) or resource compiler (RC) cannot change what is under test.
run_env() { env -u RS_DRIVER_LOCK -u RS_DRIVER_MIRROR -u MAKEFLAGS -u MFLAGS -u MAKELEVEL -u CMAKE_GENERATOR \
  -u RC PATH="$tmp/bin:$PATH" "$@"; }
# flat: standard input with CMake's line breaks inside a long message made spaces.
flat() { tr -s ' \n' ' '; }
# configure <build-dir> [VAR=value...] [-- <cmake arguments>]: configures with $generator from
# $tmp, where a relative RS_DRIVER_LOCK would name an existing lock; sets out (flat) and status.
configure() {
  local b="$1" e=() a=(); shift
  while (($#)); do if [[ "$1" == -- ]]; then shift; a=("$@"); break; fi; e+=("$1"); shift; done
  out="$(cd "$tmp" && run_env ${e[@]+"${e[@]}"} cmake -G "$generator" -S "$proj" -B "$b" ${a[@]+"${a[@]}"} 2>&1)" &&
    status=0 || status=$?
  out="$(printf '%s' "$out" | flat)"
}
# build [VAR=value...]: builds $b with 8 jobs, without RS_DRIVER_LOCK; sets out and status.
build() {
  local tool=(make -C "$b" -j8)
  [[ "$generator" == Ninja ]] && tool=(ninja -C "$b" -j8)
  rm -f "$tmp/curl-called"; out="$(run_env "$@" "${tool[@]}" 2>&1)" && status=0 || status=$?
}
fetches() { printf '%s\n' "$out" | grep -c "fetch-driver: $platform: the driver set (4 files) matches the lock" || true; }
records() { grep -q "\"$1\"" "$gen/driver_info.h"; }
# newer <file>...: those written since $tmp/marker; find compares the full time stamp, where bash
# 4.2's -nt compares whole seconds.
newer() { find "$@" -newer "$tmp/marker" -print 2>/dev/null || echo "(find failed)"; }
# next_second: waits until a file written now has a time stamp in a later second than the one it
# was called in, so that a lock changed next is newer in whole seconds than every file the last
# build wrote. Ninja before 1.9 (DuckDB's linux images install ninja-build 1.8.2) compares whole
# seconds, so it would take a lock changed within the second of a build for up to date. It reads
# a file's time stamp, not date's, because file times trail the clock by up to a kernel tick.
next_second() {
  local s; s="$(date +%s)"
  until touch "$tmp/clock" && (($(stat -c %Y "$tmp/clock") > s)); do sleep 0.1; done
}
# identity_names <lock>: the identity file records <lock> and the SHA-256 of the build's copy,
# which holds <lock>'s bytes.
identity_names() {
  grep -qx "lock $1" "$b/redshift_driver_identity.txt" &&
    grep -qx "sha256 $(sha "$b/redshift_driver.lock")" "$b/redshift_driver_identity.txt" &&
    [[ "$(sha "$b/redshift_driver.lock")" == "$(sha "$1")" ]]
}
# depends_on_set <target>: the target waits for redshift_driver_set in the graph the generator runs.
depends_on_set() {
  if [[ "$generator" == Ninja ]]; then
    ninja -C "$b" -t query "cmake_object_order_depends_target_$1" | grep -qx '    || redshift_driver_set'
  else grep -qx "CMakeFiles/$1.dir/all: CMakeFiles/redshift_driver_set.dir/all" "$b/CMakeFiles/Makefile2"; fi
}

# The build cases, in one build directory per generator, each named with the generator.
build_cases() {
  generator="$1"
  local g=" ($1)" d="$tmp/$1"
  mkdir -p "$d"; cp -p "$tmp/"{first,second,third}.lock "$d/"
  b="$d/build" gen="$d/build/redshift_driver"
  object="$b/CMakeFiles/redshift_extension.dir/src/transport/driver/embedded_driver_elf.S.o"
  configure "$b" "RS_DRIVER_LOCK=$d/first.lock"
  if [[ "$status" != 0 ]]; then fail "the scratch project configures$g (exit $status; $out)"; return; fi

  if depends_on_set redshift_extension && depends_on_set redshift_loadable_extension && depends_on_set redshift_driver_tests; then
    pass "the extension, loadable extension and test targets each depend on redshift_driver_set$g"
  else fail "the targets that read the set depend on redshift_driver_set$g"; fi
  build "RS_DRIVER_MIRROR=$tmp/mirror"
  if [[ "$status" == 0 && "$(fetches)" == 1 && -f "$object" ]] && records "$first_sha" && [[ ! -e "$tmp/curl-called" ]] &&
    identity_names "$d/first.lock"; then
    pass "a parallel build fetches the set once and assembles it, and the identity file names the lock$g"
  else fail "a parallel build fetches once$g (exit $status; $(fetches) fetches; $out)"; fi

  touch "$tmp/marker"
  build
  local changed; changed="$(newer "$gen" "$object")"
  if [[ "$status" == 0 && "$(fetches)" == 0 && -z "$changed" && "$out" != *"Building ASM object"* &&
    ! -e "$tmp/curl-called" ]]; then
    pass "an unchanged rebuild fetches, assembles and rewrites nothing$g"
  else fail "unchanged rebuild$g (exit $status; $(fetches) fetches; rewritten: $changed; $out)"; fi
  # Every make target configures first (make release), so the same lock must cost nothing.
  configure "$b" "RS_DRIVER_LOCK=$d/first.lock"
  build
  changed="$(newer "$gen" "$object" "$b/redshift_driver.lock" "$b/redshift_driver_identity.txt")"
  if [[ "$status" == 0 && "$(fetches)" == 0 && -z "$changed" && "$out" != *"Building ASM object"* ]]; then
    pass "configuring again with the same lock rewrites neither the lock's copy nor the identity file, and fetches nothing$g"
  else fail "configure again with the same lock$g (exit $status; $(fetches) fetches; rewritten: $changed; $out)"; fi
  # A newer lock makes the build configure again by itself, without RS_DRIVER_LOCK: the cache keeps
  # the lock it was configured with, and its unchanged content fetches nothing. The checked-in
  # lock's package is in no mirror, so falling back to it would fail on the stand-in curl.
  next_second
  touch "$d/first.lock"
  build
  if [[ "$status" == 0 && "$(printf '%s' "$out" | flat)" == *"embedding the locked driver for $platform from $d/first.lock"* &&
    "$(fetches)" == 0 ]] && records "$first_sha" && identity_names "$d/first.lock"; then
    pass "a configure the build starts by itself, without RS_DRIVER_LOCK, keeps the configured lock$g"
  else fail "a configure the build starts keeps the lock$g (exit $status; $(fetches) fetches; $out)"; fi

  # Removing a set file fetches the set again from the package the first fetch kept, with no
  # mirror and no download: a notice, then the driver; the embed object is assembled again.
  rm -f "$gen/NOTICE" "$object"
  build
  if [[ "$status" == 0 && "$(fetches)" == 1 && "$out" == *"package from the copy kept in $gen/packages"* && -f "$object" &&
    "$(sha "$gen/NOTICE")" == "$(sha "$repo/third_party/redshift_odbc/NOTICE")" && ! -e "$tmp/curl-called" ]]; then
    pass "a deleted notice is fetched again from the kept copy and the set assembled again$g"
  else fail "deleted notice$g (exit $status; $(fetches) fetches; $out)"; fi
  rm -f "$gen/librsodbc64.so"
  build
  if [[ "$status" == 0 && "$(fetches)" == 1 && "$out" == *"package from the copy kept in $gen/packages"* &&
    "$(sha "$gen/librsodbc64.so" 2>/dev/null)" == "$first_sha" && ! -e "$tmp/curl-called" ]]; then
    pass "a deleted driver is fetched again from the kept copy$g"
  else fail "deleted driver$g (exit $status; $(fetches) fetches; $out)"; fi
  # A deleted copy of the lock: the build configures again by itself and recreates it from the
  # configured lock.
  rm -f "$b/redshift_driver.lock"
  build
  if [[ "$status" == 0 && "$(sha "$gen/librsodbc64.so" 2>/dev/null)" == "$first_sha" && ! -e "$tmp/curl-called" ]] &&
    identity_names "$d/first.lock"; then
    pass "a deleted copy of the lock is recreated from the configured lock by a plain build$g"
  else fail "deleted lock copy$g (exit $status; $out)"; fi

  next_second
  touch "$tmp/marker"
  configure "$b" "RS_DRIVER_LOCK=$d/second.lock"
  build "RS_DRIVER_MIRROR=$tmp/mirror"
  if [[ "$status" == 0 && "$(fetches)" == 1 && "$(newer "$object")" == "$object" ]] && records "$second_sha" &&
    ! records "$first_sha" && identity_names "$d/second.lock"; then
    pass "switching RS_DRIVER_LOCK to an older lock file fetches and assembles that lock's driver$g"
  else fail "lock switch$g (exit $status; $(fetches) fetches; $out)"; fi
  # Another lock copied over the one in use with an older time stamp starts no configure. The build
  # embeds the lock as the last configure read and checked it, even when it fetches again.
  cp -p "$d/second.lock" "$d/current.lock"
  configure "$b" "RS_DRIVER_LOCK=$d/current.lock"
  build "RS_DRIVER_MIRROR=$tmp/mirror"
  cp -p "$d/third.lock" "$d/current.lock"
  rm -f "$gen/librsodbc64.so"
  build "RS_DRIVER_MIRROR=$tmp/mirror"
  if [[ "$status" == 0 && "$(fetches)" == 1 ]] && records "$second_sha" && ! records "$third_sha"; then
    pass "a build that does not configure embeds the configured lock, not an older one copied over it, even when it fetches$g"
  else fail "a lock copied over the one in use, no configure$g (exit $status; $(fetches) fetches; $out)"; fi
  next_second
  touch "$tmp/marker"
  configure "$b" "RS_DRIVER_LOCK=$d/current.lock"
  build "RS_DRIVER_MIRROR=$tmp/mirror"
  if [[ "$status" == 0 && "$(fetches)" == 1 && "$(newer "$object")" == "$object" ]] && records "$third_sha" &&
    ! records "$second_sha" && identity_names "$d/current.lock"; then
    pass "the next configure fetches and assembles the lock copied over the one in use$g"
  else fail "a lock copied over the one in use, then configure$g (exit $status; $(fetches) fetches; $out)"; fi
  # Edited in place, so newer than the build files: the build configures again, which checks it.
  next_second
  grep -v "^$platform " "$d/third.lock" > "$d/current.lock"
  build "RS_DRIVER_MIRROR=$tmp/mirror"
  if [[ "$status" != 0 && "$(printf '%s' "$out" | flat)" == *"the driver lock $d/current.lock has no driver row for $platform"* ]]; then
    pass "a lock edited in place configures again at the next build, which refuses it$g"
  else fail "lock edited in place$g (exit $status; $out)"; fi
}
build_cases "Unix Makefiles"
if command -v ninja >/dev/null; then build_cases Ninja; else echo "# ninja is not on PATH; the build cases ran with Unix Makefiles only"; fi

# The lock a build directory keeps in its cache (REDSHIFT_DRIVER_LOCK), with Unix Makefiles: a
# refused candidate changes nothing, a lock restored after a refused configure builds again, the
# checked-in lock's path goes back to it, and a cached lock that is gone is named, with the way out.
generator="Unix Makefiles"
checked_in="$proj/third_party/redshift_odbc/driver.lock" # as the scratch project names it
hint="set RS_DRIVER_LOCK to another lock, or to $checked_in for the checked-in one"
c="$tmp/cached"; b="$c/build" gen="$c/build/redshift_driver"
mkdir -p "$c"; cp -p "$tmp/first.lock" "$c/current.lock"
printf 'not a lock\n' > "$tmp/junk.lock"
grep -v "^$platform " "$tmp/first.lock" > "$tmp/no-row.lock"
# keeps_current: the cache, the build's copy and the identity file still name current.lock as it was.
keeps_current() {
  grep -qx "REDSHIFT_DRIVER_LOCK:FILEPATH=$c/current.lock" "$b/CMakeCache.txt" &&
    [[ "$(sha "$b/redshift_driver.lock")" == "$first_sha_lock" ]] && identity_names "$c/current.lock"
}
first_sha_lock="$(sha "$tmp/first.lock")"
configure "$b" "RS_DRIVER_LOCK=$c/current.lock"
build "RS_DRIVER_MIRROR=$tmp/mirror"
configure "$b" "RS_DRIVER_LOCK=$tmp/junk.lock"; junk_status="$status" junk_out="$out"
configure "$b" "RS_DRIVER_LOCK=$tmp/no-row.lock"; no_row_status="$status" no_row_out="$out"
if [[ "$junk_status" != 0 && "$junk_out" == *"fails check-lock.sh; $hint"* && "$no_row_status" != 0 &&
  "$no_row_out" == *"has no driver row for $platform; $hint"* ]] && keeps_current; then
  configure "$b"
  if [[ "$status" == 0 && "$out" == *"from $c/current.lock"* ]]; then build; fi
fi
if [[ "$status" == 0 && "$(fetches)" == 0 ]] && records "$first_sha" && keeps_current; then
  pass "a refused RS_DRIVER_LOCK, naming the way back, leaves the cache, the copy and the identity file as they were"
else fail "a refused RS_DRIVER_LOCK leaves the build directory's lock (exit $status; $junk_out $no_row_out $out)"; fi
# The lock in use refused in place, then restored with its old time stamp: no configure follows,
# and the build must still embed it rather than the refused bytes.
cp -p "$c/current.lock" "$tmp/current.saved"
printf 'not a lock\n' > "$c/current.lock"
build
refused_build="$status $(printf '%s' "$out" | flat)"
cp -p "$tmp/current.saved" "$c/current.lock"
build
if [[ "$refused_build" == [1-9]*"the driver lock $c/current.lock fails check-lock.sh"* && "$status" == 0 &&
  "$(fetches)" == 0 ]] && records "$first_sha" && keeps_current; then
  pass "after a configure that refused the lock in use, the restored lock builds again without a configure"
else fail "a lock restored after a refused configure (refused build: $refused_build; exit $status; $out)"; fi
configure "$b" "RS_DRIVER_LOCK=$checked_in"
back_status="$status" back_out="$out"
configure "$b"
if [[ "$back_status" == 0 && "$back_out" == *"from $checked_in"* && "$status" == 0 && "$out" == *"from $checked_in"* ]] &&
  grep -qx "REDSHIFT_DRIVER_LOCK:FILEPATH=$checked_in" "$b/CMakeCache.txt" && identity_names "$checked_in"; then
  pass "RS_DRIVER_LOCK set to the checked-in lock's path goes back to it, and later configures keep it"
else fail "the way back to the checked-in lock (exit $back_status, $status; $back_out $out)"; fi
cp -p "$tmp/first.lock" "$c/gone.lock"
configure "$b" "RS_DRIVER_LOCK=$c/gone.lock"
rm -f "$c/gone.lock"
build
if [[ "$status" != 0 && "$(printf '%s' "$out" | flat)" == *"this build directory embeds the lock '$c/gone.lock' (REDSHIFT_DRIVER_LOCK), which must be an existing file named by absolute path; $hint"* ]]; then
  pass "a build whose cached lock is gone fails, naming it and the way back"
else fail "a cached lock that is gone (exit $status; $out)"; fi

# refused <name> <message> [VAR=value...] [-- <cmake arguments>]: a fresh configure fails with
# <message> and leaves nothing fetched.
refused() {
  local name="$1" message="$2"; shift 2
  configure "$tmp/refused" "$@"
  if [[ "$status" != 0 && "$out" == *"$message"* && ! -e "$tmp/refused/redshift_driver/driver_info.h" ]]; then pass "$name"
  else fail "$name (exit $status; $out)"; fi
  rm -rf "${tmp:?}/refused"
}
refused "configuration refuses a relative RS_DRIVER_LOCK" \
  "RS_DRIVER_LOCK must name an existing lock by absolute path, not 'first.lock'" "RS_DRIVER_LOCK=first.lock"
refused "configuration refuses an RS_DRIVER_LOCK that does not exist" \
  "RS_DRIVER_LOCK must name an existing lock by absolute path, not '$tmp/missing.lock'" "RS_DRIVER_LOCK=$tmp/missing.lock"
refused "configuration refuses a lock that fails check-lock.sh" "the driver lock $tmp/junk.lock fails check-lock.sh" \
  "RS_DRIVER_LOCK=$tmp/junk.lock"
refused "configuration refuses a lock with no driver row for the platform" \
  "the driver lock $tmp/no-row.lock has no driver row for $platform" "RS_DRIVER_LOCK=$tmp/no-row.lock"
refused "configuration refuses a platform whose architecture the C compiler does not target" \
  "the platform is $other, but the C compiler does not target 64-bit glibc Linux" \
  "RS_DRIVER_LOCK=$tmp/first.lock" -- "-DDUCKDB_EXPLICIT_PLATFORM=$other"
# Without DUCKDB_EXPLICIT_PLATFORM, the other Linux processor's name is derived as DuckDB's, so the
# compile check names it.
refused "without DUCKDB_EXPLICIT_PLATFORM, a Linux $other_processor target is $other, which the C compiler does not target" \
  "the platform is $other, but the C compiler does not target 64-bit glibc Linux" \
  "RS_DRIVER_LOCK=$tmp/first.lock" -- -DCMAKE_SYSTEM_NAME=Linux "-DCMAKE_SYSTEM_PROCESSOR=$other_processor"
# Stand-in compilers for the host's architecture: one whose <features.h> defines nothing, as a
# libc other than glibc's would not define __GLIBC__, and one without __LP64__.
mkdir -p "$tmp/no-glibc"
echo '/* not glibc */' > "$tmp/no-glibc/features.h"
echo '#undef __LP64__' > "$tmp/no-lp64.h"
printf '#!/bin/sh\nexec %s -isystem %s "$@"\n' "$cc_cmd" "$tmp/no-glibc" > "$tmp/bin/cc-no-glibc"
printf '#!/bin/sh\nexec %s -include %s "$@"\n' "$cc_cmd" "$tmp/no-lp64.h" > "$tmp/bin/cc-no-lp64"
chmod +x "$tmp/bin/cc-no-glibc" "$tmp/bin/cc-no-lp64"
# The whole message once, with Linux's remedy.
refused "configuration refuses a C compiler that does not target glibc, saying to set DUCKDB_PLATFORM" \
  "the platform is $platform, but the C compiler does not target 64-bit glibc Linux on $arch; set DUCKDB_PLATFORM to the target's DuckDB platform" \
  "RS_DRIVER_LOCK=$tmp/first.lock" "CC=$tmp/bin/cc-no-glibc"
refused "configuration refuses a C compiler without 64-bit pointers (__LP64__)" \
  "the platform is $platform, but the C compiler does not target 64-bit glibc Linux" \
  "RS_DRIVER_LOCK=$tmp/first.lock" "CC=$tmp/bin/cc-no-lp64"

# Without DUCKDB_EXPLICIT_PLATFORM the platform is composed from CMake's target system and
# processor, with DuckDB's names for the four systems and two processors mapped; SunOS and riscv64
# are not, so they are kept as CMake names them. A project with no languages sets them after
# project(), as a toolchain file would before it. None of the other targets has an embed, so none
# needs a compiler or fetches; the macOS ones and windows_amd64 do, and their compile check is given
# a failed result in the cache, so each stops there, on any host, with the platform it derived and
# its remedies in the message (for macOS the platform's one architecture, or another DUCKDB_PLATFORM).
# windows_amd64 pins CMake 3.21, so the PE embed's version guard never stops it first on an older host.
names="$tmp/names"; mkdir -p "$names"
ln -s "$repo/src" "$names/src"; ln -s "$repo/third_party" "$names/third_party"
cat > "$names/CMakeLists.txt" <<'EOF'
cmake_minimum_required(VERSION 3.19)
project(platform_names NONE)
set(CMAKE_SYSTEM_NAME "${TEST_SYSTEM}")
set(CMAKE_SYSTEM_PROCESSOR "${TEST_PROCESSOR}")
set(CMAKE_OSX_ARCHITECTURES "${TEST_OSX_ARCHITECTURES}")
set(REDSHIFT_DRIVER embedded)
if(DEFINED TEST_CMAKE_VERSION)
  set(CMAKE_VERSION "${TEST_CMAKE_VERSION}")
endif()
include(src/transport/driver/sources.cmake)
EOF
misnamed=""
while read -r system processor osx_archs want; do
  rm -rf "${names:?}/build"
  if [[ "$system" == Darwin ]]; then
    archs="${osx_archs#-}"; osx_arch=x86_64
    [[ "$want" != osx_arm64 ]] || osx_arch=arm64
    names_out="$(run_env cmake -G "Unix Makefiles" -S "$names" -B "$names/build" "-DTEST_SYSTEM=$system" \
      "-DTEST_PROCESSOR=$processor" "-DTEST_OSX_ARCHITECTURES=$archs" "-DREDSHIFT_DRIVER_TARGETS_${want}_${archs//;/_}=0" 2>&1)" &&
      misnamed+="$system $processor $osx_archs: configured; " ||
      [[ "$(printf '%s' "$names_out" | flat)" == *"the platform is $want, but the C compiler does not target 64-bit macOS on __${osx_arch}__; build one architecture, with OSX_BUILD_ARCH=$osx_arch, or set DUCKDB_PLATFORM to the target's DuckDB platform"* ]] ||
      misnamed+="$system $processor $osx_archs: $(printf '%s' "$names_out" | flat); "
    continue
  fi
  if [[ "$want" == windows_amd64 ]]; then
    names_out="$(run_env cmake -G "Unix Makefiles" -S "$names" -B "$names/build" "-DTEST_SYSTEM=$system" \
      "-DTEST_PROCESSOR=$processor" -DREDSHIFT_DRIVER_TARGETS_windows_amd64=0 -DTEST_CMAKE_VERSION=3.21.0 2>&1)" &&
      misnamed+="$system $processor: configured; " ||
      [[ "$(printf '%s' "$names_out" | flat)" == *"the platform is windows_amd64, but the C compiler does not target 64-bit MSVC Windows on _M_X64; set DUCKDB_PLATFORM to the target's DuckDB platform"* ]] ||
      misnamed+="$system $processor: $(printf '%s' "$names_out" | flat); "
    continue
  fi
  names_out="$(run_env cmake -G "Unix Makefiles" -S "$names" -B "$names/build" "-DTEST_SYSTEM=$system" \
    "-DTEST_PROCESSOR=$processor" "-DTEST_OSX_ARCHITECTURES=${osx_archs#-}" 2>&1)" &&
    grep -qx "platform $want" "$names/build/redshift_driver_identity.txt" && ! grep -rq fetch-driver "$names/build" ||
    misnamed+="$system $processor $osx_archs: $(sed -n 's/^platform //p' "$names/build/redshift_driver_identity.txt" 2>/dev/null) $(printf '%s' "$names_out" | flat); "
done <<'EOF'
Darwin arm64 - osx_arm64
Darwin x86_64 - osx_amd64
Darwin arm64 x86_64 osx_amd64
Darwin x86_64 arm64;x86_64 osx_amd64
Windows AMD64 - windows_amd64
Windows ARM64 - windows_arm64
FreeBSD amd64 - freebsd_amd64
Linux riscv64 - linux_riscv64
SunOS amd64 - SunOS_amd64
SunOS riscv64 - SunOS_riscv64
EOF
if [[ -z "$misnamed" ]]; then
  pass "without DUCKDB_EXPLICIT_PLATFORM, mapped targets get DuckDB's names (osx_arm64, windows_amd64, freebsd_amd64, ...), and any other system or processor is kept; windows_amd64 has the PE embed's compile check"
else fail "platform names without DUCKDB_EXPLICIT_PLATFORM ($misnamed)"; fi
# The PE embed needs CMake 3.21, the ELF and Mach-O ones 3.19: CMAKE_VERSION is set in the scratch
# project, so the guard is checked whatever cmake runs it.
for version in 3.20.0 3.21.0; do
  rm -rf "${names:?}/build"
  names_out="$(run_env cmake -G "Unix Makefiles" -S "$names" -B "$names/build" \
    -DTEST_SYSTEM=Windows \
    -DTEST_PROCESSOR=AMD64 -DREDSHIFT_DRIVER_TARGETS_windows_amd64=0 \
      "-DTEST_CMAKE_VERSION=$version" 2>&1 |
    flat)" || true
  case "$version" in
  3.20.0) want="embedding the driver for Windows needs CMake 3.21 or newer" ;;
  *) want="the platform is windows_amd64, but the C compiler does not target 64-bit MSVC Windows" ;;
  esac
  if [[ "$names_out" == *"$want"* ]]; then pass \
    "with CMake $version, a windows_amd64 configure stops at: $want"
  else fail "with CMake $version, a windows_amd64 configure stops at: $want (got: $names_out)"; fi
done
# The compile check itself, with the host's C compiler standing in: a MinGW compiler defines _WIN64
# and _M_X64 but not _MSC_VER, and is refused; with _MSC_VER too, the configure passes the check and
# stops later, at the resource compiler this host lacks.
mingw="$tmp/mingw"; mkdir -p "$mingw"
ln -s "$repo/src" "$mingw/src"; ln -s "$repo/third_party" "$mingw/third_party"
sed -e 's/^project(platform_names NONE)$/project(compile_check C)/' "$names/CMakeLists.txt" > "$mingw/CMakeLists.txt"
for defines in "-D_WIN64 -D_M_X64" "-D_WIN64 -D_M_X64 -D_MSC_VER=1930"; do
  rm -rf "${mingw:?}/build"
  names_out="$(run_env cmake -G "Unix Makefiles" -S "$mingw" -B "$mingw/build" -DTEST_SYSTEM=Windows \
    -DTEST_PROCESSOR=AMD64 -DTEST_CMAKE_VERSION=3.21.0 "-DCMAKE_C_FLAGS=$defines" 2>&1 | flat)" || true
  refused="the C compiler does not target 64-bit MSVC Windows on _M_X64"
  if [[ "$defines" != *_MSC_VER* ]]; then
    name="a compiler that defines _WIN64 and _M_X64 but not _MSC_VER (MinGW) is refused for windows_amd64"
    if [[ "$names_out" == *"$refused"* ]]; then pass "$name"; else fail "$name (got: $names_out)"; fi
  else
    name="a compiler that also defines _MSC_VER passes the windows_amd64 compile check"
    if [[ "$names_out" != *"$refused"* && "$names_out" == *"No CMAKE_RC_COMPILER could be found"* ]]; then
      pass "$name"
    else fail "$name (got: $names_out)"; fi
  fi
done
rm -rf "${names:?}/build"
names_out="$(run_env cmake -G "Unix Makefiles" -S "$names" -B "$names/build" -DTEST_SYSTEM=Linux \
  -DTEST_PROCESSOR=riscv64 -DTEST_CMAKE_VERSION=3.20.0 2>&1 | flat)" && status=0 || status=$?
if [[ "$status" == 0 && "$names_out" != *"3.21"* ]]; then pass \
  "with CMake 3.20, a platform without the PE embed configures"
else fail \
  "with CMake 3.20, a platform without the PE embed configures (exit $status; $names_out)"; fi
# An ELF embed passes both version guards with CMake 3.20 and stops at its compile check.
rm -rf "${names:?}/build"
names_out="$(run_env cmake -G "Unix Makefiles" -S "$names" -B "$names/build" -DTEST_SYSTEM=Linux \
  -DTEST_PROCESSOR=x86_64 -DREDSHIFT_DRIVER_TARGETS_linux_amd64=0 -DTEST_CMAKE_VERSION=3.20.0 2>&1 |
  flat)" || true
if [[ "$names_out" == *"the platform is linux_amd64, but the C compiler does not target 64-bit glibc Linux"* &&
  "$names_out" != *"3.21"* ]]; then pass "with CMake 3.20, a linux_amd64 embed passes the version guards"
else fail "with CMake 3.20, a linux_amd64 embed passes the version guards (got: $names_out)"; fi

# The Windows set's outputs, as fetch-driver.sh writes them: the libraries after the notices, and a
# renamed file under its embedded name, so a wrong name would make the build fetch at every run.
set_files() {  # set_files <lock line>...: the list redshift_windows_set_files makes, on one line
  local lines="" l
  for l in "$@"; do lines+="\"$l\" "; done
  cat > "$tmp/set-files.cmake" <<EOF
include("$repo/src/transport/driver/set_files.cmake")
set(files /g/rsodbc.dll /g/LICENSE)
set(lines $lines)
redshift_windows_set_files(files windows_amd64 /g "\${lines}")
message("\${files}")
EOF
  cmake -P "$tmp/set-files.cmake" 2>&1
}
renamed="$(set_files "driver windows_amd64 rsodbc.dll h driver" \
  "library windows_amd64 crypto.dll h library_crypto_dll" \
  "library windows_amd64 ssl.dll h library_ssl_dll" \
    "rename windows_amd64 crypto.dll abcdefghijk23456.dll" \
  "rename windows_amd64 rsodbc.dll bcdefghijk234567.dll" "notice LICENSE h notice_license" \
  "library linux_amd64 other.so h x")"
if [[ "$renamed" == "/g/bcdefghijk234567.dll;/g/LICENSE;/g/abcdefghijk23456.dll;/g/ssl.dll" ]]; then
  pass "the Windows set's outputs: libraries after the notices, a renamed file under its embedded name"
else fail \
  "the Windows set's outputs: libraries after the notices, a renamed file under its embedded name (got: $renamed)"; fi
plain="$(set_files "library windows_amd64 crypto.dll h library_crypto_dll")"
if [[ "$plain" == "/g/rsodbc.dll;/g/LICENSE;/g/crypto.dll" ]]; then
  pass "without rename rows, the Windows set's outputs keep the MSI's names"
else fail "without rename rows, the Windows set's outputs keep the MSI's names (got: $plain)"; fi

# The pe branch, configured here with its compile check answered and /bin/true as the resource
# compiler (the checked-in lock has windows_amd64 rows; the PE embed needs CMake 3.21). Stand-in
# targets take the deferred calls: the loadable extension and an executable that links the static
# one must each link the resource object, or a Windows image would carry no set.
if ((cmake_major > 3 || cmake_minor >= 21)); then
  pe="$tmp/pe"; mkdir -p "$pe"
  ln -s "$repo/src" "$pe/src"; ln -s "$repo/third_party" "$pe/third_party"
  cat > "$pe/CMakeLists.txt" <<'EOF'
cmake_minimum_required(VERSION 3.21)
project(pe_branch C)
set(CMAKE_SYSTEM_NAME Windows)
set(CMAKE_SYSTEM_PROCESSOR AMD64)
set(REDSHIFT_CLIENT odbc CACHE STRING "")
set(REDSHIFT_DRIVER embedded CACHE STRING "")
set(EXTENSION_NAME redshift_extension)
set(LOADABLE_EXTENSION_NAME redshift_loadable_extension)
set(Python3_EXECUTABLE python3)
include(src/transport/driver/sources.cmake)
add_library(${EXTENSION_NAME} STATIC consumer.c)
add_library(${LOADABLE_EXTENSION_NAME} SHARED consumer.c)
add_executable(redshift_driver_tests consumer.c)
target_link_libraries(redshift_driver_tests ${EXTENSION_NAME})
EOF
  echo 'int main(void) { return 0; }' > "$pe/consumer.c"
  # Outside the source folder, as build_cases builds: CMake 3.21 and 3.22 write the DEPENDS of a
  # build inside its source tree as relative paths, which the checks below would not match.
  pe_build="$tmp/pe-build"
  pe_out="$(run_env cmake -G "Unix Makefiles" -S "$pe" -B "$pe_build" -DREDSHIFT_DRIVER_TARGETS_windows_amd64=1 \
    -DCMAKE_RC_COMPILER=/bin/true 2>&1)" && pe_status=0 || pe_status=$?
  name="the windows_amd64 branch configures on Linux with a stand-in resource compiler"
  if [[ "$pe_status" == 0 && "$pe_out" == *"embedding the locked driver for windows_amd64"* ]]; then pass "$name"
  else fail "$name (exit $pe_status; $(printf '%s' "$pe_out" | flat))"; fi
  dirs="$pe_build/CMakeFiles"
  script="driver_info.py resources $pe/src/transport/driver/embedded_driver_pe.rc.in"
  script+=" $pe_build/redshift_driver/driver_info.h $pe_build/embedded_driver_pe.rc"
  rules="$dirs/redshift_driver_resources.dir/build.make"
  name="the resource script is written from the template and driver_info.h, in that order, again when either or driver_info.py changes"
  if grep -qF -- "$script" "$rules" && grep -qxF "embedded_driver_pe.rc: redshift_driver/driver_info.h" "$rules" &&
    grep -qxF "embedded_driver_pe.rc: $pe/src/transport/driver/embedded_driver_pe.rc.in" "$rules" &&
    grep -qxF "embedded_driver_pe.rc: $pe/third_party/redshift_odbc/tools/lib/driver_info.py" "$rules"; then pass "$name"
  else fail "$name (no '$script' with those DEPENDS and driver_info.py's in the resources' build.make)"; fi
  name="the resources wait for the fetch, as the targets that read the set do"
  if (b="$pe_build" generator="Unix Makefiles"; depends_on_set redshift_driver_resources); then pass "$name"
  else fail "$name"; fi
  object="CMakeFiles/redshift_driver_resources.dir/embedded_driver_pe.rc.res"
  # The set's files as the checked-in lock embeds them: each driver or library row's file under its
  # rename row's name, and the notices.
  set_names="$(awk '$1 == "windows_amd64" && ($2 == "driver" || $2 == "library") { f[$3] = $3 }
    $1 == "windows_amd64" && $2 == "rename" { r[$3] = $4 } $2 == "notice" { print $3 }
    END { for (k in f) print (k in r ? r[k] : k) }' "$repo/third_party/redshift_odbc/driver.lock")"
  missing=""
  for input in driver_info.h $set_names; do
    grep -qxF "$object: redshift_driver/$input" "$rules" || missing+=" $input"
  done
  name="the resource object is compiled again when driver_info.h or a set file, under its embedded name, changes"
  count="$(wc -l <<<"$set_names")"
  if [[ -z "$missing" && "$count" -gt 4 ]]; then pass "$name"
  elif [[ -n "$missing" ]]; then fail "$name (not in its OBJECT_DEPENDS:$missing)"
  else fail "$name (the lock gives $count windows_amd64 set files and notices, too few to be the set)"; fi
  name="the resource object is on the loadable extension's link line and a static consumer's"
  if grep -qF -- "$object" "$dirs/redshift_loadable_extension.dir/link.txt" &&
    grep -qF -- "$object" "$dirs/redshift_driver_tests.dir/link.txt"; then pass "$name"
  else fail "$name ($(cat "$dirs"/*/link.txt 2>&1 | flat))"; fi
  missing=""
  for input in tools/lib/msi.py tools/lib/cfb.py tools/lib/cab.py tools/lib/pe.py tools/lib/rename_apply.py \
    policy/windows/runtime-names.txt; do
    grep -qxF "redshift_driver/driver_info.h: $pe/third_party/redshift_odbc/$input" \
      "$dirs/redshift_driver_set.dir/build.make" || missing+=" $input"
  done
  name="the Windows fetch runs again when a reader it loads or a policy file it reads changes"
  if [[ -z "$missing" ]]; then pass "$name"; else fail "$name (not in its DEPENDS:$missing)"; fi
  name="check-build-graph-offline.sh finds the Windows graph's fetch and embed steps"
  if pe_out="$(bash "$repo/test/client/odbc/check-build-graph-offline.sh" --expect-embedded "$pe_build" 2>&1)"; then
    pass "$name"
  else fail "$name ($pe_out)"; fi
else
  pass "the windows_amd64 branch configures on Linux # SKIP cmake ${cmake_major}.${cmake_minor} is older than 3.21"
fi

# A platform with no ELF embed links the placeholder: its graph has no fetch, and the identity
# file still records the platform for embedded-build_test.sh and make test_driver.
configure "$tmp/musl" "RS_DRIVER_LOCK=$tmp/first.lock" -- "-DDUCKDB_EXPLICIT_PLATFORM=${platform}_musl"
if [[ "$status" == 0 && "$out" == *"no embedded driver for ${platform}_musl"* ]] &&
  grep -qx "platform ${platform}_musl" "$tmp/musl/redshift_driver_identity.txt" && ! grep -rq fetch-driver "$tmp/musl"; then
  pass "a platform with no ELF embed configures with no fetch and records its platform"
else fail "placeholder platform (exit $status; $out)"; fi

# With REDSHIFT_DRIVER=none (a build without the embedded driver), the driver sources.cmake adds the
# placeholder alone and reads no platform, so it adds no fetch, embed or identity file for any.
none="$tmp/none"; mkdir -p "$none"
ln -s "$repo/src" "$none/src"; ln -s "$repo/third_party" "$none/third_party"
cat > "$none/CMakeLists.txt" <<'EOF'
cmake_minimum_required(VERSION 3.19)
project(no_driver NONE)
set(REDSHIFT_DRIVER none)
include(src/transport/driver/sources.cmake)
file(WRITE "${CMAKE_BINARY_DIR}/driver_sources.txt" "${REDSHIFT_DRIVER_SOURCES}|${REDSHIFT_DRIVER_GEN_DIR}")
EOF
none_out="$(run_env cmake -G "Unix Makefiles" -S "$none" -B "$none/build" 2>&1)" && none_status=0 || none_status=$?
none_sources="$(cat "$none/build/driver_sources.txt" 2>/dev/null)" || none_sources=""
if [[ "$none_status" == 0 && "$none_sources" == "src/transport/driver/driver_not_built.cpp|" &&
  ! -e "$none/build/redshift_driver_identity.txt" ]] &&
  ! grep -rqE 'fetch-driver|embedded_driver|driver_info|redshift_driver[.]lock' "$none/build"; then
  pass "with REDSHIFT_DRIVER=none, the driver sources.cmake adds only the placeholder"
else fail "with REDSHIFT_DRIVER=none, the driver sources.cmake adds only the placeholder (exit $none_status; \
$(printf '%s' "$none_out" | flat))"; fi

echo "# $n cases, $((n - failed)) passed"
exit $(( failed > 0 ))
