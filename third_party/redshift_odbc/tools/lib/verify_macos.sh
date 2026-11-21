#!/usr/bin/env bash
# macOS verification of one driver release, run on Linux, with its trust anchors as arguments.
# verify-release.sh is the command and always passes the repository's own directory; the tests pass
# a tree that pins a test PKI.
#
#   verify_macos.sh <platform> <version> <odbc-dir> <interface-header>
#
# <platform> is osx_amd64 or osx_arm64, either of which verifies the one universal package both
# macOS builds embed, once, and prints both macOS driver rows from it, which therefore differ only
# in their platform. <odbc-dir> is a third_party/redshift_odbc tree: the pinned Apple root and
# Developer ID intermediate under signing/, the policy files under policy/, and the checked-in
# notices; <interface-header> is src/transport/odbc_driver.hpp, whose entry points the driver must
# export. The chain is anchored at the pinned root alone, never at a system trust store. The steps
# are these, each failing the run with its rule's name:
#   1. Before any download. tools: python3, and curl unless the package comes from
#      RS_DRIVER_MIRROR, on PATH; version: the version passes the lock's rule
#      (lock.py --version), and the package URL is built from it and the lock's official prefix;
#      notices: the version has a release-tag rule (<a>.<b>.<c>.0); openssl: OpenSSL 1.1.0 or
#      later, one that runs; certificate: the pinned root and intermediate are each one PEM
#      certificate, read strictly, whose DER SHA-256 is printed (lib/x509.py fingerprint); policy:
#      policy/macos/package-path.txt holds one path in the lock's grammar, inside a component's
#      Payload, whose last segment is a file name the lock accepts for the driver, and
#      policy/macos/signer.txt is printable ASCII with exactly one checksum, installer and team
#      row; entry-points: the interface header is readable.
#   2. download: the universal package, from the version's official path, by lib/download.sh,
#      whose comment gives the time and size bounds, into a new directory.
#      With RS_DRIVER_MIRROR and a recorded lock in RS_DRIVER_LOCK that names this version, it
#      and the notices are read from the retained copies instead, and the package must have that
#      lock's SHA-256 (mirror), as on Linux (lib/common.sh retained).
#   3. package, algorithm, checksum-location, checksum, signature, certificate, chain, usage, eku,
#      subject, payload: the package signature, by lib/macos_signer.sh package, with the tree's
#      pinned certificates and signer.txt's rows; the signer passes chain, usage, eku and subject
#      before either signature is parsed, the RSA signature verifies before the CMS one is parsed,
#      and the component payload's checksums are the signed ones. product: the PackageInfo beside
#      that payload names the driver's identifier and the version asked for, by lib/xar.py product,
#      as the RPM header does on Linux.
#   4. extract: the library at the policy's path, by lib/xar.py (the payload's cpio archive is
#      checked whole by lib/cpio.py, and only that regular file is written), into a new directory.
#      notices: from the release tag, as on Linux (lib/common.sh notice_rows); the payload may hold
#      each once beside the library with the tag's bytes, and any other member named like one fails
#      the run. Every Payload read uses signer.txt's
#      checksum algorithm. The signed package has authenticated every byte of the library, which
#      the lock pins by SHA-256, so the library's own code signature is not checked again.
#   5. slices, entry-points, load-commands, namespace, weak-binds, linked-libraries, search-path,
#      min-os, exports: the macOS loading policy in policy/macos/, for each slice, by lib/macho.py
#      policy; then environment: the environment-name rule on each slice, which lib/macho.py thin
#      writes out, as its own platform (osx_amd64 for x86_64, osx_arm64 for arm64), with the policy
#      files and lib/env_names.py the Linux rule uses.
#   6. rows: the lock rows, the only output on stdout, printed only once every rule has passed:
#      both macOS driver rows, the library's SHA-256 that of the extracted member, and the notice
#      rows, which are identical for every platform.
# Nothing from the package is executed, and nothing but those members is written.
set -euo pipefail

here="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
(($# == 4)) || { echo "usage: verify_macos.sh <platform> <version> <odbc-dir> <interface-header>" >&2; exit 2; }
platform="$1" version="$2" odbc_dir="$3" header="$4"
fail() { echo "verify-release: $platform: $*" >&2; exit 1; }
# shellcheck source=common.sh
. "$here/common.sh" # sha256, policy_lines, signer_checksum, no_trust_variables, notices, retained, get, notice_rows
# No variable may add a trusted certificate, or change the OpenSSL that openssl and Python's hashlib
# use, in any step.
no_trust_variables
case "$platform" in
osx_amd64 | osx_arm64) ;;
*) fail "not a macOS platform" ;;
esac
# 1. Before anything is downloaded: the tools, then the version by the lock checker's own rule,
# the openssl, the pinned certificates and the policy. The URL is the lock's official prefix and
# that version.
command -v python3 >/dev/null || fail "tools: python3 is required and was not found on PATH"
msg="$(python3 -I "$here/lock.py" --version "$version" 2>&1)" || fail "version: $msg"
# The release tag of <a>.<b>.<c>.0 is v<a>.<b>.<c>, as on Linux.
[[ "$version" == *.*.*.0 ]] || fail "notices: there is no release-tag rule for $version"
base="https://s3.amazonaws.com/redshift-downloads/drivers/odbc/$version"
package="AmazonRedshiftODBC-64-bit.$version.universal.pkg"
identifier="com.amazon.redshift.odbc2x64" # the driver's package identifier in its PackageInfo
intermediate="$odbc_dir/signing/macos/DeveloperIDG2CA.pem"
root="$odbc_dir/signing/macos/AppleRootCA.pem"

