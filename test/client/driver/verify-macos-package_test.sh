#!/usr/bin/env bash
# Tool tests for macOS package verification: lib/macos_signer.sh's package rules,
# lib/x509.py's installer and CMS rules, and lib/verify_macos.sh from the pinned certificates to
# the lock rows, downloaded or read from the retained copies. The unmodified locked package passes
# with the repository's certificates while its signer is valid; with a test PKI generated per run,
# each rule has a failing package, signed or changed by make_xar.py, that fails at that rule.
# verify_macos.sh downloads through the stand-in curl of verify-test-lib.sh, and every case that
# must not download runs it with FAKE_CURL=fail, so no case can reach the real curl. Run by
# run-tool-tests.sh.
set -euo pipefail

tmp="$(mktemp -d)"
trap 'rm -rf -- "${tmp:?}"' EXIT
# shellcheck source=macos-test-lib.sh
source "$(dirname "${BASH_SOURCE[0]}")/macos-test-lib.sh"

if sha1_refused; then skip "$(basename "$0")" "$no_sha1"; finish; fi
mpki="$tmp/mpki"
make_pki "$mpki" || { fail "a test PKI is generated"; finish; }
lib_path=./AmazonRedshiftODBC.pkg/Payload/librsodbc64.dylib
aws_installer="Developer ID Installer: AMZN Mobile LLC"
signed_by() { # signed_by <cert> [<key>]: a sign spec with that certificate, the test intermediate and root
  printf '{"key": "%s", "certs": ["%s", "%s", "%s"]}' "$mpki/${2:-$1}.key" "$mpki/$1.pem" "$mpki/int.pem" "$mpki/root.pem"
}
# package <json spec>: writes $tmp/p.pkg. rule [<int> <root>]: runs the package rules on it, with
# the test PKI unless other anchors are given, and $alg, $installer and $team when set; sets
# out_text and status.
package() { python3 -I "$here/make_xar.py" "$tmp/p.pkg" "$1"; }
rule() {
  out_text="$(bash "$lib/macos_signer.sh" package "${1:-$mpki/int.pem}" "${2:-$mpki/root.pem}" "${alg:-sha1}" \
    "${installer:-$aws_installer}" "${team:-94KV3E626L}" "$tmp/p.pkg" "$lib_path" 2>&1)" && status=0 || status=$?
}
repo_rule() { # repo_rule <package> [env...]: the package rules with the repository's own certificates
  local p="$1"; shift
  out_text="$(env "$@" bash "$lib/macos_signer.sh" package "$odbc/signing/macos/DeveloperIDG2CA.pem" \
    "$odbc/signing/macos/AppleRootCA.pem" sha1 "$aws_installer" 94KV3E626L "$p" "$mac_path" 2>&1)" && status=0 || status=$?
}

# The real package is verified at the current time; once its signer has expired these cases skip.
real="$(real_package)"
if [[ -z "$real" ]]; then skip "the unmodified locked package passes the package rules" "$no_real"
elif ! real_valid; then skip "the unmodified locked package passes the package rules" "$expired"
else
  repo_rule "$real"
  expect "the unmodified locked package passes the package rules with the repository's certificates" 0 \
    "the package is signed (RSA and CMS, sha1 checksum) by the pinned installer identity"
  # The pinned identity's certificate is public: a package that carries it, with an RSA signature by
  # another key and a CMS room that is not BER, passes the signer checks and fails at the RSA
  # signature, so the CMS signature is never parsed. The CMS key and signer only let make_xar.py
  # sign; cms-garbage replaces what it signs.
  mkdir "$tmp/real-parts"; python3 -I "$lib/xar.py" signed "$real" "$tmp/real-parts" sha1
  openssl x509 -inform DER -in "$tmp/real-parts/signer.der" -out "$tmp/real-signer.pem"
  package "{\"sign\": {\"key\": \"$mpki/installer.key\", \"certs\": [\"$tmp/real-signer.pem\", \"$odbc/signing/macos/DeveloperIDG2CA.pem\"], \"cms_key\": \"$mpki/installer.key\", \"cms_signer\": \"$mpki/installer.pem\"}, \"break\": [\"cms-garbage\"]}"
  repo_rule "$tmp/p.pkg"
  expect "the pinned certificate with another key's RSA signature fails at the RSA signature, before the CMS signature is parsed" 1 \
    "signature: the RSA signature does not verify over the stored checksum with the signing certificate's key"
fi

package "{\"sign\": $(signed_by installer)}"; rule
expect "a package signed by the test installer identity passes with the test PKI" 0 "signed (RSA and CMS, sha1 checksum)"
package "{\"sign\": {\"key\": \"$mpki/installer.key\", \"certs\": [\"$mpki/installer.pem\"], \"cms\": false}}"; rule
expect "an RSA signature alone fails: 2.3.0.0 has both" 1 "signature: the package's signatures are not one RSA and one CMS signature"
package "{\"sign\": $(signed_by installer)}"; repo_rule "$tmp/p.pkg"
expect "the test-signed package fails with the repository's certificates" 1 "chain: the certificate does not chain to the pinned Apple root"

package '{}'; rule
expect "an unsigned package fails" 1 "signature: the package is not signed"
package "{\"sign\": $(signed_by other-team)}"; rule
expect "a package signed with another Team ID fails" 1 "subject: the certificate's subject has CN='Developer ID Installer: AMZN Mobile LLC (ABCDE12345)'"
package "{\"sign\": $(signed_by installer)}"; team=ABCDE12345 rule
expect "the test identity fails when signer.txt names another team" 1 "subject: the certificate's subject has CN='Developer ID Installer: AMZN Mobile LLC (94KV3E626L)'; exactly CN=Developer ID Installer: AMZN Mobile LLC (ABCDE12345) is required"
installer="Developer ID Installer: Other LLC" rule
expect "the test identity fails when signer.txt names another installer" 1 "subject: the certificate's subject has CN='Developer ID Installer: AMZN Mobile LLC (94KV3E626L)'"
package "{\"sign\": $(signed_by forged)}"; rule
expect "a forged self-signed certificate with AWS's subject fails" 1 "chain: the certificate does not chain"
package "{\"sign\": {\"key\": \"$mpki/rogue.key\", \"certs\": [\"$mpki/rogue.pem\", \"$mpki/int2.pem\", \"$mpki/root2.pem\"]}}"; rule
expect "AWS's identity issued under another root fails" 1 "chain: the certificate does not chain"
package "{\"sign\": $(signed_by wrong-role)}"; rule
expect "a Developer ID Application certificate fails as the package signer" 1 \
  "usage: the certificate has critical extensions this tool does not accept: 1.2.840.113635.100.6.1.13"
package "{\"sign\": $(signed_by crit-other)}"; rule
expect "an installer certificate with a critical extension that is not Apple's fails" 1 \
  "usage: the certificate has critical extensions this tool does not accept: 1.2.3.4"
package "{\"sign\": $(signed_by direct)}"; rule
expect "a certificate issued by the pinned root itself fails the chain's shape" 1 "chain: the chain is not the certificate, the pinned intermediate and the pinned root"
package "{\"sign\": $(signed_by no-marker)}"; rule
expect "a certificate without the Developer ID Installer marker fails" 1 "usage: the certificate is not a Developer ID Installer certificate (no 1.2.840.113635.100.6.1.14 extension)"
package "{\"sign\": $(signed_by wrong-eku)}"; rule
expect "a certificate without the installer usage fails" 1 "eku: the certificate's extended key usage (1.3.6.1.5.5.7.3.3) does not include Developer ID Installer"
package "{\"sign\": $(signed_by ca-leaf)}"; rule
expect "a CA certificate as the signer fails" 1 "usage: the signing certificate is a CA certificate"
package "{\"sign\": $(signed_by ou-other)}"; rule
expect "another OU fails" 1 "subject: the certificate's subject has OU='ABCDE12345'"
package "{\"sign\": $(signed_by uid-other)}"; rule
expect "another UID fails" 1 "subject: the certificate's subject has UID='ABCDE12345'"
package "{\"sign\": $(signed_by o-other)}"; rule
expect "another O fails" 1 "subject: the certificate's subject has O='Other LLC'"
package "{\"sign\": $(signed_by ou-twice)}"; rule
expect "a second OU fails" 1 "subject: the certificate's subject has OU='94KV3E626L' + 'OTHER'"
package "{\"sign\": $(signed_by cn-twice)}"; rule
expect "a second CN fails" 1 "subject: the certificate's subject has CN='Developer ID Installer: AMZN Mobile LLC (94KV3E626L)' + 'Other'"
package "{\"sign\": $(signed_by installer), \"break\": [\"signature-style\"]}"; rule
expect "a signature that is not RSA fails" 1 "signature: the package's signatures are not one RSA and one CMS signature"
package "{\"sign\": $(signed_by installer), \"break\": [\"two-signatures\"]}"; rule
expect "two RSA signatures fail" 1 "signature: the package's signatures are not one RSA and one CMS signature"
package "{\"sign\": $(signed_by installer), \"break\": [\"two-cms\"]}"; rule
expect "two CMS signatures fail" 1 "signature: the package's signatures are not one RSA and one CMS signature"
package "{\"sign\": $(signed_by installer), \"break\": [\"cms-style\"]}"; rule
expect "a CMS signature of another style fails" 1 "signature: the package's signatures are not one RSA and one CMS signature"
package "{\"sign\": {\"key\": \"$mpki/installer.key\", \"certs\": [\"$mpki/installer.pem\", \"$mpki/int.pem\"], \"cms_signer\": \"$mpki/other-team.pem\", \"cms_key\": \"$mpki/other-team.key\"}, \"break\": [\"cms-signer-other\"]}"; rule
expect "RSA and CMS signatures naming different signers fail" 1 "signature: the RSA and CMS signatures name different signing certificates"
package "{\"sign\": $(signed_by installer), \"break\": [\"cms-trailing\"]}"; rule
expect "data after the CMS signature in its room fails" 1 "signature: the CMS signature is not one this tool accepts"
package "{\"sign\": $(signed_by installer), \"break\": [\"checksum-size-short\"]}"; rule
expect "a stored checksum of the wrong size fails at its location" 1 "checksum-location: the stored checksum is 19 bytes; sha1 needs 20"
package "{\"sign\": $(signed_by installer), \"break\": [\"toc-changed\"]}"; rule
expect "one changed table-of-contents byte fails at the checksum" 1 "checksum: the stored checksum"
package "{\"sign\": $(signed_by installer)}"
python3 -I -c 'import sys; p = sys.argv[1]; d = bytearray(open(p, "rb").read()); d[40] ^= 1; open(p, "wb").write(d)' "$tmp/p.pkg"; rule
expect "one changed compressed table-of-contents byte fails before any signature is read" 1 "package: the table of contents is not a zlib stream"
package "{\"sign\": $(signed_by installer), \"break\": [\"payload-changed\"]}"; rule
expect "one changed payload byte fails at the payload checksum, after the signer passed" 1 "payload: AmazonRedshiftODBC.pkg/Payload's archived checksum does not match"
package "{\"alg\": \"md5\", \"sign\": $(signed_by installer)}"; rule
expect "a header naming another checksum algorithm than sha1 fails at the package" 1 \
  "package: the xar header has size 28 and checksum algorithm 2; this reader accepts only size 28 and sha1 (1)"
