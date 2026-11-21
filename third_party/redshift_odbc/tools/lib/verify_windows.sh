#!/usr/bin/env bash
# Windows verification of one driver release, from Linux, with its trust anchors as arguments.
# verify-release.sh is the command and always passes the repository's own directory and the system
# trust store; the tests pass a test PKI.
#
#   verify_windows.sh <platform> <version> <odbc-dir> <interface-header> <ca-bundle>
#
# The steps, each failing the run with its rule's name:
#   1. Before any download. version, notices and tools as on Linux; openssl: OpenSSL 1.1.0 or
#      later; policy: policy/windows/product.txt and msi-files.txt are well formed (lib/msi.py
#      policy), and so are the loading policy's files (lib/windows_policy.py policy); entry-points:
#      the interface header is readable, and expected-exports.txt holds every entry point it lists;
#      trust-store: the system CA bundle exists. The intermediate Linux pins, which the MSI's chain
#      carries too, is read and its DER SHA-256 printed.
#   2. download: the MSI from the version's official path, or from RS_DRIVER_MIRROR's retained copy
#      by the same rule as on Linux.
#   3. cfb, chain, eku, subject, signature, digest: lib/windows_signer.sh.
#   4. files: lib/msi.py check, which reads the signed MSI's tables and cabinet and takes the set's
#      files out; notices: from the release tag, as on Linux.
#   5. image, imports, prerequisites, delay-imports, bound-imports, forwarders, exports,
#      load-flags, runtime-names: the Windows loading policy on each file of the set
#      (lib/windows_policy.py check), every problem on its own line.
#   6. environment: the environment-name rule on the set's files as windows_amd64
#      (lib/env_names.py).
#   7. rows: the lock rows, the only output on stdout, printed only once every rule has passed.
# Nothing from the MSI is executed, and nothing but the set's files is written.
set -euo pipefail

here="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
(($# == 5)) || { echo "usage: verify_windows.sh <platform> <version> <odbc-dir> <interface-header> <ca-bundle>" >&2; exit 2; }
platform="$1" version="$2" odbc_dir="$3" header="$4" bundle="$5"
fail() { echo "verify-release: $platform: $*" >&2; exit 1; }
# shellcheck source=common.sh
. "$here/common.sh" # sha256, no_trust_variables, check_openssl, retained, get, notice_rows
no_trust_variables
[[ "$platform" == windows_amd64 ]] || fail "not a Windows platform"

# 1. Before anything is downloaded.
command -v python3 >/dev/null || fail "tools: python3 is required and was not found on PATH"
msg="$(python3 -I "$here/lock.py" --version "$version" 2>&1)" || fail "version: $msg"
[[ "$version" == *.*.*.0 ]] || fail "notices: there is no release-tag rule for $version"
base="https://s3.amazonaws.com/redshift-downloads/drivers/odbc/$version"
package="AmazonRedshiftODBC64-$version.msi"
intermediate="$odbc_dir/signing/linux/DigiCertTrustedG4CodeSigningRSA4096SHA3842021CA1.pem"
retained "$version" "$base/$package" "$platform"
if [[ -z "$mirror" ]]; then
  command -v curl >/dev/null || fail "tools: curl is required and was not found on PATH"
fi
check_openssl
fingerprint="$(python3 -I "$here/x509.py" fingerprint "$intermediate" 2>&1)" ||
  fail "certificate: the pinned intermediate: $fingerprint"
echo "verify-release: $platform: the pinned intermediate ${intermediate##*/} has DER SHA-256 $fingerprint" >&2
msg="$(python3 -I "$here/msi.py" policy "$odbc_dir/policy" 2>&1)" || fail "$msg"
[[ -f "$header" && -r "$header" ]] || fail "entry-points: the interface header $header is missing or unreadable"
msg="$(python3 -I "$here/windows_policy.py" policy "$odbc_dir/policy" "$header" 2>&1)" ||
  fail "$msg"
[[ -n "$bundle" && -f "$bundle" ]] ||
  fail "trust-store: no system CA bundle${bundle:+ at $bundle}; install the distribution's CA certificates (ca-certificates)"

# 2. The MSI, into a new directory.
work="$(mktemp -d)"
trap 'rm -rf -- "${work:?}"' EXIT
mkdir "$work/download" "$work/files"
get "$base/$package" "$want_package_sha" "$work/download/$package" 268435456
package_sha="$(sha256 "$work/download/$package")" || fail "package: $package_sha"
if [[ -n "$mirror" ]]; then
  [[ "$package_sha" == "$want_package_sha" ]] ||
    fail "mirror: the retained $mirror/$want_package_sha has SHA-256 $package_sha, not the SHA-256 the recorded lock names"
  echo "verify-release: $platform: $package comes from the copy retained as $want_package_sha, not from $base" >&2
fi

# 3. The signer.
bash "$here/windows_signer.sh" verify "$intermediate" "$bundle" "$work/download/$package" 2> "$work/signer" ||
  fail "$(head -n 1 "$work/signer")"
echo "verify-release: $platform: $package is signed by AWS's certificate, through the pinned intermediate" >&2

# 4. The files, and the notices.
file_rows="$(python3 -I "$here/msi.py" check "$work/download/$package" "$odbc_dir/policy" "$version" \
  "$base/$package" "$package_sha" "$work/files" 2> "$work/files.err")" || fail "$(head -n 1 "$work/files.err")"
echo "verify-release: $platform: the MSI is $version of the driver, runs no custom action in an administrative sequence and holds the listed files" >&2
notice_rows "$version" "$odbc_dir" "$work" ""
echo "verify-release: $platform: the notices equal the checked-in copies" >&2

# 5. The loading policy on each file, then 6. the environment-name rule on the set, each problem on
# its own line.
driver="$(awk '$2 == "driver" { print $3; exit }' <<<"$file_rows")"
each_line() { printf '%s\n' "$1" | LC_ALL=C sed "2,\$s/^/verify-release: $platform: /"; }
msg="$(python3 -I "$here/windows_policy.py" check "$work/files" "$driver" "$odbc_dir/policy" \
  "$header" 2>&1)" || fail "$(each_line "$msg")"
echo "verify-release: $platform: $(each_line "$msg")" >&2
msg="$(python3 -I "$here/env_names.py" "$odbc_dir/policy" "$work/files" "$platform" 2>&1)" ||
  fail "$(each_line "$msg")"
echo "verify-release: $platform: $msg" >&2

# 7. The lock rows.
printf '%s\n' "$file_rows" "${rows[@]}"
