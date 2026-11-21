#!/usr/bin/env bash
# Tool tests for the Windows signer rules, third_party/redshift_odbc/tools/lib/windows_signer.sh
# with cfb.py and x509.py there, on MSIs written at test time by make_msi.py and signed with a test
# PKI: a signed MSI passes; each broken certificate, signature, digest or compound file fails at
# its rule; and the environment cannot add a trusted certificate. Run by run-tool-tests.sh.
set -euo pipefail

tmp="$(mktemp -d)"
trap 'rm -rf -- "${tmp:?}"' EXIT
# shellcheck source=verify-test-lib.sh
source "$(dirname "${BASH_SOURCE[0]}")/verify-test-lib.sh"
# shellcheck source=bounded.sh
source "$(dirname "${BASH_SOURCE[0]}")/bounded.sh" # a reader that loops fails its case, in 20 seconds
signer="$tools/lib/windows_signer.sh"
make_msi="$repo/test/client/driver/make_msi.py"
t="$(test_tree windows)"
# AWS's Windows signer, whose OU is not the Linux signer's.
win_subject="$(aws_subject_with OU 'Amazon Redshift')"
pki_cert wleaf ca "$win_subject" codesign

# msi <name> <json spec>: writes $tmp/<name>.msi. signed <leaf> [<more spec>]: the spec of an MSI
# signed by <leaf>'s key with the test intermediate as its chain.
msi() { python3 -I "$make_msi" "$tmp/$1.msi" "$2" || fail "make_msi.py writes $1.msi"; }
signed() { echo "{\"sign\": {\"leaf\": \"$pki/$1.pem\", \"key\": \"$pki/$1.key\", \"chain\": [\"$pki/ca.pem\"]${3:-}}${2:+, $2}}"; }
# run <name> [<bundle>], with any extra environment in env_extra: verifies $tmp/<name>.msi against
# the test tree's pinned intermediate.
env_extra=()
run() {
  out_text="$(bounded env ${env_extra[@]+"${env_extra[@]}"} bash "$signer" verify \
    "$t/signing/linux/$pinned_name" "${2:-$pki/bundle.pem}" "$tmp/$1.msi" 2>&1)" && status=0 || status=$?
}

msi good "$(signed wleaf)"
run good; expect "an MSI signed by a leaf with AWS's Windows subject, through the pinned intermediate, passes" 0 ""
msi v4 "$(signed wleaf '"version": 4')"
run v4; expect "a version 4 compound file passes" 0 ""
msi ex "$(signed wleaf '"ex": true')"
run ex; expect "an MSI with MsiDigitalSignatureEx passes" 0 ""
msi sha512 "$(signed wleaf "" ', "md": "sha512"')"
run sha512; expect "an MSI digest in SHA-512 passes" 0 ""
# The real MSI's signer has an RSA key, so its SignerInfo names rsaEncryption; pki_key makes EC.
openssl genpkey -algorithm RSA -pkeyopt rsa_keygen_bits:2048 -out "$pki/wrsa.key" 2>/dev/null
pki_cert wrsa ca "$win_subject" codesign
msi rsa "$(signed wrsa)"
run rsa; expect "an MSI signed with an RSA key, as the real one is, passes" 0 ""

# The signer: chain, usage and subject, as on Linux.
msi unsigned '{}'
run unsigned; expect "an unsigned MSI fails" 1 "signer: the MSI is not signed"
run good "$pki/ca.pem"
expect "the pinned intermediate as the only trust anchor, with no root, fails at the chain" 1 "chain: the certificate does not chain"
pki_cert linux-ou ca "$aws_subject" codesign
msi linux-ou "$(signed linux-ou)"
run linux-ou; expect "the Linux signer's OU fails at the subject" 1 "subject: the certificate's subject has OU='AWS Redshift'"
pki_cert other-org ca "$(aws_subject_with O 'Example Corp' | sed "s|OU=AWS Redshift|OU=Amazon Redshift|")" codesign
msi other-org "$(signed other-org)"
run other-org; expect "an MSI signed by another publisher fails at the subject" 1 "subject: the certificate's subject has O='Example Corp'"
# O and OU are not what a CA validates, so the extended validation attributes are pinned too.
pki_cert other-serial ca \
  "$(aws_subject_with serialNumber 1234567 | sed "s|OU=AWS Redshift|OU=Amazon Redshift|")" codesign
