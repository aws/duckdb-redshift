"""Writes small MSIs for the tool tests, including ones no MSI tool would write: compound files
whose chains, directory or sizes are wrong, signatures over other bytes, and databases whose
product, files, sequences or cabinet are not the release's.

    python3 -I make_msi.py <out.msi> '<json spec>'

The spec is an object; every key is optional:

  version  3 (default, 512-byte sectors) or 4 (4096-byte sectors)
  streams  {name: text}: more root streams, beside the fixture's own (a summary stream, a few
           small and large streams, a storage holding one stream, and in the root and the storage
           two streams named above U+3800, as MSI tables are); "@<file>" takes a file's bytes
  sign     {"leaf": <leaf.pem>, "key": <leaf.key>, "chain": [<cert.pem>...], "md": <digest>}: an
           Authenticode signature in \\x05DigitalSignature, made with openssl cms over the MSI
           digest (md, sha256 by default), whose certificates are the leaf and the chain;
           "nocerts": true leaves every certificate out, "sip": <hex> names another subject
           interface package, "econtent": <oid> another content type, "keyid": true names the
           signer by key identifier, "signer_md": <digest> is the signer's own digest (sha256
           by default), "digest_length": <n> keeps the MSI digest's first n bytes, and
           "second": [<cert.pem>, <key.pem>] adds a signer
  ex       true: also \\x05MsiDigitalSignatureEx, the hash of every entry's metadata
  files    [[File key, long name, Directory key, text or "@<file>"], ...]: an MSI database in
           place of the fixture's streams: the string pool and the File, Component, Directory,
           Media, Property, CustomAction and administrative sequence tables, and one embedded
           cabinet holding the files in uncompressed LZX blocks
  directories  [[key, parent, DefaultDir], ...] (default: TARGETDIR, PFiles64 and INSTALLDIR,
           "AMAZON~1|Amazon Redshift ODBC Driver (x64)")
  product, product_version  the Property table's (default: Amazon Redshift ODBC Driver (x64),
           2.3.0.0)
  custom_action  a sequence table: a custom action runs in it
  cabinet, last_sequence, media_twice  the Media table's cabinet (default #Data1.cab), its last
           sequence (default: every file), and a second row naming the same cabinet
  codepage, column_gap, drop_tables  the string pool's code page (default 1252; 65001 writes the
           strings as UTF-8, any other as cp1252), a table whose columns are numbered from 2, and
           tables left out of _Tables and _Columns
  no_columns  tables left out of _Columns only
  pool_extra, data_extra, stream_extra  hex bytes appended to the string pool, its data, and
           {table: hex} to a table's stream
  compression    the cabinet folder's compression type (default LZX with a 2^21 window)
  difat    true: also a large stream "Pad", so the FAT needs more sectors than the header's 109
           entries list and the rest are listed by a DIFAT sector
  nest     n: the fixture's storage holds its stream n storages deep, each storage inside the last
  break    a list of changes that make the file wrong; see BREAKS

The digest is computed here, from the streams as written, and not by lib/cfb.py, so a test
compares two separate readings of the rule. Standard library only, and openssl to sign.
"""

import hashlib
import json
import os
import struct
import subprocess
import sys
import tempfile

