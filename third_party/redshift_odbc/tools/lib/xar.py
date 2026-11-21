"""Takes one regular file out of a component payload of a macOS flat package.

    python3 -I xar.py extract <package> <path-in-package> <out-file> <algorithm>
    python3 -I xar.py signed <package> <out-dir> <algorithm>
    python3 -I xar.py payload <package> <path-in-package> <algorithm>
    python3 -I xar.py product <package> <path-in-package> <identifier> <version> <algorithm>
    python3 -I xar.py find <package> <algorithm> <name>...
    python3 -I xar.py path <path-in-package>

A flat package is a xar archive: a header, a zlib-compressed XML table of contents, and a heap that
holds each archived file. <path-in-package> names the file where `pkgutil --expand-full` puts it:
the path of a component's Payload in the table of contents, then the file's path inside that
payload, for example ./AmazonRedshiftODBC.pkg/Payload/librsodbc64.dylib (the first segment named
Payload ends the component's part). It must pass the lock checker's path grammar.

This reader accepts only the forms the locked release uses and refuses every other as drift, which
a reviewed change adds: a header whose checksum algorithm is sha1 (number 1), checksums in
<algorithm> (signer.txt's, which must be sha1 too), and a payload that is a gzip-compressed cpio
archive in the odc format Apple's tools write. Every file the table of contents describes may
decode to at most 64 MiB, and all of them to at most 128 MiB together (2.3.0.0's largest is its
12 MB Payload), checked before any is decompressed, so a package cannot make the reader hold more
than that. A payload is read from the heap only after checking that its location lies inside the
file and that its bytes match the archived and extracted checksums the table of contents records.
Its odc archive is rewritten header by header into newc as it streams, and every member goes
through lib/cpio.py, the member-checked reader fetch-driver.sh uses for Linux. So the same rules
apply: <out-file>, which must not exist yet, appears only when exactly one member has the path and
it is a regular file with one link, no member is absolute or has a .. segment, and the whole archive
is well formed; nothing else reaches the file system. Nothing from the package is executed.

signed, payload and product are the package rules of macOS verification, with the accepted checksum
algorithm as an argument. signed checks, in order, each failing with its rule's name:
  algorithm          <algorithm> is sha1, the one algorithm this reader computes;
  package            the header and the table of contents are well formed (as extract requires),
                     the header naming sha1;
  algorithm          the table of contents names sha1 too;
  checksum-location  the stored checksum's offset and size, from the table of contents, lie inside
                     the file and the size is the algorithm's;
  checksum           the stored checksum equals the checksum of the table of contents' bytes, which
                     is what the signature covers;
  signature          the package has exactly one RSA signature and one CMS signature, as 2.3.0.0
                     has, each inside the file and each with its certificates, the first of
                     which, the signer's, is the same for both;
and then writes, into <out-dir>, which must be empty: checksum (the stored checksum), rsa.sig,
signer.der and cms.ber, for openssl to verify (lib/macos_signer.sh). payload checks the component
Payload of <path-in-package> against its archived and extracted checksums, which must use
<algorithm>; extract requires the same of the Payload it reads. product checks the PackageInfo
beside that Payload, in the same component, the metadata Apple's installer reads: its checksums use
<algorithm>, it declares no document type, and its <pkg-info> names exactly <identifier> and <version> (product), so a validly signed package of
another product or version fails, as the signed RPM header's name and version do on Linux. find
prints "<name> <path-in-package>" for every member of a component's payload whose last segment is
one of <name>..., found by lib/cpio.py, which checks the whole archive and refuses such a path with
a space or a byte that is not printable ASCII; every Payload it reads must use <algorithm>, and an
entry named Payload that is not a file is no payload and is skipped. A file outside every
payload is never reported, since extract cannot take it. path checks that <path-in-package> is one
extract accepts; verification checks its file name with the lock checker (lock.py --name).

On failure it prints one line to stderr, naming what is wrong, and exits 1. Standard library only,
Python 3.6 and later.
"""

import base64
import binascii
import hashlib
import importlib.util
import os
import re
import struct
import sys
import xml.etree.ElementTree as ElementTree
from xml.parsers import expat
import zlib

