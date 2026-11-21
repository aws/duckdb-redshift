"""Writes small macOS flat packages (xar) for the tool tests, including ones no packaging tool
would write: member types a normal user cannot create, odd payload formats, and tables of contents
that point outside the file.

    python3 -I make_xar.py <out.pkg> '<json spec>'

The spec is an object; every key is optional:

  members      [[path, kind, nlink, data], ...] in the payload's cpio archive, in order. kind is
               file, link, dir, fifo or dev; nlink defaults to 1; data defaults to "data of <path>",
               and "@<file>" takes a file's bytes. Default: one file ./librsodbc64.dylib.
  cpio         odc (default, as Apple's tools write) or newc
  compress     gzip (default) or bzip2
  component    the component directory holding Payload (default AmazonRedshiftODBC.pkg); "" puts
               Payload at the top of the package
  encoding     octet (default) or zlib: how the table of contents stores Payload in the heap
  alg          the header's and table of contents' checksum algorithm: sha1 (default), sha256 (a
               header that names it), md5 or none
  payload_alg  the algorithm of Payload's archived and extracted checksums (default: alg, or sha1
               when alg is none)
  shape        a list of the 2.3.0.0 package's shapes, which the reader must accept; see SHAPES
  nest         a number of directories, each inside the one before, beside the component
  break        a list of changes that make the package wrong; see BREAKS
  sign         {"key": <key.pem>, "certs": [<signer.pem>, <more.pem>...], "cms": true,
               "cms_key": <key.pem>, "cms_signer": <cert.pem>}: signs the table of contents' checksum
               as productsign does, with an RSA signature (PKCS #1 v1.5) and, unless cms is false, a
               detached CMS signature, both naming certs in the table of contents; cms_key and
               cms_signer make the CMS signature another key's, "cms_nodetach": true puts the
               checksum inside it, and "cms_second": [<key.pem>, <cert.pem>] adds a second signer.
               The CMS signature's digest is always SHA-1, as 2.3.0.0's is. Needs openssl.
  top_file     a name: a file of that name at the top of the package, outside every payload
  package_info {"identifier": <id>, "version": <version>, "tag": <root element>, "alg": <alg>,
               "raw": <text>, "absent": true}: the component's PackageInfo, beside its Payload, as
               Apple's tools write it (default: identifier com.amazon.redshift.odbc2x64, version
               MAKE_XAR_VERSION's, else 2.3.0.0, root <pkg-info>, checksums in payload_alg); raw is its whole text, and
               absent leaves it out
  other_component
               {"members": [[path, kind, data], ...], "alg": <alg>, "raw": <text>,
               "payload_type": <type>}: a second component, Other.pkg, whose Payload holds those
               members (or is that text), with its checksums in alg (default: payload_alg);
               payload_type makes its Payload entry that type, such as directory, with no data
  big          [[name, size, zeros], ...]: files at the top of the package, or, named <dir>/<name>, in a
               directory there of their own, each stored as the zlib
               stream of <zeros> zero bytes (streamed, so a large one costs little memory) with the
               decoded size <size> in the table of contents; size "rest:<n>" makes the decoded sizes
               of every file in the table of contents add up to exactly <n>

Standard library only, Python 3.6 and later.
"""

import base64
import hashlib
import json
import os
import stat
import struct
import subprocess
import sys
import tempfile
import zlib