msi other-serial "$(signed other-serial)"
run other-serial
expect "a leaf with AWS's O and OU but another serialNumber fails at the subject" 1 \
  "subject: the certificate's subject has serialNumber='1234567'"
pki_cert other-state ca \
  "$(aws_subject_with jurisdictionST Nevada | sed "s|OU=AWS Redshift|OU=Amazon Redshift|")" codesign
msi other-state "$(signed other-state)"
run other-state
expect "a leaf with AWS's O and OU but another jurisdictionST fails at the subject" 1 \
  "subject: the certificate's subject has jurisdictionST='Nevada'"
pki_self wforged "$win_subject" codesign
msi forged "$(signed wforged)"
run forged; expect "a forged self-signed certificate with AWS's subject fails at the chain" 1 "chain: the certificate does not chain"
cat "$pki/root.pem" "$pki/root2.pem" > "$pki/two-roots.pem"
pki_cert other-root root2 "$win_subject" codesign
msi other-root "$(signed other-root)"
run other-root "$pki/two-roots.pem"
expect "AWS's subject from another CA in the trust store fails at the chain" 1 "chain: the chain is not the certificate, the pinned intermediate and a trusted root"
pki_cert direct root "$win_subject" codesign
msi direct "$(signed direct)"
run direct; expect "a chain of two certificates fails at the chain" 1 "chain: the chain is not the certificate, the pinned intermediate and a trusted root"
pki_cert wserver ca "$win_subject" serverauth
msi server "$(signed wserver)"
run server; expect "a certificate without Code Signing fails at the extended key usage" 1 "eku: the certificate's extended key usage"

# The signature and the digest.
msi content "$(signed wleaf '"break": ["content"]')"
run content; expect "a changed SpcIndirectDataContent fails at the signature" 1 "signature: the signature does not verify"
msi stream "$(signed wleaf '"break": ["stream-byte"]')"
run stream; expect "one changed byte in a signed stream fails at the digest" 1 "digest: the MSI's streams have the sha256 digest"
msi clsid "$(signed wleaf '"break": ["clsid"]')"
run clsid; expect "a changed storage CLSID fails at the digest" 1 "digest: the MSI's streams have the sha256 digest"
msi ex-byte "$(signed wleaf '"ex": true, "break": ["ex-byte"]')"
run ex-byte; expect "a changed MsiDigitalSignatureEx fails at the digest" 1 "digest: \\x05MsiDigitalSignatureEx does not match the MSI's metadata"
msi sha1 "$(signed wleaf "" ', "md": "sha1"')"
run sha1; expect "an MSI digest in SHA-1 fails" 1 "signature: the MSI digest's algorithm 1.3.14.3.2.26 is not SHA-256, SHA-384 or SHA-512"
msi short-digest "$(signed wleaf "" ', "digest_length": 20')"
run short-digest; expect "a 20-byte MSI digest under the SHA-256 algorithm fails" 1 "signature: the MSI digest is not the 32 bytes of a sha256 digest"
# The SignerInfo's own algorithms, which bind the MSI digest to the key: SHA-224, outside the
# accepted digests, stands for SHA-1 and MD5 here, since some hosts' openssl will not sign SHA-1.
msi signer-sha224 "$(signed wleaf "" ', "signer_md": "sha224"')"
run signer-sha224
expect "a signer whose own digest is SHA-224 fails, though the MSI digest is SHA-256" 1 \
  "signature: the signer's digest algorithm 2.16.840.1.101.3.4.2.4 is not SHA-256, SHA-384 or SHA-512"