HERE = os.path.dirname(os.path.abspath(__file__))
MAGIC = b"xar!"
HEADER_SIZE = 28
MAX_PACKAGE = 256 << 20   # the bound fetch-driver.sh puts on every package
MAX_TOC = 16 << 20        # the 2.3.0.0 table of contents is 15 KB
MAX_DEPTH = 32
CHUNK = 1 << 20
MAX_ENTRY = 64 << 20      # what one file of the table of contents may decode to
MAX_DECODED = 128 << 20   # and all of them together
# The checksum algorithm this reader computes, which the header names by number: 2.3.0.0's.
HEADER_SHA1 = 1
DIGESTS = {"sha1": hashlib.sha1}
OCTET, ZLIB = "application/octet-stream", "application/x-gzip"  # xar's x-gzip is a zlib stream
NUMBER = re.compile(r"(0|[1-9][0-9]{0,17})\Z")
HEX = re.compile(r"[0-9a-f]+\Z")
ODC_FIELDS = re.compile(rb"[0-7]{70}\Z")
DSIG = "{http://www.w3.org/2000/09/xmldsig#}"


class Failure(Exception):
    pass


def load(name):
    """A module from this directory, by its file, so nothing else on sys.path is searched. No
    bytecode is written next to it, so the tools never change the repository."""
    sys.dont_write_bytecode = True
    spec = importlib.util.spec_from_file_location("redshift_odbc_" + name, os.path.join(HERE, name + ".py"))
    module = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(module)
    return module


def number(element, tag, where):
    child = only(element, tag, where)
    text = (child.text or "").strip()
    if not NUMBER.match(text):
        raise Failure("%s: <%s> is not a number" % (where, tag))
    return int(text)


def only(element, tag, where):
    found = element.findall(tag)
    if len(found) != 1:
        raise Failure("%s has %d <%s> elements, not one" % (where, len(found), tag))
    return found[0]


class Prolog(Exception):
    """Raised at the root element, when the prolog, where a document type would be, is read."""


def refuse_document_type(xml, what="the table of contents"):
    """No document type, so no entity can be declared or expanded; external ones never are. Expat
    reports the declaration in whatever encoding the document uses, which a search for the bytes
    "<!DOCTYPE" misses in UTF-16. A document type can only come before the root element, so the
    check stops there; ElementTree parses the whole document next."""
    def refuse(*_):
        raise Failure("%s has a document type declaration" % what)

    def root(*_):
        raise Prolog()

    parser = expat.ParserCreate()
    parser.StartDoctypeDeclHandler = refuse
    parser.StartElementHandler = root
    try:
        parser.Parse(xml, True)
    except Prolog:
        return
    except expat.ExpatError as e:
        raise Failure("%s is not well-formed XML: %s" % (what, e))


