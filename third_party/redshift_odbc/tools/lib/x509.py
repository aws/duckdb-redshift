"""The certificate rules of verification that openssl does not apply, and the one DER reader of the
tools.

    python3 -I x509.py fingerprint <cert.pem>
    python3 -I x509.py leaf <leaf.pem> <eku> <attribute>=<value>...
    python3 -I x509.py cms <in.ber> <out.der>
    python3 -I x509.py authenticode <signature.der> <out-dir>
    python3 -I x509.py subject <cert.pem>

fingerprint: <cert.pem> must hold exactly one certificate; prints the SHA-256 of its DER encoding,
which verification records for each certificate it pins by checking it in. A PEM file's own hash
depends on how it was written, so the fingerprint is of the DER bytes.

leaf: <leaf.pem> must hold exactly one certificate, carrying the extended key usage <eku>
(`codesigning` for Linux, `installer` for macOS), and its subject must hold each <attribute>
exactly once with that value and nothing else for that attribute. The attributes are
`jurisdictionC`, `jurisdictionST`, `serialNumber`, `O`, `OU`, `CN` and `UID`, the attributes the
signers' certificates are pinned by; another is one line here when a rule needs it. With
`installer` (lib/macos_signer.sh, whose openssl runs with -ignore_critical for Apple's own critical
extensions) the certificate must also not be a CA, must carry Apple's Developer ID Installer marker
extension, and may have no critical extension besides basic constraints, key usage, extended key
usage and that marker. openssl checks the chain and the signatures (lib/linux_signer.sh,
lib/macos_signer.sh); this checks the identity, from the DER, so the result does not depend on how
a given openssl version prints a certificate.

cms: <in.ber>, at most 1 MiB, is one BER-encoded CMS SignedData with detached content and exactly
one signer, followed by nothing but zero bytes (a macOS package reserves room for it); writes its
encoding without them to <out.der> for openssl.

authenticode: <signature.der>, at most 1 MiB, is an MSI's \x05DigitalSignature stream: one
PKCS #7 SignedData whose content is an SpcIndirectDataContent for the MSI subject interface
package, with exactly one signer. It writes, into <out-dir>, which must be empty, content (the
SpcIndirectDataContent's value, the bytes the signer's messageDigest covers), signed.der (the
SignedData with that content detached, for openssl cms -verify), leaf.pem (the certificate the
signer's issuer and serial number name) and certificate-<n>.pem for each certificate it carries,
in order, and prints "<algorithm> <digest>", the MSI digest the
signature carries, in hex. lib/windows_signer.sh compares that digest with lib/cfb.py's.

subject: prints each attribute of <cert.pem>'s subject on its own line, as <attribute>=<value>, in
the form leaf takes them, with an attribute leaf does not read named by its object identifier.

On failure it prints one line, starting with the rule that failed (certificate, eku, subject,
usage or signature), to stderr and exits 1. Standard library only, Python 3.6 and later.
"""

import base64
import collections
import hashlib
import os
import re
import sys

MAX_CMS = 1 << 20
PEM = re.compile(rb"\s*-----BEGIN CERTIFICATE-----\r?\n([A-Za-z0-9+/=\r\n]+)-----END CERTIFICATE-----\s*\Z")
OID_EKU = "2.5.29.37"
APPLE = "1.2.840.113635.100."
EKU_OIDS = {"codesigning": "1.3.6.1.5.5.7.3.3", "installer": APPLE + "4.13"}
EKU_NAMES = {"codesigning": "Code Signing", "installer": "Developer ID Installer"}  # how a failure names it
# For the macOS signer, whose chain openssl checks with -ignore_critical: the Developer ID marker
# extension the certificate must carry, which is also the one critical extension it may have
# besides the standard three.
MARKERS = {"installer": APPLE + "6.1.14"}
OID_BC, OID_KU = "2.5.29.19", "2.5.29.15"
STANDARD_CRITICAL = {OID_BC, OID_KU, OID_EKU}
OID_SIGNED_DATA = "1.2.840.113549.1.7.2"
OID_SPC_INDIRECT_DATA = "1.3.6.1.4.1.311.2.1.4"
OID_SPC_SIPINFO = "1.3.6.1.4.1.311.2.1.30"
# The subject interface package of an MSI's signature, {000C10F1-0000-0000-C000-000000000046}.
MSI_SIP = bytes.fromhex("f1100c0000000000c000000000000046")
# The MSI digest algorithms accepted; SHA-1 is not.
DIGESTS = {"2.16.840.1.101.3.4.2.1": ("sha256", 32), "2.16.840.1.101.3.4.2.2": ("sha384", 48),
           "2.16.840.1.101.3.4.2.3": ("sha512", 64)}