package "{\"sign\": $(signed_by installer), \"break\": [\"checksum-style\"]}"; rule
expect "a table of contents naming another algorithm than the header fails" 1 "algorithm: the table of contents names the checksum algorithm 'sha256', the header sha1"
package "{\"sign\": $(signed_by installer)}"; alg=sha256 rule
expect "a signer.txt algorithm the reader does not compute fails in one line" 1 "algorithm: signer.txt accepts sha256, which this reader does not compute (sha1)"
[[ "$out_text" != *Traceback* && "$out_text" != *$'\n'* ]] && pass "that failure is one line, no traceback" || fail "that failure is one line ($out_text)"
package "{\"sign\": $(signed_by installer), \"break\": [\"checksum-outside\"]}"; rule
expect "a stored-checksum offset outside the file fails before any checksum is compared" 1 "checksum-location: the stored checksum at heap offset 1073741824, size 20, lies outside the file"
package "{\"sign\": $(signed_by installer), \"break\": [\"checksum-size-outside\"]}"; rule
expect "a stored-checksum offset plus size outside the file fails before any checksum is compared" 1 "checksum-location: the stored checksum at heap offset 0, size 1073741824, lies outside the file"
package "{\"sign\": $(signed_by installer), \"break\": [\"signature-outside\"]}"; rule
expect "a signature location outside the file fails" 1 "signature: the RSA signature at heap offset"
package "{\"sign\": $(signed_by installer), \"break\": [\"cms-outside\"]}"; rule
expect "a CMS signature location outside the file fails" 1 "signature: the CMS signature at heap offset"
package "{\"sign\": $(signed_by installer), \"break\": [\"rsa-corrupt\"]}"; rule
expect "a changed RSA signature fails" 1 "signature: the RSA signature does not verify over the stored checksum with the signing certificate's key"
package "{\"sign\": {\"key\": \"$mpki/installer.key\", \"certs\": [\"$mpki/installer.pem\", \"$mpki/int.pem\"], \"cms_key\": \"$mpki/other-team.key\", \"cms_signer\": \"$mpki/other-team.pem\"}}"; rule
expect "a CMS signature by another certificate than the one the package names fails" 1 "signature: the CMS signature does not verify over the stored checksum by the signing certificate"
package "{\"payload_alg\": \"md5\", \"sign\": $(signed_by installer)}"; rule
expect "a Payload whose checksums use another algorithm than signer.txt's fails at the payload" 1 \
  "payload: AmazonRedshiftODBC.pkg/Payload's archived checksum uses the algorithm 'md5', which is not accepted"
package "{\"sign\": $(signed_by installer), \"big\": [[\"Big\", $(((64 << 20) + 1)), 1]]}"; rule
expect "a file that declares a decoded size over 64 MiB fails before the signer is read" 1 \
  "package: the table of contents gives Big a decoded size of $(((64 << 20) + 1)) bytes"
package "{\"sign\": $(signed_by installer), \"break\": [\"no-keyinfo\"]}"; rule
expect "a signature that names no certificate fails" 1 "signature: the RSA signature has no certificates"
package "{\"sign\": $(signed_by installer), \"break\": [\"bad-base64\"]}"; rule
expect "a certificate that is not base64 fails" 1 "signature: a certificate of the RSA signature is not valid base64"
package "{\"sign\": $(signed_by installer), \"break\": [\"cert-not-x509\"]}"; rule
expect "a signing certificate openssl cannot read fails" 1 "certificate: the package's signing certificate is not a certificate openssl reads"
package "{\"sign\": $(signed_by installer), \"break\": [\"cms-garbage\"]}"; rule
expect "a CMS room that is not BER fails at the signature" 1 "signature: the CMS signature is not one this tool accepts"
package "{\"sign\": $(signed_by forged), \"break\": [\"cms-garbage\"]}"; rule
expect "an unpinned signer fails at the chain before its CMS signature is parsed" 1 "chain: the certificate does not chain"
package "{\"sign\": {\"key\": \"$mpki/other-team.key\", \"certs\": [\"$mpki/installer.pem\", \"$mpki/int.pem\"], \"cms_key\": \"$mpki/installer.key\"}, \"break\": [\"cms-garbage\"]}"; rule
expect "the test installer's certificate with another key's RSA signature fails at the RSA signature, before the CMS signature is parsed" 1 \
  "signature: the RSA signature does not verify over the stored checksum with the signing certificate's key"
package "{\"sign\": {\"key\": \"$mpki/installer.key\", \"certs\": [\"$mpki/installer.pem\", \"$mpki/int.pem\"], \"cms_nodetach\": true}}"; rule
expect "a CMS signature that carries its content fails" 1 "signature: the CMS signature is not one this tool accepts: its content is not detached"
package "{\"sign\": {\"key\": \"$mpki/installer.key\", \"certs\": [\"$mpki/installer.pem\", \"$mpki/int.pem\"], \"cms_second\": [\"$mpki/other-team.key\", \"$mpki/other-team.pem\"]}}"; rule
expect "a CMS signature with two signers fails" 1 "signature: the CMS signature is not one this tool accepts: it does not have exactly one signer"

# The trust store is only the pinned root: TLS variables that name the test intermediate and root,
# as a bundle and as a hashed directory, would let openssl build a chain to them; with the
# isolation the test-signed package still fails with the repository's certificates, at the first
# chain rule rather than at the chain's shape.
cat "$mpki/int.pem" "$mpki/root.pem" > "$tmp/bundle.pem"
mkdir -p "$tmp/certdir"; cp "$mpki/int.pem" "$mpki/root.pem" "$tmp/certdir/"; openssl rehash "$tmp/certdir" 2>/dev/null
package "{\"sign\": $(signed_by installer)}"
repo_rule "$tmp/p.pkg" SSL_CERT_FILE="$tmp/bundle.pem" SSL_CERT_DIR="$tmp/certdir" OPENSSL_CONF=/dev/null
expect "TLS variables naming the test PKI, as a file and a hashed directory, add no trust" 1 \
  "chain: the certificate does not chain to the pinned Apple root through the pinned intermediate"
# openssl verify is given no CA directory or store besides the pinned root, on any host: the
# stand-in openssl records the chain check's options.
rm -f "$OPENSSL_LOG"; PATH="$tmp/openssl-bin:$PATH" rule
want_store=yes; [[ "$(openssl verify -help 2>&1 || true)" == *-no-CAstore* ]] || want_store=no
if [[ "$status" == 0 ]] && openssl_args verify -no-CApath && openssl_args verify -ignore_critical &&
  { [[ "$want_store" == no ]] || openssl_args verify -no-CAstore; }; then
  pass "the chain check passes -no-CApath, and -no-CAstore where openssl offers it, with -ignore_critical"