FREE, END, FATSECT, DIFSECT = 0xFFFFFFFF, 0xFFFFFFFE, 0xFFFFFFFD, 0xFFFFFFFC
# A version 3 FAT sector chains 128 sectors, so 110 FAT sectors need more than 109 * 128 sectors.
DIFAT_PAD = 110 * 128 * 512
NONE = 0xFFFFFFFF
STORAGE, STREAM, ROOT = 1, 2, 5
TIMES = struct.pack("<QQ", 0x01D2000000000001, 0x01D2000000000002)
BREAKS = {
    "loop": "a large stream's last sector points back at its first",
    "shared": "two large streams start on the same sector",
    "fat-in-chain": "a large stream's chain runs into a FAT sector",
    "orphan": "a sector in no chain",
    "unreachable": "a directory entry no tree reaches",
    "twice": "a directory entry two trees reach",
    "duplicate": "two root streams whose names differ only in case",
    "misorder": "the root's siblings in the wrong order",
    "high-size": "nonzero high size bits on a stream of a version 3 file",
    "size": "a large stream's size longer than its chain",
    "cutoff": "a large stream's size below the mini-stream cutoff",
    "trailing": "bytes after the last sector",
    "prefix": "a root stream whose name starts like the signature's",
    "stream-byte": "after signing, one byte of a signed stream changed",
    "clsid": "after signing, the storage's CLSID changed",
    "ex-byte": "after signing, one byte of MsiDigitalSignatureEx changed",
    "content": "after signing, one byte of the signed SpcIndirectDataContent changed",
    "cab-size": "a cabinet file whose size is over the reader's bound",
    "file-size": "a File table size that is not the cabinet's",
    "loose": "a file marked as stored outside the cabinets",
    "cab-extra": "a cabinet file the File table does not name",
    "content-octet": "the SpcIndirectDataContent inside an OCTET STRING, as CMS would put it",
    "signature-algorithm": "the signer's signature algorithm named ecdsa-with-SHA224",
}
COLUMNS = {  # table: [(column, type)], each type as _Columns stores it less 0x8000
    "Property": [("Property", 0x2D48), ("Value", 0x0F00)],
    "File": [
        ("File", 0x2D48),
        ("Component_", 0x0D48),
        ("FileName", 0x0DFF),
        ("FileSize", 0x0104),
        ("Version", 0x1D48),
        ("Language", 0x1D14),
        ("Attributes", 0x1102),
        ("Sequence", 0x0104),
    ],
    "Component": [
        ("Component", 0x2D48),
        ("ComponentId", 0x1D26),
        ("Directory_", 0x0D48),
        ("Attributes", 0x0102),
        ("Condition", 0x1DFF),
        ("KeyPath", 0x1D48),
    ],
    "Directory": [("Directory", 0x2D48), ("Directory_Parent", 0x1D48), ("DefaultDir", 0x0DFF)],
    "Media": [
        ("DiskId", 0x2102),
        ("LastSequence", 0x0104),
        ("DiskPrompt", 0x1D40),
        ("Cabinet", 0x1DFF),
        ("VolumeLabel", 0x1D20),
        ("Source", 0x1D48),
    ],
    "CustomAction": [("Action", 0x2D48), ("Type", 0x0102), ("Source", 0x1D48), ("Target", 0x1DFF)],
    "AdminExecuteSequence": [("Action", 0x2D48), ("Condition", 0x1DFF), ("Sequence", 0x1102)],
    "AdminUISequence": [("Action", 0x2D48), ("Condition", 0x1DFF), ("Sequence", 0x1102)],
}
MIME = "0123456789ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz._"


class Node(object):
    def __init__(self, name, kind, data=b"", children=None, clsid=b"\0" * 16):
        self.name, self.kind, self.data, self.clsid = name, kind, data, clsid
        self.children = children or []
        self.raw = name.encode("utf-16-le")
        self.left = self.right = self.child = NONE
        self.size_high = 0


def key(node):
    return (len(node.raw), tuple(ord(c.upper()) if len(c.upper()) == 1 else ord(c) for c in node.name))


def stream_name(name, table):
    """The compound-file name of an MSI stream, as Windows Installer packs it."""
    out, i = [chr(0x4840)] if table else [], 0
    while i < len(name):
        if name[i] in MIME and i + 1 < len(name) and name[i + 1] in MIME:
            out.append(chr(0x3800 + MIME.index(name[i]) + (MIME.index(name[i + 1]) << 6)))
            i += 2
        else:
            out.append(chr(0x4800 + MIME.index(name[i])) if name[i] in MIME else name[i])
            i += 1
    return "".join(out)