# The signer's signature algorithms accepted: RSA, whose hash is the signer's digest algorithm,
# and RSA or ECDSA with SHA-256, SHA-384 or SHA-512. SHA-1 and MD5 are not.
SIGNATURES = {"1.2.840.113549.1.1.1", "1.2.840.113549.1.1.11", "1.2.840.113549.1.1.12",
              "1.2.840.113549.1.1.13", "1.2.840.10045.4.3.2", "1.2.840.10045.4.3.3",
              "1.2.840.10045.4.3.4"}
# The subject attributes a rule may name. The two jurisdiction attributes are the extended
# validation ones DigiCert issues under (RFC 3739 and the CA/Browser Forum's EV guidelines).
ATTRIBUTE_OIDS = {"jurisdictionC": "1.3.6.1.4.1.311.60.2.1.3",
                  "jurisdictionST": "1.3.6.1.4.1.311.60.2.1.2",
                  "serialNumber": "2.5.4.5", "O": "2.5.4.10", "OU": "2.5.4.11", "CN": "2.5.4.3",
                  "UID": "0.9.2342.19200300.100.1.1"}
# DER string types an attribute value may have, and how to decode each.
STRING_TYPES = {0x0C: "utf-8", 0x13: "ascii", 0x16: "ascii", 0x14: "latin-1", 0x1E: "utf-16-be",
                0x1C: "utf-32-be"}


class Failure(Exception):
    pass


def tlv(data, pos, end, ber=False):
    """Reads one element at pos: returns (tag, value start, value end, element end). DER only,
    unless ber, which also allows a length not in its shortest form and the indefinite length of
    a constructed element, ended by its end-of-contents octets (a CMS signature is BER)."""
    if pos + 2 > end:
        raise ValueError("truncated element")
    tag, length = data[pos], data[pos + 1]
    if tag & 0x1F == 0x1F:
        raise ValueError("high tag numbers are not used here")
    pos += 2
    if length == 0x80:
        if not ber or not tag & 0x20:
            raise ValueError("indefinite length where it is not allowed")
        inner = pos
        while True:
            if inner + 2 > end:
                raise ValueError("an indefinite-length element has no end")
            if data[inner:inner + 2] == b"\0\0":
                return tag, pos, inner, inner + 2
            inner = tlv(data, inner, end, ber)[3]
    if length & 0x80:
        count = length & 0x7F
        if not 1 <= count <= 4 or pos + count > end:
            raise ValueError("bad length")
        length = int.from_bytes(data[pos:pos + count], "big")
        if not ber and (length < 0x80 or data[pos] == 0):
            raise ValueError("length not in its shortest form")
        pos += count
    if pos + length > end:
        raise ValueError("element runs past its parent")
    return tag, pos, pos + length, pos + length


# An element read by elements(): its tag, its value's start and end, and its own start and end.
Element = collections.namedtuple("Element", "tag start end outer_start outer_end")


def elements(data, start, end, ber=False):
    """Every element in data[start:end], which they must fill exactly."""
    out = []
    while start < end:
        tag, value_start, value_end, outer_end = tlv(data, start, end, ber)
        out.append(Element(tag, value_start, value_end, start, outer_end))
        start = outer_end
    return out


def children(data, start, end, ber=False):
    """The (tag, start, end) of every element in data[start:end], which they must fill exactly."""
    return [element[:3] for element in elements(data, start, end, ber)]