else fail "the chain check's options (exit $status; $(tr '\n' ' ' < "$OPENSSL_LOG"))"; fi
# An OpenSSL configuration that loads a provider that does not exist leaves openssl with no
# digest at all, so the package passes only because the rules run without OPENSSL_CONF.
printf 'openssl_conf = c\n[c]\nproviders = p\n[p]\nnosuch = n\n[n]\nactivate = 1\n' > "$tmp/no-provider.cnf"
OPENSSL_CONF="$tmp/no-provider.cnf" rule
expect "OPENSSL_CONF does not reach openssl" 0 "signed (RSA and CMS, sha1 checksum)"
# The chain is checked at the current time: a test intermediate that has expired fails it.
openssl req -new -key "$mpki/int.key" -subj "/CN=Developer ID Certification Authority/OU=G2/O=Test Root Inc./C=US" \
  -out "$tmp/old-int.csr" 2>/dev/null
printf 'basicConstraints=critical,CA:TRUE,pathlen:0\nkeyUsage=critical,keyCertSign,cRLSign\nsubjectKeyIdentifier=hash\n' > "$tmp/old-int.ext"
if openssl x509 -req -in "$tmp/old-int.csr" -CA "$mpki/root.pem" -CAkey "$mpki/root.key" -set_serial 7 -days -1 \
  -extfile "$tmp/old-int.ext" -out "$tmp/old-int.pem" 2>/dev/null ||
  openssl x509 -req -in "$tmp/old-int.csr" -CA "$mpki/root.pem" -CAkey "$mpki/root.key" -set_serial 7 \
    -not_before 20200101000000Z -not_after 20200102000000Z -extfile "$tmp/old-int.ext" -out "$tmp/old-int.pem" 2>/dev/null; then
  rule "$tmp/old-int.pem"
  expect "an intermediate that has expired fails at the chain, checked at the current time" 1 "chain: the certificate does not chain"
else skip "an intermediate that has expired fails at the chain" "this openssl cannot write an expired certificate"; fi

# The openssl on PATH.
mkdir -p "$tmp/no-openssl" "$tmp/old-openssl" "$tmp/broken-openssl"
for c in bash dirname env python3; do ln -s "$(command -v "$c")" "$tmp/no-openssl/$c"; done
out_text="$(PATH="$tmp/no-openssl" bash "$lib/macos_signer.sh" openssl 2>&1)" && status=0 || status=$?
expect "no openssl on PATH fails" 1 "openssl: openssl is required and was not found on PATH"
printf '#!/bin/sh\necho "OpenSSL 1.0.2k-fips  26 Jan 2017"\n' > "$tmp/old-openssl/openssl"; chmod +x "$tmp/old-openssl/openssl"
out_text="$(PATH="$tmp/old-openssl:$PATH" bash "$lib/macos_signer.sh" openssl 2>&1)" && status=0 || status=$?
expect "an OpenSSL older than 1.1.0 fails" 1 "openssl: verification needs OpenSSL 1.1.0 or later, and openssl on PATH is: OpenSSL 1.0.2k"
out_text="$(bash "$lib/macos_signer.sh" openssl 2>&1)" && status=0 || status=$?
expect "this host's openssl passes the openssl rule" 0 ""

# A host whose openssl refuses SHA-1 signatures, as RHEL 9's DEFAULT crypto policy does: stand-in
# openssls that refuse the sha1 digest in RSA signatures (pkeyutl), refuse to verify a CMS signature
# whose digest is SHA-1 (one that holds SHA-1's algorithm identifier, 06 05 2b 0e 03 02 1a), or
# both, and run the real one otherwise. Verifying the package is what tests the host: the failure
# is at signature, and names the host's crypto policy as a cause.
nosha1() { # nosha1 <dir> <pkeyutl|cms|both>
  mkdir -p "$1"
  printf '%s\n' '#!/usr/bin/env bash' \
    "[[ $2 == cms ]] || for a in \"\$@\"; do" \
    '  [[ "$a" != digest:sha1 ]] || { echo "pkeyutl: Can'"'"'t set parameter digest:sha1: digest not allowed" >&2; exit 1; }' \
    'done' \
    "if [[ $2 != pkeyutl && \"\$1 \${2:-}\" == \"cms -verify\" ]]; then" \
    '  prev=""; for a in "$@"; do [[ "$prev" != -in ]] || input="$a"; prev="$a"; done' \
    '  od -An -v -tx1 "$input" | tr -d " \n" | grep -q 06052b0e03021a && { echo "cms: digest not allowed" >&2; exit 1; }' \
    'fi' \
    "exec $(command -v openssl) \"\$@\"" > "$1/openssl"
  chmod +x "$1/openssl"
}
nosha1 "$tmp/nosha1-openssl" both; nosha1 "$tmp/nosha1-pkeyutl" pkeyutl; nosha1 "$tmp/nosha1-cms" cms
for o in openssl pkeyutl cms; do
  PATH="$tmp/nosha1-$o:$PATH" sha1_refused && [[ "$no_sha1" == *"digest not allowed)" ]] &&
    pass "the suites' SHA-1 probe sees the $o stand-in's refusal and quotes it" || fail "the SHA-1 probe and the $o stand-in ($no_sha1)"
done
# An openssl that fails every RSA signature, or every CMS verification, is broken, not refusing
# SHA-1: the probe's SHA-256 control fails too, so the cases run and fail rather than skip.
mkdir -p "$tmp/nosign" "$tmp/nocmsverify"
printf '#!/usr/bin/env bash\n[[ "$1" != pkeyutl ]] || { echo "pkeyutl: broken" >&2; exit 1; }\nexec %s "$@"\n' "$(command -v openssl)" > "$tmp/nosign/openssl"
printf '#!/usr/bin/env bash\n[[ "$1 ${2:-}" != "cms -verify" ]] || { echo "cms: broken" >&2; exit 1; }\nexec %s "$@"\n' "$(command -v openssl)" > "$tmp/nocmsverify/openssl"
chmod +x "$tmp/nosign/openssl" "$tmp/nocmsverify/openssl"
PATH="$tmp/nosign:$PATH" sha1_refused && fail "the SHA-1 probe takes an openssl that signs nothing for a SHA-1 refusal" ||
  pass "the SHA-1 probe does not take an openssl that signs nothing for a SHA-1 refusal"
PATH="$tmp/nocmsverify:$PATH" sha1_refused && fail "the SHA-1 probe takes an openssl whose cms -verify always fails for a SHA-1 refusal" ||
  pass "the SHA-1 probe does not take an openssl whose cms -verify always fails for a SHA-1 refusal"
policy_hint="or a host crypto policy can refuse sha1 signatures (RHEL 9's DEFAULT refuses SHA-1), so verify on a host whose OpenSSL allows them"
package "{\"sign\": $(signed_by installer)}"
PATH="$tmp/nosha1-openssl:$PATH" rule
expect "an openssl that refuses SHA-1 signatures fails at the signature, naming the host's policy" 1 \
  "signature: the RSA signature does not verify over the stored checksum with the signing certificate's key, $policy_hint: pkeyutl:"
PATH="$tmp/nosha1-pkeyutl:$PATH" rule
expect "an openssl that refuses SHA-1 in RSA signatures alone fails at the RSA signature" 1 "$policy_hint: pkeyutl: Can't set parameter digest:sha1"
PATH="$tmp/nosha1-cms:$PATH" rule
expect "an openssl that refuses SHA-1 in CMS signatures alone fails at the CMS signature" 1 \
  "signature: the CMS signature does not verify over the stored checksum by the signing certificate, $policy_hint: cms: digest not allowed"

# macos_signer.sh's own arguments, and xar.py signed's out directory.
out_text="$(bash "$lib/macos_signer.sh" package a b 2>&1)" && status=0 || status=$?
expect "a wrong argument count is a usage error" 2 "usage: macos_signer.sh openssl"
out_text="$(bash "$lib/macos_signer.sh" pin a b c 2>&1)" && status=0 || status=$?
expect "the old pin subcommand is a usage error" 2 "usage: macos_signer.sh openssl"
package "{\"sign\": $(signed_by installer)}"; rm -rf -- "${tmp:?}/parts"; mkdir "$tmp/parts"; : > "$tmp/parts/x"
out_text="$(python3 -I "$lib/xar.py" signed "$tmp/p.pkg" "$tmp/parts" sha1 2>&1)" && status=0 || status=$?
expect "xar.py signed refuses an out directory that is not empty" 1 "$tmp/parts is not empty"

# lib/x509.py cms run directly: the one DER reader, with its BER flag.
cms_case() { # cms_case <name> <exit> <substring> <python expression for the bytes>
  python3 -I -c "import sys; open(sys.argv[1], 'wb').write($4)" "$tmp/c.ber"
  rm -f -- "$tmp/c.der"
  out_text="$(python3 -I "$lib/x509.py" cms "$tmp/c.ber" "$tmp/c.der" 2>&1)" && status=0 || status=$?
  expect "$1" "$2" "$3"
}
# SignedData { version, digest algorithms, detached content, signers } with one (empty) signer: the
# smallest CMS the rule accepts, in DER, in BER with a length longer than it needs, and in BER with
# indefinite lengths, each followed by the zero bytes of its room.
sd='b"\x02\x01\x01\x31\x00\x30\x0b\x06\x09\x2a\x86\x48\x86\xf7\x0d\x01\x07\x01\x31\x02\x30\x00"'
oid='b"\x06\x09\x2a\x86\x48\x86\xf7\x0d\x01\x07\x02"'
cms_case "a minimal detached SignedData with one signer passes" 0 "" "b'\\x30\\x25' + $oid + b'\\xa0\\x18\\x30\\x16' + $sd + bytes(40)"
python3 -I -c 'import sys; d = open(sys.argv[1], "rb").read(); sys.exit(len(d) != 39)' "$tmp/c.der" &&
  pass "its encoding is written without the room's zero bytes" || fail "the written CMS ($(wc -c < "$tmp/c.der") bytes)"
