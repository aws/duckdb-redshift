#!/usr/bin/env bash
# Linux verification of one driver release, with its trust anchors as arguments. verify-release.sh
# is the command and always passes the repository's own directory and the system trust store; the
# tests pass a test PKI.
#
#   verify_linux.sh <platform> <version> <odbc-dir> <interface-header> <ca-bundle>
#
# <odbc-dir> is a third_party/redshift_odbc tree: the pinned intermediate under signing/, the policy
# files under policy/, and the checked-in notices; <interface-header> is
# src/transport/odbc_driver.hpp, whose entry points the driver must export. The steps are these,
# each failing the run with its rule's name:
#   1. Before any download. version: the version passes the lock's rule (lock.py --version), and
#      the package URL is built from it and the lock's official prefix; openssl: OpenSSL 1.1.0 or
#      later; tools: python3, readelf, od, and curl unless the release files come from
#      RS_DRIVER_MIRROR, on PATH; policy: policy/linux/package-path.txt holds one
#      path in the lock's grammar, whose last segment is a file name the lock accepts for the
#      driver; notices: the version has a release-tag rule (<a>.<b>.<c>.0); entry-points: the
#      interface header is readable; trust-store: the system CA bundle exists. The pinned
#      intermediate is read here and its DER SHA-256 printed (lib/x509.py fingerprint).
#   2. download: the package, its detached signature and its certificate, from the version's
#      official path, by lib/download.sh, whose comment gives the time and size bounds, into a new
#      directory; or from RS_DRIVER_MIRROR's retained copies by the rule README.md gives ("What
#      verification proves").
#   3, 4 and 5. chain, eku, subject, signature: lib/linux_signer.sh; then package: the signed
#      package's RPM header names the driver package, the requested version and architecture,
#      because the signature covers only the bytes.
#   6. extract: the member at the policy's path, by lib/rpm.py --payload into lib/cpio.py (the
#      payload's archive is checked whole, and only that regular file is written), into a new
#      directory. notices: each of LICENSE, NOTICE and THIRD_PARTY_LICENSES comes from the driver's
#      public repository at the version's release tag, or from the retained copies with
#      RS_DRIVER_MIRROR, each into a new directory, and must equal the checked-in copy. The release
#      tag's copy ships: the package may hold each notice once, beside the driver, as a regular file
#      with the tag's bytes (2.3.1.0's packages do), and any other member named like a notice fails
#      the run.
#   7. machine, dynamic-view, strings, exports, entry-points, needed, search-path, textrel, dynamic,
#      flags, stack, debug, glibc: the Linux policy in policy/linux/, by lib/elf_policy.sh; then
#      environment: every environment-variable name the driver holds is classified in policy/, by
#      lib/env_names.py.
#   8. rows: the lock rows, the only output on stdout, printed only once every rule has passed, the
#      driver's SHA-256 that of the extracted member. Their notice rows apply to every platform and
#      are identical for all of them, so a lock holds each once.
# Nothing from the package is executed, and nothing but those members is written.
set -euo pipefail

here="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
(($# == 5)) || { echo "usage: verify_linux.sh <platform> <version> <odbc-dir> <interface-header> <ca-bundle>" >&2; exit 2; }
platform="$1" version="$2" odbc_dir="$3" header="$4" bundle="$5"
fail() { echo "verify-release: $platform: $*" >&2; exit 1; }
# shellcheck source=common.sh
. "$here/common.sh" # sha256, policy_lines, no_trust_variables, notices, retained, get, notice_rows
# No variable may add a trusted certificate, or change the OpenSSL that openssl and Python's hashlib
# use, in any step.
no_trust_variables
case "$platform" in
linux_amd64) arch=x86_64 ;;
linux_arm64) arch=aarch64 ;;
*) fail "not a Linux platform" ;;
esac
# 1. Before anything is downloaded: the tools the rules run, so a missing one names itself rather
# than failing inside a rule; then the version by the lock checker's own rule, the openssl and the
# package path. The URL is the lock's official prefix and that version.
for t in python3 readelf od; do command -v "$t" >/dev/null || fail "tools: $t is required and was not found on PATH"; done
msg="$(python3 -I "$here/lock.py" --version "$version" 2>&1)" || fail "version: $msg"
# The release tag of <a>.<b>.<c>.0 is v<a>.<b>.<c> (v2.3.0 for 2.3.0.0), as for every release since
# 2.1.1; any other version needs a reviewed rule here before a notice can come from a tag. It
# depends only on the argument, so it is refused here, before anything is downloaded.
[[ "$version" == *.*.*.0 ]] || fail "notices: there is no release-tag rule for $version"
base="https://s3.amazonaws.com/redshift-downloads/drivers/odbc/$version"
package="AmazonRedshiftODBC-64-bit-$version.$arch.rpm"
intermediate="$odbc_dir/signing/linux/DigiCertTrustedG4CodeSigningRSA4096SHA3842021CA1.pem"
signer() { bash "$here/linux_signer.sh" "$@" 2>&1 >/dev/null | sed "s/^/verify-release: $platform: /" >&2; return "${PIPESTATUS[0]}"; }