msi signer-ecdsa-sha224 "$(signed wleaf '"break": ["signature-algorithm"]')"
run signer-ecdsa-sha224
expect "a signer whose signature algorithm is ECDSA with SHA-224 fails" 1 \
  "signature: the signer's signature algorithm 1.2.840.10045.4.3.1 is not RSA or ECDSA"
msi junk '{"streams": {"\u0005DigitalSignature": "not a signature"}}'
run junk; expect "a signature stream that is no SignedData fails at the signature" 1 "signature: the MSI's signature is not one this tool accepts"
msi nocerts "$(signed wleaf "" ', "nocerts": true')"
run nocerts; expect "a signature that carries no signer's certificate fails" 1 "0 of its certificates are the signer's, not one"
msi sip "$(signed wleaf "" ', "sip": "00112233445566778899aabbccddeeff"')"
run sip; expect "a signature for another subject interface package fails" 1 "it is not the signature of an MSI"
# The GUID's bytes with Data4's first byte one place late, which is not the MSI subject interface
# package's.
msi old-sip "$(signed wleaf "" ', "sip": "f1100c000000000000c0000000000046"')"
run old-sip
expect "a signature whose subject interface package is the GUID misspelt fails" 1 \
  "it is not the signature of an MSI"
# The constant is the GUID its comment names, in the byte order a signature stores a GUID in.
# Bash 3.2 matches quotes inside $(...) across a here-document, so this one writes to a file.
python3 -I -B - "$tools/lib/x509.py" > "$tmp/sip.out" 2>&1 <<'EOF' && status=0 || status=$?
import importlib.util, re, sys, uuid
spec = importlib.util.spec_from_file_location("x509", sys.argv[1])
x509 = importlib.util.module_from_spec(spec)
spec.loader.exec_module(x509)
guids = re.findall(r"MSI's signature, \{([0-9A-F-]{36})\}", open(sys.argv[1]).read())
ok = guids and uuid.UUID(guids[0]).bytes_le == x509.MSI_SIP
print("ok" if ok else "%s %s" % (guids, x509.MSI_SIP.hex()))
EOF
out_text="$(cat "$tmp/sip.out")"
name="MSI_SIP is the bytes of the GUID its comment names"
if [[ "$status" == 0 && "$out_text" == ok ]]; then pass "$name"
else fail "$name (exit $status; $out_text)"; fi
msi econtent "$(signed wleaf "" ', "econtent": "1.2.840.113549.1.7.1"')"
run econtent; expect "a signature over other content than an SpcIndirectDataContent fails" 1 "its content is not an SpcIndirectDataContent"
head -c 1048577 /dev/zero > "$tmp/big-signature"
msi big-signature "{\"streams\": {\"\\u0005DigitalSignature\": \"@$tmp/big-signature\"}}"
run big-signature; expect "a signature stream over 1 MiB fails" 1 "signature: the MSI's signature is larger than 1048576 bytes"
mkdir "$tmp/used"; : > "$tmp/used/x"
python3 -I "$tools/lib/cfb.py" signature "$tmp/good.msi" "$tmp/good.sig"
out_text="$(python3 -I "$tools/lib/x509.py" authenticode "$tmp/good.sig" "$tmp/used" 2>&1)" && status=0 || status=$?
expect "authenticode writes only into an empty directory" 1 "signature: $tmp/used is not empty"

# The compound file, refused before any digest is computed.
for pair in "loop:the chain of 'Big' (entry 2) loops" "shared:sector 0 is in 'Big' (entry 2) and in 'Big2'" \
  "fat-in-chain:is in the FAT and in 'Big'" "orphan:is in no chain" "unreachable:is not reached from the root" \
  "twice:is reached twice" "duplicate:out of order or with equal names" "misorder:out of order or with equal names" \
  "high-size:has nonzero high size bits in a version 3 file" "size:disagrees with its chain" \
  "cutoff:mini sector 0 is in" "trailing:the file has bytes after its last sector" \
  "prefix:named like a signature stream"; do
  b="${pair%%:*}"
  msi "break-$b" "$(signed wleaf "\"break\": [\"$b\"]")"
  run "break-$b"
  if [[ "$out_text" == "cfb: "* ]]; then expect "a compound file with the $b break fails before any digest" 1 "${pair#*:}"
  else fail "a compound file with the $b break fails before any digest (exit $status; $out_text)"; fi
