# Helpers for the macOS verification suites (verify-macos-*_test.sh), sourced, never run on their
# own. They build on verify-test-lib.sh (pass, fail, expect, finish, sha, require_tools, the
# release cache, the stand-in curl and hostile_conf) and add the macOS test inputs, all built at
# test time. Each suite sets tmp first. Provides:
#
#   skip <name> <reason>       a skipped TAP case
#   mac_row, mac_name, mac_sha, mac_url, mac_pkg_sha, mac_path, version
#                              the osx_arm64 row of the lock under test (RS_DRIVER_LOCK or the
#                              checked-in one), which a rejected lock fails here
#   real_package               prints the path of the locked universal package, from
#                              release-cache.sh (RS_DRIVER_MIRROR or one download into the
#                              cache), or nothing when the lock has no macOS row; when it cannot
#                              be had the suite fails, so a default run never skips it
#   real_valid                 succeeds while the real package's signing certificate is inside its
#                              validity, read from the package (2030-09-26 for 2.3.0.0), after which
#                              the cases that verify the real package skip, as verification of
#                              that version stops
#   make_pki <dir>             a test PKI with Apple's shapes, generated per run (below)
#   mac_tree <dir> <pki>       a copy of third_party/redshift_odbc whose pinned Apple certificates
#                              are the test PKI's, and whose policy fits tiny_library
#   tiny_library <out>         a tiny universal library make_macho.py writes: it exports the
#                              interface's entry points and holds the one known environment name
#   serve_notices [<dir>]      the checked-in notices, served as the release tag's
#
# These suites download through lib/download.sh, which needs only curl, so a fresh macOS host can
# run them. The stand-in curl serves $FAKE_CURL_DIR, $tmp/serve unless a case sets another.
# shellcheck shell=bash

mac_repo="$(cd "$(dirname "${BASH_SOURCE[0]}")/../../.." && pwd)"
# shellcheck source=release-cache.sh
source "$mac_repo/test/client/driver/release-cache.sh" # release_file
base_lock="${RS_DRIVER_LOCK:-$mac_repo/third_party/redshift_odbc/driver.lock}"
[[ "$base_lock" == /* ]] || base_lock="$PWD/$base_lock"
# A rejected lock fails the suite here.
checked_lock="$(bash "$mac_repo/third_party/redshift_odbc/tools/check-lock.sh" "$base_lock")"
mac_row="$(awk '$1 == "driver" && $2 == "osx_arm64"' <<<"$checked_lock")"
read -r _ _ mac_name mac_sha _ mac_url mac_pkg_sha mac_path <<<"$mac_row"
version="$(awk '$1 == "version" { print $2 }' <<<"$checked_lock")"
# The packages make_xar.py writes name that version unless a case's spec gives another.
export MAKE_XAR_VERSION="$version"
# shellcheck source=verify-test-lib.sh
source "$mac_repo/test/client/driver/verify-test-lib.sh"
here="$repo/test/client/driver"
lib="$tools/lib"
skip() { n=$((n+1)); echo "ok $n - $1 # SKIP $2"; }
real_package() {
  [[ -n "$mac_row" ]] || return 0
  release_file "$mac_pkg_sha" "$mac_url" "$mac_pkg_sha" ||
    { echo "not ok - the locked macOS package ${mac_url##*/} could not be copied or downloaded" >&2; return 1; }
}
no_real="the lock under test has no osx_arm64 row"
real_valid() {
  local d
  d="$(mktemp -d)"
  python3 -I "$mac_repo/third_party/redshift_odbc/tools/lib/xar.py" signed "$real" "$d" sha1 &&
    openssl x509 -inform DER -in "$d/signer.der" -noout -checkend 0 >/dev/null
  local ok=$?
  rm -rf -- "${d:?}"
  return "$ok"
}
expired="the locked package's signer is outside its validity, so the pipeline no longer verifies it"