KINDS = {
    "file": stat.S_IFREG | 0o755,
    "link": stat.S_IFLNK | 0o777,
    "dir": stat.S_IFDIR | 0o755,
    "fifo": stat.S_IFIFO | 0o644,
    "dev": stat.S_IFCHR | 0o644,
}
HEADER_ALGORITHMS = {"none": 0, "sha1": 1, "md5": 2}
DIGESTS = {"sha1": hashlib.sha1, "sha256": hashlib.sha256, "md5": hashlib.md5}
SHAPES = {
    "repeated-names": "every file repeats its <name>, Payload three times",
    "signature-elements": "the table of contents has an RSA and a CMS signature element",
    "heap-trailing": "1,811 bytes follow the heap's last file",
}
BREAKS = {
    "payload-checksum": "Payload's recorded checksums are of other bytes",
    "payload-outside": "Payload's heap offset points past the end of the file",
    "names-differ": "Payload has a second <name> that differs",
    "two-payloads": "the component holds Payload twice",
    "doctype": "the table of contents has a document type declaration",
    # rework() applies the breaks from here to "short" to the package build() wrote.
    "doctype-bare": "the table of contents has a document type declaration that declares nothing",
    "entity-used": "the document type declares an entity, and Payload's name is that entity",
    "doctype-utf16": "as entity-used, in a UTF-16 table of contents, where the bytes <!DOCTYPE are not",
    "root-tag": "the table of contents' root element is <xbr>",
    "no-toc": "<xar> holds no <toc>",
    "no-name": "Distribution has no <name>",
    "name-flat": "the top level holds one file named AmazonRedshiftODBC.pkg/Payload, not the component",
    "name-dotdot": "Distribution is named ..",
    "name-dot": "Distribution is named .",
    "name-empty": "Distribution's name is empty",
    "two-data": "Payload has two <data> elements",
    "offset-not-number": "Payload's <offset> is not a decimal number",
    "payload-size": "Payload's <size> is one more than its bytes",
    "checksum-style-unknown": "Payload's archived checksum names an algorithm the reader does not know",
    "zlib-not-zlib": "the zlib-encoded Payload's bytes are not a zlib stream (with encoding zlib)",
    "zlib-truncated": "the zlib-encoded Payload's stream is cut short (with encoding zlib)",
    "zlib-trailing": "data follows the zlib-encoded Payload's stream (with encoding zlib)",
    "gzip-truncated": "the payload's gzip stream is cut short",
    "odc-mtime": "an odc member's mtime does not fit newc's 32 bits",
    "odc-size": "an odc member's size does not fit newc's 32 bits",
    "odc-name-size-zero": "an odc member's name size is 0",
    "odc-name-size-long": "an odc member's name size is over 4096",
    "odc-truncated": "the odc archive ends inside its first member",
    "odc-magic": "the odc trailer's header has another magic",
    "odc-trailing": "bytes that are not zero follow the odc trailer, inside the compressed payload",
    "odc-padding": "4,608 zero bytes follow the odc trailer's 512-byte block, inside the compressed payload",
    "version": "the header's version is 2",
    "header-size": "the header's size is 32 with a numbered checksum algorithm",
    "header-algorithm": "the header's checksum algorithm number is 4",
    "toc-size-bound": "the header gives the table of contents a decoded size over 16 MiB",
    "toc-length-bound": "the header gives the table of contents a stored size over 16 MiB",
    "toc-past-end": "the header gives the table of contents a stored size that runs past the file",
    "toc-trailing": "data follows the table of contents' zlib stream, inside its stored size",
    "toc-truncated": "the table of contents' zlib stream is cut short; the header gives what it decodes to",
    "toc-not-zlib": "the table of contents is not a zlib stream",
    "toc-not-xml": "the table of contents ends before </xar>",
    "prolog-not-xml": "text that is not XML comes before the root element",
    "short": "the file is only the four bytes xar!",
    "toc-size": "the header gives the table of contents another uncompressed size",
    "gzip-trailing": "data follows the payload's gzip stream",
    "payload-directory": "Payload is a directory in the table of contents",
    "encoding-bzip2": "Payload's encoding is one the reader does not accept",
    "magic": "the header's magic is not xar!",
    "odc-not-octal": "an odc header field holds a digit that is not octal",
    "extracted-checksum": "Payload's extracted checksum is of other bytes",
    "checksum-outside": "the stored checksum's offset points past the end of the file",
    "checksum-size-outside": "the stored checksum's size runs past the end of the file",
    "checksum-style": "the table of contents names another checksum algorithm than the header",
    "signature-outside": "the RSA signature's offset points past the end of the file",
    "rsa-corrupt": "one byte of the RSA signature is changed",
    "toc-changed": "one byte of the table of contents is changed after signing",
    "payload-changed": "one byte of the payload is changed after signing",
    "checksum-size-short": "the stored checksum's size is one byte short of the algorithm's",
    "signature-style": "the signature's style is not RSA",
    "two-signatures": "the table of contents has a second RSA signature",
    "cms-signer-other": "the CMS signature's certificates name another signer than the RSA one",
    "cms-trailing": "a byte that is not zero follows the CMS signature in its room",
    "two-cms": "the table of contents has a second CMS signature",
    "cms-style": "the CMS signature's style is not CMS",
    "cms-outside": "the CMS signature's offset points past the end of the file",
    "no-keyinfo": "the RSA signature names no certificate",
    "bad-base64": "the RSA signature's certificate is not valid base64",
    "cert-not-x509": "the signing certificate both signatures name is not a certificate",
    "cms-garbage": "the CMS signature's room holds bytes that are not BER",
}
DSIG = "http://www.w3.org/2000/09/xmldsig#"
CMS_ROOM = 9216  # what productsign reserves


