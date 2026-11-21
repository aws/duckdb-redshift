"""Reads an RPM package: the identity its header records, or its payload.

    python3 -I rpm.py <package>
    python3 -I rpm.py --payload <package> | python3 -I cpio.py ...

The first prints "<name> <version> <release> <arch>" and exits 0. Verification runs it on the
bytes whose detached signature it has just checked, because the signature covers only the bytes,
not the version or platform they were downloaded as.

--payload writes the payload's cpio archive, decompressed, to stdout, as rpm2cpio does, for
lib/cpio.py, which checks every member. The header must record a cpio payload compressed with
gzip, the form every driver package so far has, and the gzip stream must end the file; any other
form fails until a reviewed change accepts it.

Either fails, printing why the file is not an RPM package this reader accepts, and exits 1. Only
the lead and the two headers are parsed; nothing in the package is executed. Standard library
only, Python 3.6 and later.
"""

import os
import struct
import sys
import zlib

LEAD_SIZE = 96
LEAD_MAGIC = b"\xed\xab\xee\xdb"
HEADER_MAGIC = b"\x8e\xad\xe8\x01"
MAX_ENTRIES = 1 << 16
MAX_STORE = 64 << 20
IDENTITY = (1000, 1001, 1002, 1022)  # name, version, release, arch
PAYLOAD = (1124, 1125)  # payload format, payload compressor
TAGS = {1000: "name", 1001: "version", 1002: "release", 1022: "arch", 1124: "payload format",
        1125: "payload compressor"}
STRING = 6
CHUNK = 1 << 20


def header(f, offset):
    """(entries, store, end offset) of the header structure at offset."""
    f.seek(offset)
    intro = f.read(16)
    if len(intro) != 16 or intro[:4] != HEADER_MAGIC:
        raise ValueError("no RPM header at offset %d" % offset)
    count, size = struct.unpack(">II", intro[8:16])
    if count > MAX_ENTRIES or size > MAX_STORE:
        raise ValueError("the RPM header at offset %d is larger than this reader accepts" % offset)
    index = f.read(16 * count)
    store = f.read(size)
    if len(index) != 16 * count or len(store) != size:
        raise ValueError("the package ends inside its header")
    entries = [struct.unpack(">IIII", index[16 * i:16 * i + 16]) for i in range(count)]
    return entries, store, offset + 16 + 16 * count + size


def read(f, wanted):
    """The values of the wanted string tags in the main header, and the payload's offset."""
    if f.read(4) != LEAD_MAGIC:
        raise ValueError("not an RPM package")
    _, _, end = header(f, LEAD_SIZE)  # the signature header, padded to eight bytes
    entries, store, payload = header(f, end + (8 - end % 8) % 8)
    found = {}
    for tag, kind, offset, count in entries:
        if tag not in wanted:
            continue
        if tag in found:
            raise ValueError("the header records tag %d twice" % tag)
        end = store.find(b"\0", offset)
        if kind != STRING or count != 1 or offset >= len(store) or end < 0:
            raise ValueError("the header's tag %d is not one string" % tag)
        value = store[offset:end].decode("ascii", "replace")
        if not value or not all(0x21 <= ord(c) <= 0x7E for c in value):
            raise ValueError("the header's tag %d is empty or holds a byte that is not printable "
                             "ASCII" % tag)
        found[tag] = value
    missing = [TAGS[t] for t in wanted if t not in found]
    if missing:
        raise ValueError("the header records no %s" % ", ".join(missing))
    return [found[t] for t in wanted], payload


def write_payload(f):
    """Streams the gzip payload, decompressed, to stdout; the stream must end the file."""
    (form, compressor), offset = read(f, PAYLOAD)
    if (form, compressor) != ("cpio", "gzip"):
        raise ValueError("the payload is %s compressed with %s; this reader takes cpio compressed "
                         "with gzip" % (form, compressor))
    f.seek(offset)
    gunzip = zlib.decompressobj(16 + zlib.MAX_WBITS)
    while not gunzip.eof:
        data = gunzip.unconsumed_tail or f.read(CHUNK)
        if not data:
            raise ValueError("the package ends inside its gzip payload")
        out = memoryview(gunzip.decompress(data, CHUNK))  # at most 1 MiB held at a time
        while out:
            out = out[os.write(1, out):]
    if gunzip.unused_data or f.read(1):
        raise ValueError("data follows the package's gzip payload")


def main(argv):
    try:
        if len(argv) == 2:
            with open(argv[1], "rb") as f:
                sys.stdout.write(" ".join(read(f, IDENTITY)[0]) + "\n")
        elif len(argv) == 3 and argv[1] == "--payload":
            with open(argv[2], "rb") as f:
                write_payload(f)
        else:
            sys.stderr.write("usage: rpm.py <package> | rpm.py --payload <package>\n")
            return 2
    except BrokenPipeError:
        # The reader stopped early, as cpio.py does on a bad archive: end quietly, so only the
        # reader's message is reported.
        os._exit(141)
    except (ValueError, OSError, zlib.error) as e:
        sys.stderr.write("%s\n" % e)
        return 1
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv))