cms_case "a BER length longer than it needs passes" 0 "" "b'\\x30\\x81\\x25' + $oid + b'\\xa0\\x18\\x30\\x16' + $sd"
cms_case "BER indefinite lengths pass" 0 "" "b'\\x30\\x80' + $oid + b'\\xa0\\x80\\x30\\x80' + $sd + bytes(6)"
cms_case "a CMS room over 1 MiB fails" 1 "signature: the CMS signature's room is larger than 1048576 bytes" 'bytes((1 << 20) + 1)'
cms_case "CMS elements nested too deep fail in one line" 1 "signature: the CMS signature nests its elements too deeply" 'b"\x30\x80" * 5000'
cms_case "a CMS signature that is not a SEQUENCE fails" 1 "it is not one SEQUENCE followed only by zero bytes" 'b"\x31\x00"'
cms_case "a CMS signature that is not SignedData fails" 1 "it is not SignedData" 'b"\x30\x0f\x06\x09\x2a\x86\x48\x86\xf7\x0d\x01\x07\x01\xa0\x02\x30\x00"'
cms_case "an indefinite length without its end fails" 1 "an indefinite-length element has no end" 'b"\x30\x80\x02\x01\x00"'
cms_case "an indefinite length whose end lies past its parent fails" 1 "an indefinite-length element has no end" \
  'b"\x30\x02\x30\x80\x00\x00"'
cms_case "a high tag number fails" 1 "high tag numbers are not used here" 'b"\x3f\x01\x00"'
cms_case "a length of more than four bytes fails" 1 "bad length" 'b"\x30\x85\x00\x00\x00\x00\x01\x00"'
cms_case "a CMS signature cut inside an element's header fails" 1 "truncated element" 'b"\x30"'
cms_case "SignedData whose content is not in a [0] element fails" 1 "it is not SignedData" \
  'b"\x30\x0f\x06\x09\x2a\x86\x48\x86\xf7\x0d\x01\x07\x02\xa1\x02\x30\x00"'
cms_case "SignedData whose last element is not a SET of signers fails" 1 "it does not have exactly one signer" \
  'b"\x30\x25\x06\x09\x2a\x86\x48\x86\xf7\x0d\x01\x07\x02\xa0\x18\x30\x16\x02\x01\x01\x31\x00\x30\x0b\x06\x09\x2a\x86\x48\x86\xf7\x0d\x01\x07\x01\xa1\x02\x05\x00"'
cms_case "a primitive element of indefinite length fails" 1 "indefinite length where it is not allowed" \
  'b"\x30\x80\x04\x80\x00\x00\x00\x00"'
cms_case "a content type with a cut object identifier fails" 1 "malformed object identifier" \
  "b'\\x30\\x25\\x06\\x09\\x2a\\x86\\x48\\x86\\xf7\\x0d\\x01\\x07\\x82\\xa0\\x18\\x30\\x16' + $sd"
out_text="$(python3 -I "$lib/x509.py" cms "$tmp/no-such.ber" "$tmp/c.der" 2>&1)" && status=0 || status=$?
expect "an unreadable CMS signature fails" 1 "signature: cannot read $tmp/no-such.ber"

# lib/x509.py leaf installer run directly: the rules openssl's -ignore_critical leaves to it.
leaf() { # leaf <leaf.pem> [<eku>]
  out_text="$(python3 -I "$lib/x509.py" leaf "$1" "${2:-installer}" "CN=$aws_installer (94KV3E626L)" OU=94KV3E626L \
    UID=94KV3E626L "O=AMZN Mobile LLC" 2>&1)" && status=0 || status=$?
}
leaf "$mpki/installer.pem"
expect "the installer rule accepts AWS's test installer certificate" 0 ""
leaf "$tmp/no-such.pem"
expect "an unreadable signing certificate fails" 1 "certificate: cannot read $tmp/no-such.pem"
for c in "crit-other:1.2.3.4" "crit-policy:2.5.29.32" "crit-ski:2.5.29.14"; do
  leaf "$mpki/${c%%:*}.pem"
  expect "the installer rule refuses AWS's certificate with a critical ${c#*:}" 1 \
    "usage: the certificate has critical extensions this tool does not accept: ${c#*:}"
done
# The Linux code-signing rule does not apply the installer's Apple rules: the critical extension
# a Linux leaf never has is left to openssl there.
leaf "$mpki/crit-other.pem" codesigning
expect "the codesigning rule leaves critical extensions to openssl" 1 "eku: the certificate's extended key usage (1.2.840.113635.100.4.13) does not include Code Signing"
# bad_cert <kind> <out.pem>: a certificate-shaped DER with one encoding fault (signatures unchecked).
bad_cert() {
  python3 -I -c 'import base64, sys
def enc(tag, body):
    n = len(body)
    head = bytes([n]) if n < 0x80 else bytes([0x81, n]) if n < 0x100 else bytes([0x82]) + n.to_bytes(2, "big")
    return bytes([tag]) + head + body
def seq(*xs):
    return enc(0x30, b"".join(xs))
kind = sys.argv[1]
oid = lambda h: enc(0x06, bytes.fromhex(h))
name = seq(enc(0x31, seq(oid("550403"), enc(0x0C, b"x"))))
ext = lambda h, tag=0x04: seq(oid(h), enc(tag, bytes.fromhex("040100")))
flagged = lambda h, flag: seq(oid(h), enc(0x01, bytes([flag])), enc(0x04, bytes.fromhex("0500")))
exts = {"ext-twice": [ext("551d0e"), ext("551d0e")], "ext-not-octet": [ext("551d0e", 0x05)], "bad-oid": [ext("80")],
        "past-parent": [bytes.fromhex("307f00")], "flag-false": [flagged("2a0304", 0)],
        "flag-true": [flagged("2a0304", 0xFF)]}.get(kind, [ext("551d0e")])
serial = {"high-tag": "1f010101", "long-length": "02810101", "bad-length": "02850000000101",
          "leading-zero": "0283000090" + "01" * 0x90}
serial = bytes.fromhex(serial[kind]) if kind in serial else enc(0x02, b"\x01")
tbs = seq(enc(0xA0, enc(0x02, b"\x02")), serial, seq(oid("2a")), name, seq(), name, seq(), enc(0xA3, seq(*exts)))
if kind == "indefinite":
    tbs = bytes.fromhex("3080") + tbs[2:] + bytes(2)
der = seq(tbs, seq(oid("2a")), enc(0x03, bytes(1)))
der = {"trailing": der + bytes(1), "two-part": seq(tbs, seq(oid("2a")))}.get(kind, der)
text = base64.b64encode(der).decode()
open(sys.argv[2], "w").write("-----BEGIN CERTIFICATE-----\n%s\n-----END CERTIFICATE-----\n" % text)' "$1" "$2"
}
# Each extension's critical flag is its BOOLEAN's value, not its presence.
bad_cert flag-true "$tmp/flag.pem"; leaf "$tmp/flag.pem"
expect "an extension flagged critical TRUE is critical" 1 "usage: the certificate has critical extensions this tool does not accept: 1.2.3.4"
bad_cert flag-false "$tmp/flag.pem"; leaf "$tmp/flag.pem"
expect "an extension whose critical flag is FALSE is not critical" 1 "usage: the certificate is not a Developer ID Installer certificate"
# A certificate is DER: the BER the CMS reader accepts is refused in one.
for c in "trailing:not one DER SEQUENCE" "two-part:not a certificate" "ext-twice:extension 2.5.29.14 appears twice" \
  "ext-not-octet:extension 2.5.29.14 has no OCTET STRING value" "high-tag:high tag numbers are not used here" \
  "long-length:length not in its shortest form" "leading-zero:length not in its shortest form" \
  "bad-length:bad length" "past-parent:element runs past its parent" \
  "indefinite:indefinite length where it is not allowed" "bad-oid:malformed object identifier"; do
  bad_cert "${c%%:*}" "$tmp/bad.pem"; leaf "$tmp/bad.pem"
  expect "a certificate with ${c%%:*} fails as one this tool does not read" 1 "certificate: $tmp/bad.pem is not a certificate this tool reads: ${c#*:}"
done

