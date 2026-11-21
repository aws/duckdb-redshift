"""Writes a one-folder LZX cabinet for the decoder's tests, with every block type the decoder
reads: verbatim blocks, an aligned-offset block, an odd-sized uncompressed block that crosses a
frame, matches by each repeated offset, length footers, and the x86 E8 translation over several
frames. make_msi.py stays simple and writes only uncompressed blocks; this file is the encoder
for the decoder's other paths.

    python3 -I make_lzx_cab.py <out.cab> <out.raw> [<window bits>]

<out.raw> is the bytes the cabinet's one file must decode to. The window is 2^16 bytes unless
<window bits>, 15 to 21, names another; its position slots size the main tree, and in every window
the matches reach its last position slot, or the encoder fails. A window over 2^16 also gets stored
random bytes and then copies of the sample from past them, so its matches reach that far, through
slots over 31 with their own bases and extra bits. The encoder is written from the format,
separately from lib/cab.py: its Huffman codes are complete but not optimal, its matcher is greedy,
and its E8 step is the encoder's direction of the translation. Standard library only.
"""

import random
import struct
import sys

FRAME = 32768
# Position slots per window; a match reaches at most the window's size less 3 bytes back.
SLOTS = {15: 30, 16: 32, 17: 34, 18: 36, 19: 38, 20: 42, 21: 50}
# Complete but not uniform, so an aligned offset decoded as 3 plain bits comes out wrong.
ALIGNED = [1, 2, 3, 4, 5, 6, 7, 7]
E8_SIZE = 12000000
BASE, EXTRA = [], []
for k in range(26):
    EXTRA += [max(0, min(k - 1, 17))] * 2
b = 0
for e in EXTRA:
    BASE.append(b)
    b += 1 << e


class Writer(object):
    """16-bit little-endian words, most significant bit first, and raw bytes between them."""

    def __init__(self):
        self.out, self.word, self.count = bytearray(), 0, 0

    def bits(self, value, n):
        for i in range(n - 1, -1, -1):
            self.word = self.word << 1 | (value >> i & 1)
            self.count += 1
            if self.count == 16:
                self.out += struct.pack("<H", self.word)
                self.word = self.count = 0

    def align(self):
        if self.count:
            self.bits(0, 16 - self.count)


def lengths(used, total):
    """A complete code over the used symbols: 2^k - m of them get k - 1 bits, the rest k."""
    used = sorted(set(used)) or [0]
    if len(used) == 1:
        used.append(used[0] + 1 if used[0] + 1 < total else used[0] - 1)
    k = (len(used) - 1).bit_length()
    out = [0] * total
    for i, s in enumerate(sorted(used)):
        out[s] = k - 1 if i < (1 << k) - len(used) else k
    return out


def codes(lens):
    out, code = {}, 0
    for length in range(1, 17):
        for s, l in enumerate(lens):
            if l == length:
                out[s] = (code, length)
                code += 1
        code <<= 1
    return out


PRETREE = lengths(range(20), 20)
PRECODES = codes(PRETREE)


def write_lengths(w, new, old):
    """The pretree, then new as deltas from old, with zero runs (17, 18) and equal runs (19)."""
    for l in PRETREE:
        w.bits(l, 4)
    x = 0
    while x < len(new):
        run = 1
        while x + run < len(new) and new[x + run] == new[x] and old[x + run] == old[x]:
            run += 1
        if new[x] == 0 and run >= 20:
            run = min(run, 51)
            w.bits(*PRECODES[18]), w.bits(run - 20, 5)
        elif new[x] == 0 and run >= 4:
            run = min(run, 19)
            w.bits(*PRECODES[17]), w.bits(run - 4, 4)
        elif run >= 4:
            run = min(run, 5)
            w.bits(*PRECODES[19]), w.bits(run - 4, 1), w.bits(*PRECODES[(old[x] - new[x]) % 17])
        else:
            run = 1
            w.bits(*PRECODES[(old[x] - new[x]) % 17])
        x += run