done
# Each header, FAT, MiniFAT and directory rule on its own: one change to a good MSI's bytes.
# patch_case <name> <needle> <msi> <python that changes data>, with word(), put(), entry(),
# fat_at() and sector() to find the fields.
patch_case() {
  python3 -I -B - "$tmp/$3.msi" "$tmp/patched.msi" "$4" <<'EOF'
import struct, sys
data = bytearray(open(sys.argv[1], "rb").read())
size = 1 << struct.unpack_from("<H", data, 30)[0]
def word(at): return struct.unpack_from("<I", data, at)[0]
def put(at, value): struct.pack_into("<I", data, at, value)
def sector(i): return (i + 1) * size
fats = [word(76 + 4 * i) for i in range(min(word(44), 109))]
d = word(68)
for _ in range(word(72)):
    fats += [word(sector(d) + 4 * k) for k in range(size // 4 - 1)]
    d = word(sector(d) + size - 4)
fats = fats[:word(44)]
def fat_at(i): return sector(fats[i * 4 // size]) + i * 4 % size
def entry(name):  # the directory entry whose name field is <name>
    raw = (name.encode("utf-16-le") + b"\0\0").ljust(64, b"\0")
    at = data.find(raw)
    while at % 128:
        at = data.find(raw, at + 1)
    return at
exec(sys.argv[3])
open(sys.argv[2], "wb").write(data)
EOF
  run patched
  if [[ "$out_text" == "cfb: "* ]]; then expect "$1" 1 "$2"; else fail "$1 (exit $status; $out_text)"; fi
}
patch_case "a file that is not a compound file fails" "cfb: not a compound file" good 'data[:8] = b"x" * 8'
patch_case "another byte order fails" "cfb: the header is not a version 3 or 4 compound file this tool reads" good 'data[28] = 0'
patch_case "a version 3 file with 4096-byte sectors fails" \
  "cfb: the header is not a version 3 or 4 compound file this tool reads" good \
  'struct.pack_into("<H", data, 30, 12)'
patch_case "a mini sector shift other than 6 fails" \
  "cfb: the header is not a version 3 or 4 compound file this tool reads" good \
  'struct.pack_into("<H", data, 32, 7)'
patch_case "a mini-stream cutoff other than 4096 fails" \
  "cfb: the header is not a version 3 or 4 compound file this tool reads" good \
  'put(56, 0x2000)'
patch_case "a version 3 file that counts directory sectors fails" "cfb: a version 3 file counts its directory sectors" good 'put(40, 1)'
patch_case "a version 4 header sector with bytes after the header fails" "cfb: a version 4 header sector holds bytes after the header" v4 'data[600] = 1'
patch_case "a version 4 directory count that is not its chain's fails" "cfb: the directory chain's length is not the header's count" v4 'put(40, word(40) + 1)'
patch_case "a DIFAT start with no DIFAT sectors fails" "cfb: the DIFAT chain does not end where the header says" good 'put(68, 0)'
patch_case "a DIFAT that lists more FAT sectors than the header counts fails" "cfb: the DIFAT lists more FAT sectors than the header counts" good 'put(76 + 4 * word(44), 0)'
patch_case "a FAT entry past the file's sectors that is not free fails" "cfb: the FAT does not cover exactly the file's sectors" good 'put(fat_at(len(data) // size - 1), 0)'
patch_case "a FAT sector not marked as one fails" "cfb: sector " good 'put(fat_at(fats[0]), 0xFFFFFFFE)'
patch_case "a FAT entry that points outside the file fails" "s FAT entry points outside the file" good 'put(fat_at(word(entry("Big") + 116)), 0x00FFFF00)'
patch_case "a stream that starts outside the file fails" "cfb: sector 16776960 is outside the file" good 'put(entry("Big") + 116, 0x00FFFF00)'
patch_case "a root entry of another type fails" "cfb: directory entry 0 is not the root" good 'data[entry("Root Entry") + 66] = 1'
patch_case "a second root entry fails" "cfb: a second root entry" good 'data[entry("Small") + 66] = 5'
patch_case "an unknown entry type fails" "cfb: directory entry 5 has the unknown type 3" good 'data[entry("Small") + 66] = 3'
patch_case "an entry name longer than its field fails" "cfb: directory entry 5's name is not 1 to 31 characters ended by NUL" good \
  'struct.pack_into("<H", data, entry("Small") + 64, 66)'
patch_case "a MiniFAT start with no MiniFAT sectors fails" "cfb: the header gives the MiniFAT a start but no sectors" good 'put(64, 0)'
patch_case "a MiniFAT count that is not its chain's fails" "cfb: the MiniFAT chain's length is not the header's count" good 'put(64, 2)'
patch_case "a MiniFAT entry past the mini stream that is not free fails" "cfb: the MiniFAT does not cover exactly the mini stream's sectors" good \
  'put(sector(word(60)) + 4 * ((word(entry("Root Entry") + 120) + 63) // 64), 0)'
patch_case "a mini sector in no chain fails" "is in no chain" good '
root = entry("Root Entry"); count = (word(root + 120) + 63) // 64
put(root + 120, word(root + 120) + 64); put(sector(word(60)) + 4 * count, 0xFFFFFFFE)'
patch_case "a mini chain that leaves the mini stream fails" "cfb: 'Small' (entry 5)'s mini chain leaves the mini stream" good 'put(entry("Small") + 116, 999)'
patch_case "a mini stream size that disagrees with its chain fails" "cfb: 'Small' (entry 5)'s size 10 disagrees with its mini chain of 2 sectors" good \
  'put(entry("Small") + 120, 10)'
patch_case "a tree that names an unused entry fails" "s tree names entry 15, which is not in use" good 'put(entry("Root Entry") + 76, 15)'
patch_case "a stream with children fails" "cfb: stream 'Small' (entry 5) has children" good 'put(entry("Small") + 76, 0)'
# More than 109 FAT sectors, so the DIFAT chain lists the rest.
msi difat "$(signed wleaf '"difat": true')"
run difat; expect "an MSI whose FAT sectors a DIFAT sector lists passes" 0 ""
patch_case "a DIFAT chain that loops fails" "cfb: the chain of the DIFAT loops at sector" difat \
  'put(72, 2); put(sector(word(68)) + size - 4, word(68))'
patch_case "a DIFAT sector whose FAT entry does not mark it fails" \
  "s FAT entry disagrees with the sectors the header lists" difat \
  'put(fat_at(word(68)), 0xFFFFFFFD)'
patch_case "a DIFAT chain that does not end at its last sector fails" \
  "cfb: the DIFAT chain does not end where the header says" difat \
  'put(sector(word(68)) + size - 4, 0)'
patch_case "an empty directory chain fails" "cfb: the directory is empty" good 'put(48, 0xFFFFFFFE)'
# Unsigned, so the last small stream is one that fills its mini sectors, and a mini stream cut
# short leaves it short.
msi full '{"streams": {"Zfull": "'"$(printf 'z%.0s' {1..128})"'"}}'
patch_case "a small stream past the end of a short mini stream fails" \
  "cfb: 'Zfull' (entry 13) runs past the end of the mini stream" full \
  'root = entry("Root Entry"); put(root + 120, word(root + 120) - 10)'
msi german "$(signed wleaf '"streams": {"Stra\u00dfe": "x"}')"
run german; expect "a name whose upper case is two characters is ordered by its own" 0 ""
msi deep-storages '{"nest": 2000}'
out_text="$(python3 -I "$tools/lib/cfb.py" digest "$tmp/deep-storages.msi" sha256 2>&1)" && status=0 || status=$?
expect "storages nested past Python's recursion limit fail the digest, not with a traceback" 1 \
  "cfb: the MSI's storages nest too deeply"
# Signed with the metadata stream, so the recursion that fails is the metadata digest's.
msi deep-signed "$(signed wleaf '"nest": 2000, "ex": true')"
out_text="$(python3 -I "$tools/lib/cfb.py" digest "$tmp/deep-signed.msi" sha256 2>&1)" && status=0 || status=$?
expect "storages nested past the limit fail the metadata digest too, not with a traceback" 1 \
  "cfb: the MSI's storages nest too deeply"
mkdir "$tmp/a-directory.msi"
run a-directory; expect "an MSI that cannot be read fails" 1 "cfb: cannot read $tmp/a-directory.msi"

# The signature's own refusals.
msi octet "$(signed wleaf '"break": ["content-octet"]')"
run octet; expect "an SpcIndirectDataContent inside an OCTET STRING fails" 1 "its SpcIndirectDataContent is not one DER SEQUENCE"
pki_cert wleaf2 ca "$win_subject" codesign
msi two-signers "$(signed wleaf "" ", \"second\": [\"$pki/wleaf2.pem\", \"$pki/wleaf2.key\"]")"
run two-signers; expect "a signature with two signers fails" 1 "it does not have exactly one signer"
msi keyid "$(signed wleaf "" ', "keyid": true')"
run keyid; expect "a signer named by key identifier fails" 1 "its signer is not named by issuer and serial number"
python3 -I -c 'import sys; open(sys.argv[1], "wb").write(bytes.fromhex("0400"))' "$tmp/octet.der"
python3 -I -c 'import sys; open(sys.argv[1], "wb").write(bytes.fromhex("300706022a03a00100"))' "$tmp/other.der"
python3 -I -c 'import sys; open(sys.argv[1], "wb").write(b"\x30\x80" * 5000)' "$tmp/deep.der"
for pair in "octet:it is not one SEQUENCE" "other:it is not SignedData" "deep:signature: the MSI's signature nests its elements too deeply"; do
  msi "sig-${pair%%:*}" "{\"streams\": {\"\\u0005DigitalSignature\": \"@$tmp/${pair%%:*}.der\"}}"
  run "sig-${pair%%:*}"; expect "a signature stream with ${pair%%:*} content fails" 1 "${pair#*:}"
done
touch "$tmp/taken.der"
out_text="$(python3 -I "$tools/lib/cfb.py" signature "$tmp/good.msi" "$tmp/taken.der" 2>&1)" && status=0 || status=$?
expect "signature fails at an out file it cannot write, not with a traceback" 1 "signer: cannot write $tmp/taken.der: File exists"
out_text="$(python3 -I "$tools/lib/x509.py" authenticode "$tmp" "$tmp/used" 2>&1)" && status=0 || status=$?
expect "authenticode names a signature it cannot read" 1 "signature: cannot read $tmp"
printf 'not a certificate\n' > "$tmp/junk.pem"
out_text="$(bash "$signer" record "$tmp/junk.pem" "$tmp/good.msi" 2>&1)" && status=0 || status=$?
expect "record refuses a pinned intermediate it cannot read" 1 "must hold exactly one PEM certificate"
# macOS has no truncate(1).
python3 -I -c 'import sys; open(sys.argv[1], "wb").truncate((64 << 20) + 1)' "$tmp/huge.msi"
run huge; expect "a file over 64 MiB fails before it is read" 1 "cfb: the file is larger than 67108864 bytes"

# The environment cannot add a trusted certificate or change openssl's configuration.
mkdir -p "$tmp/certdir"; cp "$pki/root2.pem" "$tmp/certdir/$(openssl x509 -hash -noout -in "$pki/root2.pem").0"
env_extra=(SSL_CERT_FILE="$pki/root2.pem" SSL_CERT_DIR="$tmp/certdir" SSL_CERT_URI="file:$pki/root2.pem")
run other-root; expect "a root named only by SSL_CERT_FILE, SSL_CERT_DIR or SSL_CERT_URI is not trusted" 1 "chain: the certificate does not chain"
run good; expect "with those variables set, the good MSI still passes" 0 ""
env_extra=()
if hostile_case "with an OPENSSL_CONF that openssl applies, the good MSI still passes"; then
  env_extra=(OPENSSL_CONF="$hostile_conf"); run good; env_extra=()
  expect "with an OPENSSL_CONF that openssl applies, the good MSI still passes" 0 ""
fi
run good "$tmp/no-such-bundle.pem"
expect "a trust store that does not exist fails at the chain" 1 "chain: the trust store $tmp/no-such-bundle.pem does not exist"

# record: what a lock update reads from the real MSI.
out_text="$(bash "$signer" record "$t/signing/linux/$pinned_name" "$tmp/good.msi" 2>&1)" && status=0 || status=$?
if [[ "$status" == 0 && "$out_text" == *"DER SHA-256 $(der_sha256 "$pki/ca.pem"), the pinned intermediate"* &&
  "$out_text" == *"  OU=Amazon Redshift"* && "$out_text" == *"  jurisdictionST=Delaware"* &&
  "$out_text" == *"digest: sha256 "*" matches the MSI's streams"* ]]; then
  pass "record prints each certificate, marks the pinned intermediate, and prints the signer's subject"
else fail "record prints each certificate, marks the pinned intermediate, and prints the signer's subject (exit $status; $out_text)"; fi
out_text="$(bash "$signer" record "$t/signing/linux/$pinned_name" "$tmp/stream.msi" 2>&1)" && status=0 || status=$?
expect "record still refuses a digest that does not match" 1 "digest: the MSI's streams have the sha256 digest"
nl=$'\n'
pki_cert newline-ou ca "${win_subject/OU=Amazon Redshift/OU=Amazon Redshift${nl}  CN=Forged}" codesign
msi newline-ou "$(signed newline-ou)"
out_text="$(bash "$signer" record "$t/signing/linux/$pinned_name" "$tmp/newline-ou.msi" 2>&1)" && status=0 || status=$?
name="record quotes a subject value holding a newline, so the value cannot forge a line"
if [[ "$status" == 0 && "$out_text" == *"  OU='Amazon Redshift\\n  CN=Forged'"* && "$out_text" != *"$nl  CN=Forged"* ]]
then pass "$name"; else fail "$name (exit $status; $out_text)"; fi
# A certificate whose serialNumber is a NumericString, a type parse() does not decode: only the
# fields parse() reads are written, as subject reads no signature.
python3 -I -B - "$tmp/numeric.pem" <<'EOF'
import base64, sys
def tlv(tag, body):
    return bytes([tag, len(body)]) + body
name = tlv(0x31, tlv(0x30, tlv(0x06, bytes([0x55, 0x04, 0x05])) + tlv(0x12, b"4152954")))
tbs = tlv(0x30, tlv(0x02, b"\x01") + tlv(0x30, b"") * 3 + tlv(0x30, name) + tlv(0x30, b""))
body = base64.encodebytes(tlv(0x30, tbs + tlv(0x30, b"") + tlv(0x03, b"\x00"))).decode("ascii")
with open(sys.argv[1], "w") as f:
    f.write("-----BEGIN CERTIFICATE-----\n" + body + "-----END CERTIFICATE-----\n")
EOF
out_text="$(python3 -I "$tools/lib/x509.py" subject "$tmp/numeric.pem" 2>&1)" && status=0 || status=$?
expect "subject names a value of a type it does not decode, with no traceback" 0 \
  "serialNumber=(a value of a type this tool does not decode)"

out_text="$(python3 -I "$make_msi" "$tmp/typo.msi" '{"break": ["lop"]}' 2>&1)" && status=0 || status=$?
expect "make_msi.py refuses a break it does not know, so a case cannot test nothing" 2 "make_msi.py: unknown break lop"
out_text="$(bash "$signer" verify 2>&1)" && status=0 || status=$?
expect "a wrong argument count is a usage error" 2 "usage: windows_signer.sh"
finish