# verify_macos.sh: step 1, download or retained copies, package rules, extraction, notices, the
# library's policy, rows. A package that gets past extraction holds a tiny library make_macho.py
# writes.
tiny_library "$tmp/lib.dylib"
libm="[\"./librsodbc64.dylib\", \"file\", 1, \"@$tmp/lib.dylib\"]"
tree="$tmp/tree"; mac_tree "$tree" "$mpki"
# verify <odbc-dir> [env...]: for $platform (osx_arm64) and $verify_version when set, without the
# suite's own RS_DRIVER_MIRROR and RS_DRIVER_LOCK unless a case passes them.
verify() {
  local d="$1"; shift; rm -f "$FAKE_CURL_LOG" "$FAKE_CURL_LOG.args" "$FAKE_CURL_LOG.env"
  out_text="$(env -u RS_DRIVER_MIRROR -u RS_DRIVER_LOCK PATH="$tmp/bin:$PATH" "$@" bash "$d/tools/lib/verify_macos.sh" \
    "${platform:-osx_arm64}" "${verify_version:-$version}" "$d" "$header" 2>&1)" && status=0 || status=$?
}
pkg_name="AmazonRedshiftODBC-64-bit.$version.universal.pkg"
official="https://s3.amazonaws.com/redshift-downloads/drivers/odbc/$version"
lock_rows() { awk '{ sub(/#.*/, "") } $1 ~ /^osx_/ || $1 == "*" { $1 = $1; print }' "$base_lock"; }
printed_rows() { awk '$1 ~ /^osx_/ || $1 == "*" { $1 = $1; print }' <<<"$out_text"; }
serve_notices
# The unmodified locked package passes every rule, the environment-name rule on both slices
# included, and prints the lock's macOS and notice rows, downloaded and from the retained copies.
if [[ -z "$real" ]]; then skip "the unmodified locked package passes verification" "$no_real"
elif ! real_valid; then skip "the unmodified locked package passes verification" "$expired"
else
  cp "$real" "$FAKE_CURL_DIR/$pkg_name"
  verify "$odbc"
  first="$status $(printed_rows)"
  if [[ "$status" == 0 && "$(printed_rows)" == "$(lock_rows)" && "$(head -1 "$FAKE_CURL_LOG")" == "$official/$pkg_name" ]]; then
    pass "the unmodified locked package passes verification and prints the lock's macOS and notice rows"
  else fail "the unmodified locked package passes verification and prints the lock's macOS and notice rows (exit $status; $out_text)"; fi
  verify "$odbc" SSL_CERT_FILE="$mpki/root.pem" SSL_CERT_DIR="$mpki" OPENSSL_CONF=/dev/null
  [[ "$status $(printed_rows)" == "$first" ]] && pass "TLS and OpenSSL variables change nothing for the real package" ||
    fail "TLS and OpenSSL variables change nothing for the real package ($out_text)"
  mkdir -p "$tmp/real-retained"; cp "$real" "$tmp/real-retained/$mac_pkg_sha"
  for f in LICENSE NOTICE THIRD_PARTY_LICENSES; do cp "$odbc/$f" "$tmp/real-retained/$mac_pkg_sha-$f"; done
  verify "$odbc" FAKE_CURL=fail RS_DRIVER_MIRROR="$tmp/real-retained" RS_DRIVER_LOCK="$base_lock"
  if [[ "$status" == 0 && "$(printed_rows)" == "$(lock_rows)" && ! -e "$FAKE_CURL_LOG" ]]; then
    pass "the locked package's retained copies pass offline and print the lock's rows"
  else fail "the retained copies of the locked package (exit $status; $out_text)"; fi
fi

package "{\"sign\": $(signed_by installer), \"members\": [$libm]}"; cp "$tmp/p.pkg" "$FAKE_CURL_DIR/$pkg_name"
verify "$tree"
cp "$tmp/p.pkg" "$tmp/signed.pkg" # kept for the retained copies below; each signing differs
lib_sha="$(sha "$tmp/lib.dylib")" pkg_sha="$(sha "$tmp/signed.pkg")"
want_driver="driver  librsodbc64.dylib  $lib_sha  $official/$pkg_name  $pkg_sha  $lib_path"
expect "a test-signed package passes verification in a tree pinning the test PKI" 0 "osx_arm64  $want_driver"
signed_first="$status $out_text"
[[ "$(grep -c "^osx_amd64  $want_driver\$" <<<"$out_text") $(grep -c "^osx_arm64  $want_driver\$" <<<"$out_text")" == "1 1" ]] &&
  pass "one run prints both macOS rows, alike but for their platform" || fail "both macOS rows ($out_text)"
[[ "$out_text" == *"*  notice  THIRD_PARTY_LICENSES  "*"  https://raw.githubusercontent.com/aws/amazon-redshift-odbc-driver/v${version%.*}/THIRD_PARTY_LICENSES"* ]] &&
  pass "the notices come from the release tag and the rows name it" || fail "notice rows ($out_text)"
# Nothing but the run's own temporary directories is written, and they are gone afterwards, after a
# pass and after a failure alike: an empty TMPDIR and working directory stay empty.
mkdir -p "$tmp/clean-tmp" "$tmp/clean-cwd"
( cd "$tmp/clean-cwd" && verify "$tree" TMPDIR="$tmp/clean-tmp"; echo "$status" > "$tmp/clean-status" )
( cd "$tmp/clean-cwd" && verify "$tree" TMPDIR="$tmp/clean-tmp" FAKE_CURL=404; echo "$status" >> "$tmp/clean-status" )
[[ "$(tr '\n' ' ' < "$tmp/clean-status")" == "0 1 " && -z "$(ls -A "$tmp/clean-tmp" "$tmp/clean-cwd" | grep -v ':$' || true)" ]] &&
  pass "verification leaves an empty TMPDIR and working directory empty, passing or failing" ||
  fail "verification left files behind: $(ls -A "$tmp/clean-tmp" "$tmp/clean-cwd")"