# make_pki <dir>: root.pem and int.pem (subject "Developer ID Certification Authority", issued by
# the root), and keys and certificates issued by int.pem: installer (AWS's installer identity),
# other-team (the installer name with another team), wrong-role (AWS's installer subject with the
# Developer ID Application extensions). forged is a self-signed certificate with AWS's installer
# subject; rogue is AWS's installer identity issued by a second test intermediate under a second
# test root (root2.pem), neither of which is pinned. direct, no-marker, wrong-eku, ca-leaf,
# ou-other, uid-other and o-other each break one rule. crit-other, crit-policy and crit-ski are
# AWS's installer identity with one more critical extension (an OID that is not Apple's, certificate
# policies, the Subject Key Identifier): openssl ignores critical extensions it does not know, so
# x509.py must refuse them.
make_pki() {
  local d="$1" team=94KV3E626L org="AMZN Mobile LLC"
  mkdir -p "$d"
  cat > "$d/ca.ext" <<'EOF'
basicConstraints=critical,CA:TRUE,pathlen:0
keyUsage=critical,keyCertSign,cRLSign
subjectKeyIdentifier=hash
authorityKeyIdentifier=keyid
EOF
  cat > "$d/installer.ext" <<'EOF'
basicConstraints=critical,CA:FALSE
keyUsage=critical,digitalSignature
extendedKeyUsage=critical,1.2.840.113635.100.4.13
subjectKeyIdentifier=hash
authorityKeyIdentifier=keyid
1.2.840.113635.100.6.1.14=critical,DER:0500
EOF
  cat > "$d/application.ext" <<'EOF'
basicConstraints=critical,CA:FALSE
keyUsage=critical,digitalSignature
extendedKeyUsage=critical,codeSigning
subjectKeyIdentifier=hash
authorityKeyIdentifier=keyid
1.2.840.113635.100.6.1.13=critical,DER:0500
EOF
  # The self-signed certificates take their extensions from this file alone, never from the host's
  # openssl configuration, whose defaults differ between OpenSSL 1.1 and 3 (a second Subject Key
  # Identifier from them makes OpenSSL 1.1 refuse the root).
  cat > "$d/req.cnf" <<'EOF'
[req]
distinguished_name = dn
[dn]
[root]
basicConstraints = critical,CA:TRUE
keyUsage = critical,keyCertSign,cRLSign
subjectKeyIdentifier = hash
[forged]
extendedKeyUsage = critical,1.2.840.113635.100.4.13
1.2.840.113635.100.6.1.14 = critical,DER:0500
EOF
  ossl() { openssl "$@" 2>/dev/null; }
  root() { # root <name> <cn>
    ossl req -x509 -newkey rsa:2048 -nodes -keyout "$d/$1.key" -out "$d/$1.pem" -days 30 -sha256 \
      -subj "/C=US/O=Test Root Inc./OU=Test Certification Authority/CN=$2" -config "$d/req.cnf" -extensions root
  }
  issue() { # issue <name> <issuer> <subject> <extfile>
    ossl req -new -newkey rsa:2048 -nodes -keyout "$d/$1.key" -out "$d/$1.csr" -subj "$3"
    ossl x509 -req -in "$d/$1.csr" -CA "$d/$2.pem" -CAkey "$d/$2.key" -set_serial "0x$(od -An -N8 -tx1 /dev/urandom | tr -d ' \n')" \
      -days 30 -sha256 -extfile "$d/$4" -out "$d/$1.pem"
  }
  root root "Test Root CA"
  issue int root "/CN=Developer ID Certification Authority/OU=G2/O=Test Root Inc./C=US" ca.ext
  issue installer int "/UID=$team/CN=Developer ID Installer: $org ($team)/OU=$team/O=$org/C=US" installer.ext
  issue other-team int "/UID=ABCDE12345/CN=Developer ID Installer: $org (ABCDE12345)/OU=ABCDE12345/O=$org/C=US" installer.ext
  issue wrong-role int "/UID=$team/CN=Developer ID Installer: $org ($team)/OU=$team/O=$org/C=US" application.ext
  ossl req -x509 -newkey rsa:2048 -nodes -keyout "$d/forged.key" -out "$d/forged.pem" -days 30 -sha256 \
    -subj "/UID=$team/CN=Developer ID Installer: $org ($team)/OU=$team/O=$org/C=US" -config "$d/req.cnf" -extensions forged
  # One rule broken each: issued by the root itself; no Developer ID marker; the code-signing usage
  # instead of the installer one; a CA certificate; one subject attribute other than AWS's.
  sed '/^1\.2\.840\.113635\.100\.6\.1\.14=/d' "$d/installer.ext" > "$d/no-marker.ext"
  sed 's/^extendedKeyUsage=.*/extendedKeyUsage=critical,codeSigning/' "$d/installer.ext" > "$d/wrong-eku.ext"
  sed 's/^basicConstraints=.*/basicConstraints=critical,CA:TRUE/' "$d/installer.ext" > "$d/ca-leaf.ext"
  local good="/UID=$team/CN=Developer ID Installer: $org ($team)/OU=$team/O=$org/C=US"
  issue direct root "$good" installer.ext
  issue no-marker int "$good" no-marker.ext
  issue wrong-eku int "$good" wrong-eku.ext
  issue ca-leaf int "$good" ca-leaf.ext
  issue ou-other int "/UID=$team/CN=Developer ID Installer: $org ($team)/OU=ABCDE12345/O=$org/C=US" installer.ext
  issue uid-other int "/UID=ABCDE12345/CN=Developer ID Installer: $org ($team)/OU=$team/O=$org/C=US" installer.ext
  issue o-other int "/UID=$team/CN=Developer ID Installer: $org ($team)/OU=$team/O=Other LLC/C=US" installer.ext
  # An attribute twice: the team's OU and another, and AWS's CN and another.
  issue ou-twice int "/UID=$team/CN=Developer ID Installer: $org ($team)/OU=$team/OU=OTHER/O=$org/C=US" installer.ext
  issue cn-twice int "/UID=$team/CN=Developer ID Installer: $org ($team)/CN=Other/OU=$team/O=$org/C=US" installer.ext
  # One more critical extension than the role allows.
  { cat "$d/installer.ext"; echo "1.2.3.4=critical,DER:0500"; } > "$d/crit-other.ext"
  { cat "$d/installer.ext"; echo "certificatePolicies=critical,1.2.3.4"; } > "$d/crit-policy.ext"
  sed 's/^subjectKeyIdentifier=hash$/subjectKeyIdentifier=critical,hash/' "$d/installer.ext" > "$d/crit-ski.ext"
  issue crit-other int "$good" crit-other.ext
  issue crit-policy int "$good" crit-policy.ext
  issue crit-ski int "$good" crit-ski.ext
  root root2 "Test Root CA"
  issue int2 root2 "/CN=Developer ID Certification Authority/OU=G2/O=Test Root Inc./C=US" ca.ext
  issue rogue int2 "/UID=$team/CN=Developer ID Installer: $org ($team)/OU=$team/O=$org/C=US" installer.ext
  [[ -s "$d/rogue.pem" && -s "$d/forged.pem" && -s "$d/wrong-role.pem" && -s "$d/crit-ski.pem" ]]
}

