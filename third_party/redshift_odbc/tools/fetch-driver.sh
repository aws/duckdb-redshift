#!/usr/bin/env bash
# Fetches the locked driver package for one platform, proves it is the locked bytes, and leaves the
# platform's driver set and driver_info.h in <out-dir>. The build's only network access.
#
#   fetch-driver.sh <platform> <out-dir>
#
# The lock is the one check-lock.sh reads: RS_DRIVER_LOCK when set, otherwise the checked-in one. It
# is read once, and driver_info.h is generated from that reading.
# The package comes from the first of these that holds it: $RS_DRIVER_MIRROR/<package-sha256> when
# RS_DRIVER_MIRROR is set (a retained copy, which a tampered copy therefore always fails); only when
# the mirror lacks it, <out-dir>/packages/<package-sha256>, a copy an earlier run kept, used only
# when its SHA-256 is the lock's and otherwise replaced; the lock's URL, exactly as
# written, through lib/download.sh, whose comment gives the time and size bounds it downloads
# within. Whichever it is, its SHA-256 must equal the lock's, and the mirror's file, or the kept
# copy when it is looked at, must be a regular file. The driver is the one member at the lock's
# path in the package, which must be a regular file and is embedded unchanged; its SHA-256 must
# equal the lock's too. On macOS that is the universal library, never thinned, stripped or
# re-signed, and both macOS platforms take the same file from the same package. On Windows the
# package is the MSI, and the set is the driver and every library row's file, each taken from the
# MSI's cabinet at its extract path by lib/msi.py, the reader verification uses. A lock with rename
# rows renames the whole set: lib/rename_apply.py applies its rename and patch rows, checking each
# file's bytes before and after, and each file is embedded under its rename row's name with the
# lock's sha256, so the set holds the renamed bytes.
# The notices are the checked-in copies, checked against the lock. Nothing from the package is
# executed, and nothing but the set's files is taken from it.
#
# <out-dir> has one writer at a time. On success it holds the set's files under their embedded names,
# then driver_info.h, renamed in last: it marks a complete set, and consumers read nothing until it
# exists. On any failure <out-dir> holds no driver_info.h, and the message names the platform, the
# file, and the expected and actual SHA-256.
set -euo pipefail

here="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
odbc_dir="$here/.."
(($# == 2)) || { echo "usage: fetch-driver.sh <platform> <out-dir>" >&2; exit 2; }
platform="$1"
fail() { echo "fetch-driver: $platform: $*" >&2; exit 1; }
# driver_info.h marks a complete set, so it goes first, and every failure from here on removes it
# again (the EXIT trap below).
mkdir -p -- "$2" && out="$(cd -- "$2" && pwd)" || fail "cannot create $2"
rm -f -- "$out/driver_info.h"
max_package_bytes=268435456 # 256 MiB; the 2.3.0.0 packages are 8.6 MB (Linux) and 12.4 MB (macOS)

# shellcheck source=lib/common.sh
. "$here/lib/common.sh" # sha256 <file> (streamed), size_of <file>, signer_checksum
require() { for t in "$@"; do command -v "$t" >/dev/null || fail "$t is required and was not found on PATH"; done; }

case "$platform" in
linux_amd64) machine=62 machine_name=x86-64 ;;   # e_machine EM_X86_64
linux_arm64) machine=183 machine_name=aarch64 ;; # e_machine EM_AARCH64
osx_amd64 | osx_arm64) machine="" ;;             # one universal library for both
windows_amd64) machine=msi ;;
*) fail "unknown platform" ;;
esac
require python3

# The lock is read once, into the work directory as .lock, a name no set file can have, and checked
# there by check-lock.sh; everything below, driver_info.h included, uses that check's normalised
# text, so a lock file changed meanwhile cannot reach the header.
if [[ -n "${RS_DRIVER_LOCK+set}" ]]; then
  [[ -n "$RS_DRIVER_LOCK" ]] || fail "RS_DRIVER_LOCK is set but empty"
  lock_file="$RS_DRIVER_LOCK"
else
  lock_file="$odbc_dir/driver.lock"
fi
work="$(mktemp -d "$out/.fetch-$platform.XXXXXX")"
trap 'status=$?; rm -rf -- "${work:?}"; ((status == 0)) || rm -f -- "$out/driver_info.h"' EXIT
cp -- "$lock_file" "$work/.lock" 2>/dev/null || fail "cannot read the lock $lock_file"
lock="$(bash "$here/check-lock.sh" "$work/.lock")" || fail "the lock failed check-lock.sh"
row="$(printf '%s\n' "$lock" | awk -v p="$platform" '$1 == "driver" && $2 == p')"
[[ -n "$row" ]] || fail "the lock has no driver row for this platform"
read -r _ _ name want_sha _ url pkg_sha path_in_pkg _ <<<"$row"
set_files=("$name")
# On Windows, the <extract path>=<name> pairs lib/msi.py takes the set's files by, the driver's first.
takes=("$path_in_pkg=$name")
renamed=false
while read -r kind p lib _ _ _ _ lib_path _; do
  if [[ "$p" == "$platform" && "$kind" == library ]]; then set_files+=("$lib"); takes+=("$lib_path=$lib"); fi
  if [[ "$p" == "$platform" && "$kind" =~ ^(rename|patch)$ ]]; then renamed=true; fi
