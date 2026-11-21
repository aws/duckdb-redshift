"""Reads a cabinet, the archive an MSI's files are stored in, with its LZX decoder.

    python3 -I cab.py list <cabinet>

lib/msi.py imports it: files(data, max_file, max_total) returns {name: bytes} for every file of a
cabinet held in data. list prints "<sha256>  <name>" for each file of a cabinet file, within the
bounds lib/msi.py uses, or the reason it cannot, and exits 1. It reads only what the 2.3.0.0 MSI uses, one cabinet that continues in no
other, whose folders are LZX-compressed, and refuses every other form by name: another compression
(none, MSZIP, Quantum), a reserved area, a file that spans folders. Every file's size and every
folder's decoded size are bounded before anything is decompressed, and each folder must decode to
exactly the sum of its blocks' sizes, which must cover its files exactly. Data checksums are not
checked: lib/msi.py checks every file it takes against the lock's SHA-256.

The decoder follows the LZX format as the cabinet holds it: one stream per folder, 32 KiB frames
aligned to 16 bits, verbatim, aligned-offset and uncompressed blocks, the three repeated offsets
and the x86 E8 call translation. A Huffman table that is not complete, a match that reaches before
the start of the folder or past its frame, and input that ends early all fail. Standard library
only, Python 3.6 and later.
"""

import hashlib
import struct
import sys

FRAME = 32768
# A run of lengths may write past its tree, by at most 51, as the encoder emits them; what it writes
# there is a delta base for the next tree read, so it is kept, not refused.
SAFETY = 64
SLOTS = {15: 30, 16: 32, 17: 34, 18: 36, 19: 38, 20: 42, 21: 50}
EXTRA_BITS, POSITION_BASE = [], []
for _i in range(26):
    EXTRA_BITS += [max(0, min(_i - 1, 17))] * 2
_base = 0
for _bits in EXTRA_BITS:
    POSITION_BASE.append(_base)
    _base += 1 << _bits
LZX = 3


class Failure(Exception):
    pass


class Bits(object):
    """LZX's bit stream: 16-bit little-endian words, read most significant bit first."""

    def __init__(self, data):
        self.data, self.pos, self.buf, self.left = data, 0, 0, 0

    def ensure(self, n):
        while self.left < n:
            if self.pos + 2 > len(self.data) + 4:
                raise Failure("cab: the LZX data ends early")
            word = self.data[self.pos:self.pos + 2].ljust(2, b"\0")
            self.pos += 2
            self.buf = (self.buf << 16) | word[0] | (word[1] << 8)
            self.left += 16

    def read(self, n):
        if n == 0:
            return 0
        self.ensure(n)
        self.left -= n
        value = self.buf >> self.left
        self.buf &= (1 << self.left) - 1
        return value

    def symbol(self, table):
        if table is None:
            raise Failure("cab: an LZX symbol is read from an empty Huffman tree")
        self.ensure(16)
        entry = table[self.buf >> (self.left - 16)]
        self.left -= entry & 31
        self.buf &= (1 << self.left) - 1
        return entry >> 5

    def raw(self, n):
        if self.pos + n > len(self.data):
            raise Failure("cab: an uncompressed LZX block ends early")
        out = self.data[self.pos:self.pos + n]
        self.pos += n
        return out


def table(lengths):
    """A 16-bit lookup table of a canonical Huffman code, each entry symbol << 5 | length; None when
    every length is 0."""
    if not any(lengths):
        return None
    out, code = [0] * 65536, 0
    for length in range(1, 17):
        for symbol, l in enumerate(lengths):
            if l == length:
                span = 1 << (16 - length)
                if code + span > 65536:
                    raise Failure("cab: an LZX Huffman tree is over-full")
                out[code:code + span] = [symbol << 5 | length] * span
                code += span
    if code != 65536:
        raise Failure("cab: an LZX Huffman tree is not complete")
    return out