def oid(data, start, end):
    arcs, value = [], 0
    for b in data[start:end]:
        value = (value << 7) | (b & 0x7F)
        if not b & 0x80:
            arcs.append(value)
            value = 0
    if not arcs or data[end - 1] & 0x80:  # empty, or ending inside an arc
        raise ValueError("malformed object identifier")
    first = min(arcs[0] // 40, 2)
    return ".".join(str(a) for a in [first, arcs[0] - 40 * first] + arcs[1:])


def read_pem(path, rule):
    """The DER bytes of the one certificate in a PEM file."""
    try:
        with open(path, "rb") as f:
            # One byte more than the bound, so a file over it is refused rather than read in part:
            # the rule is that the file holds exactly one certificate, and a second one hidden
            # after a megabyte of padding would otherwise pass with the first one's fingerprint.
            text = f.read((1 << 20) + 1)
    except OSError as e:
        raise Failure("%s: cannot read %s: %s" % (rule, path, e.strerror))
    if len(text) > 1 << 20:
        raise Failure("%s: %s is larger than 1 MiB; a certificate file holds one PEM certificate"
                      % (rule, path))
    # PEM is anchored at both ends and its body cannot hold a '-', so a match is exactly one block.
    match = PEM.match(text)
    if not match:
        raise Failure("%s: %s must hold exactly one PEM certificate and nothing else" % (rule, path))
    try:
        # Strict: the body's class admits '=' anywhere, and a lenient decoder would skip a stray one.
        return base64.b64decode(re.sub(rb"\s", b"", match.group(1)), validate=True)
    except ValueError:
        raise Failure("%s: %s is not valid base64" % (rule, path))


def parse(der, path):
    """The certificate's subject attributes and its extensions, each (critical, value start, value
    end)."""
    try:
        tag, s, e, _ = tlv(der, 0, len(der))
        if tag != 0x30 or e != len(der):
            raise ValueError("not one DER SEQUENCE")
        top = children(der, s, e)
        if len(top) != 3 or top[0][0] != 0x30:
            raise ValueError("not a certificate")
        tbs = children(der, top[0][1], top[0][2])
        if tbs[0][0] == 0xA0:  # [0] version
            tbs = tbs[1:]
        subject = tbs[4]
        if subject[0] != 0x30:
            raise ValueError("the subject is not a Name")
        attributes = []
        for rdn in children(der, subject[1], subject[2]):
            for _, a_s, a_e in children(der, rdn[1], rdn[2]):
                (_, o_s, o_e), (v_tag, v_s, v_e) = children(der, a_s, a_e)
                encoding = STRING_TYPES.get(v_tag)
                text = der[v_s:v_e].decode(encoding) if encoding else None
                attributes.append((oid(der, o_s, o_e), text))
        extensions = {}
        for tag, x_s, x_e in tbs[6:]:
            if tag != 0xA3:  # [1] and [2] unique identifiers are skipped
                continue
            for _, ext_s, ext_e in children(der, *children(der, x_s, x_e)[0][1:]):
                parts = children(der, ext_s, ext_e)
                name = oid(der, parts[0][1], parts[0][2])
                if name in extensions:
                    raise ValueError("extension %s appears twice" % name)
                if parts[-1][0] != 0x04:
                    raise ValueError("extension %s has no OCTET STRING value" % name)
                critical = len(parts) == 3 and parts[1][0] == 0x01 and der[parts[1][1]:parts[1][2]] != b"\0"
                extensions[name] = (critical, parts[-1][1], parts[-1][2])
        return {"attributes": attributes, "extensions": extensions}
    except (ValueError, IndexError, UnicodeDecodeError) as e:
        raise Failure("certificate: %s is not a certificate this tool reads: %s" % (path, e))


def check_apple(der, extensions, eku):
    """The rules openssl's -ignore_critical leaves to this tool for a Developer ID certificate."""
    if OID_BC in extensions:
        _, s, e = extensions[OID_BC]
        flags = children(der, *tlv(der, s, e)[1:3])
        if flags and flags[0][0] == 0x01 and der[flags[0][1]:flags[0][2]] != b"\0":
            raise Failure("usage: the signing certificate is a CA certificate")
    unknown = sorted(n for n, (critical, _, _) in extensions.items()
                     if critical and n not in STANDARD_CRITICAL | {MARKERS[eku]})
    if unknown:
        raise Failure("usage: the certificate has critical extensions this tool does not accept: %s"
                      % ", ".join(unknown))
    if MARKERS[eku] not in extensions:
        raise Failure("usage: the certificate is not a %s certificate (no %s extension)"
                      % (EKU_NAMES[eku], MARKERS[eku]))


def check_leaf(leaf_path, eku, wanted):
    leaf_der = read_pem(leaf_path, "certificate")
    leaf = parse(leaf_der, leaf_path)
    if eku in MARKERS:
        check_apple(leaf_der, leaf["extensions"], eku)
    if OID_EKU not in leaf["extensions"]:
        raise Failure("eku: the certificate has no extended key usage; %s is required" % EKU_NAMES[eku])
    _, s, e = leaf["extensions"][OID_EKU]
    _, v_s, v_e, _ = tlv(leaf_der, s, e)
    usages = [oid(leaf_der, u_s, u_e) for tag, u_s, u_e in children(leaf_der, v_s, v_e) if tag == 0x06]
    if EKU_OIDS[eku] not in usages:
        raise Failure("eku: the certificate's extended key usage (%s) does not include %s"
                      % (", ".join(usages), EKU_NAMES[eku]))
    for label, want in wanted:
        values = [v for o, v in leaf["attributes"] if o == ATTRIBUTE_OIDS[label]]
        if values != [want]:
            raise Failure("subject: the certificate's subject has %s=%s; exactly %s=%s is required"
                          % (label, " + ".join(repr(v) for v in values) or "nothing", label, want))


def check_cms(in_path, out_path):
    try:
        with open(in_path, "rb") as f:
            data = f.read(MAX_CMS + 1)
    except OSError as e:
        raise Failure("signature: cannot read %s: %s" % (in_path, e.strerror))
    if len(data) > MAX_CMS:
        raise Failure("signature: the CMS signature's room is larger than %d bytes" % MAX_CMS)
    try:
        tag, s, e, end = tlv(data, 0, len(data), ber=True)
        if tag != 0x30 or data[end:].strip(b"\0"):
            raise ValueError("it is not one SEQUENCE followed only by zero bytes")
        content_type, explicit = children(data, s, e, ber=True)[:2]
        if oid(data, content_type[1], content_type[2]) != OID_SIGNED_DATA or explicit[0] != 0xA0:
            raise ValueError("it is not SignedData")
        signed = children(data, *children(data, explicit[1], explicit[2], ber=True)[0][1:], ber=True)
        if len(children(data, signed[2][1], signed[2][2], ber=True)) != 1:
            raise ValueError("its content is not detached")
        signer_infos = signed[-1]
        if signer_infos[0] != 0x31 or len(children(data, signer_infos[1], signer_infos[2], ber=True)) != 1:
            raise ValueError("it does not have exactly one signer")
    except (ValueError, IndexError) as e:
        raise Failure("signature: the CMS signature is not one this tool accepts: %s" % e)
    except RecursionError:
        # Indefinite-length elements nest by recursion; a signature nested this deep is no CMS.
        raise Failure("signature: the CMS signature nests its elements too deeply")
    with open(out_path, "wb") as f:
        f.write(data[:end])


def der_element(tag, body):
    n = len(body)
    size = (n.bit_length() + 7) // 8
    length = bytes([n]) if n < 0x80 else bytes([0x80 | size]) + n.to_bytes(size, "big")
    return bytes([tag]) + length + body


def inside(data, element):
    """The BER elements inside `element`'s value."""
    return elements(data, element.start, element.end, ber=True)


def whole(data, element):
    """`element`'s bytes with its tag and length, so it can be copied unchanged."""
    return data[element.outer_start:element.outer_end]


def algorithm_name(data, identifier):
    """The OID of an AlgorithmIdentifier, which its parameters follow."""
    first = inside(data, identifier)[0]
    return oid(data, first.start, first.end)


def issuer_and_serial(der):
    """A certificate's issuer and serialNumber, each as whole DER elements."""
    _, start, end, _ = tlv(der, 0, len(der))
    fields = inside(der, elements(der, start, end, ber=True)[0])  # the TBSCertificate's fields
    if fields[0].tag == 0xA0:  # the version, when present
        fields = fields[1:]
    serial, _signature, issuer = fields[:3]
    return whole(der, issuer), whole(der, serial)


def pem(cert):
    return ("-----BEGIN CERTIFICATE-----\n%s-----END CERTIFICATE-----\n"
            % base64.encodebytes(cert).decode("ascii")).encode("ascii")


def check_authenticode(in_path, out_dir):
    try:
        with open(in_path, "rb") as f:
            data = f.read(MAX_CMS + 1)
    except OSError as e:
        raise Failure("signature: cannot read %s: %s" % (in_path, e.strerror))
    if len(data) > MAX_CMS:
        raise Failure("signature: the MSI's signature is larger than %d bytes" % MAX_CMS)
    try:
        top = elements(data, 0, len(data), ber=True)
        if len(top) != 1 or top[0].tag != 0x30:
            raise ValueError("it is not one SEQUENCE")
        content_type, explicit = inside(data, top[0])
        signed_data = oid(data, content_type.start, content_type.end) == OID_SIGNED_DATA
        if not signed_data or explicit.tag != 0xA0:
            raise ValueError("it is not SignedData")
        # version, digestAlgorithms, encapContentInfo, then [0] certificates and [1] CRLs if any,
        # then signerInfos.
        signed = inside(data, inside(data, explicit)[0])
        content_info, signer_infos = signed[2], signed[-1]
        encap = inside(data, content_info)
        if len(encap) != 2 or oid(data, encap[0].start, encap[0].end) != OID_SPC_INDIRECT_DATA:
            raise ValueError("its content is not an SpcIndirectDataContent")
        # The messageDigest covers the SpcIndirectDataContent's value, without its tag and length.
        tag, c_s, c_e, c_end = tlv(data, encap[1].start, encap[1].end)
        if tag != 0x30 or c_end != encap[1].end:
            raise ValueError("its SpcIndirectDataContent is not one DER SEQUENCE")
        indirect, digest_info = elements(data, c_s, c_e)
        sip_type, sip_info = elements(data, indirect.start, indirect.end)
        sip = elements(data, sip_info.start, sip_info.end)
        if oid(data, sip_type.start, sip_type.end) != OID_SPC_SIPINFO or len(sip) < 2 or \
                sip[1].tag != 0x04 or data[sip[1].start:sip[1].end] != MSI_SIP:
            raise ValueError("it is not the signature of an MSI")
        algorithm, digest = elements(data, digest_info.start, digest_info.end)
        name = algorithm_name(data, algorithm)
        if name not in DIGESTS:
            raise Failure("signature: the MSI digest's algorithm %s is not SHA-256, SHA-384 or "
                          "SHA-512" % name)
        if digest.tag != 0x04 or digest.end - digest.start != DIGESTS[name][1]:
            raise Failure("signature: the MSI digest is not the %d bytes of a %s digest"
                          % (DIGESTS[name][1], DIGESTS[name][0]))
        signers = inside(data, signer_infos)
        if signer_infos.tag != 0x31 or len(signers) != 1:
            raise ValueError("it does not have exactly one signer")
        # version, sid, digestAlgorithm, [0] signedAttrs if any, signatureAlgorithm, signature.
        signer = inside(data, signers[0])
        sid, signer_digest = signer[1], signer[2]
        signature_algorithm = signer[4] if signer[3].tag == 0xA0 else signer[3]
        if sid.tag != 0x30:
            raise ValueError("its signer is not named by issuer and serial number")
        if algorithm_name(data, signer_digest) not in DIGESTS:
            raise Failure("signature: the signer's digest algorithm %s is not SHA-256, SHA-384 or "
                          "SHA-512" % algorithm_name(data, signer_digest))
        if algorithm_name(data, signature_algorithm) not in SIGNATURES:
            raise Failure("signature: the signer's signature algorithm %s is not RSA or ECDSA with "
                          "SHA-256, SHA-384 or SHA-512" % algorithm_name(data, signature_algorithm))
        issuer, serial = (whole(data, x) for x in inside(data, sid))
        leaves, certificates = [], []
        for field in signed[3:-1]:
            if field.tag != 0xA0:  # [0] certificates; [1] CRLs are not read
                continue
            for cert in inside(data, field):
                der = whole(data, cert)
                certificates.append(der)
                if issuer_and_serial(der) == (issuer, serial):
                    leaves.append(der)
        if len(leaves) != 1:
            raise ValueError("%d of its certificates are the signer's, not one" % len(leaves))
        # The same SignedData with the content left out, which openssl reads as detached content.
        body = b"".join(whole(data, x) for x in signed[:2]) + \
            der_element(0x30, whole(data, encap[0])) + b"".join(whole(data, x) for x in signed[3:])
        detached = der_element(0x30, whole(data, content_type) +
                               der_element(0xA0, der_element(0x30, body)))
    except (ValueError, IndexError) as e:
        raise Failure("signature: the MSI's signature is not one this tool accepts: %s" % e)
    except RecursionError:
        raise Failure("signature: the MSI's signature nests its elements too deeply")
    if os.listdir(out_dir):
        raise Failure("signature: %s is not empty" % out_dir)
    files = [("content", data[c_s:c_e]), ("signed.der", detached), ("leaf.pem", pem(leaves[0]))]
    files += [("certificate-%d.pem" % (i + 1), pem(c)) for i, c in enumerate(certificates)]
    for out_name, out_bytes in files:
        with open(os.path.join(out_dir, out_name), "xb") as f:
            f.write(out_bytes)
    return "%s %s" % (DIGESTS[name][0], data[digest[1]:digest[2]].hex())


def main(argv):
    try:
        if len(argv) == 3 and argv[1] == "fingerprint":
            der = read_pem(argv[2], "certificate")
            parse(der, argv[2])  # the bytes must be a certificate this tool reads, not any DER
            sys.stdout.write(hashlib.sha256(der).hexdigest() + "\n")
        elif len(argv) >= 4 and argv[1] == "leaf" and argv[3] in EKU_OIDS:
            wanted = []
            for pair in argv[4:]:
                label, _, value = pair.partition("=")
                if label not in ATTRIBUTE_OIDS:
                    sys.stderr.write("x509.py: %s is not a subject attribute this tool reads\n" % label)
                    return 2
                wanted.append((label, value))
            check_leaf(argv[2], argv[3], wanted)
        elif len(argv) == 4 and argv[1] == "cms":
            check_cms(argv[2], argv[3])
        elif len(argv) == 4 and argv[1] == "authenticode":
            sys.stdout.write(check_authenticode(argv[2], argv[3]) + "\n")
        elif len(argv) == 3 and argv[1] == "subject":
            labels = dict((v, k) for k, v in ATTRIBUTE_OIDS.items())
            for name, value in parse(read_pem(argv[2], "certificate"), argv[2])["attributes"]:
                # Quoted when not printable, so a value holding a newline cannot forge a line. A value
                # of a string type parse() does not decode is named as such, not printed.
                if value is None:
                    shown = "(a value of a type this tool does not decode)"
                else:
                    shown = value if value.isprintable() else ascii(value)
                sys.stdout.write("%s=%s\n" % (labels.get(name, name), shown))
        else:
            sys.stderr.write("usage: x509.py fingerprint <cert.pem> | "
                             "leaf <leaf.pem> <eku> <attribute>=<value>... | "
                             "cms <in.ber> <out.der> | authenticode <signature.der> <out-dir> | "
                             "subject <cert.pem>\n")
            return 2
    except Failure as e:
        sys.stderr.write("%s\n" % e)
        return 1
    except (ValueError, IndexError) as e:
        sys.stderr.write("certificate: a certificate extension is malformed: %s\n" % e)
        return 1
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv))
