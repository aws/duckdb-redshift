#!/usr/bin/env bash
# Tool tests for the Linux signer rules, third_party/redshift_odbc/tools/lib/linux_signer.sh and
# x509.py, with a test PKI generated per run: each broken certificate or signature fails at its
# rule, and the environment cannot add a trusted certificate. Run by run-tool-tests.sh.
set -euo pipefail

tmp="$(mktemp -d)"
trap 'rm -rf -- "${tmp:?}"' EXIT
# shellcheck source=verify-test-lib.sh
source "$(dirname "${BASH_SOURCE[0]}")/verify-test-lib.sh"
signer="$tools/lib/linux_signer.sh"
x509="$tools/lib/x509.py"
t="$(test_tree signer)"
printf 'package bytes\n' > "$tmp/package"

# run <leaf> [<signature-by> [<bundle> [<package>]]], with any extra environment in env_extra:
# signs the package with <signature-by>'s key (default: the leaf's) and verifies it against the
# test tree's pinned intermediate.
env_extra=()
run() {
  local leaf="$1" by="${2:-$1}" bundle="${3:-$pki/bundle.pem}" package="${4:-$tmp/package}"
  pki_sign "$by" "$tmp/package" "$tmp/signature"
  out_text="$(env ${env_extra[@]+"${env_extra[@]}"} bash "$signer" verify \
    "$t/signing/linux/$pinned_name" "$bundle" "$pki/$leaf.pem" "$tmp/signature" "$package" 2>&1)" &&
    status=0 || status=$?
}

run leaf; expect "a leaf with AWS's subject and Code Signing, issued by the pinned intermediate, passes" 0 ""
run leaf root; expect "a signature by another key fails at the signature" 1 "signature: the detached signature does not verify"
printf 'changed\n' > "$tmp/changed"
run leaf leaf "$pki/bundle.pem" "$tmp/changed"
expect "a signature over other bytes fails at the signature" 1 "signature: the detached signature does not verify"

pki_cert other ca "$(aws_subject_with O 'Example Corp')" codesign
run other; expect "a certificate from another publisher fails at the subject" 1 "subject: the certificate's subject has O='Example Corp'"
pki_cert unit ca "$(aws_subject_with OU 'AWS Glue')" codesign
run unit; expect "AWS's organisation with another unit fails at the subject" 1 "subject: the certificate's subject has OU='AWS Glue'"
pki_cert two ca "/jurisdictionC=US/jurisdictionST=Delaware/serialNumber=4152954/O=Amazon Web Services, Inc./OU=AWS Redshift/OU=Other/CN=x" codesign
run two; expect "a second OU fails at the subject" 1 "subject: the certificate's subject has OU='AWS Redshift' + 'Other'"
# The extended-validation attributes: each is required exactly as the signer's certificate has it,
# so a look-alike from another jurisdiction, state or company registration fails.
for pair in "jurisdictionC GB" "jurisdictionST Kentucky" "serialNumber 9999999"; do
  read -r attribute value <<<"$pair"
  pki_cert "ev-$attribute" ca "$(aws_subject_with "$attribute" "$value")" codesign
  run "ev-$attribute"
  expect "a leaf whose $attribute is $value fails at the subject" 1 "subject: the certificate's subject has $attribute='$value'"
done
pki_cert no-jurisdiction ca "/C=US/O=Amazon Web Services, Inc./OU=AWS Redshift/CN=Amazon Web Services, Inc." codesign
run no-jurisdiction
expect "a leaf without the extended-validation attributes fails at the subject" 1 "subject: the certificate's subject has jurisdictionC=nothing"

pki_cert server ca "$aws_subject" serverauth
run server; expect "a certificate without Code Signing fails at the extended key usage" 1 "eku: the certificate's extended key usage (1.3.6.1.5.5.7.3.1) does not include Code Signing"
pki_cert noeku ca "$aws_subject" noeku
run noeku; expect "a certificate with no extended key usage fails at it" 1 "eku: the certificate has no extended key usage"