done <<<"$lock"
# The name a Windows file is embedded under: its rename row's, else its own.
embedded_name() {
  printf '%s\n' "$lock" | awk -v p="$platform" -v n="$1" '$1 == "rename" && $2 == p && $3 == n { e = $4 }
    END { print (e == "" ? n : e) }'
}
if $renamed; then
  # Every importer is rewritten to the generated names, so a file left under its own name would be one
  # no importer loads, and a patch without a rename would be embedded under a name it does not have.
  for f in "${set_files[@]}"; do
    [[ "$(embedded_name "$f")" != "$f" ]] || fail "the lock renames the set but has no rename row for $f"
  done
  for i in "${!set_files[@]}"; do set_files[i]="$(embedded_name "${set_files[i]}")"; done
fi
while read -r kind notice _; do if [[ "$kind" == notice ]]; then set_files+=("$notice"); fi; done <<<"$lock"
for f in "${set_files[@]}"; do rm -f -- "$out/$f"; done

# 1. The package, from the first source that holds it, checked against the lock.
cache="$out/packages"
kept="$cache/$pkg_sha"
mirrored="${RS_DRIVER_MIRROR:+$RS_DRIVER_MIRROR/$pkg_sha}"
pkg="$work/package"
small_enough() { (($(size_of "$1") <= max_package_bytes)); }
absent() { [[ ! -e "$1" && ! -L "$1" ]]; }
# Nothing is hashed, copied or replaced through a link, a FIFO (which would block the build) or a
# directory (which would take a new kept copy inside it on every run): the run stops and names it.
regular_or_absent() {
  absent "$1" || [[ -f "$1" && ! -L "$1" ]] ||
    fail "$1 is not a regular file (a link, FIFO, directory or device); remove it"
}
# took: which of the three sources the package came from, so what to keep is decided by that and
# not by the wording of the message $from carries.
kept_ok=false took=""
if [[ -n "$mirrored" ]] && ! absent "$mirrored"; then
  # The mirror holds it: the kept copy is never read or replaced, so nothing stale there can stop the
  # build.
  regular_or_absent "$mirrored"
  small_enough "$mirrored" || fail "RS_DRIVER_MIRROR/$pkg_sha is larger than $max_package_bytes bytes"
  cp -- "$mirrored" "$pkg"; from="RS_DRIVER_MIRROR"; took=mirror
else
  [[ -z "$mirrored" ]] || echo "fetch-driver: $platform: RS_DRIVER_MIRROR has no $pkg_sha; trying the kept copy, then $url" >&2
  regular_or_absent "$kept"
  # A kept copy that is too large or has another SHA-256 is not trusted and not an error: it is
  # skipped, saying why, and replaced below.
  if [[ -f "$kept" ]] && ! small_enough "$kept"; then
    echo "fetch-driver: $platform: the copy kept in $cache is larger than $max_package_bytes bytes; replacing it" >&2
  elif [[ -f "$kept" ]]; then
    got="$(sha256 "$kept" 2>/dev/null || echo unreadable)"
    if [[ "$got" == "$pkg_sha" ]]; then kept_ok=true; else
      echo "fetch-driver: $platform: the copy kept in $cache has SHA-256 $got, the lock expects $pkg_sha; replacing it" >&2
    fi
  fi
  if $kept_ok; then
    cp -- "$kept" "$pkg"; from="the copy kept in $cache"; took=kept
  else
    # lib/download.sh: HTTPS only, no redirects, no .curlrc, bounded as its comment gives; it
    # names a missing curl.
    msg="$(bash "$here/lib/download.sh" "$url" "$pkg" "$max_package_bytes" 2>&1)" || fail "$msg"
    from="$url"; took=download
  fi
fi
got="$(sha256 "$pkg")"
[[ "$got" == "$pkg_sha" ]] || fail "package $(basename "$url") from $from has SHA-256 $got, the lock expects $pkg_sha"
# Keep the checked package for a later run without the mirror, replacing a damaged copy. Best effort:
# a failure here only costs a later download. The copy that was just used is never written again: a
# good kept copy keeps its inode, and after a mirror fetch a copy is kept only where nothing stands
# yet, so what does stand there is never read or replaced then.
case "$took" in
mirror) keep_it=false; absent "$kept" && keep_it=true ;;
kept) keep_it=false ;;
*) keep_it=true ;;
esac
if $keep_it; then
  { mkdir -p -- "$cache" && keep="$(mktemp "$cache/.keep.XXXXXX")" && cp -- "$pkg" "$keep" &&
    mv -f -- "$keep" "$kept"; } 2>/dev/null ||
    echo "fetch-driver: $platform: could not keep the package in $cache" >&2
