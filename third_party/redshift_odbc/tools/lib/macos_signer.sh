#!/usr/bin/env bash
# The macOS signer rules of verification, run on Linux, with the trust anchors and the signer as
# arguments: lib/verify_macos.sh always passes the repository's pinned certificates and signer.txt's
# rows, and the tests pass a test PKI.
#
#   macos_signer.sh openssl
#   macos_signer.sh package <intermediate.pem> <root.pem> <algorithm> <installer> <team> <package> <path-in-package>
#
# openssl checks that the openssl on PATH runs and is OpenSSL 1.1.0 or later (lib/common.sh, as
# on Linux); lib/verify_macos.sh runs it once, before anything is downloaded, so package does not
# check it again. package then checks, in this order, each failing with its rule's name:
#   package, algorithm, checksum-location, checksum, signature (the parts)
#              lib/xar.py signed: the header's checksum algorithm is <algorithm>; the stored
#              checksum lies inside the file and equals the checksum of the table of contents; one
#              RSA and one CMS signature, one signing certificate.
#   certificate
#              openssl reads the signing certificate.
#   chain      openssl builds a chain from the signing certificate through <intermediate.pem> to
#              <root.pem>, which are the only certificates it is given, and it must be exactly
#              those three: the certificate and depth 1 from the untrusted list, which holds only
#              the pinned intermediate, and the root from the trusted file, which holds only the
#              pinned root. So the intermediate signed the certificate.
#   usage, eku, subject
#              lib/x509.py leaf installer: a Developer ID Installer certificate, not a CA, no
#              other critical extension (usage), Apple's installer usage (eku), its CN
#              "<installer> (<team>)", its OU and UID <team> and its O the organization in
#              <installer>, each once (subject).
#   signature  the RSA signature verifies over the stored checksum with the signing certificate's
#              key (PKCS #1 v1.5, <algorithm>'s digest), and so does the CMS signature, by that
#              certificate and no other. 2.3.0.0's are SHA-1, which a host crypto policy can
#              refuse (RHEL 9's DEFAULT does); the failure says so.
#   payload    lib/xar.py payload: the component Payload of <path-in-package> matches the
#              checksums the signed table of contents records, in <algorithm>.
# The signing certificate is public, so a forged package can carry it and pass the signer checks:
# it is the RSA signature, verified before the CMS signature is parsed, that limits the BER reader
# and openssl's CMS decoder to packages whose table of contents the pinned identity signed. The CMS
# signature's own bytes are not covered by it, so both readers still take hostile input.
#
# Apple's certificates carry critical extensions of Apple's own, so openssl runs with
# -ignore_critical and lib/x509.py accepts only those it knows. The chain is checked at the current
# time, as on Linux. openssl runs with no CA directory or store besides <root.pem>, and without the
# variables that would add one or load other configuration or code, so the environment cannot add a
# trusted certificate. A failure prints the rule and the reason to stderr and exits 1; success
# prints a one-line summary on stdout.
set -euo pipefail

here="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
fail() { echo "$*" >&2; exit 1; }
# shellcheck source=common.sh
. "$here/common.sh" # no_trust_variables, check_openssl, store_flags, chain_shape
no_trust_variables
oneline() { printf '%s' "$1" | tr '\n' ' '; }

case "${1:-} $#" in
"openssl 1") check_openssl; exit 0 ;;
"package 8") ;;
*)
  echo "usage: macos_signer.sh openssl" >&2
  echo "       macos_signer.sh package <intermediate.pem> <root.pem> <algorithm> <installer> <team> <package> <path-in-package>" >&2
  exit 2 ;;
esac
intermediate="$2" root="$3" alg="$4" installer="$5" team="$6" pkg="$7" path="$8"
work="$(mktemp -d)"
trap 'rm -rf -- "${work:?}"' EXIT
mkdir "$work/parts"
# xar <command> <args...>: lib/xar.py, its failure with its rule's name, without the xar.py: prefix.
xar() {
  local msg
  msg="$(python3 -I "$here/xar.py" "$@" 2>&1)" || fail "${msg#xar.py: }"
}

xar signed "$pkg" "$work/parts" "$alg"

# certificate, chain, usage, eku, subject.
openssl x509 -inform DER -in "$work/parts/signer.der" -out "$work/signer.pem" 2>/dev/null ||
  fail "certificate: the package's signing certificate is not a certificate openssl reads"
store_flags
out="$(openssl verify "${store[@]}" -ignore_critical -CAfile "$root" -untrusted "$intermediate" -show_chain "$work/signer.pem" 2>&1)" ||
  fail "chain: the certificate does not chain to the pinned Apple root through the pinned intermediate: $(oneline "$out")"
[[ "$(chain_shape "$out")" == "depth=0u depth=1u depth=2t " ]] ||
  fail "chain: the chain is not the certificate, the pinned intermediate and the pinned root: $(oneline "$out")"
python3 -I "$here/x509.py" leaf "$work/signer.pem" installer "CN=$installer ($team)" "OU=$team" "UID=$team" \
  "O=${installer#*: }"

# signature: RSA, then CMS, over the stored checksum, by the certificate just checked.
policy="a host crypto policy can refuse $alg signatures (RHEL 9's DEFAULT refuses SHA-1), so verify on a host whose OpenSSL allows them"
openssl x509 -in "$work/signer.pem" -pubkey -noout > "$work/key.pem" 2>/dev/null ||
  fail "signature: cannot read the signing certificate's public key"
out="$(openssl pkeyutl -verify -pubin -inkey "$work/key.pem" -pkeyopt rsa_padding_mode:pkcs1 \
  -pkeyopt "digest:$alg" -sigfile "$work/parts/rsa.sig" -in "$work/parts/checksum" 2>&1)" ||
  fail "signature: the RSA signature does not verify over the stored checksum with the signing certificate's key, or $policy: $(oneline "$out")"
python3 -I "$here/x509.py" cms "$work/parts/cms.ber" "$work/cms.der"
out="$(openssl cms -verify -binary -inform DER -in "$work/cms.der" -content "$work/parts/checksum" \
  -noverify -nointern -certfile "$work/signer.pem" -out /dev/null 2>&1)" ||
  fail "signature: the CMS signature does not verify over the stored checksum by the signing certificate, or $policy: $(oneline "$out")"

xar payload "$pkg" "$path" "$alg"
echo "the package is signed (RSA and CMS, $alg checksum) by the pinned installer identity, through the pinned intermediate and root"