pki_self forged "$aws_subject" codesign
run forged; expect "a forged self-signed certificate with AWS's subject fails at the chain" 1 "chain: the certificate does not chain"
cat "$pki/root.pem" "$pki/root2.pem" > "$pki/two-roots.pem"
pki_cert other-root root2 "$aws_subject" codesign
run other-root other-root "$pki/two-roots.pem"
expect "AWS's subject and Code Signing from a second root in the trust store fail at the chain" 1 "chain: the chain is not the certificate, the pinned intermediate and a trusted root"
# The pinned intermediate's name and key, self-signed and trusted: openssl accepts the chain, and
# only its shape shows that it skips the pinned intermediate.
openssl req -new -x509 -key "$pki/ca.key" -subj "/C=US/O=Test CA/CN=Test Code Signing CA" -days 2 \
  -config "$pki/openssl.cnf" -extensions root -out "$pki/ca-self.pem" 2>/dev/null
cat "$pki/root.pem" "$pki/ca-self.pem" > "$pki/with-self.pem"
run leaf leaf "$pki/with-self.pem"
expect "the pinned intermediate's key trusted directly fails at the chain" 1 "chain: the chain is not the certificate, the pinned intermediate and a trusted root"
# The pinned intermediate itself in the trust store: openssl takes it from there, a trusted depth 1, so the chain
# has the right length and only the depth-1 "(untrusted)" mark shows that it did not come from the pin.
cat "$pki/root.pem" "$pki/ca.pem" > "$pki/with-intermediate.pem"
run leaf leaf "$pki/with-intermediate.pem"
expect "the pinned intermediate taken from the trust store fails at the chain" 1 "chain: the chain is not the certificate, the pinned intermediate and a trusted root"
pki_cert look-alike root "/C=US/O=Test CA/CN=Test Code Signing CA" ca
pki_cert by-look-alike look-alike "$aws_subject" codesign
run by-look-alike; expect "a leaf from an intermediate with the pinned one's name but another key fails at the chain" 1 "chain: the certificate does not chain"
cat "$pki/leaf.pem" "$pki/ca.pem" > "$pki/two-certs.pem"; cp "$pki/leaf.key" "$pki/two-certs.key"
run two-certs; expect "a certificate file holding two certificates fails" 1 "must hold exactly one PEM certificate"

# The intermediate is the pin: another certificate in its place fails at the chain, because it is
# then the only untrusted certificate openssl is given and the leaf does not chain through it.
cp "$pki/root2.pem" "$t/signing/linux/$pinned_name"
run leaf; expect "another certificate in the pinned intermediate's place fails at the chain" 1 "chain: the certificate does not chain"
t="$(test_tree signer2)"
# x509.py fingerprint, which verification prints, is the DER hash, not the file's: it is the same
# for the same certificate written differently, and refuses anything else.
printf '\n\n' >> "$t/signing/linux/$pinned_name"
out_text="$(python3 -I "$x509" fingerprint "$t/signing/linux/$pinned_name" 2>&1)" && status=0 || status=$?
if [[ "$status" == 0 && "$out_text" == "$(der_sha256 "$pki/ca.pem")" ]]; then
  pass "x509.py fingerprint prints the DER SHA-256, whatever the file's trailing bytes"
else fail "fingerprint (exit $status; $out_text)"; fi
cat "$pki/leaf.pem" "$pki/ca.pem" > "$t/signing/linux/$pinned_name"
out_text="$(python3 -I "$x509" fingerprint "$t/signing/linux/$pinned_name" 2>&1)" && status=0 || status=$?
expect "x509.py fingerprint refuses a file holding two certificates" 1 "must hold exactly one PEM certificate"
# A second certificate cannot hide behind padding: a file over 1 MiB is refused outright, so the
# fingerprint printed is always of the only certificate in it.
t="$(test_tree padded)"
{ cat "$pki/ca.pem"; python3 -I -c 'print("\n" * 1100000, end="")'; cat "$pki/root2.pem"; } > "$t/signing/linux/$pinned_name"
out_text="$(python3 -I "$x509" fingerprint "$t/signing/linux/$pinned_name" 2>&1)" && status=0 || status=$?
expect "x509.py fingerprint refuses a file larger than 1 MiB" 1 "is larger than 1 MiB"
# The bytes in the PEM block must be a certificate, not any DER: the fingerprint of something else
# would be printed as a certificate's.
python3 -I -c 'import base64, sys
open(sys.argv[1], "w").write("-----BEGIN CERTIFICATE-----\n"
                             + base64.encodebytes(b"\x30\x03\x02\x01\x01").decode()
                             + "-----END CERTIFICATE-----\n")' "$t/signing/linux/$pinned_name"
