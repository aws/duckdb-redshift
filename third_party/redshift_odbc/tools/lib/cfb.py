"""Reads a compound file, the container an MSI is, and computes the MSI's Authenticode digest.

    python3 -I cfb.py signature <msi> <out.der>
    python3 -I cfb.py digest <msi> <sha256|sha384|sha512>

The reader is strict, so that this reader and Windows's cannot see different streams. It refuses
a file over 64 MiB (2.3.0.0's MSI is 4.8 MB) before reading it, and then every ambiguity:
  - a sector in more or fewer than one chain, counting the FAT's and DIFAT's own sectors and the
    free ones, so a chain that loops, two chains that share a sector and an orphaned sector all
    fail, and so does a mini sector of the mini stream outside exactly one mini chain;
  - a directory entry not reached exactly once from the root, and siblings that are not in the
    compound file's order (shorter names first, then by upper case), which also refuses two
    names that are equal under that rule;
  - nonzero high size bits in a version 3 file, and a size that disagrees with its chain or with
    the mini-stream cutoff;
  - bytes after the last sector.
A stream of size 0 has no chain, whatever its start sector says.

signature writes the root's \\x05DigitalSignature stream to <out.der>, failing "signer: the MSI is
not signed" when there is none. digest prints the hex digest Windows computes for an MSI's
signature: when \\x05MsiDigitalSignatureEx is present, its bytes must equal the hash of every
entry's metadata, and that hash comes first; then every stream's bytes and every storage's CLSID,
in the order of their raw UTF-16LE names, leaving out the two signature streams at the root. A
root stream whose name only starts like one of them is refused, as readers disagree on it.

A reader of the tables takes a stream's bytes through stream(). On failure it prints one line,
"cfb: <reason>" or "signer: <reason>", to stderr and exits 1. Standard library only, Python 3.6
and later.
"""

import hashlib
import struct
import sys

MAX_FILE = 64 << 20
SIGNATURE = "\x05DigitalSignature"
SIGNATURE_EX = "\x05MsiDigitalSignatureEx"
FREE, END, FAT, DIFAT = 0xFFFFFFFF, 0xFFFFFFFE, 0xFFFFFFFD, 0xFFFFFFFC
NONE = 0xFFFFFFFF
MAGIC = bytes.fromhex("d0cf11e0a1b11ae1")
MINI_SECTOR, CUTOFF = 64, 4096
STORAGE, STREAM, ROOT = 1, 2, 5


class Failure(Exception):
    pass