def read_lengths(bits, lengths, first, last):
    """Updates lengths[first:last] from the pretree-coded deltas that follow."""
    pretree = table([bits.read(4) for _ in range(20)])
    x = first
    while x < last:
        z = bits.symbol(pretree)
        if z in (17, 18):
            run = bits.read(4) + 4 if z == 17 else bits.read(5) + 20
            values = [0] * run
        elif z == 19:
            run = bits.read(1) + 4
            values = [(lengths[x] - bits.symbol(pretree)) % 17] * run
        else:
            run, values = 1, [(lengths[x] - z) % 17]
        lengths[x:x + run] = values  # at most 51 past its tree, inside SAFETY
        x += run


def lzx(data, window_bits, out_size):
    """Decodes one folder's LZX stream to out_size bytes."""
    if window_bits not in SLOTS:
        raise Failure("cab: the LZX window of 2^%d bytes is not one LZX defines" % window_bits)
    main_count = 256 + SLOTS[window_bits] * 8
    main_lengths, length_lengths = [0] * (main_count + SAFETY), [0] * (249 + SAFETY)
    main = length = aligned = None
    r0 = r1 = r2 = 1
    bits = Bits(data)
    window = bytearray()
    out = bytearray()
    e8_size = (bits.read(16) << 16 | bits.read(16)) if bits.read(1) else 0
    e8_started = False
    kind, remaining, block_length = None, 0, 0
    while len(window) < out_size:
        frame_start = len(window)
        frame_end = min(frame_start + FRAME, out_size)
        while len(window) < frame_end:
            if remaining == 0:
                if kind == 3 and block_length & 1:
                    bits.raw(1)
                kind = bits.read(3)
                remaining = block_length = bits.read(16) << 8 | bits.read(8)
                if kind == 2:
                    aligned = table([bits.read(3) for _ in range(8)])
                if kind in (1, 2):
                    read_lengths(bits, main_lengths, 0, 256)
                    read_lengths(bits, main_lengths, 256, main_count)
                    main = table(main_lengths[:main_count])
                    e8_started = e8_started or main_lengths[0xE8] != 0
                    read_lengths(bits, length_lengths, 0, 249)
                    length = table(length_lengths[:249])
                elif kind == 3:
                    e8_started = True
                    if bits.left == 0:
                        bits.ensure(16)
                    bits.buf = bits.left = 0
                    r0, r1, r2 = struct.unpack("<III", bits.raw(12))
                    if 0 in (r0, r1, r2):
                        raise Failure("cab: an LZX repeated offset is 0")
                else:
                    raise Failure("cab: unknown LZX block type %d" % kind)
                continue
            run = min(remaining, frame_end - len(window))
            remaining -= run
            if kind == 3:
                window += bits.raw(run)
                continue
            end = len(window) + run
            while len(window) < end:
                symbol = bits.symbol(main)
                if symbol < 256:
                    window.append(symbol)
                    continue
                symbol -= 256
                match = symbol & 7
                if match == 7:
                    match += bits.symbol(length)
                match += 2
                slot = symbol >> 3
                if slot == 0:
                    offset = r0
                elif slot == 1:
                    offset, r1 = r1, r0
                elif slot == 2:
                    offset, r2 = r2, r0
                else:
                    extra = EXTRA_BITS[slot]
                    offset = POSITION_BASE[slot] - 2
                    if kind == 2 and extra >= 3:
                        offset += (bits.read(extra - 3) << 3) + bits.symbol(aligned)
                    else:
                        offset += bits.read(extra)
                    r1, r2 = r0, r1
                r0 = offset
                if len(window) + match > end:
                    raise Failure("cab: an LZX match runs past its block or frame")
                if offset > len(window):
                    raise Failure("cab: an LZX match reaches before the start of the folder")
                start = len(window) - offset
                if offset >= match:
                    window += window[start:start + match]
                else:
                    window += (window[start:] * (match // offset + 1))[:match]
        frame = window[frame_start:frame_end]
        if e8_started and e8_size and frame_start // FRAME < 32768 and len(frame) > 10:
            translate(frame, frame_start, e8_size)
        out += frame
        if bits.left > 0:
            bits.ensure(16)
        bits.read(bits.left & 15)
    return bytes(out)


def translate(frame, position, size):
    """Undoes the encoder's E8 translation: each CALL's absolute target back to a relative one."""
    i = frame.find(b"\xe8", 0, len(frame) - 10)
    while i != -1:
        target = struct.unpack_from("<i", frame, i + 1)[0]
        here = position + i
        if -here <= target < size:
            struct.pack_into("<I", frame, i + 1, (target - here if target >= 0 else target + size) & 0xFFFFFFFF)
        i = frame.find(b"\xe8", i + 5, len(frame) - 10)


def files(data, max_file, max_total, total_of=""):
    try:
        (magic, _, size, _, files_at, _, minor, major, folders, count, flags, _, _) = \
            struct.unpack_from("<4sIIIIIBBHHHHH", data, 0)
    except struct.error:
        raise Failure("cab: the cabinet's header is truncated")
    if magic != b"MSCF" or (major, minor) != (1, 3) or size != len(data):
        raise Failure("cab: not a version 1.3 cabinet whose size is its stream's")
    if flags:
        raise Failure("cab: the cabinet continues another, or reserves room (flags 0x%x)" % flags)
    try:
        folder_rows = [struct.unpack_from("<IHH", data, 36 + 8 * i) for i in range(folders)]
        entries, pos = [], files_at
        for _ in range(count):
            file_size, offset, folder = struct.unpack_from("<IIH", data, pos)
            end = data.index(b"\0", pos + 16)
            entries.append((data[pos + 16:end].decode("ascii"), file_size, offset, folder))
            pos = end + 1
    except (struct.error, ValueError, UnicodeDecodeError):
        raise Failure("cab: the cabinet's folder or file table is malformed")
    if any(e[3] >= folders for e in entries):
        raise Failure("cab: a file names a folder the cabinet does not have, or spans folders")
    out, total = {}, 0
    for index, (start, blocks, compression) in enumerate(folder_rows):
        if compression & 0xF != LZX:
            raise Failure("cab: folder %d's compression %d is not LZX, the only one this tool reads"
                          % (index, compression & 0xF))
        mine = sorted((e for e in entries if e[3] == index), key=lambda e: e[2])
        chunks, decoded = [], 0
        for _ in range(blocks):
            try:
                _, packed, unpacked = struct.unpack_from("<IHH", data, start)
            except struct.error:
                raise Failure("cab: folder %d's data blocks run past the cabinet" % index)
            if unpacked > FRAME or start + 8 + packed > len(data):
                raise Failure("cab: folder %d has a data block over 32 KiB or past the cabinet" % index)
            chunks.append(data[start + 8:start + 8 + packed])
            decoded += unpacked
            start += 8 + packed
        expected = 0
        for name, file_size, offset, _ in mine:
            if file_size > max_file:
                raise Failure("cab: %s decodes to %d bytes, over the %d-byte bound" % (name, file_size, max_file))
            if offset != expected:
                raise Failure("cab: folder %d's files do not follow each other from its start" % index)
            expected += file_size
        total += decoded
        if expected != decoded or total > max_total:
            together = "" if total == decoded else " (%d with the folders before it)" % total
            raise Failure("cab: folder %d decodes to %d bytes%s, its files hold %d, and the bound is %d%s"
                          % (index, decoded, together, expected, max_total, total_of))
        stream = lzx(b"".join(chunks), compression >> 8 & 0x1F, decoded)
        for name, file_size, offset, _ in mine:
            if name in out:
                raise Failure("cab: the cabinet holds %s twice" % name)
            out[name] = stream[offset:offset + file_size]
    return out


def main(argv):
    if len(argv) != 3 or argv[1] != "list":
        sys.stderr.write("usage: cab.py list <cabinet>\n")
        return 2
    try:
        with open(argv[2], "rb") as f:
            data = f.read((64 << 20) + 1)
        for name, body in sorted(files(data, 64 << 20, 128 << 20).items()):
            sys.stdout.write("%s  %s\n" % (hashlib.sha256(body).hexdigest(), name))
    except (Failure, OSError) as e:
        sys.stderr.write("%s\n" % e)
        return 1
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv))