class Package(object):
    """A xar file read whole, with its header, its table of contents and its file entries."""

    def __init__(self, path):
        try:
            with open(path, "rb") as f:
                self.data = f.read(MAX_PACKAGE + 1)
        except OSError as e:
            raise Failure("cannot read the package %s: %s" % (path, e.strerror))
        if len(self.data) > MAX_PACKAGE:
            raise Failure("the package is larger than %d bytes" % MAX_PACKAGE)
        self.read_header()
        self.read_toc()
        self.files, self.decoded = {}, 0
        for element in self.toc.findall("file"):
            self.add_file(element, "", 1)

    def read_header(self):
        if len(self.data) < HEADER_SIZE or self.data[:4] != MAGIC:
            raise Failure("not a xar archive")
        size, version, self.toc_length, toc_size, algorithm = struct.unpack(">HHQQI", self.data[4:HEADER_SIZE])
        if version != 1:
            raise Failure("xar header version %d is not 1" % version)
        if algorithm != HEADER_SHA1 or size != HEADER_SIZE:
            raise Failure("the xar header has size %d and checksum algorithm %d; this reader accepts only "
                          "size %d and sha1 (1)" % (size, algorithm, HEADER_SIZE))
        self.algorithm = "sha1"
        if not 0 < toc_size <= MAX_TOC or not 0 < self.toc_length <= MAX_TOC:
            raise Failure("the table of contents' sizes are out of bounds")
        self.toc_start = size
        self.heap = size + self.toc_length
        if self.heap > len(self.data):
            raise Failure("the table of contents runs past the end of the file")
        self.toc_size = toc_size

    def read_toc(self):
        self.toc_bytes = self.data[self.toc_start:self.heap]
        d = zlib.decompressobj()
        try:
            xml = d.decompress(self.toc_bytes, self.toc_size + 1)
        except zlib.error as e:
            raise Failure("the table of contents is not a zlib stream: %s" % e)
        if len(xml) != self.toc_size or not d.eof or d.unused_data or d.unconsumed_tail:
            raise Failure("the table of contents does not have the size the header gives, or data "
                          "follows it")
        refuse_document_type(xml)
        try:
            root = ElementTree.fromstring(xml)
        except ElementTree.ParseError as e:
            raise Failure("the table of contents is not well-formed XML: %s" % e)
        if root.tag != "xar":
            raise Failure("the table of contents is not a <xar> document")
        self.toc = only(root, "toc", "<xar>")

    def add_file(self, element, parent, depth):
        if depth > MAX_DEPTH:
            raise Failure("the table of contents nests files deeper than %d" % MAX_DEPTH)
        # Some writers repeat <name>; every copy must agree, so no reader can see another name.
        names = [(n.text or "") for n in element.findall("name")]
        if not names or any(n != names[0] for n in names):
            raise Failure("a file under %r in the table of contents has no name, or names that "
                          "differ: %r" % (parent or "/", names))
        name = names[0]
        if name in ("", ".", "..") or "/" in name or "\0" in name:
            raise Failure("a file under %r in the table of contents has the name %r"
                          % (parent or "/", name))
        path = parent + "/" + name if parent else name
        if path in self.files:
            raise Failure("the table of contents has two files at %s" % path)
        # Every size is bounded here, before anything is decompressed, whether it is read or not.
        for data in element.findall("data"):
            size = number(data, "size", path)
            if size > MAX_ENTRY:
                raise Failure("the table of contents gives %s a decoded size of %d bytes, more than the %d "
                              "a file may have" % (path, size, MAX_ENTRY))
            self.decoded += size
            if self.decoded > MAX_DECODED:
                raise Failure("the table of contents' files decode to more than %d bytes together"
                              % MAX_DECODED)
        self.files[path] = element
        for child in element.findall("file"):
            self.add_file(child, path, depth + 1)

    def region(self, offset, length, what):
        """The heap bytes at offset, after checking that they lie inside the file."""
        if offset < 0 or length < 0 or self.heap + offset + length > len(self.data):
            raise Failure("%s at heap offset %d, size %d, lies outside the file" % (what, offset, length))
        return self.data[self.heap + offset:self.heap + offset + length]

    def file_bytes(self, path, algorithm):
        """A regular file's bytes, checked against both of its checksums, which must use algorithm,
        and decoded."""
        element = self.files.get(path)
        if element is None:
            raise Failure("the package has no %s" % path)
        kind = (only(element, "type", path).text or "").strip()
        if kind != "file":
            raise Failure("%s in the package is a %s, not a file" % (path, kind or "file of no type"))
        data = only(element, "data", path)
        stored = self.region(number(data, "offset", path), number(data, "length", path), path)
        checksum(only(data, "archived-checksum", path), stored, path + "'s archived checksum", algorithm)
        encoding = only(data, "encoding", path).get("style")
        if encoding == OCTET:
            content = stored
        elif encoding == ZLIB:
            d = zlib.decompressobj()
            try:
                content = d.decompress(stored, number(data, "size", path) + 1)
            except zlib.error as e:
                raise Failure("%s is not the zlib stream its encoding names: %s" % (path, e))
            if not d.eof or d.unused_data or d.unconsumed_tail:
                raise Failure("%s's zlib stream is truncated or followed by data" % path)
        else:
            raise Failure("%s has the encoding %r, which this reader does not accept" % (path, encoding))
        if len(content) != number(data, "size", path):
            raise Failure("%s is %d bytes, not the size the table of contents gives" % (path, len(content)))
        checksum(only(data, "extracted-checksum", path), content, path + "'s extracted checksum", algorithm)
        return content


def checksum(element, data, what, algorithm):
    style = element.get("style")
    if style not in DIGESTS or style != algorithm:
        raise Failure("%s uses the algorithm %r, which is not accepted" % (what, style))
    want = (element.text or "").strip()
    got = DIGESTS[style](data).hexdigest()
    if not HEX.match(want) or got != want:
        raise Failure("%s does not match: the table of contents records %s, the bytes hash to %s"
                      % (what, want, got))


def split_path(path):
    """(the component's Payload in the table of contents, the member's path in the payload)."""
    problem = load("lock").check_path(path)
    if problem:
        raise Failure("%s: %s" % (path, problem))
    segments = path[2:].split("/")
    if "Payload" not in segments[:-1]:
        raise Failure("%s does not name a file inside a component's Payload" % path)
    at = segments.index("Payload") + 1
    return "/".join(segments[:at]), "./" + "/".join(segments[at:])