def e8_forward(data):
    """The encoder's E8 step, per frame: a CALL's relative target becomes absolute."""
    data = bytearray(data)
    for start in range(0, len(data), FRAME):
        end = min(start + FRAME, len(data))
        i = start
        while end - start > 10 and i < end - 10:
            if data[i] == 0xE8:
                value = struct.unpack_from("<i", data, i + 1)[0]
                if -i <= value < E8_SIZE - i:
                    struct.pack_into("<i", data, i + 1, value + i)
                elif E8_SIZE - i <= value < E8_SIZE:
                    struct.pack_into("<i", data, i + 1, value - E8_SIZE)
                i += 5
            else:
                i += 1
    return bytes(data)


def sample():
    rng = random.Random(7)
    words = [
        b"driver",
        b"Redshift",
        b"connect",
        b"%08x" % rng.getrandbits(32),
        b"\xe8\x10\x00\x00\x00",
        b"\xe8\xf0\xff\xff\xff",
        b"\xe8" + struct.pack("<i", E8_SIZE - 3),
        b"\xe8\x00\x00\x00\x80",
    ]
    out = bytearray()
    while len(out) < 100000:
        r = rng.random()
        if r < 0.5:
            out += rng.choice(words) + b" "
        elif r < 0.6:
            out += bytes(rng.getrandbits(8) for _ in range(rng.randint(1, 40)))
        elif r < 0.7:
            out += bytes([rng.getrandbits(8)]) * rng.randint(3, 300)
        else:
            out += out[-rng.randint(1, min(len(out), 60000) or 1) :][: rng.randint(2, 300)] if out else b"x"
    return bytes(out[:100000])


def tokens(window, start, end, r, reach):
    """Greedy matches within [start, end), never across it, at most reach back, preferring a
    repeated offset."""
    out, i, last = [], start, {}
    for j in range(max(0, start - reach), start):
        last[window[j : j + 3]] = j
    while i < end:
        best = None
        for offset in r + [1]:
            if offset <= i:
                n = 0
                while n < 257 and i + n < end and window[i + n] == window[i + n - offset]:
                    n += 1
                if n >= 2 and (best is None or n > best[1]):
                    best = (offset, n)
        j = last.get(window[i : i + 3])
        if j is not None and i - j <= reach:
            n = 0
            while n < 257 and i + n < end and window[i + n] == window[j + n]:
                n += 1
            if n >= 3 and (best is None or n > best[1] + 1):
                best = (i - j, n)
        if best:
            out.append(best)
            n = best[1]
            match(best[0], n, r)
        else:
            out.append(window[i])
            n = 1
        for p in range(i, i + n):
            last[window[p : p + 3]] = p
        i += n
    return out


def match(offset, n, r):
    """A match's (main symbol, length footer or None, slot, offset bits), updating the repeated
    offsets r as the decoder will."""
    if offset in r:
        slot = r.index(offset)
        r[0], r[slot] = r[slot], r[0]
        value = 0
    else:
        slot = max(s for s in range(len(BASE)) if BASE[s] <= offset + 2)
        value = offset + 2 - BASE[slot]
        r[:] = [offset, r[0], r[1]]
    return 256 + slot * 8 + min(n - 2, 7), (n - 9 if n >= 9 else None), slot, value


