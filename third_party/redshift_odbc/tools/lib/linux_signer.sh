#!/usr/bin/env bash
# The Linux signer rules of verification, openssl, chain, eku, subject and signature, with the trust
# anchors as arguments: verify-release.sh always passes the repository's pinned intermediate and the
# system trust store, and the tests pass a test PKI.
#
#   linux_signer.sh openssl
#   linux_signer.sh verify <intermediate.pem> <ca-bundle> <certificate.pem> <signature> <package>
#
# openssl checks that the openssl on PATH is OpenSSL 1.1.0 or later, the first with the verify
# options the chain rule needs; an older one would refuse every release for the wrong reason.
# lib/verify_linux.sh runs it once, before anything is downloaded, so verify does not check it
# again. verify then checks, in this order:
#   chain      openssl builds a chain from <certificate.pem> to a root in <ca-bundle>, with
#              <intermediate.pem> as the only untrusted certificate, and the chain must be exactly
#              the certificate, that intermediate and that root: the certificate itself and the
#              certificate at depth 1 came from the untrusted list, the root from the trust store.
#              So depth 1 is the pinned intermediate, whose signature on the certificate openssl
#              has just verified, and a certificate issued by any other CA in the bundle fails,
#              whatever its subject.
#   eku        the certificate carries the Code Signing extended key usage.
#   subject    its subject is AWS's, attribute by attribute (lib/x509.py, from the DER).
#   signature  <signature> is a SHA-256 signature over <package> by the certificate's key.
# The signer is checked by chain and identity, never by the leaf's fingerprint, which changes when
# the certificate is renewed. The chain is checked at the current time: a release whose certificate
# has expired is not verified again.
#
# openssl runs with no CA directory or store besides <ca-bundle>, and openssl and the Python helpers
# run without the variables that would add one or load other configuration or code (common.sh,
# no_trust_variables). A failure prints the rule and the reason to stderr and exits 1.
set -euo pipefail

here="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
fail() { echo "$*" >&2; exit 1; }
# shellcheck source=common.sh
. "$here/common.sh" # no_trust_variables, check_openssl, store_flags, chain_shape
no_trust_variables
# AWS's own Redshift code-signing identity, as the extended-validation certificate records it.
subject=(jurisdictionC=US jurisdictionST=Delaware serialNumber=4152954
  "O=Amazon Web Services, Inc." "OU=AWS Redshift")
case "${1:-} $#" in
"openssl 1") check_openssl; exit 0 ;;
"verify 6") ;;
*)
  echo "usage: linux_signer.sh openssl" >&2
  echo "       linux_signer.sh verify <intermediate.pem> <ca-bundle> <certificate.pem> <signature> <package>" >&2
  exit 2 ;;
esac
intermediate="$2" bundle="$3" cert="$4" signature="$5" package="$6"
[[ -f "$bundle" ]] || fail "chain: the trust store $bundle does not exist"

# chain. With OpenSSL 3, -no-CAstore also keeps the default certificate store out.
store_flags
out="$(openssl verify "${store[@]}" -CAfile "$bundle" -untrusted "$intermediate" -show_chain "$cert" 2>&1)" ||
  fail "chain: the certificate does not chain to a root in $bundle through the pinned intermediate: $(printf '%s' "$out" | tr '\n' ' ')"
# -show_chain prints one depth=<n>: line per certificate, marking the ones that came from the
# untrusted list (the certificate itself and the pinned intermediate) with "(untrusted)".
[[ "$(chain_shape "$out")" == "depth=0u depth=1u depth=2t " ]] ||
  fail "chain: the chain is not the certificate, the pinned intermediate and a trusted root: $(printf '%s' "$out" | tr '\n' ' ')"

# eku, subject.
python3 -I "$here/x509.py" leaf "$cert" codesigning "${subject[@]}"

# signature, with the key of the certificate just checked.
key="$(mktemp)"
trap 'rm -f -- "${key:?}"' EXIT
openssl x509 -in "$cert" -pubkey -noout > "$key" 2>/dev/null || fail "signature: cannot read the certificate's public key"
out="$(openssl dgst -sha256 -verify "$key" -signature "$signature" "$package" 2>&1)" ||
  fail "signature: the detached signature does not verify over the package with the certificate's key: $(printf '%s' "$out" | tr '\n' ' ')"
