"""Wraps a cpio archive in a minimal RPM package for the verification suites.

    python3 -I make_rpm.py <out> <name> <version> <release> <arch> < archive

The package has the RPM lead, an empty signature header, a header that records the name, version,
release and architecture and the payload format and compressor, and then the archive compressed
with gzip, as a driver package holds it. third_party/redshift_odbc/tools/lib/rpm.py reads the header
and the payload the same way it reads AWS's packages, so a suite needs no rpm2cpio.

PAYLOAD_FORMAT, PAYLOAD_COMPRESSOR and PAYLOAD_RAW in the environment make a package that rpm.py
must refuse: another declared format or compressor, or an uncompressed payload
under the gzip declaration. Standard library only.
"""

import os
import struct
import sys
import zlib

LEAD = b"\xed\xab\xee\xdb\x03\x00" + b"\x00" * 4 + b"test".ljust(66, b"\x00") + struct.pack(">HH", 1, 5) + b"\x00" * 16
MAGIC = b"\x8e\xad\xe8\x01\x00\x00\x00\x00"


def structure(entries):
    index, store = b"", b""
    for tag, value in entries:
        index += struct.pack(">IIII", tag, 6, len(store), 1)
        store += value.encode() + b"\x00"
    return MAGIC + struct.pack(">II", len(entries), len(store)) + index + store


def gzip_bytes(data):
    deflate = zlib.compressobj(9, zlib.DEFLATED, 16 + zlib.MAX_WBITS)
    return deflate.compress(data) + deflate.flush()


def main(argv):
    out, name, version, release, arch = argv[1:6]
    header = structure(
        [
            (1000, name),
            (1001, version),
            (1002, release),
            (1022, arch),
            (1124, os.environ.get("PAYLOAD_FORMAT", "cpio")),
            (1125, os.environ.get("PAYLOAD_COMPRESSOR", "gzip")),
        ]
    )
    archive = sys.stdin.buffer.read()
    payload = archive if os.environ.get("PAYLOAD_RAW") else gzip_bytes(archive)
    with open(out, "wb") as f:
        f.write(LEAD + structure([]) + header + payload)


if __name__ == "__main__":
    main(sys.argv)