def encode(raw, plan, window_bits=16):
    """The LZX stream, its frame ends, and the highest position slot its matches use."""
    window = e8_forward(raw)
    w, frames = Writer(), []
    w.bits(1, 1)
    w.bits(E8_SIZE >> 16, 16), w.bits(E8_SIZE & 0xFFFF, 16)
    main_old, length_old = [0] * (256 + SLOTS[window_bits] * 8), [0] * 249
    r = [1, 1, 1]
    reach = (1 << window_bits) - 3
    pos, highest = 0, 0
    for kind, size in plan:
        end = min(pos + size, len(window))
        w.bits(kind, 3), w.bits((end - pos) >> 8, 16), w.bits((end - pos) & 0xFF, 8)
        if kind == 3:
            if w.count == 0:
                w.bits(0, 16)
            w.align()
            w.out += struct.pack("<III", *r)
            p = pos
            while p < end:
                frame_end = min((p // FRAME + 1) * FRAME, end)
                w.out += window[p:frame_end]
                if frame_end % FRAME == 0:
                    frames.append(len(w.out))
                p = frame_end
            if (end - pos) & 1:
                w.out += b"\0"  # the decoder skips it before the next block
            pos = end
            continue
        # The block's literals and matches, cut at frame ends, as no match may cross one.
        items, p = [], pos
        while p < end:
            frame_end = min((p // FRAME + 1) * FRAME, end)
            items += [
                t if isinstance(t, int) else match(t[0], t[1], r) for t in tokens(window, p, frame_end, list(r), reach)
            ]
            items.append(("frame", frame_end))
            p = frame_end
        matches = [i for i in items if isinstance(i, tuple) and i[0] != "frame"]
        highest = max([highest] + [m[2] for m in matches])
        main = lengths({0xE8} | {i for i in items if isinstance(i, int)} | {m[0] for m in matches}, len(main_old))
        footers = {m[1] for m in matches if m[1] is not None}
        length = lengths(footers, 249) if footers else [0] * 249
        aligned = ALIGNED
        if kind == 2:
            for l in aligned:
                w.bits(l, 3)
        write_lengths(w, main[:256], main_old[:256])
        write_lengths(w, main[256:], main_old[256:])
        write_lengths(w, length, length_old)
        main_old, length_old = main, length
        mc, lc, ac = codes(main), codes(length), codes(aligned)
        for item in items:
            if isinstance(item, int):
                w.bits(*mc[item])
            elif item[0] == "frame":
                if item[1] % FRAME == 0:
                    w.align()
                    frames.append(len(w.out))
            else:
                symbol, footer, slot, value = item
                w.bits(*mc[symbol])
                if footer is not None:
                    w.bits(*lc[footer])
                if slot >= 3 and kind == 2 and EXTRA[slot] >= 3:
                    w.bits(value >> 3, EXTRA[slot] - 3)
                    w.bits(*ac[value & 7])
                elif slot >= 3:
                    w.bits(value, EXTRA[slot])
        pos = end
    w.align()
    frames.append(len(w.out))
    return bytes(w.out), sorted(set(frames)), highest


def cabinet(data, frames, total, name=b"lzx.bin", window_bits=16):
    sizes = [min(FRAME, total - i * FRAME) for i in range((total + FRAME - 1) // FRAME)]
    cuts = [0] + frames[: len(sizes) - 1] + [len(data)]
    blocks = b"".join(
        struct.pack("<IHH", 0, cuts[i + 1] - cuts[i], s) + data[cuts[i] : cuts[i + 1]] for i, s in enumerate(sizes)
    )
    files = struct.pack("<IIHHHH", total, 0, 0, 0x5000, 0, 0x20) + name + b"\0"
    start = 36 + 8 + len(files)
    head = struct.pack("<4sIIIIIBBHHHHH", b"MSCF", 0, start + len(blocks), 0, 44, 0, 3, 1, 1, 1, 0, 0, 0)
    return head + struct.pack("<IHH", start, len(sizes), 3 | window_bits << 8) + files + blocks


def main(argv):
    if len(argv) not in (3, 4) or len(argv) == 4 and argv[3] not in map(str, SLOTS):
        sys.stderr.write("usage: make_lzx_cab.py <out.cab> <out.raw> [<window bits, 15 to 21>]\n")
        return 2
    window_bits = int(argv[3]) if len(argv) == 4 else 16
    raw = sample()
    plan = [(1, 20000), (2, 30000), (3, 21001), (1, 28999)]
    if window_bits > 16:
        # Stored random bytes, then two 15,000-byte copies from the sample, a verbatim block and an
        # aligned one: at least 2^16 bytes, or as many as leave the first copy's source 35,000 bytes
        # inside the window, so the matches after them reach its last position slot, as the sample's
        # own matches do in a smaller window.
        stored = max(1 << 16, (1 << window_bits) - len(raw) - 35000)
        rng = random.Random(window_bits)
        raw += bytes(rng.getrandbits(8) for _ in range(stored)) + raw[:15000] + raw[50000:65000]
        plan += [(3, stored), (1, 15000), (2, 15000)]
    data, frames, highest = encode(raw, plan, window_bits)
    # The window's own slots are what a case in it tests, so a sample that stops short fails here.
    if highest != SLOTS[window_bits] - 1:
        sys.stderr.write(
            "make_lzx_cab.py: the matches reach position slot %d, not the window's last, %d\n"
            % (highest, SLOTS[window_bits] - 1)
        )
        return 1
    with open(argv[1], "wb") as f:
        f.write(cabinet(data, frames, len(raw), window_bits=window_bits))
    with open(argv[2], "wb") as f:
        f.write(raw)
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv))