def gunzip(blob):
    d = zlib.decompressobj(16 + zlib.MAX_WBITS)
    pending = blob
    while not d.eof:
        out = d.decompress(pending, CHUNK)
        pending = d.unconsumed_tail
        if not out and not pending:
            raise ValueError("the payload's gzip stream is truncated")
        if out:
            yield out
    if d.unused_data or d.unconsumed_tail:
        raise ValueError("data follows the payload's gzip stream")


class Reader(object):
    """read(n) over a stream of byte chunks, as lib/cpio.py reads its archive."""

    def __init__(self, chunks):
        self.chunks = iter(chunks)
        self.buffer = b""

    def read(self, n):
        parts, have = [], 0
        while have < n:
            if not self.buffer:
                chunk = next(self.chunks, None)
                if chunk is None:
                    break
                self.buffer = chunk
                continue
            take = self.buffer[:n - have]
            self.buffer = self.buffer[len(take):]
            parts.append(take)
            have += len(take)
        return b"".join(parts)

    def rest(self):
        if self.buffer:
            yield self.buffer
            self.buffer = b""
        for chunk in self.chunks:
            yield chunk


def exact(reader, n):
    data = reader.read(n)
    if len(data) != n:
        raise ValueError("the archive ends inside a member")
    return data


def odc_to_newc(reader):
    """Rewrites an odc archive into newc, member by member; lib/cpio.py checks every field it
    reads. After the trailer the rest of the stream passes unchanged, for cpio.py to check."""
    while True:
        header = exact(reader, 76)
        if header[:6] != b"070707" or not ODC_FIELDS.match(header[6:]):
            raise ValueError("not an odc cpio archive, or a header field is not octal")
        dev, ino, mode, uid, gid, nlink, rdev = (int(header[6 + 6 * i:12 + 6 * i], 8) for i in range(7))
        mtime, name_size, size = int(header[48:59], 8), int(header[59:65], 8), int(header[65:76], 8)
        if size > 0xFFFFFFFF or mtime > 0xFFFFFFFF or not 1 <= name_size <= 4096:
            raise ValueError("a member's size or name size is out of bounds")
        name = exact(reader, name_size)
        fields = (ino, mode, uid, gid, nlink, mtime, size, 0, dev, 0, rdev, name_size, 0)
        yield b"070701" + b"".join(b"%08X" % v for v in fields) + name + b"\0" * ((4 - (110 + name_size) % 4) % 4)
        left = size
        while left:
            chunk = exact(reader, min(left, CHUNK))
            left -= len(chunk)
            yield chunk
        yield b"\0" * ((4 - size % 4) % 4)
        if name == b"TRAILER!!!\0":
            for chunk in reader.rest():
                yield chunk
            return


def cpio_stream(blob):
    """The payload's decompressed odc cpio archive, rewritten into newc, as a Reader."""
    if blob[:2] != b"\x1f\x8b":
        raise Failure("the payload is not gzip compressed")
    reader = Reader(gunzip(blob))
    head = reader.read(6)
    if head != b"070707":
        raise Failure("the payload's cpio archive is not in the odc format")
    reader.buffer = head + reader.buffer
    return Reader(odc_to_newc(reader))


def extract(package, path, out_path, algorithm):
    component, member = split_path(path)
    payload = Package(package).file_bytes(component, algorithm)
    load("cpio").extract(cpio_stream(payload), member, out_path)


def ruled(name, function, *args):
    try:
        return function(*args)
    except Failure as e:
        raise Failure("%s: %s" % (name, e))


def certificates(element, where):
    texts = [c.text or "" for c in element.findall("{0}KeyInfo/{0}X509Data/{0}X509Certificate".format(DSIG))]
    if not texts:
        raise Failure("the %s has no certificates" % where)
    try:
        return [base64.b64decode(re.sub(r"\s", "", t), validate=True) for t in texts]
    except (ValueError, binascii.Error):
        raise Failure("a certificate of the %s is not valid base64" % where)