def cabinet(files, spec):
    """A one-folder cabinet: one LZX stream of a single uncompressed block, cut into 32 KiB
    frames, one data block each."""
    breaks = spec.get("break", [])
    body = b"".join(d for _, d in files)
    # 1 bit (no E8 translation), block type 3 and a 24-bit length, padded to 32 bits; then R0 to R2.
    head = 3 << 28 | len(body) << 4
    stream = struct.pack("<HH", head >> 16, head & 0xFFFF) + struct.pack("<III", 1, 1, 1)
    stream += body + (b"\0" if len(body) & 1 else b"")
    frames = [body[i : i + 32768] for i in range(0, len(body), 32768)] or [b""]
    cuts = [0] + [16 + 32768 * (i + 1) for i in range(len(frames) - 1)] + [len(stream)]
    entries = list(files) + ([("Extra", b"")] if "cab-extra" in breaks else [])
    table, offset = b"", 0
    for name, data in entries:
        size = (64 << 20) + 1 if "cab-size" in breaks and name == entries[0][0] else len(data)
        table += struct.pack("<IIHHHH", size, offset, 0, 0x5000, 0, 0x20) + name.encode() + b"\0"
        offset += len(data)
    blocks = b"".join(
        struct.pack("<IHH", 0, cuts[i + 1] - cuts[i], len(f)) + stream[cuts[i] : cuts[i + 1]]
        for i, f in enumerate(frames)
    )
    start = 44 + len(table)
    out = struct.pack("<4sIIIIIBBHHHHH", b"MSCF", 0, start + len(blocks), 0, 44, 0, 3, 1, 1, len(entries), 0, 0, 0)
    return out + struct.pack("<IHH", start, len(frames), spec.get("compression", 3 | 21 << 8)) + table + blocks


def database(spec):
    """The streams of an MSI database holding spec's files."""
    breaks = spec.get("break", [])
    files = [(k, n, d, open(t[1:], "rb").read() if t.startswith("@") else t.encode()) for k, n, d, t in spec["files"]]
    directories = spec.get(
        "directories",
        [
            ["TARGETDIR", None, "SourceDir"],
            ["PFiles64", "TARGETDIR", "PFiles64"],
            ["INSTALLDIR", "PFiles64", "AMAZON~1|Amazon Redshift ODBC Driver (x64)"],
        ],
    )
    rows = {
        "Property": [
            ("ProductName", spec.get("product", "Amazon Redshift ODBC Driver (x64)")),
            ("ProductVersion", spec.get("product_version", "2.3.0.0")),
            ("Manufacturer", "Amazon"),
        ],
        "File": [
            (
                k,
                "c_" + k,
                "x|" + n,
                len(d) + (1 if "file-size" in breaks and i == 0 else 0),
                None,
                None,
                0x2000 if "loose" in breaks and i == 0 else 0x4000,
                i + 1,
            )
            for i, (k, n, _, d) in enumerate(files)
        ],
        "Component": [("c_" + k, None, d, 0, None, k) for k, _, d, _ in files],
        "Directory": [tuple(r) for r in directories],
        "Media": [(1, spec.get("last_sequence", len(files)), None, spec.get("cabinet", "#Data1.cab"), None, None)]
        * (2 if spec.get("media_twice") else 1),
        "CustomAction": [],
        "AdminExecuteSequence": [("CostInitialize", None, 800), ("InstallAdminPackage", None, 3900)],
        "AdminUISequence": [("CostInitialize", None, 800)],
    }
    if spec.get("custom_action"):
        rows["CustomAction"].append(("SetTarget", 51, "TARGETDIR", "C:\\x"))
        rows[spec["custom_action"]].append(("SetTarget", None, 900))
    strings = []

    def ref(value):
        if value is None:
            return 0
        if value not in strings:
            strings.append(value)
        return strings.index(value) + 1

    # Each table column by column: a string as its pool reference, an integer biased by half its
    # range, 0 for null.
    streams = {}
    for table, columns in COLUMNS.items():
        data = b""
        for i, (_, kind) in enumerate(columns):
            for row in rows[table]:
                v = row[i]
                if kind & 0x0800:
                    data += struct.pack("<H", ref(v))
                elif kind & 0xFF <= 2:
                    data += struct.pack("<H", 0 if v is None else v + 0x8000)
                else:
                    data += struct.pack("<I", 0 if v is None else v + 0x80000000)
        streams[stream_name(table, True)] = data
    names = [t for t in COLUMNS if t not in spec.get("drop_tables", [])]
    streams[stream_name("_Tables", True)] = b"".join(struct.pack("<H", ref(t)) for t in names)
    cols = [
        (t, i + 1 + (t == spec.get("column_gap")), c, k)
        for t in names
        if t not in spec.get("no_columns", [])
        for i, (c, k) in enumerate(COLUMNS[t])
    ]
    streams[stream_name("_Columns", True)] = (
        b"".join(struct.pack("<H", ref(t)) for t, _, _, _ in cols)
        + b"".join(struct.pack("<H", n + 0x8000) for _, n, _, _ in cols)
        + b"".join(struct.pack("<H", ref(c)) for _, _, c, _ in cols)
        + b"".join(struct.pack("<H", k + 0x8000) for _, _, _, k in cols)
    )
    encoded = [s.encode("utf-8" if spec.get("codepage") == 65001 else "cp1252") for s in strings]
    streams[stream_name("_StringPool", True)] = (
        struct.pack("<HH", spec.get("codepage", 1252), 0)
        + b"".join(struct.pack("<HH", len(e), 1) for e in encoded)
        + bytes.fromhex(spec.get("pool_extra", ""))
    )
    streams[stream_name("_StringData", True)] = b"".join(encoded) + bytes.fromhex(spec.get("data_extra", ""))
    for table, extra in spec.get("stream_extra", {}).items():
        streams[stream_name(table, True)] += bytes.fromhex(extra)
    streams[stream_name("Data1.cab", False)] = cabinet([(k, d) for k, _, _, d in files], spec)
    return streams