out_text="$(python3 -I "$x509" fingerprint "$t/signing/linux/$pinned_name" 2>&1)" && status=0 || status=$?
expect "x509.py fingerprint refuses a PEM block that is not a certificate" 1 "is not a certificate this tool reads"
t="$(test_tree signer3)"

# More than one rule broken: the first in the order is the one reported.
run forged root; expect "a forged self-signed certificate with another key's signature fails at the chain" 1 "chain: the certificate does not chain"
pki_cert server-other ca "/C=US/O=Example Corp/OU=AWS Redshift/CN=x" serverauth
run server-other; expect "no Code Signing and another publisher fail at the extended key usage" 1 "eku: "
run unit root; expect "another unit and another key's signature fail at the subject" 1 "subject: "

# The subject needs exactly one O and one OU.
pki_cert no-ou ca "$(sed 's|/OU=[^/]*||' <<<"$aws_subject")" codesign
run no-ou; expect "a subject without OU fails" 1 "subject: the certificate's subject has OU=nothing"
pki_cert no-o ca "$(sed 's|/O=Amazon Web Services, Inc.||' <<<"$aws_subject")" codesign
run no-o; expect "a subject without O fails" 1 "subject: the certificate's subject has O=nothing"
pki_cert two-o ca "$(sed 's|/O=Amazon Web Services, Inc.|/O=Amazon Web Services, Inc./O=Other|' <<<"$aws_subject")" codesign
run two-o; expect "a subject with two O values fails" 1 "subject: the certificate's subject has O='Amazon Web Services, Inc.' + 'Other'"