for c in AppleRootCA.pem:root DeveloperIDG2CA.pem:int; do
  [[ "$out_text" == *"verify-release: osx_arm64: the pinned ${c%%:*} has DER SHA-256 $(der_sha256 "$mpki/${c#*:}.pem")"* ]] &&
    pass "the run prints ${c%%:*}'s DER SHA-256, the fingerprint the approval records" || fail "${c%%:*}'s fingerprint ($out_text)"
done
platform=osx_amd64 verify "$tree"
[[ "$status" == 0 && "$(printed_rows)" == "$(awk '$1 ~ /^osx_/ || $1 == "*" { $1 = $1; print }' <<<"${signed_first#* }")" ]] &&
  pass "osx_amd64 verifies the same package once and prints the same two rows" || fail "osx_amd64's rows (exit $status; $out_text)"
# verify_macos.sh unsets OPENSSL_CONF for every step, as Python's hashlib (xar.py, macho.py) reads
# it too: with a configuration that asks for FIPS-only algorithms, hashlib fails if it is kept.
if hostile_case "with an OPENSSL_CONF that openssl and Python's hashlib apply, a test-signed package still passes"; then
  verify "$tree" OPENSSL_CONF="$hostile_conf"
  [[ "$status $out_text" == "$signed_first" ]] &&
    pass "with an OPENSSL_CONF that openssl and Python's hashlib apply, a test-signed package still passes" ||
    fail "with an OPENSSL_CONF that openssl and Python's hashlib apply, a test-signed package still passes (exit $status; $out_text)"
fi
verify "$odbc" SSL_CERT_FILE="$mpki/root.pem" SSL_CERT_DIR="$mpki" SSL_CERT_URI="file:$mpki/root.pem"
expect "the test-signed package fails with the repository's certificates even with TLS variables naming the test root" 1 "chain: the certificate does not chain"
verify "$tree" PATH="$tmp/nosha1-openssl:$tmp/bin:$PATH"
expect "an openssl that refuses SHA-1 signatures fails the run at the signature, naming the host's policy" 1 \
  "verify-release: osx_arm64: signature: the RSA signature does not verify over the stored checksum with the signing certificate's key, $policy_hint"
verify "$tree" PATH="$tmp/nosha1-cms:$tmp/bin:$PATH"
expect "an openssl that refuses the CMS signature fails the run at the signature, naming the host's policy" 1 \
  "verify-release: osx_arm64: signature: the CMS signature does not verify over the stored checksum by the signing certificate, $policy_hint"

package "{\"sign\": $(signed_by installer), \"members\": [[\"./librsodbc64.dylib\", \"link\"]]}"; cp "$tmp/p.pkg" "$FAKE_CURL_DIR/$pkg_name"
verify "$tree"
expect "a signed package with a link at the library's path fails at extraction" 1 "extract: cannot take $lib_path from the package: the member at ./librsodbc64.dylib is not a regular file"
package "{\"sign\": $(signed_by installer), \"members\": [$libm, [\"../escape\", \"file\"]]}"; cp "$tmp/p.pkg" "$FAKE_CURL_DIR/$pkg_name"
verify "$tree"
expect "a signed package with a ../ member fails at extraction" 1 "extract: cannot take $lib_path from the package: member b'../escape' has an absolute path"
package "{\"sign\": $(signed_by installer), \"members\": [$libm], \"package_info\": {\"alg\": \"md5\"}, \"payload_alg\": \"sha1\"}"
cp "$tmp/p.pkg" "$FAKE_CURL_DIR/$pkg_name"; verify "$tree"
expect "a rule's failure leads with its name, not xar.py's" 1 "verify-release: osx_arm64: product: AmazonRedshiftODBC.pkg/PackageInfo's archived checksum"

package "{\"sign\": $(signed_by installer), \"members\": [$libm]}"; cp "$tmp/p.pkg" "$FAKE_CURL_DIR/$pkg_name"
printf 'changed\n' > "$FAKE_CURL_DIR/THIRD_PARTY_LICENSES"
verify "$tree"
expect "a changed THIRD_PARTY_LICENSES at the source fails, naming it" 1 "notices: THIRD_PARTY_LICENSES from https://"
rm "$FAKE_CURL_DIR/THIRD_PARTY_LICENSES"
verify "$tree"
expect "a missing notice fails" 1 "download: downloading https://raw.githubusercontent.com/aws/amazon-redshift-odbc-driver/v${version%.*}/THIRD_PARTY_LICENSES failed"
serve_notices
mv "$tree/NOTICE" "$tmp/NOTICE.saved"; verify "$tree"
expect "a missing checked-in notice fails" 1 "notices: the checked-in NOTICE is missing"
mv "$tmp/NOTICE.saved" "$tree/NOTICE"
# The release tag's copy ships: the payload may hold each notice once beside the library, with the
# tag's bytes, as 2.3.1.0's does, and the rows still name the tag. Other bytes there, or a notice
# anywhere else, fail the run; one elsewhere fails before anything is downloaded.
package "{\"sign\": $(signed_by installer), \"members\": [$libm, [\"./LICENSE\", \"file\", 1, \"@$odbc/LICENSE\"]]}"
cp "$tmp/p.pkg" "$FAKE_CURL_DIR/$pkg_name"; verify "$tree"
expect "the release tag's notice beside the library passes, and the row names the tag" 0 \
  "*  notice  LICENSE  $(sha "$odbc/LICENSE")  https://raw.githubusercontent.com/aws/amazon-redshift-odbc-driver/v${version%.*}/LICENSE"
printf 'another licence text\n' > "$tmp/other-licence"
package "{\"sign\": $(signed_by installer), \"members\": [$libm, [\"./LICENSE\", \"file\", 1, \"@$tmp/other-licence\"]]}"
cp "$tmp/p.pkg" "$FAKE_CURL_DIR/$pkg_name"; verify "$tree"
expect "a notice beside the library with other bytes than the release tag's fails the run" 1 \
  "notices: the package's ./AmazonRedshiftODBC.pkg/Payload/LICENSE has SHA-256 $(sha "$tmp/other-licence"); LICENSE from https://"
package "{\"sign\": $(signed_by installer), \"members\": [$libm, [\"./a/NOTICE\", \"file\"], [\"./b/LICENSE\", \"link\"]]}"
cp "$tmp/p.pkg" "$FAKE_CURL_DIR/$pkg_name"; verify "$tree"
expect "every notice the package holds elsewhere is named, a link among them" 1 \
  "notices: the package holds ./AmazonRedshiftODBC.pkg/Payload/a/NOTICE, ./AmazonRedshiftODBC.pkg/Payload/b/LICENSE, named like a notice"
grep -q '/LICENSE$' "$FAKE_CURL_LOG" && fail "the release tag's LICENSE was downloaded before the refusal" || pass "nothing is downloaded once the package holds a notice elsewhere"
package "{\"sign\": $(signed_by installer), \"members\": [$libm], \"top_file\": \"LICENSE\"}"
cp "$tmp/p.pkg" "$FAKE_CURL_DIR/$pkg_name"; verify "$tree"
expect "a LICENSE outside every payload is not a notice; it comes from the release tag" 0 \
  "*  notice  LICENSE  $(sha "$odbc/LICENSE")  https://raw.githubusercontent.com/aws/amazon-redshift-odbc-driver/v${version%.*}/LICENSE"
package "{\"sign\": $(signed_by installer), \"members\": [$libm, [\"./a b/LICENSE\", \"file\"]]}"
cp "$tmp/p.pkg" "$FAKE_CURL_DIR/$pkg_name"; verify "$tree"
expect "a notice path with a space fails" 1 "notices: cannot read the package's members: member b'./a b/LICENSE' has a space"
package "{\"sign\": $(signed_by installer), \"members\": [$libm], \"other_component\": {\"raw\": \"not an archive\"}}"
cp "$tmp/p.pkg" "$FAKE_CURL_DIR/$pkg_name"; verify "$tree"
expect "another component's payload that cannot be read fails" 1 "notices: cannot read the package's members: the payload is not gzip compressed"
package "{\"sign\": $(signed_by installer), \"members\": [$libm], \"other_component\": {\"alg\": \"md5\", \"members\": [[\"./README\", \"file\"]]}}"
cp "$tmp/p.pkg" "$FAKE_CURL_DIR/$pkg_name"; verify "$tree"
expect "another Payload whose checksums use another algorithm than signer.txt's fails" 1 \
  "notices: cannot read the package's members: Other.pkg/Payload's archived checksum uses the algorithm 'md5', which is not accepted"
package "{\"sign\": $(signed_by installer), \"members\": [$libm], \"other_component\": {\"payload_type\": \"directory\"}}"
cp "$tmp/p.pkg" "$FAKE_CURL_DIR/$pkg_name"; verify "$tree"
expect "an entry named Payload that is a directory is no payload" 0 \
  "*  notice  LICENSE  $(sha "$odbc/LICENSE")  https://raw.githubusercontent.com/aws/amazon-redshift-odbc-driver/v${version%.*}/LICENSE"

# product: the signed PackageInfo beside the library's Payload names the driver and the version.
# Each package below is validly signed by the test installer identity.
product_case() { # product_case <name> <substring> <package_info spec>
  package "{\"sign\": $(signed_by installer), \"members\": [$libm], \"package_info\": $3}"
  cp "$tmp/p.pkg" "$FAKE_CURL_DIR/$pkg_name"; verify "$tree"
  expect "$1" 1 "$2"
}
need="verification needs <pkg-info> identifier 'com.amazon.redshift.odbc2x64', version '$version'"
product_case "a signed package of another product fails" \
  "verify-release: osx_arm64: product: AmazonRedshiftODBC.pkg/PackageInfo names <pkg-info> identifier 'com.amazon.redshift.jdbc', version '$version'; $need" \
  '{"identifier": "com.amazon.redshift.jdbc"}'
product_case "a signed package of another version fails" \
  "product: AmazonRedshiftODBC.pkg/PackageInfo names <pkg-info> identifier 'com.amazon.redshift.odbc2x64', version '2.2.4.0'; $need" \
  '{"version": "2.2.4.0"}'
product_case "a PackageInfo whose root is not <pkg-info> fails" "product: AmazonRedshiftODBC.pkg/PackageInfo names <installer-gui-script>" \
  '{"tag": "installer-gui-script"}'
product_case "a PackageInfo without an identifier or a version fails" "names <pkg-info> identifier None, version None" \
  '{"raw": "<pkg-info/>"}'
product_case "a package without a PackageInfo fails" "product: the package has no AmazonRedshiftODBC.pkg/PackageInfo" '{"absent": true}'
product_case "a PackageInfo whose checksums use another algorithm than signer.txt's fails" \
  "product: AmazonRedshiftODBC.pkg/PackageInfo's archived checksum uses the algorithm 'md5', which is not accepted" '{"alg": "md5"}'
product_case "a PackageInfo with a document type fails" "product: AmazonRedshiftODBC.pkg/PackageInfo has a document type declaration" \
  '{"raw": "<!DOCTYPE pkg-info [<!ENTITY v \"2.3.0.0\">]><pkg-info identifier=\"com.amazon.redshift.odbc2x64\" version=\"&v;\"/>"}'
product_case "a PackageInfo that is not XML fails" "product: AmazonRedshiftODBC.pkg/PackageInfo is not well-formed XML" '{"raw": "<pkg-info"}'
package "{\"sign\": $(signed_by installer), \"members\": [$libm]}"
cp "$tmp/p.pkg" "$FAKE_CURL_DIR/AmazonRedshiftODBC-64-bit.2.4.0.0.universal.pkg"; cp "$tmp/p.pkg" "$FAKE_CURL_DIR/$pkg_name"
verify_version=2.4.0.0 verify "$tree"
expect "a validly signed $version package served at another version's path fails at product" 1 \
  "product: AmazonRedshiftODBC.pkg/PackageInfo names <pkg-info> identifier 'com.amazon.redshift.odbc2x64', version '$version'; verification needs <pkg-info> identifier 'com.amazon.redshift.odbc2x64', version '2.4.0.0'"

# Step 1, before any download: each failure below runs with a stand-in curl that fails loudly if
# it is called.
no_download() { # no_download <name> <substring> [env...]
  local name="$1" needle="$2"; shift 2
  verify "$tree" FAKE_CURL=fail "$@"
  if [[ -e "$FAKE_CURL_LOG" ]]; then fail "$name (it fetched: $(tr '\n' ' ' < "$FAKE_CURL_LOG"))"; else expect "$name" 1 "$needle"; fi
}
printf '# a path outside the grammar\n./AmazonRedshiftODBC.pkg/Payload/../librsodbc64.dylib\n' > "$tree/policy/macos/package-path.txt"
no_download "a package path outside the grammar fails before any download" "policy: policy/macos/package-path.txt: "
for p in driver_info.h:reserved LICENSE:"a notice's"; do
  printf './AmazonRedshiftODBC.pkg/Payload/%s\n' "${p%%:*}" > "$tree/policy/macos/package-path.txt"
  no_download "a package path ending in ${p%%:*} fails the lock's file-name rule before any download" "policy: policy/macos/package-path.txt: check-lock: '${p%%:*}': file name '${p%%:*}' is ${p#*:}"
done
printf './AmazonRedshiftODBC.pkg/Payload\n' > "$tree/policy/macos/package-path.txt"
no_download "a package path that names the Payload itself fails before any download" \
  "policy: policy/macos/package-path.txt: ./AmazonRedshiftODBC.pkg/Payload does not name a file inside a component's Payload"
rm "$tree/policy/macos/package-path.txt"
no_download "a missing package-path.txt fails before any download" "policy: policy/macos/package-path.txt is missing"
cp "$odbc/policy/macos/package-path.txt" "$tree/policy/macos/package-path.txt"
# signer.txt: exactly one checksum, installer and team row, each with a value.
mv "$tree/policy/macos/signer.txt" "$tmp/signer.saved"
no_download "a missing signer.txt fails before any download" "policy: policy/macos/signer.txt is missing"
rows_ok=("checksum sha1" "team 94KV3E626L" "installer Developer ID Installer: AMZN Mobile LLC")
signer_case() { # signer_case <name> <substring> <row>...
  local name="$1" needle="$2"; shift 2
  printf '%s\n' "$@" > "$tree/policy/macos/signer.txt"
  no_download "$name" "$needle"
}
signer_case "two team rows fail before any download" "policy: policy/macos/signer.txt must have exactly one team row, with a value" "${rows_ok[@]}" "team ABCDE12345"
signer_case "two installer rows fail" "must have exactly one installer row" "${rows_ok[@]}" "installer Developer ID Installer: Other"
signer_case "no installer row fails" "must have exactly one installer row" "checksum sha1" "team 94KV3E626L"
signer_case "an empty installer row fails" "must have exactly one installer row" "checksum sha1" "team 94KV3E626L" "installer"
signer_case "no checksum row fails" "must have exactly one checksum row" "team 94KV3E626L" "installer Developer ID Installer: AMZN Mobile LLC"
signer_case "two checksum rows fail" "must have exactly one checksum row" "${rows_ok[@]}" "checksum sha256"
signer_case "an empty team row fails" "must have exactly one team row" "checksum sha1" "team" "installer Developer ID Installer: AMZN Mobile LLC"
signer_case "a checksum row with two algorithms fails" "policy: policy/macos/signer.txt must have exactly one checksum row, \"checksum <algorithm>\"" \
  "checksum sha1 sha256" "${rows_ok[@]:1}"
signer_case "an installer that is not a Developer ID Installer name fails" \
  "the installer row is \"installer Developer ID Installer: <organization>\"" "checksum sha1" "team 94KV3E626L" "installer Developer ID Application: AMZN Mobile LLC"
signer_case "an installer with no organization fails" \
  "the installer row is \"installer Developer ID Installer: <organization>\"" "checksum sha1" "team 94KV3E626L" "installer Developer ID Installer: "
# A byte that is not ASCII fails in one line, in any locale (macOS's awk stops on one its UTF-8
# locale cannot decode, so the policy files are read in the C locale).
signer_case "a signer.txt holding a byte that is not ASCII fails" \
  "policy: policy/macos/signer.txt holds a byte that is not printable ASCII" "checksum sha1" "team 94KV3E626L" \
  "$(printf 'installer Developer ID Installer: AMZN Mobile LLC \xe9')"
mv "$tmp/signer.saved" "$tree/policy/macos/signer.txt"
# A checksum row the reader does not compute passes step 1 and fails at the package's algorithm.
cp "$tree/policy/macos/signer.txt" "$tmp/signer.saved"
sed -i.bak 's/^checksum .*/checksum md5/' "$tree/policy/macos/signer.txt"
package "{\"sign\": $(signed_by installer), \"members\": [$libm]}"; cp "$tmp/p.pkg" "$FAKE_CURL_DIR/$pkg_name"
verify "$tree"
expect "a signer.txt algorithm the reader does not compute fails at algorithm" 1 "verify-release: osx_arm64: algorithm: signer.txt accepts md5, which this reader does not compute (sha1)"
# Valid rows naming another signer: signer.txt's installer and team are what the package's
# certificate must carry, so a reviewed change there is what lets another signer through.
package "{\"sign\": $(signed_by installer), \"members\": [$libm]}"; cp "$tmp/p.pkg" "$FAKE_CURL_DIR/$pkg_name"
printf '%s\n' "checksum sha1" "team ABCDE12345" "installer Developer ID Installer: AMZN Mobile LLC" > "$tree/policy/macos/signer.txt"
verify "$tree"
expect "a signer.txt naming another team refuses AWS's test-signed package" 1 \
  "verify-release: osx_arm64: subject: the certificate's subject has CN='Developer ID Installer: AMZN Mobile LLC (94KV3E626L)'"
printf '%s\n' "checksum sha1" "team 94KV3E626L" "installer Developer ID Installer: Other LLC" > "$tree/policy/macos/signer.txt"
verify "$tree"
expect "a signer.txt naming another installer refuses AWS's test-signed package" 1 \
  "verify-release: osx_arm64: subject: the certificate's subject has CN='Developer ID Installer: AMZN Mobile LLC (94KV3E626L)'"
printf '%s\n' "checksum sha1" "team ABCDE12345" "installer Developer ID Installer: AMZN Mobile LLC" > "$tree/policy/macos/signer.txt"
package "{\"sign\": $(signed_by other-team), \"members\": [$libm]}"; cp "$tmp/p.pkg" "$FAKE_CURL_DIR/$pkg_name"
verify "$tree"
expect "a package of the team signer.txt names passes" 0 "osx_arm64  driver  librsodbc64.dylib"
mv "$tmp/signer.saved" "$tree/policy/macos/signer.txt"; rm -f "$tree/policy/macos/signer.txt.bak"
# certificate: each pinned certificate is exactly one PEM certificate, so openssl gets only the
# bytes whose fingerprint the run prints. So a pinned file that holds a second certificate after
# 1 MiB of whitespace is refused before any download.
for c in AppleRootCA.pem DeveloperIDG2CA.pem; do
  cp "$tree/signing/macos/$c" "$tmp/$c.saved"
  { cat "$tmp/$c.saved"; python3 -I -c 'import sys; sys.stdout.write("\n" * (1 << 20))'; cat "$mpki/root2.pem"; } > "$tree/signing/macos/$c"
  no_download "a pinned $c with a second certificate after 1 MiB of whitespace fails before any download" \
    "verify-release: osx_arm64: certificate: the pinned $c: certificate: $tree/signing/macos/$c is larger than 1 MiB"
  cat "$tmp/$c.saved" "$mpki/root2.pem" > "$tree/signing/macos/$c"
  no_download "a pinned $c holding two certificates fails before any download" "must hold exactly one PEM certificate and nothing else"
  rm "$tree/signing/macos/$c"
  no_download "a missing pinned $c fails before any download" "verify-release: osx_arm64: certificate: the pinned $c: certificate: cannot read"
  cp "$tmp/$c.saved" "$tree/signing/macos/$c"
done
printf -- '-----BEGIN CERTIFICATE-----\nMIIB\n-----END CERTIFICATE-----\n' > "$tree/signing/macos/DeveloperIDG2CA.pem"
no_download "a pinned PEM block that is not a certificate fails before any download" "is not a certificate this tool reads"
cp "$tmp/DeveloperIDG2CA.pem.saved" "$tree/signing/macos/DeveloperIDG2CA.pem"
verify_version=2.3.0 no_download "a version that is not four numbers fails before any download" \
  "verify-release: osx_arm64: version: check-lock: '2.3.0': version '2.3.0' is not four dot-separated numbers"
verify_version=2.3.0.1 no_download "a version that is not <a>.<b>.<c>.0 fails before any download: no release-tag rule" \
  "verify-release: osx_arm64: notices: there is no release-tag rule for 2.3.0.1"
platform=linux_amd64 no_download "a platform that is not macOS fails before any download" "verify-release: linux_amd64: not a macOS platform"
no_download "an OpenSSL older than 1.1.0 fails before any download" "verify-release: osx_arm64: openssl: verification needs OpenSSL 1.1.0" \
  PATH="$tmp/old-openssl:$tmp/bin:$PATH"
printf '#!/bin/sh\n[ "$1" != version ] || { echo "error while loading shared libraries: libcrypto.so.3" >&2; exit 127; }\n' > "$tmp/broken-openssl/openssl"
chmod +x "$tmp/broken-openssl/openssl"
no_download "an openssl that does not run fails before any download" "verify-release: osx_arm64: openssl: openssl on PATH does not run: error while loading shared libraries" \
  PATH="$tmp/broken-openssl:$tmp/bin:$PATH"
for missing in python3 curl; do
  d="$tmp/without-$missing"; mkdir -p "$d"; ln -s "$(command -v bash)" "$(command -v dirname)" "$(command -v env)" "$(command -v openssl)" "$d/"
  for t in python3 awk sed cp head mkdir mktemp rm tr; do [[ "$t" == "$missing" ]] || ln -sf "$(command -v "$t")" "$d/"; done
  [[ "$missing" == curl ]] || ln -s "$tmp/bin/curl" "$d/"
  no_download "without $missing on PATH, the run fails before any download, naming it" \
    "verify-release: osx_arm64: tools: $missing is required and was not found on PATH" PATH="$d"
done

# The download, through lib/download.sh: its bounds reach curl, and SSL_CERT_FILE does not.
# has_args <n> <flag> <value>: the n-th curl call had <flag> followed by <value>.
has_args() {
  awk -v n="$1" -v f="$2" -v v="$3" '$0 == "@@" { call++; next } call == n - 1 && prev == f && $0 == v { ok = 1 } { prev = $0 }
    END { exit !ok }' "$FAKE_CURL_LOG.args"
}
package "{\"sign\": $(signed_by installer), \"members\": [$libm]}"; cp "$tmp/p.pkg" "$FAKE_CURL_DIR/$pkg_name"
verify "$tree" SSL_CERT_FILE="$mpki/root.pem"
calls="$(($(wc -l < "$FAKE_CURL_LOG")))" # BSD wc pads its count
if [[ "$status" == 0 && "$(head -1 "$FAKE_CURL_LOG")" == "$official/$pkg_name" && "$calls" == 4 ]] && has_args 1 --max-filesize 268435456 &&
  has_args 1 --proto '=https' && has_args 1 --proto-redir '=https' && has_args 1 --max-redirs 0 && has_args 2 --max-filesize 1048576 &&
  [[ "$(grep -c '^-q$' "$FAKE_CURL_LOG.args")" == "$calls" && "$(grep -c '^--path-as-is$' "$FAKE_CURL_LOG.args")" == "$calls" ]] &&
  ! grep -qE '^(-L|--location|-k|--insecure)$' "$FAKE_CURL_LOG.args" &&
  [[ "$(sort -u "$FAKE_CURL_LOG.env")" == "SSL_CERT_FILE=unset" ]]; then
  pass "the package and the three notices are downloaded through download.sh: HTTPS only, no redirects, 256 MiB and 1 MiB, without SSL_CERT_FILE"
else fail "download arguments (exit $status; $(tr '\n' ' ' < "$FAKE_CURL_LOG.args"); $(cat "$FAKE_CURL_LOG.env"))"; fi
verify "$tree" FAKE_CURL=301
expect "a redirect for the package fails and is not followed" 1 "download: $official/$pkg_name answered HTTP 301; redirects are not followed"

# The retained copies (the driver mirror): with
# RS_DRIVER_MIRROR and a recorded lock that names the version, the package and the notices are read
# from <sha256> and <sha256>-<notice>, the package checked against the lock's hash, and nothing is
# downloaded, so curl is not needed. The test lock's two macOS rows describe the test package.
mirror_dir="$tmp/retained"; mkdir -p "$mirror_dir"
cp "$tmp/signed.pkg" "$mirror_dir/$pkg_sha"
for f in LICENSE NOTICE THIRD_PARTY_LICENSES; do cp "$odbc/$f" "$mirror_dir/$pkg_sha-$f"; done
test_lock() { # test_lock <out> [<platform>...]: a lock of the test package for those platforms (default both)
  local out="$1" p platforms=(osx_amd64 osx_arm64); shift
  (($# == 0)) || platforms=("$@")
  { printf 'format 1\nversion %s\nstatus development\n' "$version"
    for p in "${platforms[@]}"; do printf '%s  %s\n' "$p" "$want_driver"; done
    awk '$1 == "*" && $2 == "notice"' "$base_lock"; } > "$out"
}
test_lock "$tmp/test.lock"
offline="$tmp/offline-bin"; mkdir -p "$offline"
for t in bash dirname env python3 openssl awk sed cp head mkdir mktemp rm tr; do ln -sf "$(command -v "$t")" "$offline/"; done
verify "$tree" FAKE_CURL=fail RS_DRIVER_MIRROR="$mirror_dir" RS_DRIVER_LOCK="$tmp/test.lock" PATH="$offline"
if [[ "$status" == 0 && "$(printed_rows)" == "$(awk '$1 ~ /^osx_/ || $1 == "*" { $1 = $1; print }' <<<"${signed_first#* }")" &&
  "$out_text" == *"comes from the copy retained as $pkg_sha, not from $official"* ]]; then
  pass "the retained copies pass offline, without curl, and print the same rows"
else fail "the retained copies offline (exit $status; $out_text)"; fi
test_lock "$tmp/amd64.lock" osx_amd64
verify "$tree" FAKE_CURL=fail RS_DRIVER_MIRROR="$mirror_dir" RS_DRIVER_LOCK="$tmp/amd64.lock"
expect "a recorded lock with only the osx_amd64 row serves osx_arm64's run too: one package" 0 "comes from the copy retained as $pkg_sha"
cp "$tmp/lib.dylib" "$mirror_dir/$pkg_sha"
verify "$tree" FAKE_CURL=fail RS_DRIVER_MIRROR="$mirror_dir" RS_DRIVER_LOCK="$tmp/test.lock"
expect "a retained package without the recorded SHA-256 fails at mirror" 1 \
  "mirror: the retained $mirror_dir/$pkg_sha has SHA-256 $lib_sha, not the SHA-256 the recorded lock names"
cp "$tmp/signed.pkg" "$mirror_dir/$pkg_sha"
mv "$mirror_dir/$pkg_sha-NOTICE" "$tmp/retained-NOTICE"
verify "$tree" FAKE_CURL=fail RS_DRIVER_MIRROR="$mirror_dir" RS_DRIVER_LOCK="$tmp/test.lock"
expect "a missing retained notice fails at mirror" 1 "mirror: $mirror_dir/$pkg_sha-NOTICE is missing or is not a regular file"
mv "$tmp/retained-NOTICE" "$mirror_dir/$pkg_sha-NOTICE"
verify "$tree" FAKE_CURL=fail RS_DRIVER_MIRROR="" RS_DRIVER_LOCK="$tmp/test.lock"
expect "an empty RS_DRIVER_MIRROR fails before any download" 1 "mirror: RS_DRIVER_MIRROR is set but empty"
verify "$tree" RS_DRIVER_MIRROR="$mirror_dir"
if [[ "$status" == 0 && "$out_text" == *"RS_DRIVER_MIRROR is set but no recorded lock is in RS_DRIVER_LOCK; downloading from $official"* &&
  "$(head -1 "$FAKE_CURL_LOG")" == "$official/$pkg_name" ]]; then
  pass "a mirror without a recorded lock says so and downloads"
else fail "a mirror without a recorded lock (exit $status; $out_text)"; fi
sed "s/^version .*/version 2.2.4.0/; s|/$version/|/2.2.4.0/|" "$tmp/test.lock" > "$tmp/other.lock"
verify "$tree" RS_DRIVER_MIRROR="$mirror_dir" RS_DRIVER_LOCK="$tmp/other.lock"
expect "a recorded lock for another version says so and downloads" 0 "the recorded lock $tmp/other.lock is not for $version; downloading from $official"
{ printf 'format 1\nversion %s\nstatus development\n' "$version"
  printf 'linux_amd64  driver  librsodbc64.so  %s  %s/AmazonRedshiftODBC-64-bit-%s.x86_64.rpm  %s  ./opt/x.so\n' "$lib_sha" "$official" "$version" "$pkg_sha"
  awk '$1 == "*" && $2 == "notice"' "$base_lock"; } > "$tmp/linux.lock"
verify "$tree" RS_DRIVER_MIRROR="$mirror_dir" RS_DRIVER_LOCK="$tmp/linux.lock"
expect "a recorded lock without a macOS row says so and downloads" 0 "has no osx_amd64 or osx_arm64 driver row; downloading from $official"
sed "s|$official/$pkg_name|$official/Other.universal.pkg|" "$tmp/test.lock" > "$tmp/url.lock"
verify "$tree" FAKE_CURL=fail RS_DRIVER_MIRROR="$mirror_dir" RS_DRIVER_LOCK="$tmp/url.lock"
expect "a recorded lock whose macOS row names another URL fails at mirror" 1 \
  "mirror: the recorded lock's osx_amd64 row is for $official/Other.universal.pkg, not $official/$pkg_name"
printf 'format 1\n' > "$tmp/bad.lock"
verify "$tree" FAKE_CURL=fail RS_DRIVER_MIRROR="$mirror_dir" RS_DRIVER_LOCK="$tmp/bad.lock"
expect "a recorded lock check-lock.sh refuses fails at mirror" 1 "mirror: the recorded lock failed check-lock.sh"
# The two rows one run prints make a lock check-lock.sh accepts; rows that differ in more than their
# platform it refuses.
{ printf 'format 1\nversion %s\nstatus development\n' "$version"; awk '$1 ~ /^osx_/ || $1 == "*"' <<<"${signed_first#* }"; } > "$tmp/printed.lock"
bash "$tools/check-lock.sh" "$tmp/printed.lock" >/dev/null 2>&1 && pass "the rows one run prints make a lock check-lock.sh accepts" ||
  fail "the printed rows' lock ($(bash "$tools/check-lock.sh" "$tmp/printed.lock" 2>&1))"
sed "s/^\(osx_amd64  driver  librsodbc64.dylib  \)$lib_sha/\1$pkg_sha/" "$tmp/printed.lock" > "$tmp/differ.lock"
out_text="$(bash "$tools/check-lock.sh" "$tmp/differ.lock" 2>&1)" && status=0 || status=$?
expect "macOS rows that differ in more than their platform fail check-lock.sh" 1 "in more than its platform"

finish