retained "$version" "$base/$package" osx_amd64 osx_arm64
# curl: lib/download.sh requires it, so a missing one fails here, before any download, rather than
# at the first one. With the retained copies nothing is downloaded, and it is not needed.
if [[ -z "$mirror" ]]; then
  command -v curl >/dev/null || fail "tools: curl is required and was not found on PATH"
fi
msg="$(bash "$here/macos_signer.sh" openssl 2>&1)" || fail "$msg"
for cert in "$root" "$intermediate"; do
  fingerprint="$(python3 -I "$here/x509.py" fingerprint "$cert" 2>&1)" ||
    fail "certificate: the pinned ${cert##*/}: $fingerprint"
  echo "verify-release: $platform: the pinned ${cert##*/} has DER SHA-256 $fingerprint" >&2
done
path="$(policy_lines "$odbc_dir" policy/macos/package-path.txt)" # one path; two lines fail the grammar
msg="$(python3 -I "$here/xar.py" path "$path" 2>&1)" || fail "policy: policy/macos/package-path.txt: ${msg#xar.py: }"
name="${path##*/}"
msg="$(python3 -I "$here/lock.py" --name "$name" 2>&1)" || fail "policy: policy/macos/package-path.txt: $msg"
signer_rows="$(policy_lines "$odbc_dir" policy/macos/signer.txt)"
LC_ALL=C awk '/[^ -~]/ { exit 1 }' <<<"$signer_rows" ||
  fail "policy: policy/macos/signer.txt holds a byte that is not printable ASCII"
row() { # row <key>: signer.txt's one <key> row's value
  policy_row "$signer_rows" "$1" ||
    fail "policy: policy/macos/signer.txt must have exactly one $1 row, with a value"
}
alg="$(signer_checksum "$odbc_dir")" || exit 1
installer="$(row installer)" || exit 1
team="$(row team)" || exit 1
[[ "$installer" == "Developer ID Installer: "?* ]] ||
  fail "policy: policy/macos/signer.txt: the installer row is \"installer Developer ID Installer: <organization>\""
[[ -f "$header" && -r "$header" ]] || fail "entry-points: the interface header $header is missing or unreadable"

# 2. The package, into a new directory.
work="$(mktemp -d)"
trap 'rm -rf -- "${work:?}"' EXIT
mkdir "$work/download"
get "$base/$package" "$want_package_sha" "$work/download/package" 268435456
package_sha="$(sha256 "$work/download/package")" || fail "package: $package_sha"
if [[ -n "$mirror" ]]; then
  [[ "$package_sha" == "$want_package_sha" ]] ||
    fail "mirror: the retained $mirror/$want_package_sha has SHA-256 $package_sha, not the SHA-256 the recorded lock names"
  echo "verify-release: $platform: $package comes from the copy retained as $want_package_sha, not from $base" >&2
fi

# 3. The package signature, and the payload's checksums; then the product.
msg="$(bash "$here/macos_signer.sh" package "$intermediate" "$root" "$alg" "$installer" "$team" \
  "$work/download/package" "$path" 2>&1)" || fail "$msg"
echo "verify-release: $platform: $package: $msg" >&2
msg="$(python3 -I "$here/xar.py" product "$work/download/package" "$path" "$identifier" "$version" "$alg" 2>&1)" ||
  fail "${msg#xar.py: }"
echo "verify-release: $platform: the package is $identifier $version" >&2

# 4, extract: the library, only the regular file at the policy's path, into a new directory.
mkdir "$work/driver"
msg="$(python3 -I "$here/xar.py" extract "$work/download/package" "$path" "$work/driver/$name" "$alg" 2>&1)" ||
  fail "extract: cannot take $path from the package: ${msg#xar.py: }"
driver_sha="$(sha256 "$work/driver/$name")" || fail "extract: $driver_sha"

# 4, notices: from the release tag, or from the retained copies. As on Linux, the release tag's copy
# ships: the payload may hold each notice once beside the library, with the tag's bytes.
found="$(python3 -I "$here/xar.py" find "$work/download/package" "$alg" "${notices[@]}" 2>&1)" ||
  fail "notices: cannot read the package's members: ${found#xar.py: }"
take_notice() { python3 -I "$here/xar.py" extract "$work/download/package" "$1" "$2" "$alg"; }
notice_rows "$version" "$odbc_dir" "$work" "$found" "${path%/*}" take_notice
echo "verify-release: $platform: the notices equal the checked-in copies" >&2

# 5. The macOS loading policy on both slices, then the environment-name rule on each, every problem
# it finds printed on its own line.
msg="$(python3 -I "$here/macho.py" policy "$work/driver/$name" "$odbc_dir/policy/macos" "$header" 2>&1)" || fail "$msg"
echo "verify-release: $platform: $msg" >&2
mkdir "$work/slices"
msg="$(python3 -I "$here/macho.py" thin "$work/driver/$name" "$work/slices" 2>&1)" || fail "$msg"
for slice in x86_64:osx_amd64 arm64:osx_arm64; do
  msg="$(python3 -I "$here/env_names.py" "$odbc_dir/policy" "$work/slices/${slice%:*}" "${slice#*:}" 2>&1)" ||
    fail "${slice%:*}: $(printf '%s\n' "$msg" | LC_ALL=C sed "2,\$s/^/verify-release: $platform: ${slice%:*}: /")"
  echo "verify-release: $platform: ${slice%:*}: $msg" >&2
done

# 6. The lock rows. The version, the path and the file name passed the lock checker's rules in step
# 1; the URL is the official prefix and that version, whichever copy of the bytes was read. Every
# hash is in a variable, so a failed one cannot leave an empty field in a printed row.
printf '%s\n' "osx_amd64  driver  $name  $driver_sha  $base/$package  $package_sha  $path" \
  "osx_arm64  driver  $name  $driver_sha  $base/$package  $package_sha  $path" "${rows[@]}"