header="$repo/src/transport/odbc_driver.hpp"
# entry_points: the interface's entry points, one per line.
entry_points() {
  python3 -I -c 'import re, sys
text = open(sys.argv[1]).read()
match = re.search(r"#define REDSHIFT_ODBC_ENTRY_POINTS\(X\)((?:[^\n]*\\\n)*[^\n]*)", text)
print("\n".join("SQL" + n for n in re.findall(r"X\(([A-Za-z0-9_]+)\)", match.group(1))))' "$header"
}
# entry_points_json [<name> | -<name>]...: the same names as a JSON list, for make_macho.py's
# exports, without each -<name> and with each <name> added at the end.
entry_points_json() {
  entry_points | python3 -I -c 'import json, sys
args = sys.argv[1:]
drop = [a[1:] for a in args if a.startswith("-")]
print(json.dumps([n for n in sys.stdin.read().split() if n not in drop] + [a for a in args if not a.startswith("-")]))' "$@"
}
tiny_library() {
  local exports
  exports="$(entry_points_json)"
  python3 -I "$here/make_macho.py" "$1" "{\"exports\": $exports, \"strings\": [\"SSL_CERT_FILE\"]}"
}

# mac_tree <dir> <pki>: the repository's tools, policy and notices, with the test PKI's root and
# intermediate as the pinned certificates and the export and environment lists of tiny_library.
mac_tree() {
  mkdir -p "$1/signing/macos"
  cp -R "$odbc/tools" "$odbc/policy" "$odbc/LICENSE" "$odbc/NOTICE" "$odbc/THIRD_PARTY_LICENSES" "$1/"
  cp "$2/root.pem" "$1/signing/macos/AppleRootCA.pem"
  cp "$2/int.pem" "$1/signing/macos/DeveloperIDG2CA.pem"
  entry_points > "$1/policy/macos/expected-exports.txt"
  printf 'SSL_CERT_FILE  CaFile\n' > "$1/policy/known-environment.txt"
  : > "$1/policy/refused-environment.txt"
  : > "$1/policy/ignored-strings.txt"
}

mkdir -p "$tmp/serve"
export FAKE_CURL_DIR="$tmp/serve"
# serve_notices [<dir>]: the release tag's notices, which equal the checked-in copies by the lock.
serve_notices() { cp "$odbc/LICENSE" "$odbc/NOTICE" "$odbc/THIRD_PARTY_LICENSES" "${1:-$FAKE_CURL_DIR}/"; }