# x509.py refuses a certificate it cannot read unambiguously. der_case <name> <python list of (oid,
# value tag[, value hex])> [<mode>]: a hand-built certificate with those extensions, each holding
# the given value or a Code Signing usage, checked by x509.py against the test intermediate. The
# mode adds bytes after it (trailing), writes its length in a longer form than DER allows
# (long-length), makes it claim one byte more than it holds (runs-past), leaves out its signature
# (two-top), makes its subject a SET (subject-set), adds a high tag number (high-tag) or an issuer
# unique identifier (unique-id), ends a subject attribute's object identifier inside an arc
# (oid-trailing), or checks it as its own issuer (self).
der_case() {
  python3 -I -c '
import base64, sys
def tlv(tag, body):
    n = len(body)
    if n < 0x80:
        return bytes([tag, n]) + body
    size = (n.bit_length() + 7) // 8
    return bytes([tag, 0x80 | size]) + n.to_bytes(size, "big") + body
def oid(text):
    arcs = [int(a) for a in text.split(".")]
    out = bytes([40 * arcs[0] + arcs[1]])
    for a in arcs[2:]:
        chunk = [a & 0x7F]
        a >>= 7
        while a:
            chunk.insert(0, 0x80 | (a & 0x7F))
            a >>= 7
        out += bytes(chunk)
    return tlv(0x06, out)
name = tlv(0x30, tlv(0x31, tlv(0x30, oid("2.5.4.3") + tlv(0x0C, b"x"))))
alg = tlv(0x30, oid("1.2.840.113549.1.1.11") + b"\x05\x00")
eku = tlv(0x30, oid("1.3.6.1.5.5.7.3.3"))
exts = b"".join(tlv(0x30, oid(e[0]) + tlv(e[1], bytes.fromhex(e[2]) if len(e) > 2 else eku))
               for e in eval(sys.argv[2]))
mode = sys.argv[3] if len(sys.argv) > 3 else ""
if mode == "oid-trailing":
    name = tlv(0x30, tlv(0x31, tlv(0x30, tlv(0x06, bytes.fromhex("55040380")) + tlv(0x0C, b"x"))))
subject = tlv(0x31, name[2:]) if mode == "subject-set" else name
unique = tlv(0x81, b"\x00\x01") if mode == "unique-id" else b""
high = b"\x9f\x01\x00" if mode == "high-tag" else b""
validity = tlv(0x30, tlv(0x17, b"260101000000Z") * 2)
spki = tlv(0x30, tlv(0x30, oid("1.2.840.113549.1.1.1") + b"\x05\x00") + tlv(0x03, b"\x00"))
tbs = tlv(0x30, tlv(0xA0, tlv(0x02, b"\x02")) + tlv(0x02, b"\x01") + alg + name + validity + subject + spki
          + unique + tlv(0xA3, tlv(0x30, exts)) + high)
der = tlv(0x30, tbs + alg + (b"" if mode == "two-top" else tlv(0x03, b"\x00")))
size = der[1] & 0x7F  # the outer SEQUENCE is over 127 bytes, so its length takes size bytes
body = der[2 + size:]
if mode == "trailing":
    der += b"\x00\x00"
elif mode == "long-length":
    der = b"\x30" + bytes([0x81 + size]) + b"\x00" + len(body).to_bytes(size, "big") + body
elif mode == "runs-past":
    der = b"\x30" + bytes([0x80 + size]) + (len(body) + 1).to_bytes(size, "big") + body
open(sys.argv[1], "w").write("-----BEGIN CERTIFICATE-----\n" + base64.encodebytes(der).decode()
                             + "-----END CERTIFICATE-----\n")' "$pki/$1.pem" "$2" ${3:+"$3"}
  out_text="$(python3 -I "$x509" leaf "$pki/$1.pem" codesigning "O=Amazon Web Services, Inc." 2>&1)" && status=0 || status=$?
}
der_case one-eku '[("2.5.29.37", 0x04)]'
expect "x509.py: the hand-built certificate with one extended key usage is read (it fails only at the subject)" 1 "subject: the certificate's subject has O=nothing"
der_case dup-eku '[("2.5.29.37", 0x04), ("2.5.29.37", 0x04)]'
expect "x509.py: a certificate with two extended key usage extensions fails" 1 "extension 2.5.29.37 appears twice"
der_case not-octet '[("2.5.29.37", 0x30)]'
expect "x509.py: an extension whose value is not an OCTET STRING fails" 1 "extension 2.5.29.37 has no OCTET STRING value"
der_case two-top '[("2.5.29.37", 0x04)]' two-top
expect "x509.py: a certificate without its signature fails" 1 "is not a certificate this tool reads: not a certificate"
der_case subject-set '[("2.5.29.37", 0x04)]' subject-set
expect "x509.py: a subject that is not a Name fails" 1 "is not a certificate this tool reads: the subject is not a Name"
out_text="$(python3 -I "$x509" leaf "$pki/leaf.pem" serverauth 2>&1)" && status=0 || status=$?
expect "x509.py leaf takes no extended key usage but codesigning and installer" 2 "usage: x509.py"
der_case high-tag '[("2.5.29.37", 0x04)]' high-tag
expect "x509.py: a high tag number fails" 1 "is not a certificate this tool reads: high tag numbers are not used"
der_case unique-id '[("2.5.29.37", 0x04)]' unique-id
expect "x509.py: an issuer unique identifier is skipped, not read as extensions" 1 "subject: the certificate's subject has O=nothing"
der_case trailing '[("2.5.29.37", 0x04)]' trailing
expect "x509.py: bytes after the certificate fail" 1 "is not a certificate this tool reads: not one DER SEQUENCE"
der_case long-length '[("2.5.29.37", 0x04)]' long-length
expect "x509.py: a length not in its shortest form fails" 1 "is not a certificate this tool reads: length not in its shortest form"
der_case runs-past '[("2.5.29.37", 0x04)]' runs-past
expect "x509.py: an element longer than what holds it fails" 1 "is not a certificate this tool reads: element runs past its parent"
der_case oid-trailing '[("2.5.29.37", 0x04)]' oid-trailing
expect "x509.py: an object identifier that ends inside an arc fails" 1 "is not a certificate this tool reads: malformed object identifier"