def fixture(spec):
    if "files" in spec:
        nodes = [Node("\x05SummaryInformation", STREAM, b"summary\n" * 20)]
        nodes += [Node(n, STREAM, d) for n, d in sorted(database(spec).items())]
        return Node("Root Entry", ROOT, children=nodes, clsid=bytes.fromhex("84100c0000000000c000000000000046"))

    sub = Node("Storage", STORAGE, children=[Node("Inner", STREAM, b"inner stream\n" * 9)], clsid=bytes(range(16)))
    for _ in range(spec.get("nest", 0)):
        sub.children = [Node("S", STORAGE, children=sub.children)]

    # Names in the range MSI table streams use, where raw UTF-16LE order is not code-point order.
    def packed():
        return [Node("\u3841a", STREAM, b"packed a"), Node("\u3910b", STREAM, b"packed b")]

    sub.children += packed()
    nodes = [
        Node("\x05SummaryInformation", STREAM, b"summary\n" * 20),
        Node("Big", STREAM, bytes(range(256)) * 20),
        Node("Big2", STREAM, b"b" * 4096),
        Node("Edge", STREAM, b"e" * 4095),
        Node("Small", STREAM, b"s" * 70),
        Node("Empty", STREAM),
        sub,
    ] + packed()
    nodes += [
        Node(n, STREAM, open(t[1:], "rb").read() if t.startswith("@") else t.encode())
        for n, t in sorted(spec.get("streams", {}).items())
    ]
    return Node("Root Entry", ROOT, children=nodes, clsid=bytes.fromhex("84100c0000000000c000000000000046"))


# The MSI digest, as Windows computes it: streams and storages in the order of their raw names.
def ordered(node, top):
    return [
        c
        for c in sorted(node.children, key=lambda c: c.raw)
        if not (top and c.name in ("\x05DigitalSignature", "\x05MsiDigitalSignatureEx"))
    ]


def content_hash(node, h, top):
    for c in ordered(node, top):
        if c.kind == STREAM:
            h.update(c.data)
        else:
            content_hash(c, h, False)
    h.update(node.clsid)


def metadata_hash(node, h, top):
    if node.kind != ROOT:
        h.update(node.raw)
    h.update(node.clsid + b"\0" * 4)
    if node.kind != ROOT:
        h.update(TIMES)
    for c in ordered(node, top):
        if c.kind == STREAM:
            h.update(c.raw + struct.pack("<I", len(c.data)) + b"\0" * 4 + TIMES)
        else:
            metadata_hash(c, h, False)


def der(tag, body):
    n = len(body)
    size = (n.bit_length() + 7) // 8
    return bytes([tag]) + (bytes([n]) if n < 0x80 else bytes([0x80 | size]) + n.to_bytes(size, "big")) + body


