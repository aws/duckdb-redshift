#!/usr/bin/env bash
# The Windows signer rules of verification, with the trust anchors as arguments: verify-release.sh,
# once it checks windows_amd64, passes the intermediate Linux pins and the system trust store, and
# the tests pass a test PKI.
#
#   windows_signer.sh verify <intermediate.pem> <ca-bundle> <package.msi>
#   windows_signer.sh record <intermediate.pem> <package.msi>
#
# verify checks, in this order, each failure one line "<rule>: <reason>":
#   cfb        lib/cfb.py reads the MSI's compound file strictly and takes its \x05DigitalSignature
#              stream; with none, signer: the MSI is not signed.
#   signature  lib/x509.py reads it as one SignedData over an MSI's SpcIndirectDataContent, with
#              one signer, and takes out the signer's certificate.
#   chain      openssl builds a chain from that certificate to a root in <ca-bundle>, with
#              <intermediate.pem> as the only untrusted certificate, and the chain must be exactly
#              the certificate, that intermediate and that root, as linux_signer.sh requires. The
#              certificates the signature carries are not used. The chain is checked at the current
#              time.
#   eku        the certificate carries the Code Signing extended key usage.
#   subject    its subject holds each attribute pinned below exactly once; others are not checked.
#   signature  openssl verifies the SignedData over the SpcIndirectDataContent with that
#              certificate alone.
#   digest     the MSI digest the signature carries equals the one lib/cfb.py computes over the
#              streams, including the \x05MsiDigitalSignatureEx metadata hash when present.
# record prints each certificate the signature carries (its subject, its SHA-1 thumbprint, as
# Windows shows it, its DER SHA-256, and whether it is the pinned intermediate) and the signer's
# subject in the form the pin below takes, then runs verify's digest rule; it trusts nothing and
# decides nothing.
#
# openssl and the Python helpers run without the variables that would add a trusted certificate
# or load other configuration (common.sh, no_trust_variables).
set -euo pipefail

here="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
fail() { echo "$*" >&2; exit 1; }
# shellcheck source=common.sh
. "$here/common.sh" # no_trust_variables, store_flags, chain_shape
no_trust_variables
# AWS's Windows code-signing identity, as record prints it for the 2.3.0.0 MSI. The extended
# validation attributes are what a CA validates; O and OU alone are not. Its OU differs from the
# Linux signer's, so nothing is taken from linux_signer.sh.
subject=(jurisdictionC=US jurisdictionST=Delaware serialNumber=4152954
  "O=Amazon Web Services, Inc." "OU=Amazon Redshift" "CN=Amazon Web Services, Inc.")
case "${1:-} $#" in
"verify 4") intermediate="$2" bundle="$3" msi="$4" ;;
"record 3") intermediate="$2" msi="$3" ;;
*)
  echo "usage: windows_signer.sh verify <intermediate.pem> <ca-bundle> <package.msi>" >&2
  echo "       windows_signer.sh record <intermediate.pem> <package.msi>" >&2
  exit 2 ;;
esac
work="$(mktemp -d)"
trap 'rm -rf -- "${work:?}"' EXIT
mkdir "$work/signature"
python3 -I "$here/cfb.py" signature "$msi" "$work/signature.der" || exit 1
line="$(python3 -I "$here/x509.py" authenticode "$work/signature.der" "$work/signature")" || exit 1 # it has said why
read -r algorithm signed_digest <<<"$line"
leaf="$work/signature/leaf.pem"
digest_rule() {
  local got
  got="$(python3 -I "$here/cfb.py" digest "$msi" "$algorithm")" || exit 1
  [[ "$got" == "$signed_digest" ]] ||
    fail "digest: the MSI's streams have the $algorithm digest $got, and its signature covers $signed_digest"
}

if [[ "$1" == record ]]; then
  pinned="$(python3 -I "$here/x509.py" fingerprint "$intermediate")" || exit 1
  for cert in "$work/signature"/certificate-*.pem; do
    der="$(python3 -I "$here/x509.py" fingerprint "$cert")" || exit 1
    echo "${cert##*/}: $(openssl x509 -noout -subject -nameopt RFC2253 -in "$cert")"
    echo "  SHA-1 $(openssl x509 -noout -fingerprint -sha1 -in "$cert" | sed 's/.*=//; s/://g'), DER SHA-256 $der$([[ "$der" != "$pinned" ]] || echo ", the pinned intermediate")"
  done
  echo "signer subject:"
  python3 -I "$here/x509.py" subject "$leaf" | sed 's/^/  /'
  digest_rule
  echo "digest: $algorithm $signed_digest matches the MSI's streams"
  exit 0
fi

[[ -f "$bundle" ]] || fail "chain: the trust store $bundle does not exist"
store_flags
out="$(openssl verify "${store[@]}" -CAfile "$bundle" -untrusted "$intermediate" -show_chain "$leaf" 2>&1)" ||
  fail "chain: the certificate does not chain to a root in $bundle through the pinned intermediate: $(printf '%s' "$out" | tr '\n' ' ')"
[[ "$(chain_shape "$out")" == "depth=0u depth=1u depth=2t " ]] ||
  fail "chain: the chain is not the certificate, the pinned intermediate and a trusted root: $(printf '%s' "$out" | tr '\n' ' ')"
python3 -I "$here/x509.py" leaf "$leaf" codesigning "${subject[@]}"
out="$(openssl cms -verify -binary -inform DER -in "$work/signature/signed.der" -content "$work/signature/content" \
  -noverify -nointern -certfile "$leaf" -out /dev/null 2>&1)" ||
  fail "signature: the signature does not verify over its SpcIndirectDataContent with the certificate's key: $(printf '%s' "$out" | tr '\n' ' ')"
digest_rule