# The release files: the retained copies in RS_DRIVER_MIRROR, named by the package's SHA-256 as the
# driver mirror keeps them, when the recorded lock in RS_DRIVER_LOCK names this version and platform
# (lib/common.sh retained), and otherwise the official path. The retained package is checked against
# that lock before it is read, so this reads other bytes than a download only when they are the
# bytes already verified; the rules are the same either way, and nothing here chooses what is
# trusted.
retained "$version" "$base/$package" "$platform"
# curl: lib/download.sh requires it, so a missing one fails here, before any download, rather than
# at the first one. With the retained copies nothing is downloaded, and it is not needed.
if [[ -z "$mirror" ]]; then
  command -v curl >/dev/null || fail "tools: curl is required and was not found on PATH"
fi
signer openssl || exit 1
fingerprint="$(python3 -I "$here/x509.py" fingerprint "$intermediate" 2>&1)" ||
  fail "certificate: the pinned intermediate: $fingerprint"
echo "verify-release: $platform: the pinned intermediate ${intermediate##*/} has DER SHA-256 $fingerprint" >&2
path="$(policy_lines "$odbc_dir" policy/linux/package-path.txt)" # one path; two lines fail the grammar
msg="$(python3 -I "$here/lock.py" --path "$path" 2>&1)" || fail "policy: policy/linux/package-path.txt: $msg"
name="${path##*/}"
msg="$(python3 -I "$here/lock.py" --name "$name" 2>&1)" || fail "policy: policy/linux/package-path.txt: $msg"
[[ -f "$header" && -r "$header" ]] || fail "entry-points: the interface header $header is missing or unreadable"
[[ -n "$bundle" && -f "$bundle" ]] ||
  fail "trust-store: no system CA bundle${bundle:+ at $bundle}; install the distribution's CA certificates (ca-certificates)"

# 2. The release files, into a new directory.
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
get "$base/${package%.rpm}-signature.bin" "$want_package_sha-signature.bin" "$work/download/signature" 65536
get "$base/${package%.rpm}-certificate.pem" "$want_package_sha-certificate.pem" "$work/download/certificate" 65536

# 3, 4 and 5: chain, eku, subject and signature.
signer verify "$intermediate" "$bundle" "$work/download/certificate" \
  "$work/download/signature" "$work/download/package" || exit 1
echo "verify-release: $platform: $package is signed by AWS's certificate, through the pinned intermediate" >&2

# 5, package: the signed bytes are the release asked for: <a>.<b>.<c>.<d> is RPM version
# <a>.<b>.<c>, release <d>.
want="AmazonRedshiftODBC-64-bit ${version%.*} ${version##*.} $arch"
got="$(python3 -I "$here/rpm.py" "$work/download/package" 2>&1)" ||
  fail "package: the signed package's RPM header cannot be read: $got"
[[ "$got" == "$want" ]] ||
  fail "package: the signed package is $got (name version release architecture), not the requested $want"

# 6, extract: the driver, only the regular file at the policy's path, into a new directory.
take() { # take <path-in-package> <new-dir> <name>
  local msg
  mkdir "$2"
  msg="$({ python3 -I "$here/rpm.py" --payload "$work/download/package" |
    python3 -I "$here/cpio.py" "$1" "$2/$3"; } 2>&1)" ||
    fail "extract: cannot take $1 from the package: $msg"
}
take "$path" "$work/driver" "$name"
driver_sha="$(sha256 "$work/driver/$name")" || fail "extract: $driver_sha"

# 6, notices: from the release tag, or from the retained copies. The release tag's copy ships: the
# package may hold each notice once beside the driver, as a regular file with the tag's bytes, and
# any other member named like a notice fails the run.
found="$({ python3 -I "$here/rpm.py" --payload "$work/download/package" |
  python3 -I "$here/cpio.py" --find "${notices[@]}"; } 2>&1)" ||
  fail "notices: cannot read the package's members: $found"
take_notice() { python3 -I "$here/rpm.py" --payload "$work/download/package" | python3 -I "$here/cpio.py" "$1" "$2"; }
notice_rows "$version" "$odbc_dir" "$work" "$found" "${path%/*}" take_notice
echo "verify-release: $platform: the notices equal the checked-in copies" >&2

# 7. The Linux loading policy.
msg="$(bash "$here/elf_policy.sh" "$odbc_dir/policy/linux" "$header" "$platform" "$work/driver/$name" 2>&1)" ||
  fail "$msg"
echo "verify-release: $platform: $msg" >&2

# 7, environment: every problem the rule finds is printed, each on its own line.
msg="$(python3 -I "$here/env_names.py" "$odbc_dir/policy" "$work/driver/$name" "$platform" 2>&1)" ||
  fail "$(printf '%s\n' "$msg" | sed "2,\$s/^/verify-release: $platform: /")"
echo "verify-release: $platform: $msg" >&2

# 8. The lock rows. The version, the path and the file name passed the lock checker's rules in step
# 1; the URL is the official prefix and that version, whichever copy of the bytes was read. Every
# hash is in a variable, so a failed one cannot leave an empty field in a printed row.
printf '%s\n' "$platform  driver  $name  $driver_sha  $base/$package  $package_sha  $path" "${rows[@]}"