def signed(package, out_dir, algorithm):
    if algorithm not in DIGESTS:
        raise Failure("algorithm: signer.txt accepts %s, which this reader does not compute (%s)"
                      % (algorithm, ", ".join(sorted(DIGESTS))))
    pkg = ruled("package", Package, package)  # whose header must name sha1
    element = ruled("checksum-location", only, pkg.toc, "checksum", "the table of contents")
    if element.get("style") != pkg.algorithm:
        raise Failure("algorithm: the table of contents names the checksum algorithm %r, the header %s"
                      % (element.get("style"), pkg.algorithm))
    offset = ruled("checksum-location", number, element, "offset", "<checksum>")
    size = ruled("checksum-location", number, element, "size", "<checksum>")
    stored = ruled("checksum-location", pkg.region, offset, size, "the stored checksum")
    computed = DIGESTS[pkg.algorithm](pkg.toc_bytes).digest()
    if size != len(computed):
        raise Failure("checksum-location: the stored checksum is %d bytes; %s needs %d"
                      % (size, pkg.algorithm, len(computed)))
    if stored != computed:
        raise Failure("checksum: the stored checksum %s is not the table of contents' %s %s"
                      % (binascii.hexlify(stored).decode(), pkg.algorithm, binascii.hexlify(computed).decode()))
    rsa = pkg.toc.findall("signature")
    cms = pkg.toc.findall("x-signature")
    if not rsa:
        raise Failure("signature: the package is not signed")
    if len(rsa) != 1 or len(cms) != 1 or rsa[0].get("style") != "RSA" or cms[0].get("style") != "CMS":
        raise Failure("signature: the package's signatures are not one RSA and one CMS signature")
    parts = {"checksum": stored}
    signers = []
    for element, name, what in ((rsa[0], "rsa.sig", "RSA signature"), (cms[0], "cms.ber", "CMS signature")):
        offset = ruled("signature", number, element, "offset", what)
        size = ruled("signature", number, element, "size", what)
        parts[name] = ruled("signature", pkg.region, offset, size, "the " + what)
        signers.append(ruled("signature", certificates, element, what)[0])
    if signers[0] != signers[1]:
        raise Failure("signature: the RSA and CMS signatures name different signing certificates")
    parts["signer.der"] = signers[0]
    if os.listdir(out_dir):
        raise Failure("%s is not empty" % out_dir)
    for name, data in sorted(parts.items()):
        with open(os.path.join(out_dir, name), "xb") as f:
            f.write(data)


def payload(package, path, algorithm):
    component = ruled("payload", split_path, path)[0]
    ruled("payload", Package(package).file_bytes, component, algorithm)


def product(package, path, identifier, version, algorithm):
    """The PackageInfo of the component that holds path's Payload names identifier and version."""
    component = ruled("product", split_path, path)[0]
    info = component[:-len("Payload")] + "PackageInfo"
    xml = ruled("product", Package(package).file_bytes, info, algorithm)
    ruled("product", refuse_document_type, xml, info)
    try:
        root = ElementTree.fromstring(xml)
    except ElementTree.ParseError as e:
        raise Failure("product: %s is not well-formed XML: %s" % (info, e))
    got = (root.get("identifier"), root.get("version"))
    if root.tag != "pkg-info" or got != (identifier, version):
        raise Failure("product: %s names <%s> identifier %r, version %r; verification needs <pkg-info> "
                      "identifier %r, version %r" % (info, root.tag, got[0], got[1], identifier, version))


def find(package, algorithm, names):
    pkg = Package(package)
    for path in sorted(pkg.files):
        if path.split("/")[-1] == "Payload" and (only(pkg.files[path], "type", path).text or "").strip() == "file":
            for base, member in load("cpio").find(cpio_stream(pkg.file_bytes(path, algorithm)), names):
                print("%s ./%s/%s" % (base, path, member[2:] if member.startswith("./") else member))


# command: (number of arguments after it, at least that many when True, the function)
COMMANDS = {
    "extract": (4, False, lambda a: extract(*a)),
    "signed": (3, False, lambda a: signed(*a)),
    "payload": (3, False, lambda a: payload(*a)),
    "product": (5, False, lambda a: product(*a)),
    "find": (3, True, lambda a: find(a[0], a[1], set(a[2:]))),
    "path": (1, False, lambda a: split_path(*a)),
}


def main(argv):
    command = COMMANDS.get(argv[1] if len(argv) > 1 else "")
    args = argv[2:]
    if not command or len(args) < command[0] or (not command[1] and len(args) != command[0]):
        sys.stderr.write("usage: xar.py extract <package> <path-in-package> <out-file> <algorithm>\n"
                         "       xar.py signed <package> <out-dir> <algorithm>\n"
                         "       xar.py payload <package> <path-in-package> <algorithm>\n"
                         "       xar.py product <package> <path-in-package> <identifier> <version> <algorithm>\n"
                         "       xar.py find <package> <algorithm> <name>...\n"
                         "       xar.py path <path-in-package>\n")
        return 2
    try:
        command[2](args)
    except Failure as e:
        sys.stderr.write("xar.py: %s\n" % e)
        return 1
    except (ValueError, OSError, EOFError, zlib.error) as e:
        sys.stderr.write("xar.py: %s\n" % e)
        return 1
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv))