# An expired or not-yet-valid leaf fails at the chain, which is always checked at the current time:
# such a version is reused from its recorded lock, never verified again.
# The chain is a second one made with openssl ca, whose dates can be set.
mkdir -p "$pki/dated"; : > "$pki/dated/index.txt"; echo 01 > "$pki/dated/serial"
printf '%s\n' "[ca]" "default_ca = dated" "[dated]" "database = $pki/dated/index.txt" "new_certs_dir = $pki/dated" \
  "serial = $pki/dated/serial" "default_md = sha256" "policy = any" "unique_subject = no" "email_in_dn = no" \
  "preserve = yes" "[any]" "countryName = optional" "organizationName = optional" \
  "organizationalUnitName = optional" "commonName = optional" > "$pki/ca.cnf"
dated() { # dated <name> <issuer|self> <subject> <section> <notBefore> <notAfter>, dates as YYYYMMDDHHMMSSZ
  local issuer=(-cert "$pki/$2.pem" -keyfile "$pki/$2.key")
  pki_key "$1"
  [[ "$2" != self ]] || issuer=(-selfsign -keyfile "$pki/$1.key")
  openssl req -new -key "$pki/$1.key" -subj "$3" -config "$pki/openssl.cnf" -out "$pki/$1.csr" 2>/dev/null
  openssl ca -batch -notext -config "$pki/ca.cnf" "${issuer[@]}" -in "$pki/$1.csr" -out "$pki/$1.pem" \
    -startdate "$5" -enddate "$6" -extfile "$pki/openssl.cnf" -extensions "$4" 2>/dev/null
}
dated rootd self "/CN=Dated Test Root" root 20010101000000Z 20991231235959Z
dated cad rootd "/C=US/O=Test CA/CN=Dated Code Signing CA" ca 20010101000000Z 20991231235959Z
dated expired cad "$aws_subject" codesign 20100101000000Z 20110101000000Z
dated future cad "$aws_subject" codesign 20900101000000Z 20910101000000Z
t="$(test_tree dated cad)"
run expired expired "$pki/rootd.pem"
expect "an expired leaf fails at the chain, whose time is always now" 1 "chain: the certificate does not chain"
run future future "$pki/rootd.pem"
expect "a leaf that is not yet valid fails at the chain" 1 "chain: the certificate does not chain"
t="$(test_tree signer4)"

# A stray '=' inside the pinned intermediate's base64, which a lenient decoder would skip, fails
# when verification reads the certificate, before anything else (verify_linux.sh step 1).
t="$(test_tree stray-pad)"
python3 -I -c 'import sys
p = sys.argv[1]; lines = open(p).read().split("\n")
lines[2] = lines[2][:10] + "=" + lines[2][10:]
open(p, "w").write("\n".join(lines))' "$t/signing/linux/$pinned_name"
out_text="$(python3 -I "$x509" fingerprint "$t/signing/linux/$pinned_name" 2>&1)" && status=0 || status=$?
expect "a stray '=' in the pinned certificate's base64 fails when it is read" 1 "certificate: $t/signing/linux/$pinned_name is not valid base64"
t="$(test_tree signer5)"

# The openssl rule, which verify_linux.sh runs once before any download, so verify does not run it
# again: an openssl older than 1.1.0 lacks the verify options the chain rule needs.
mkdir -p "$tmp/old-openssl"
signer_openssl() { # with any extra environment in env_extra
  out_text="$(env ${env_extra[@]+"${env_extra[@]}"} bash "$signer" openssl 2>&1)" && status=0 || status=$?
}
for v in "OpenSSL 1.0.2k-fips  26 Jan 2017" "LibreSSL 3.3.6"; do
  printf '#!/usr/bin/env bash\n[[ "$1" != version ]] || { echo "%s"; exit 0; }\nexec %s "$@"\n' "$v" "$(command -v openssl)" > "$tmp/old-openssl/openssl"
  chmod +x "$tmp/old-openssl/openssl"
  env_extra=(PATH="$tmp/old-openssl:$PATH"); signer_openssl; env_extra=()
  expect "${v%%  *} is refused by the openssl rule" 1 "openssl: verification needs OpenSSL 1.1.0 or later, and openssl on PATH is: $v"