def oid(text):
    arcs = [int(a) for a in text.split(".")]
    out = bytes([arcs[0] * 40 + arcs[1]])
    for a in arcs[2:]:
        chunk = [a & 0x7F]
        a >>= 7
        while a:
            chunk.insert(0, 0x80 | (a & 0x7F))
            a >>= 7
        out += bytes(chunk)
    return der(0x06, out)


DIGEST_OIDS = {
    "sha1": "1.3.14.3.2.26",
    "sha256": "2.16.840.1.101.3.4.2.1",
    "sha384": "2.16.840.1.101.3.4.2.2",
    "sha512": "2.16.840.1.101.3.4.2.3",
}


def split(data, start, end):
    """(tag, element start, value start, element end) of each DER element in data[start:end]."""
    out = []
    while start < end:
        tag, length, pos = data[start], data[start + 1], start + 2
        if length & 0x80:
            count = length & 0x7F
            length, pos = int.from_bytes(data[pos : pos + count], "big"), pos + count
        out.append((tag, start, pos, pos + length))
        start = pos + length
    return out


def sign(root, spec, tmp):
    """The \\x05DigitalSignature stream: a detached CMS signature by openssl, with the
    SpcIndirectDataContent put inside it, as Authenticode carries it."""
    s = spec["sign"]
    md = s.get("md", "sha256")
    h = hashlib.new(md)
    if spec.get("ex"):
        pre = hashlib.new(md)
        metadata_hash(root, pre, True)
        root.children.append(Node("\x05MsiDigitalSignatureEx", STREAM, pre.digest()))
        h.update(pre.digest())
    content_hash(root, h, True)
    sip = der(
        0x30,
        der(0x02, b"\x01")
        + der(0x04, bytes.fromhex(s.get("sip", "f1100c0000000000c000000000000046")))
        + b"".join(der(0x02, b"\x00") for _ in range(5)),
    )
    indirect = der(
        0x30,
        der(0x30, oid("1.3.6.1.4.1.311.2.1.30") + sip)
        + der(0x30, der(0x30, oid(DIGEST_OIDS[md]) + b"\x05\x00") + der(0x04, h.digest()[: s.get("digest_length")])),
    )
    value = indirect[split(indirect, 0, len(indirect))[0][2] :]  # without its tag and length
    with open(os.path.join(tmp, "content"), "wb") as f:
        f.write(value)
    command = [
        "openssl",
        "cms",
        "-sign",
        "-binary",
        "-md",
        s.get("signer_md", "sha256"),
        "-in",
        os.path.join(tmp, "content"),
        "-signer",
        s["leaf"],
        "-inkey",
        s["key"],
        "-econtent_type",
        s.get("econtent", "1.3.6.1.4.1.311.2.1.4"),
        "-outform",
        "DER",
        "-out",
        os.path.join(tmp, "cms.der"),
    ]
    if s.get("keyid"):
        command.append("-keyid")
    if s.get("second"):
        command += ["-signer", s["second"][0], "-inkey", s["second"][1]]
    if s.get("nocerts"):
        command.append("-nocerts")
    elif s.get("chain"):
        with open(os.path.join(tmp, "chain.pem"), "wb") as f:
            for c in s["chain"]:
                f.write(open(c, "rb").read())
        command += ["-certfile", os.path.join(tmp, "chain.pem")]
    subprocess.check_call(command)
    cms = open(os.path.join(tmp, "cms.der"), "rb").read()
    if "signature-algorithm" in spec.get("break", []):
        # The signer's ecdsa-with-SHA256 is the last one: the SignerInfo follows the certificates.
        ecdsa_sha256, ecdsa_sha224 = oid("1.2.840.10045.4.3.2"), oid("1.2.840.10045.4.3.1")
        at = cms.rindex(ecdsa_sha256)
        cms = cms[:at] + ecdsa_sha224 + cms[at + len(ecdsa_sha256) :]
    # ContentInfo { oid, [0] { SignedData { version, digests, encap { oid }, ... } } }: put the
    # SpcIndirectDataContent inside encap, as [0] { SEQUENCE }.
    top = split(cms, 0, len(cms))[0]
    content_type, explicit = split(cms, top[2], top[3])
    signed = split(cms, explicit[2], explicit[3])[0]
    fields = split(cms, signed[2], signed[3])
    encap = fields[2]
    if "content" in spec.get("break", []):
        indirect = indirect[:-1] + bytes([indirect[-1] ^ 1])
    inner = der(0x04, value) if "content-octet" in spec.get("break", []) else indirect
    new_encap = der(0x30, cms[encap[2] : encap[3]] + der(0xA0, inner))
    body = b"".join(cms[f[1] : f[3]] for f in fields[:2]) + new_encap + b"".join(cms[f[1] : f[3]] for f in fields[3:])
    return der(0x30, cms[content_type[1] : content_type[3]] + der(0xA0, der(0x30, body)))