def cpio(members, form, breaks=frozenset()):
    out = []
    for i, (name, kind, nlink, data) in enumerate(members + [("TRAILER!!!", None, 1, b"")], 1):
        mode = KINDS[kind] if kind else 0
        raw = name.encode() + b"\0"
        if form.startswith("odc"):
            out.append(
                b"070707"
                + b"".join(b"%06o" % v for v in (0, i, mode, 0, 0, nlink, 0))
                + b"%011o%06o%011o" % (0, len(raw), len(data))
                + raw
                + data
            )
        else:
            fields = (i, mode, 0, 0, nlink, 0, len(data), 0, 0, 0, 0, len(raw), 0)
            head = b"070701" + b"".join(b"%08X" % v for v in fields)
            out.append(head + raw + b"\0" * ((4 - (110 + len(raw)) % 4) % 4) + data + b"\0" * ((4 - len(data) % 4) % 4))
    blob = b"".join(out)
    if form == "odc-not-octal":
        blob = blob[:20] + b"8" + blob[21:]
    blob += b"\0" * ((512 - len(blob) % 512) % 512)
    if "odc-trailing" in breaks:
        blob += b"JUNK"
    if "odc-padding" in breaks:
        blob += bytes(4608)
    return blob


def compress(blob, how, breaks):
    if how == "gzip":
        c = zlib.compressobj(9, zlib.DEFLATED, 31)
        out = c.compress(blob) + c.flush()
        return out + (b"trailing" if "gzip-trailing" in breaks else b"")
    import bz2

    return bz2.compress(blob)


def file_xml(file_id, name, data_xml, kind="file", names=None):
    names = names or [name]
    return '<file id="%d">%s<type>%s</type>%s</file>' % (
        file_id,
        data_xml,
        kind,
        "".join("<name>%s</name>" % n for n in names),
    )


def data_xml(offset, stored, content, encoding, alg, breaks):
    archived, extracted = DIGESTS[alg](stored).hexdigest(), DIGESTS[alg](content).hexdigest()
    if "payload-checksum" in breaks:
        archived = extracted = DIGESTS[alg](b"other bytes").hexdigest()
    if "extracted-checksum" in breaks:
        extracted = DIGESTS[alg](b"other bytes").hexdigest()
    return (
        "<data><length>%d</length><offset>%d</offset><size>%d</size><encoding style=\"%s\"/>"
        "<extracted-checksum style=\"%s\">%s</extracted-checksum>"
        "<archived-checksum style=\"%s\">%s</archived-checksum></data>"
        % (len(stored), offset, len(content), encoding, alg, extracted, alg, archived)
    )


HEADER_BREAKS = (
    "version",
    "header-size",
    "header-algorithm",
    "toc-size-bound",
    "toc-length-bound",
    "toc-past-end",
    "toc-trailing",
    "toc-truncated",
    "toc-not-zlib",
)
KEY_INFO = '<KeyInfo xmlns="http://www.w3.org/2000/09/xmldsig#"><X509Data><X509Certificate>MIIB</X509Certificate>'
SIGNATURES = (
    '<signature style="RSA"><offset>20</offset><size>256</size>%s</X509Data></KeyInfo></signature>'
    '<x-signature style="CMS"><offset>276</offset><size>9216</size>%s</X509Data></KeyInfo></x-signature>'
    % (KEY_INFO, KEY_INFO)
)