class Entry(object):
    """One directory entry: its name, raw and decoded, its kind, and the fields the digest reads."""

    def __init__(self, raw, index):
        name_bytes, self.kind = raw[0:64], raw[66]
        size = struct.unpack_from("<H", raw, 64)[0]
        self.left, self.right, self.child = struct.unpack_from("<III", raw, 68)
        self.clsid, self.state = raw[80:96], raw[96:100]
        self.times = raw[100:116]
        self.start, self.size_low, self.size_high = struct.unpack_from("<III", raw, 116)
        self.index, self.children = index, []
        if self.kind not in (0, STORAGE, STREAM, ROOT):
            raise Failure("cfb: directory entry %d has the unknown type %d" % (index, self.kind))
        if self.kind == 0:
            return
        units = struct.unpack_from("<%dH" % (size // 2), name_bytes) if size % 2 == 0 and 2 <= size <= 64 else ()
        if not units or units[-1] != 0 or 0 in units[:-1]:
            raise Failure("cfb: directory entry %d's name is not 1 to 31 characters ended by NUL" % index)
        self.raw_name = name_bytes[:size - 2]
        self.name = self.raw_name.decode("utf-16-le", "surrogatepass")
        # The compound file's order: shorter names first, then code unit by code unit in upper case.
        self.key = (len(units), tuple(upper(u) for u in units[:-1]))

    def label(self):
        return "%r (entry %d)" % (self.name, self.index)


def upper(unit):
    c = chr(unit).upper()
    return ord(c) if len(c) == 1 and not 0xD800 <= unit <= 0xDFFF else unit


class CompoundFile(object):
    def __init__(self, data):
        if len(data) > MAX_FILE:
            raise Failure("cfb: the file is larger than %d bytes" % MAX_FILE)
        if len(data) < 512 or data[:8] != MAGIC:
            raise Failure("cfb: not a compound file")
        (minor, major, order, shift, mini_shift) = struct.unpack_from("<HHHHH", data, 24)
        (dir_sectors, fat_sectors, first_dir, _, cutoff, first_minifat, minifat_sectors,
         first_difat, difat_sectors) = struct.unpack_from("<IIIIIIIII", data, 40)
        if order != 0xFFFE or (major, shift) not in ((3, 9), (4, 12)) or mini_shift != 6 or cutoff != CUTOFF:
            raise Failure("cfb: the header is not a version 3 or 4 compound file this tool reads")
        if major == 3 and dir_sectors != 0:
            raise Failure("cfb: a version 3 file counts its directory sectors")
        self.data, self.major, self.size = data, major, 1 << shift
        if (len(data) - self.size) % self.size or len(data) < self.size:
            raise Failure("cfb: the file has bytes after its last sector")
        if major == 4 and data[512:4096].strip(b"\0"):
            raise Failure("cfb: a version 4 header sector holds bytes after the header")
        self.count = len(data) // self.size - 1
        self.owner = [None] * self.count
        per = self.size // 4

        # The FAT, its sectors listed by the header's DIFAT and then the DIFAT chain.
        difat = list(struct.unpack_from("<109I", data, 76))
        sector = first_difat
        for _ in range(difat_sectors):
            self.own(sector, "the DIFAT")
            words = struct.unpack_from("<%dI" % per, data, self.offset(sector))
            difat += words[:-1]
            sector = words[-1]
        if sector not in (END, FREE) or (difat_sectors == 0 and first_difat not in (END, FREE)):
            raise Failure("cfb: the DIFAT chain does not end where the header says")
        if any(s != FREE for s in difat[fat_sectors:]):
            raise Failure("cfb: the DIFAT lists more FAT sectors than the header counts")
        self.fat = []
        for s in difat[:fat_sectors]:
            self.own(s, "the FAT")
            self.fat += struct.unpack_from("<%dI" % per, data, self.offset(s))
        if len(self.fat) < self.count or any(f != FREE for f in self.fat[self.count:]):
            raise Failure("cfb: the FAT does not cover exactly the file's sectors")
        for i, f in enumerate(self.fat[:self.count]):
            marked = {FAT: "the FAT", DIFAT: "the DIFAT"}.get(f)
            if marked != (self.owner[i] if self.owner[i] in ("the FAT", "the DIFAT") else None):
                raise Failure("cfb: sector %d's FAT entry disagrees with the sectors the header lists "
                              "for the FAT and the DIFAT" % i)
            if f == FREE:
                self.own(i, "free")
            elif f not in (END, FAT, DIFAT) and f >= self.count:
                raise Failure("cfb: sector %d's FAT entry points outside the file" % i)

        directory = self.chain(first_dir, "the directory")
        if major == 4 and len(directory) != dir_sectors:
            raise Failure("cfb: the directory chain's length is not the header's count")
        raw = b"".join(data[self.offset(s):self.offset(s) + self.size] for s in directory)
        self.entries = [Entry(raw[i:i + 128], i // 128) for i in range(0, len(raw), 128)]
        if not self.entries:
            raise Failure("cfb: the directory is empty")
        self.root = self.entries[0]
        if self.root.kind != ROOT or self.root.name != "Root Entry" or (self.root.left, self.root.right) != (NONE, NONE):
            raise Failure("cfb: directory entry 0 is not the root")
        if any(e.kind == ROOT for e in self.entries[1:]):
            raise Failure("cfb: a second root entry")
        self.walk()

        # The mini stream, the root's chain, and the MiniFAT that chains its 64-byte sectors.
        self.mini = self.read_big(self.root, "the mini stream")
        if minifat_sectors == 0 and first_minifat != END:
            raise Failure("cfb: the header gives the MiniFAT a start but no sectors")
        minifat_chain = self.chain(first_minifat, "the MiniFAT") if minifat_sectors else []
        if len(minifat_chain) != minifat_sectors:
            raise Failure("cfb: the MiniFAT chain's length is not the header's count")
        words = b"".join(data[self.offset(s):self.offset(s) + self.size] for s in minifat_chain)
        self.minifat = struct.unpack("<%dI" % (len(words) // 4), words)
        mini_count = (len(self.mini) + MINI_SECTOR - 1) // MINI_SECTOR
        if len(self.minifat) < mini_count or any(f != FREE for f in self.minifat[mini_count:]):
            raise Failure("cfb: the MiniFAT does not cover exactly the mini stream's sectors")
        self.mini_owner = [None] * mini_count
        for i, f in enumerate(self.minifat[:mini_count]):
            if f == FREE:
                self.mini_owner[i] = "free"
        self.streams = {}
        for e in self.entries:
            if e.kind == STREAM:
                self.streams[e.index] = self.read_stream(e)
        unowned = [i for i, o in enumerate(self.owner) if o is None]
        if unowned:
            raise Failure("cfb: sector %d is in no chain" % unowned[0])
        unowned = [i for i, o in enumerate(self.mini_owner) if o is None]
        if unowned:
            raise Failure("cfb: mini sector %d is in no chain" % unowned[0])

    def offset(self, sector):
        if sector >= self.count:
            raise Failure("cfb: sector %d is outside the file" % sector)
        return (sector + 1) * self.size

    def own(self, sector, what):
        self.offset(sector)
        if self.owner[sector] is not None:
            if self.owner[sector] == what:
                raise Failure("cfb: the chain of %s loops at sector %d" % (what, sector))
            raise Failure("cfb: sector %d is in %s and in %s" % (sector, self.owner[sector], what))
        self.owner[sector] = what

    def chain(self, sector, what):
        out = []
        while sector != END:
            self.own(sector, what)
            out.append(sector)
            # A free, FAT or DIFAT marker as the next sector is outside the file, so own() refuses it.
            sector = self.fat[sector]
        return out

    def size_of(self, entry):
        if self.major == 3 and entry.size_high:
            raise Failure("cfb: %s has nonzero high size bits in a version 3 file" % entry.label())
        return entry.size_high << 32 | entry.size_low

    def read_big(self, entry, what):
        size = self.size_of(entry)
        if size == 0:
            return b""
        sectors = self.chain(entry.start, what)
        if len(sectors) != (size + self.size - 1) // self.size:
            raise Failure("cfb: %s's size %d disagrees with its chain of %d sectors" % (what, size, len(sectors)))
        return b"".join(self.data[self.offset(s):self.offset(s) + self.size] for s in sectors)[:size]

    def read_stream(self, entry):
        size = self.size_of(entry)
        if size >= CUTOFF or size == 0:
            return self.read_big(entry, entry.label())
        out, sector = [], entry.start
        while sector != END:
            if sector >= len(self.mini_owner):
                raise Failure("cfb: %s's mini chain leaves the mini stream" % entry.label())
            if self.mini_owner[sector] is not None:
                raise Failure("cfb: mini sector %d is in %s and in %s" % (sector, self.mini_owner[sector], entry.label()))
            self.mini_owner[sector] = entry.label()
            out.append(self.mini[sector * MINI_SECTOR:(sector + 1) * MINI_SECTOR])
            sector = self.minifat[sector]
        if len(out) != (size + MINI_SECTOR - 1) // MINI_SECTOR:
            raise Failure("cfb: %s's size %d disagrees with its mini chain of %d sectors" % (entry.label(), size, len(out)))
        result = b"".join(out)[:size]
        if len(result) != size:
            raise Failure("cfb: %s runs past the end of the mini stream" % entry.label())
        return result

    def walk(self):
        """Gives every storage its children, in order, reaching each used entry exactly once."""
        reached = set([0])
        storages = [self.root]
        while storages:
            storage = storages.pop()
            pending, node, ordered = [], storage.child, []
            # An in-order walk of the storage's sibling tree, with an explicit stack.
            while pending or node != NONE:
                while node != NONE:
                    if node >= len(self.entries) or self.entries[node].kind in (0, ROOT):
                        raise Failure("cfb: %s's tree names entry %d, which is not in use" % (storage.label(), node))
                    if node in reached:
                        raise Failure("cfb: directory entry %d is reached twice" % node)
                    reached.add(node)
                    pending.append(node)
                    node = self.entries[node].left
                entry = self.entries[pending.pop()]
                if ordered and not ordered[-1].key < entry.key:
                    raise Failure("cfb: %s holds %s and %s out of order or with equal names"
                                  % (storage.label(), ordered[-1].label(), entry.label()))
                ordered.append(entry)
                node = entry.right
            storage.children = ordered
            for entry in ordered:
                if entry.kind == STREAM and entry.child != NONE:
                    raise Failure("cfb: stream %s has children" % entry.label())
                if entry.kind == STORAGE:
                    storages.append(entry)
        unreached = [e.index for e in self.entries if e.kind != 0 and e.index not in reached]
        if unreached:
            raise Failure("cfb: directory entry %d is not reached from the root" % unreached[0])

    def stream(self, storage, name):
        """The bytes of the stream <name> in <storage>, or None."""
        for e in storage.children:
            if e.kind == STREAM and e.name == name:
                return self.streams[e.index]
        return None


def signed_children(cf):
    """The root's children the digest covers, refusing a name that only starts like a signature's."""
    out = []
    for e in sorted(cf.root.children, key=lambda e: e.raw_name):
        if e.name in (SIGNATURE, SIGNATURE_EX):
            continue
        if e.name.startswith((SIGNATURE, SIGNATURE_EX)):
            raise Failure("cfb: the root holds %s, named like a signature stream" % e.label())
        out.append(e)
    return out


def metadata(entry, h):
    """What the MsiDigitalSignatureEx hash covers of one entry, as Windows writes it."""
    if entry.kind != ROOT:
        h.update(entry.raw_name)
    h.update(struct.pack("<I", entry.size_low) if entry.kind == STREAM else entry.clsid)
    h.update(entry.state)
    if entry.kind != ROOT:
        h.update(entry.times)


def prehash(storage, h, children):
    metadata(storage, h)
    for e in children:
        if e.kind == STREAM:
            metadata(e, h)
        else:
            prehash(e, h, sorted(e.children, key=lambda c: c.raw_name))


def content(cf, children, storage, h):
    for e in children:
        if e.kind == STREAM:
            h.update(cf.streams[e.index])
        else:
            content(cf, sorted(e.children, key=lambda c: c.raw_name), e, h)
    h.update(storage.clsid)


def digest(cf, algorithm):
    children = signed_children(cf)
    h = hashlib.new(algorithm)
    ex = cf.stream(cf.root, SIGNATURE_EX)
    try:
        if ex is not None:
            pre = hashlib.new(algorithm)
            prehash(cf.root, pre, children)
            if ex != pre.digest():
                raise Failure("digest: \\x05MsiDigitalSignatureEx does not match the MSI's metadata")
            h.update(ex)
        content(cf, children, cf.root, h)
    except RecursionError:
        # The digest recurses once per nested storage; storages nested this deep are no MSI.
        raise Failure("cfb: the MSI's storages nest too deeply")
    return h.hexdigest()


def load(path):
    try:
        with open(path, "rb") as f:
            data = f.read(MAX_FILE + 1)
    except OSError as e:
        raise Failure("cfb: cannot read %s: %s" % (path, e.strerror))
    return CompoundFile(data)


def main(argv):
    try:
        if len(argv) == 4 and argv[1] == "signature":
            cf = load(argv[2])
            data = cf.stream(cf.root, SIGNATURE)
            if data is None:
                raise Failure("signer: the MSI is not signed (it has no \\x05DigitalSignature stream)")
            try:
                with open(argv[3], "xb") as f:
                    f.write(data)
            except OSError as e:
                raise Failure("signer: cannot write %s: %s" % (argv[3], e.strerror))
        elif len(argv) == 4 and argv[1] == "digest" and argv[3] in ("sha256", "sha384", "sha512"):
            sys.stdout.write(digest(load(argv[2]), argv[3]) + "\n")
        else:
            sys.stderr.write("usage: cfb.py signature <msi> <out.der> | digest <msi> <sha256|sha384|sha512>\n")
            return 2
    except Failure as e:
        sys.stderr.write("%s\n" % e)
        return 1
    except struct.error as e:
        sys.stderr.write("cfb: a structure runs past the end of the file: %s\n" % e)
        return 1
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv))