def tree(node):
    """Gives each storage a balanced sibling tree of its children, in the compound file's order."""

    def build(items):
        if not items:
            return NONE
        mid = len(items) // 2
        items[mid].left, items[mid].right = build(items[:mid]), build(items[mid + 1 :])
        return items[mid].index

    for c in node.children:
        if c.kind == STORAGE:
            tree(c)
    node.child = build(sorted(node.children, key=key))


def write(root, spec):
    breaks = spec.get("break", [])
    v4 = spec.get("version", 3) == 4
    ssz = 4096 if v4 else 512
    entries = []

    def index(n):
        n.index = len(entries)
        entries.append(n)
        for c in n.children:
            index(c)

    index(root)
    if "unreachable" in breaks:
        entries.append(Node("Lost", STREAM))
        entries[-1].index = len(entries) - 1
    tree(root)
    if "misorder" in breaks:
        top = entries[root.child]
        top.left, top.right = top.right, top.left
    if "twice" in breaks:
        storage = [e for e in entries if e.kind == STORAGE][0]
        storage.child = entries[root.child].index

    sectors = []  # (data, next)

    def place(data):
        first = len(sectors)
        count = (len(data) + ssz - 1) // ssz
        for i in range(count):
            sectors.append([data[i * ssz : (i + 1) * ssz].ljust(ssz, b"\0"), first + i + 1 if i + 1 < count else END])
        return first if count else END

    mini, minifat = b"", []
    for e in entries:
        if e.kind == STREAM and 0 < len(e.data) < 4096:
            e.start = len(mini) // 64
            count = (len(e.data) + 63) // 64
            minifat += [e.start + i + 1 for i in range(count - 1)] + [END]
            mini += e.data.ljust(count * 64, b"\0")
        elif e.kind == STREAM and e.data:
            e.start = place(e.data)
        else:
            e.start = END if e.kind != STORAGE else 0
    root.start, root.data = place(mini), mini
    minifat += [FREE] * (-len(minifat) % (ssz // 4))
    minifat_start = place(struct.pack("<%dI" % len(minifat), *minifat)) if minifat else END
    minifat_count = (len(minifat) * 4 + ssz - 1) // ssz
    if "orphan" in breaks:
        place(b"orphan")
    big = [e for e in entries if e.kind == STREAM and len(e.data) >= 4096]
    if "loop" in breaks:
        sectors[big[0].start + (len(big[0].data) + ssz - 1) // ssz - 1][1] = big[0].start
    if "shared" in breaks:
        big[1].start = big[0].start
    sizes = {e.index: len(e.data) for e in entries}
    if "size" in breaks:
        sizes[big[0].index] += ssz
    if "cutoff" in breaks:
        sizes[big[0].index] = 4000
    if "high-size" in breaks:
        big[0].size_high = 1

    raw = b""
    for e in entries:
        if e.kind in (STREAM, STORAGE, ROOT):
            name = e.raw + b"\0\0"
            raw += name.ljust(64, b"\0") + struct.pack("<HBB", len(name), e.kind, 1)
            raw += struct.pack("<III", e.left, e.right, e.child) + e.clsid + b"\0" * 4
            raw += b"\0" * 16 if e.kind == ROOT else TIMES
            raw += struct.pack("<III", e.start, sizes[e.index] if e.kind != STORAGE else 0, e.size_high)
    raw += (b"\0" * 64 + struct.pack("<HBB", 0, 0, 0) + struct.pack("<III", NONE, NONE, NONE) + b"\0" * 48) * (
        (-len(raw) // 128) % (ssz // 128)
    )
    dir_start = place(raw)
    dir_count = len(raw) // ssz

    # The FAT, after everything else, sized for itself and the DIFAT sectors that list what the
    # header's 109 entries cannot.
    per = ssz // 4

    def difat_for(fats):
        return max(0, (fats - 109 + per - 2) // (per - 1))

    fat_count = 1
    while (len(sectors) + fat_count + difat_for(fat_count) + per - 1) // per > fat_count:
        fat_count += 1
    difat_count = difat_for(fat_count)
    fat_start = len(sectors)
    difat_start = fat_start + fat_count
    fat = [n for _, n in sectors] + [FATSECT] * fat_count + [DIFSECT] * difat_count
    if "fat-in-chain" in breaks:
        fat[big[0].start + (len(big[0].data) + ssz - 1) // ssz - 1] = fat_start
    fat += [FREE] * (fat_count * per - len(fat))
    fat_bytes = struct.pack("<%dI" % len(fat), *fat)
    for i in range(fat_count):
        sectors.append([fat_bytes[i * ssz : (i + 1) * ssz], FATSECT])
    listed = [fat_start + i for i in range(109, fat_count)]
    for i in range(difat_count):
        words = listed[i * (per - 1) : (i + 1) * (per - 1)]
        words += [FREE] * (per - 1 - len(words)) + [difat_start + i + 1 if i + 1 < difat_count else END]
        sectors.append([struct.pack("<%dI" % per, *words), DIFSECT])

    header = (
        bytes.fromhex("d0cf11e0a1b11ae1")
        + b"\0" * 16
        + struct.pack("<HHHHH", 0x3E, 4 if v4 else 3, 0xFFFE, 12 if v4 else 9, 6)
        + b"\0" * 6
    )
    header += struct.pack(
        "<IIIIIIIII",
        dir_count if v4 else 0,
        fat_count,
        dir_start,
        0,
        4096,
        minifat_start,
        minifat_count,
        difat_start if difat_count else END,
        difat_count,
    )
    header += struct.pack("<109I", *([fat_start + i for i in range(min(fat_count, 109))] + [FREE] * (109 - fat_count)))
    out = header.ljust(ssz, b"\0") + b"".join(d for d, _ in sectors)
    if "trailing" in breaks:
        out += b"tail"
    return out


def main(argv):
    if len(argv) != 3:
        sys.stderr.write("usage: make_msi.py <out.msi> '<json spec>'\n")
        return 2
    spec = json.loads(argv[2])
    unknown = [b for b in spec.get("break", []) if b not in BREAKS]
    if unknown:
        sys.stderr.write("make_msi.py: unknown break %s\n" % ", ".join(unknown))
        return 2
    # index() and tree() recurse once per nested storage, and nest goes past the default limit.
    sys.setrecursionlimit(1000 + 2 * spec.get("nest", 0))
    root = fixture(spec)
    breaks = spec.get("break", [])
    if "duplicate" in breaks:
        root.children.append(Node("SMALL", STREAM, b"x"))
    if "prefix" in breaks:
        root.children.append(Node("\x05DigitalSignatureX", STREAM, b"x"))
    if spec.get("difat"):
        root.children.append(Node("Pad", STREAM, b"p" * DIFAT_PAD))
    if "sign" in spec:
        with tempfile.TemporaryDirectory() as tmp:
            root.children.append(Node("\x05DigitalSignature", STREAM, sign(root, spec, tmp)))
    for b, change in (
        ("stream-byte", lambda: setattr(root.children[1], "data", b"\x01" + root.children[1].data[1:])),
        ("clsid", lambda: setattr(root.children[6], "clsid", b"\x01" * 16)),
        (
            "ex-byte",
            lambda: [
                setattr(c, "data", bytes([c.data[0] ^ 1]) + c.data[1:])
                for c in root.children
                if c.name == "\x05MsiDigitalSignatureEx"
            ],
        ),
    ):
        if b in breaks:
            change()
    with open(argv[1], "wb") as f:
        f.write(write(root, spec))
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv))