def new_payload(breaks, stored, content):
    """(stored, content) for a break that replaces Payload's bytes, or None."""
    if "zlib-not-zlib" in breaks:
        return content, content
    if "zlib-truncated" in breaks:
        return zlib.compress(content)[:-6], content
    if "zlib-trailing" in breaks:
        return zlib.compress(content) + b"trailing", content
    if "gzip-truncated" in breaks:
        return stored[: len(stored) // 2], stored[: len(stored) // 2]
    # odc header fields by offset: mtime at 48, name size at 59, file size at 65; the trailer's
    # header follows the one member at 123.
    odc = {
        "odc-mtime": (48, b"77777777777"),
        "odc-size": (65, b"77777777777"),
        "odc-name-size-zero": (59, b"000000"),
        "odc-name-size-long": (59, b"010001"),
        "odc-magic": (123, b"070700"),
        "odc-truncated": None,
    }
    for name in sorted(breaks):
        if name in odc:
            blob = bytearray(cpio([("./librsodbc64.dylib", "file", 1, b"data of ./librsodbc64.dylib")], "odc"))
            if odc[name]:
                at, value = odc[name]
                blob[at : at + len(value)] = value
            else:
                blob = blob[:100]
            blob = compress(bytes(blob), "gzip", ())
            return blob, blob
    return None


def payload_data(xml):
    """(start, end) of Payload's <data> element, which build() writes first in <file id="2">."""
    start = xml.index('<file id="2"><data>') + len('<file id="2">')
    return start, xml.index("</data>", start) + len("</data>")


def between(text, before, after):
    return text.split(before, 1)[1].split(after, 1)[0]


def edit_toc(xml, breaks, shapes, nest):
    """The table of contents with the breaks and shapes that change only its text."""
    payload_name = "&p;" if breaks & {"entity-used", "doctype-utf16"} else "Payload"
    xml = xml.replace("<name>Payload</name>", "<name>%s</name>" % payload_name)
    doctype = {
        "doctype-bare": "<!DOCTYPE xar>",
        "entity-used": '<!DOCTYPE xar [<!ENTITY p "Payload">]>',
        "doctype-utf16": '<!DOCTYPE xar [<!ENTITY p "Payload">]>',
    }
    for name in sorted(breaks & set(doctype)):
        xml = xml.replace("<xar>", doctype[name] + "<xar>", 1)
    if "root-tag" in breaks:
        xml = xml.replace("<xar>", "<xbr>").replace("</xar>", "</xbr>")
    if "prolog-not-xml" in breaks:
        xml = xml.replace("<xar>", "text<xar>", 1)
    if "toc-not-xml" in breaks:
        xml = xml.replace("</toc></xar>", "</toc>")
    if "no-toc" in breaks:
        xml = xml.replace("<toc>", "<tod>").replace("</toc>", "</tod>")
    renamed = {"no-name": "", "name-dotdot": "..", "name-dot": ".", "name-empty": ""}
    for name in breaks & set(renamed):
        name_xml = "" if name == "no-name" else "<name>%s</name>" % renamed[name]
        xml = xml.replace("<name>Distribution</name>", name_xml)
    if "name-flat" in breaks:
        xml = xml.replace('<file id="1"><type>directory</type><name>AmazonRedshiftODBC.pkg</name>', "")
        xml = xml.replace(
            "<name>%s</name></file></file>" % payload_name, "<name>AmazonRedshiftODBC.pkg/Payload</name></file>"
        )
    start, end = payload_data(xml)
    data = xml[start:end]
    if "two-data" in breaks:
        data += data
    if "offset-not-number" in breaks:
        data = data.replace("<offset>", "<offset>0x", 1)
    if "payload-size" in breaks:
        size = between(data, "<size>", "</size>")
        data = data.replace("<size>%s</size>" % size, "<size>%d</size>" % (int(size) + 1), 1)
    if "checksum-style-unknown" in breaks:
        style = between(data, '<archived-checksum style="', '"')
        data = data.replace('<archived-checksum style="%s"' % style, '<archived-checksum style="crc32"')
    xml = xml[:start] + data + xml[end:]
    if "repeated-names" in shapes:
        pieces = xml.split("</name>")
        for i, piece in enumerate(pieces[:-1]):
            name = piece.rsplit("<name>", 1)[1]
            pieces[i] = piece + ("</name><name>%s" % name) * (2 if name == payload_name else 1)
        xml = "</name>".join(pieces)
    if "signature-elements" in shapes:
        xml = xml.replace("<file ", SIGNATURES + "<file ", 1)
    if nest:
        chain = "".join('<file id="%d"><type>directory</type><name>d</name>' % (100 + i) for i in range(nest))
        xml = xml.replace("</toc>", chain + "</file>" * nest + "</toc>")
    return xml


def rework(data, spec):
    """The package build() wrote, with the breaks and shapes it leaves to this: the table of
    contents is rewritten, a replaced Payload goes to the end of the heap, and the header is written
    again, keeping any break build() made in it. Unchanged when none applies."""
    breaks, shapes = set(spec.get("break", [])), set(spec.get("shape", []))
    unknown = shapes - set(SHAPES)
    if unknown:
        raise SystemExit("make_xar.py: unknown shape %s" % ", ".join(sorted(unknown)))
    if "short" in breaks:
        return b"xar!"
    size, version, toc_length, toc_size, number = struct.unpack(">HHQQI", data[4:28])
    toc = data[size : size + toc_length]
    text = zlib.decompress(toc).decode()
    heap = bytearray(data[size + toc_length :])
    xml = text
    if "<file id=\"2\"><data>" in xml:
        start, end = payload_data(xml)
        old = xml[start:end]
        length, offset = int(between(old, "<length>", "<")), int(between(old, "<offset>", "<"))
        encoding, alg = between(old, '<encoding style="', '"'), between(old, '<archived-checksum style="', '"')
        stored = bytes(heap[offset : offset + length])
        content = zlib.decompress(stored) if encoding == "application/x-gzip" else stored
        replaced = new_payload(breaks, stored, content)
        if replaced:
            xml = xml[:start] + data_xml(len(heap), replaced[0], replaced[1], encoding, alg, ()) + xml[end:]
            heap += replaced[0]
    xml = edit_toc(xml, breaks, shapes, spec.get("nest", 0))
    if xml == text and "heap-trailing" not in shapes and not breaks & set(HEADER_BREAKS):
        return data
    if "doctype-utf16" in breaks:
        new_text = xml.replace('encoding="UTF-8"', 'encoding="UTF-16"').encode("utf-16")
    else:
        new_text = xml.encode()
    new_toc = zlib.compress(new_text)
    toc_size += len(new_text) - len(text.encode())  # keeps build()'s toc-size break
    rest = data[28:size]
    algorithm = {0: None, 1: "sha1", 2: "md5", 3: rest.split(b"\0")[0].decode()}[number]
    digest_size = DIGESTS[algorithm]().digest_size if algorithm else 0
    if digest_size and heap[:digest_size] == DIGESTS[algorithm](toc).digest():
        heap[:digest_size] = DIGESTS[algorithm](new_toc).digest()
    if "toc-trailing" in breaks:
        new_toc += b"trailing"
    if "toc-not-zlib" in breaks:
        new_toc = b"not a zlib stream"
    if "toc-truncated" in breaks:
        new_toc = new_toc[:-8]
        toc_size = len(zlib.decompressobj().decompress(new_toc))
    toc_length = len(new_toc)
    if "version" in breaks:
        version = 2
    if "header-size" in breaks:
        size = 32
    if "header-algorithm" in breaks:
        number = 4
    if "toc-size-bound" in breaks:
        toc_size = (16 << 20) + 1
    if "toc-length-bound" in breaks:
        toc_length = (16 << 20) + 1
    if "toc-past-end" in breaks:
        toc_length = len(new_toc) + len(heap) + 1
    header = data[:4] + struct.pack(">HHQQI", size, version, toc_length, toc_size, number) + rest
    return header + new_toc + bytes(heap) + (b"t8lr" + bytes(1807) if "heap-trailing" in shapes else b"")


def build(spec):
    breaks = set(spec.get("break", []))
    unknown = breaks - set(BREAKS)
    if unknown:
        raise SystemExit("make_xar.py: unknown break %s" % ", ".join(sorted(unknown)))
    members = []
    for m in spec.get("members", [["./librsodbc64.dylib", "file"]]):
        name, kind = m[0], m[1]
        nlink = m[2] if len(m) > 2 else 1
        data = m[3] if len(m) > 3 else "data of " + name
        data = open(data[1:], "rb").read() if data.startswith("@") else data.encode()
        members.append((name, kind, nlink, data))
    alg = spec.get("alg", "sha1")
    payload_alg = spec.get("payload_alg", alg if alg != "none" else "sha1")
    form = "odc-not-octal" if "odc-not-octal" in breaks else spec.get("cpio", "odc")
    payload = compress(cpio(members, form, breaks), spec.get("compress", "gzip"), breaks)

    # The heap: the table of contents' checksum, room for any signatures, then the files.
    digest_size = DIGESTS[alg]().digest_size if alg != "none" else 0
    heap = bytearray(digest_size)
    signing = spec.get("sign")
    signature_xml, rsa_at, cms_at = "", None, None
    if signing:
        rsa_size = key_bytes(signing["key"])
        rsa_at, cms_at = len(heap), len(heap) + rsa_size
        keyinfo = '<KeyInfo xmlns="%s"><X509Data>%s</X509Data></KeyInfo>' % (
            DSIG,
            "".join("<X509Certificate>%s</X509Certificate>" % pem_base64(c) for c in signing["certs"]),
        )
        if "cert-not-x509" in breaks:
            keyinfo = keyinfo.replace(
                pem_base64(signing["certs"][0]), base64.b64encode(b"not a certificate").decode(), 1
            )
        rsa_keyinfo = keyinfo
        if "no-keyinfo" in breaks:
            rsa_keyinfo = ""
        if "bad-base64" in breaks:
            rsa_keyinfo = keyinfo.replace(pem_base64(signing["certs"][0]), "!!!!", 1)
        signature_xml = '<signature style="%s"><offset>%d</offset><size>%d</size>%s</signature>' % (
            "DSA" if "signature-style" in breaks else "RSA",
            rsa_at + (1 << 30 if "signature-outside" in breaks else 0),
            rsa_size,
            rsa_keyinfo,
        )
        if "two-signatures" in breaks:
            signature_xml *= 2
        heap += b"\0" * rsa_size
        if signing.get("cms", True):
            cms_keyinfo = keyinfo
            if "cms-signer-other" in breaks:
                cms_keyinfo = keyinfo.replace(pem_base64(signing["certs"][0]), pem_base64(signing["cms_signer"]), 1)
            cms_xml = '<x-signature style="%s"><offset>%d</offset><size>%d</size>%s</x-signature>' % (
                "PKCS7" if "cms-style" in breaks else "CMS",
                cms_at + (1 << 30 if "cms-outside" in breaks else 0),
                CMS_ROOM,
                cms_keyinfo,
            )
            signature_xml += cms_xml * (2 if "two-cms" in breaks else 1)
            heap += b"\0" * CMS_ROOM
    encoding = "application/x-gzip" if spec.get("encoding") == "zlib" else "application/octet-stream"
    if "encoding-bzip2" in breaks:
        encoding = "application/x-bzip2"
    stored = zlib.compress(payload) if spec.get("encoding") == "zlib" else payload
    info_xml = package_info(spec, heap, payload_alg)  # its bytes before Payload's
    offset = len(heap) + (1 << 30 if "payload-outside" in breaks else 0)
    payload_at = len(heap)
    heap += stored
    names = ["Payload", "Payload2"] if "names-differ" in breaks else None
    kind = "directory" if "payload-directory" in breaks else "file"
    payload_xml = file_xml(2, "Payload", data_xml(offset, stored, payload, encoding, payload_alg, breaks), kind, names)
    payload_xml = info_xml + payload_xml
    if "two-payloads" in breaks:
        payload_xml += file_xml(3, "Payload", "")
    distribution = b"<installer-gui-script minSpecVersion=\"1\"/>\n"
    dist_stored = zlib.compress(distribution)
    dist_xml = file_xml(
        9, "Distribution", data_xml(len(heap), dist_stored, distribution, "application/x-gzip", payload_alg, ())
    )
    heap += dist_stored
    component = spec.get("component", "AmazonRedshiftODBC.pkg")
    tree = (
        '<file id="1"><type>directory</type><name>%s</name>%s</file>' % (component, payload_xml)
        if component
        else payload_xml
    )
    tree += extra_files(spec, heap, payload_alg)
    tree += big_files(spec, heap, payload_alg, tree + dist_xml)
    checksum_xml = (
        '<checksum style="%s"><offset>%d</offset><size>%d</size></checksum>'
        % (
            "sha256" if "checksum-style" in breaks else alg,
            1 << 30 if "checksum-outside" in breaks else 0,
            (
                1 << 30
                if "checksum-size-outside" in breaks
                else digest_size - 1 if "checksum-size-short" in breaks else digest_size
            ),
        )
        if alg != "none"
        else ""
    )
    xml = (
        '<?xml version="1.0" encoding="UTF-8"?>\n%s<xar><toc><creation-time>2026-10-16T15:34:55'
        '</creation-time>%s%s%s%s</toc></xar>\n'
        % (
            '<!DOCTYPE xar [<!ENTITY e "x">]>' if "doctype" in breaks else "",
            checksum_xml,
            signature_xml,
            tree,
            dist_xml,
        )
    ).encode()
    toc = zlib.compress(xml)
    if digest_size:
        heap[0:digest_size] = DIGESTS[alg](toc).digest()
    if signing:
        sign(signing, heap, DIGESTS[alg](toc).digest(), alg, rsa_at, cms_at)
        if "rsa-corrupt" in breaks:
            heap[rsa_at + 7] ^= 1
        if "cms-trailing" in breaks:
            heap[cms_at + CMS_ROOM - 1] = 1
        if "cms-garbage" in breaks:
            heap[cms_at : cms_at + CMS_ROOM] = b"\x01" * CMS_ROOM
    if "toc-changed" in breaks:
        xml = xml.replace(b"2026-10-16", b"2026-10-17", 1)
        toc = zlib.compress(xml)
    if "payload-changed" in breaks:
        heap[payload_at + len(stored) // 2] ^= 1
    return header_for(alg, toc, len(xml) + (1 if "toc-size" in breaks else 0), breaks) + toc + bytes(heap)


def package_info(spec, heap, alg):
    """The table of contents' entry for the component's PackageInfo, whose bytes go onto heap."""
    info = spec.get("package_info", {})
    if info.get("absent"):
        return ""
    text = info.get(
        "raw",
        '<?xml version="1.0" encoding="utf-8"?>\n<%s overwrite-permissions="true" relocatable="false" '
        'identifier="%s" postinstall-action="none" version="%s" format-version="2" '
        'install-location="/opt/amazon/redshiftodbcx64" auth="root">\n    <payload numberOfFiles="9"/>\n</%s>\n'
        % (
            info.get("tag", "pkg-info"),
            info.get("identifier", "com.amazon.redshift.odbc2x64"),
            info.get("version", os.environ.get("MAKE_XAR_VERSION", "2.3.0.0")),
            info.get("tag", "pkg-info"),
        ),
    ).encode()
    stored = zlib.compress(text)
    xml = file_xml(4, "PackageInfo", data_xml(len(heap), stored, text, "application/x-gzip", info.get("alg", alg), ()))
    heap += stored
    return xml


def extra_files(spec, heap, alg):
    """The table of contents' entries for top_file and other_component, whose bytes go onto heap."""
    xml = ""
    if spec.get("top_file"):
        data = b"a file outside every payload\n"
        xml += file_xml(20, spec["top_file"], data_xml(len(heap), data, data, "application/octet-stream", alg, ()))
        heap += data
    other = spec.get("other_component")
    if other:
        members = [
            (m[0], m[1], 1, (m[2] if len(m) > 2 else "data of " + m[0]).encode()) for m in other.get("members", [])
        ]
        blob = other["raw"].encode() if "raw" in other else compress(cpio(members, "odc"), "gzip", ())
        data = data_xml(len(heap), blob, blob, "application/octet-stream", other.get("alg", alg), ())
        heap += blob
        payload = (
            file_xml(22, "Payload", data)
            if "payload_type" not in other
            else file_xml(22, "Payload", "", other["payload_type"])
        )
        xml += '<file id="21"><type>directory</type><name>Other.pkg</name>%s</file>' % payload
    return xml


def big_files(spec, heap, alg, xml):
    """The table of contents' entries for big, whose zlib streams go onto heap; xml is the rest of
    the table of contents, whose decoded sizes a "rest:<n>" size completes to <n>."""
    out = ""
    for i, (name, size, zeros) in enumerate(spec.get("big", [])):
        c, stored = zlib.compressobj(9), []
        for at in range(0, zeros, 1 << 20):
            stored.append(c.compress(bytes(min(1 << 20, zeros - at))))
        stored = b"".join(stored) + c.flush()
        if isinstance(size, str):
            sizes = [int(piece.split("</size>", 1)[0]) for piece in (xml + out).split("<size>")[1:]]
            size = int(size.split(":", 1)[1]) - sum(sizes)
        checksum = DIGESTS[alg](stored).hexdigest()
        data = (
            "<data><length>%d</length><offset>%d</offset><size>%d</size><encoding style=\"application/x-gzip\"/>"
            "<extracted-checksum style=\"%s\">%s</extracted-checksum>"
            "<archived-checksum style=\"%s\">%s</archived-checksum></data>"
            % (len(stored), len(heap), size, alg, checksum, alg, checksum)
        )
        parent, _, base = name.rpartition("/")
        entry = file_xml(40 + i, base, data)
        out += (
            '<file id="%d"><type>directory</type><name>%s</name>%s</file>' % (30 + i, parent, entry)
            if parent
            else entry
        )
        heap += stored
    return out


def header_for(alg, toc, toc_size, breaks):
    if alg in HEADER_ALGORITHMS:
        header = struct.pack(">4sHHQQI", b"xar!", 28, 1, len(toc), toc_size, HEADER_ALGORITHMS[alg])
    else:
        header = struct.pack(">4sHHQQI", b"xar!", 64, 1, len(toc), toc_size, 3) + alg.encode().ljust(36, b"\0")
    return b"xax!" + header[4:] if "magic" in breaks else header


def openssl(*args, **kwargs):
    return subprocess.run(
        ("openssl",) + args, stdout=subprocess.PIPE, stderr=subprocess.PIPE, check=True, **kwargs
    ).stdout


def key_bytes(key):
    modulus = openssl("rsa", "-in", key, "-noout", "-modulus").decode().strip().split("=", 1)[1]
    return len(modulus) // 2


def pem_base64(path):
    text = open(path).read()
    return "".join(text.split("-----")[2].split())


def sign(signing, heap, checksum, alg, rsa_at, cms_at):
    with tempfile.TemporaryDirectory() as work:
        data = os.path.join(work, "checksum")
        with open(data, "wb") as f:
            f.write(checksum)
        rsa = openssl("pkeyutl", "-sign", "-inkey", signing["key"], "-pkeyopt", "digest:" + alg, "-in", data)
        heap[rsa_at : rsa_at + len(rsa)] = rsa
        if signing.get("cms", True):
            chain = os.path.join(work, "chain.pem")
            with open(chain, "w") as f:
                f.write("".join(open(c).read() for c in signing["certs"][1:]))
            second = signing.get("cms_second")
            # SHA-1, as 2.3.0.0's CMS signature is, so a host that refuses SHA-1 refuses this one.
            cms = openssl(
                "cms",
                "-sign",
                "-binary",
                "-md",
                "sha1",
                "-nosmimecap",
                "-outform",
                "DER",
                *(["-nodetach"] if signing.get("cms_nodetach") else []),
                "-signer",
                signing.get("cms_signer", signing["certs"][0]),
                "-inkey",
                signing.get("cms_key", signing["key"]),
                *(["-signer", second[1], "-inkey", second[0]] if second else []),
                "-certfile",
                chain,
                "-in",
                data
            )
            if len(cms) > CMS_ROOM:
                raise SystemExit("make_xar.py: the CMS signature is larger than its room")
            heap[cms_at : cms_at + len(cms)] = cms


def main(argv):
    if len(argv) != 3:
        sys.stderr.write("usage: make_xar.py <out.pkg> '<json spec>'\n")
        return 2
    spec = json.loads(argv[2])
    data = rework(build(spec), spec)
    with open(argv[1], "wb") as f:
        f.write(data)
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv))