done
env_extra=(); signer_openssl
expect "the host's openssl passes the openssl rule" 0 ""

# Without openssl on PATH the run fails, naming it; a trust store that does not exist fails at the
# chain, naming it.
mkdir -p "$tmp/no-openssl"
ln -s "$(command -v bash)" "$(command -v dirname)" "$(command -v python3)" "$tmp/no-openssl/"
env_extra=(PATH="$tmp/no-openssl"); signer_openssl; env_extra=()
expect "without openssl on PATH, the openssl rule fails naming it" 1 "openssl: openssl is required and was not found on PATH"
mkdir -p "$tmp/broken-openssl"
printf '#!/usr/bin/env bash\necho "openssl: error while loading shared libraries: libssl.so.3" >&2\nexit 127\n' > "$tmp/broken-openssl/openssl"
chmod +x "$tmp/broken-openssl/openssl"
env_extra=(PATH="$tmp/broken-openssl:$PATH"); signer_openssl; env_extra=()
expect "an openssl that does not run fails, naming the rule and with its message" 1 \
  "openssl: openssl on PATH does not run: openssl: error while loading shared libraries: libssl.so.3"
run leaf leaf "$tmp/no-such-bundle.pem"
expect "a trust store that does not exist fails at the chain" 1 "chain: the trust store $tmp/no-such-bundle.pem does not exist"

# The chain check must consult no trust store but the one it is given: a stand-in openssl that
# records every call shows -no-CApath, and -no-CAstore where this openssl offers it, on any host.
: > "$OPENSSL_LOG"
env_extra=(PATH="$tmp/openssl-bin:$PATH"); run leaf; env_extra=()
store_flags=(-no-CApath)
[[ "$(openssl verify -help 2>&1)" != *-no-CAstore* ]] || store_flags+=(-no-CAstore)
missing=()
for flag in "${store_flags[@]}"; do openssl_args verify "$flag" || missing+=("$flag"); done
if [[ "$status" == 0 && ${#missing[@]} == 0 ]] && ! openssl_args verify -CApath; then
  pass "the chain check runs openssl verify with ${store_flags[*]} and no CA directory"
else fail "chain options (exit $status; missing: ${missing[*]:-none}; $(tr '\n' ' ' < "$OPENSSL_LOG"))"; fi
# The branch taken when the certificate's public key cannot be read: the stand-in openssl refuses
# the x509 subcommand, which a working openssl never does for a certificate that passed the chain.
: > "$OPENSSL_LOG"
env_extra=(PATH="$tmp/openssl-bin:$PATH" OPENSSL_FAIL=x509); run leaf; env_extra=()
expect "a certificate whose public key openssl cannot read fails at the signature" 1 "signature: cannot read the certificate's public key"

# The environment cannot add a trusted certificate or change openssl's configuration.
mkdir -p "$tmp/certdir"; cp "$pki/root2.pem" "$tmp/certdir/$(openssl x509 -hash -noout -in "$pki/root2.pem").0"
env_extra=(SSL_CERT_FILE="$pki/root2.pem" SSL_CERT_DIR="$tmp/certdir" SSL_CERT_URI="file:$pki/root2.pem"
  SYSTEM_CA_BUNDLE="$pki/root2.pem" CURL_CA_BUNDLE="$pki/root2.pem")
run other-root; expect "a root named only by SSL_CERT_FILE, SSL_CERT_DIR, SSL_CERT_URI or SYSTEM_CA_BUNDLE is not trusted" 1 "chain: the certificate does not chain"
run leaf; expect "with those variables set, the good leaf still passes" 0 ""
env_extra=()
if hostile_case "with an OPENSSL_CONF that openssl applies, the good leaf still passes"; then
  env_extra=(OPENSSL_CONF="$hostile_conf"); run leaf; env_extra=()
  expect "with an OPENSSL_CONF that openssl applies, the good leaf still passes" 0 ""
fi

out_text="$(bash "$signer" verify 2>&1)" && status=0 || status=$?
expect "a wrong argument count is a usage error" 2 "usage: linux_signer.sh"
finish