fi

# 2. Only the member at the lock's path, which must be a regular file with one link, written by
# lib/cpio.py to a file of its own; no other member reaches the file system. lib/rpm.py reads the
# package's gzip payload, so the build needs no rpm2cpio. A macOS package is a xar archive, read
# by lib/xar.py, which checks the payload against the package's table of contents, in the checksum
# algorithm policy/macos/signer.txt accepts, and passes its cpio archive through the same
# lib/cpio.py.
if [[ "$machine" == msi ]]; then
  mkdir "$work/extracted"
  python3 -I "$here/lib/msi.py" extract "$pkg" "$work/extracted" "${takes[@]}" ||
    fail "cannot take the set's files from the MSI"
elif [[ -n "$machine" ]]; then
  python3 -I "$here/lib/rpm.py" --payload "$pkg" | python3 -I "$here/lib/cpio.py" "$path_in_pkg" "$work/extracted" ||
    fail "cannot take $path_in_pkg from the package"
  read -r -a ident <<<"$(od -An -v -tu1 -N20 -- "$work/extracted" | tr '\n' ' ')"
  [[ "${ident[*]:0:6}" == "127 69 76 70 2 1" ]] || fail "$path_in_pkg is not a 64-bit little-endian ELF file"
  ((ident[18] + 256 * ident[19] == machine)) || fail "$path_in_pkg is ELF machine $((ident[18] + 256 * ident[19])), not $machine_name"
else
  alg="$(signer_checksum "$here/..")" || exit 1 # with its own message
  python3 -I "$here/lib/xar.py" extract "$pkg" "$path_in_pkg" "$work/extracted" "$alg" ||
    fail "cannot take $path_in_pkg from the package"
  [[ "$(od -An -v -tx1 -N4 -- "$work/extracted" | tr -d ' \n')" == cafebabe ]] ||
    fail "$path_in_pkg is not a universal Mach-O file"
fi

# 3. Each file must be the locked bytes. On Windows a renamed set is first rewritten by
# lib/rename_apply.py, which checks each file's source-sha256 before its patches and its sha256 after,
# given the lock's rows in the form it reads; an unrenamed file's sha256 is its source-sha256
# (check-lock.sh). Elsewhere the file is embedded as published.
if [[ "$machine" == msi ]]; then
  from_dir="$work/extracted"
  if $renamed; then
    printf '%s\n' "$lock" | awk -v p="$platform" '$2 != p { next }
      $1 == "driver" || $1 == "library" { print p, $1, $3, $4, $9 }
      $1 == "rename" { print p, $1, $3, $4 }
      $1 == "patch" { print p, $1, $3, $4, $5, $6 }' > "$work/.rename-rows"
    msg="$(python3 -I "$here/lib/rename_apply.py" "$work/.rename-rows" "$work/extracted" "$work/renamed" 2>&1)" ||
      fail "the lock's rename and patch rows do not verify against the MSI's files: $msg"
    from_dir="$work/renamed"
  fi
  while read -r kind p f f_sha _; do
    [[ "$p" == "$platform" && "$kind" =~ ^(driver|library)$ ]] || continue
    e="$f"; if $renamed; then e="$(embedded_name "$f")"; fi
    mv -f -- "$from_dir/$e" "$work/$e" || fail "cannot move $e into place"
    got="$(sha256 "$work/$e")"
    [[ "$got" == "$f_sha" ]] || fail "$e has SHA-256 $got, the lock expects $f_sha"
  done <<<"$lock"
else
  mv -f -- "$work/extracted" "$work/$name" || fail "cannot move $path_in_pkg into place"
  got="$(sha256 "$work/$name")"
  [[ "$got" == "$want_sha" ]] || fail "$name has SHA-256 $got, the lock expects $want_sha"
fi

# 4. The notices, from the repository, checked against the lock.
while read -r kind notice notice_sha _; do
  [[ "$kind" == notice ]] || continue
  cp -- "$odbc_dir/$notice" "$work/$notice" 2>/dev/null || fail "the checked-in notice $notice is missing"
  got="$(sha256 "$work/$notice")"
  [[ "$got" == "$notice_sha" ]] || fail "notice $notice has SHA-256 $got, the lock expects $notice_sha"
done <<<"$lock"

# 5. The set into <out-dir>, then the header, generated there by lib/driver_info.py from the lock as
# read above and the files where the assembler will read them, and renamed in last.
for f in "${set_files[@]}"; do mv -f -- "$work/$f" "$out/$f" || fail "cannot move $f into $out"; done
echo "fetch-driver: $platform: the driver set (${#set_files[@]} files) matches the lock; package from $from" || true
printf '%s\n' "$lock" | python3 -I "$here/lib/driver_info.py" "$platform" "$out" "$odbc_dir/policy" ||
  fail "driver_info.h cannot be generated"
