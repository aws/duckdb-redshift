"""Changes one field in a copy of a universal Mach-O library, for the tool tests' failing inputs.

    python3 -I patch_macho.py <in> <out> <change> [<arch> [<argument>]]

Changes (arch is x86_64 or arm64):
  drop <arch>               leaves the other slice alone in the universal file
  append                    adds one byte after the last slice (arch ignored)
  subtype <arch> <n>        sets the slice's CPU subtype in the universal header
  offset <arch> <n>         sets the slice's offset in the universal header
  size <arch> <n>           sets the slice's size in the universal header
  cputype <arch> <n>        sets the slice's CPU type in the universal header
  align <arch> <n>          sets the slice's alignment in the universal header
  swap                      swaps the two slices' CPU types and subtypes in the universal header
  truncate <arch> <n>       makes the slice n bytes long in the universal header and zeroes the rest
                            of its old bytes (cutting the file when it is the last slice)
  gap                       sets a byte between the first slice's end and the second's start
  gap-header                sets a byte between the universal header and the first slice
  filetype <arch> <n>       sets the slice's Mach-O file type
  header-subtype <arch> <n> sets the CPU subtype in the slice's own Mach-O header
  magic <arch> <n>          sets the slice's Mach-O magic
  flags <arch> <n>          sets the slice's Mach-O header flags
  sizeofcmds <arch> <n>     adds n to the slice's load-command size
  ncmds <arch> <n>          adds n to the slice's load-command count
  cmdsize <arch> <n>        sets the first load command's size
  lastcmd-grow <arch> <n>   adds n to the last load command's size and to the load-command size
  add-command <arch> <n>    adds an 8-byte load command numbered n in the spare space after the
                            load commands
  retag <arch> <from>:<to>  renumbers the first load command numbered from
  dylib <arch> <path>       renames the first LC_LOAD_DYLIB's library, in place
  dylib-offset <arch> <n>   sets the first LC_LOAD_DYLIB's name offset
  dylib-fill <arch>         fills the first LC_LOAD_DYLIB's name to the end of its command, with no
                            NUL
  rpath <arch> <path>       adds an LC_RPATH in the spare space after the load commands
  minos <arch> <x.y.z>      sets LC_BUILD_VERSION's minimum OS
  platform <arch> <n>       sets LC_BUILD_VERSION's platform
  export <arch> <symbol>    renames an exported symbol, changing the last byte of its last edge in
                            the export trie (a symbol whose node has no children)
  bind-ordinal <arch> <stream>:<n>
                            sets the first one-byte library-ordinal opcode of the bind or lazy
                            stream (LC_DYLD_INFO_ONLY) to ordinal n, -15 to 15 (n <= 0 is special)
  weak-name <arch> <symbol> renames a name of the weak-bind stream (LC_DYLD_INFO_ONLY), changing its
                            last byte
  weak-flag <arch> <symbol> flags a name of the weak-bind stream as a non-weak definition
Standard library only, Python 3.6 and later.
"""

import struct
import sys

ARCH = {"x86_64": 0x01000007, "arm64": 0x0100000C}


def slices(data):
    count = struct.unpack(">I", data[4:8])[0]
    out = {}
    for i in range(count):
        cputype, _, offset, size, _ = struct.unpack(">IIIII", data[8 + 20 * i : 28 + 20 * i])
        out[cputype] = (i, offset, size)
    return out


def commands(data, base):
    ncmds, sizeofcmds = struct.unpack("<II", data[base + 16 : base + 24])
    at, out = base + 32, []
    for _ in range(ncmds):
        cmd, size = struct.unpack("<II", data[at : at + 8])
        out.append((cmd, at, size))
        at += size
    return out, base + 32 + sizeofcmds


def find(data, base, cmd):
    return [at for c, at, _ in commands(data, base)[0] if c == cmd]


def uleb(data, at):
    value = shift = 0
    while True:
        b = data[at]
        at += 1
        value |= (b & 0x7F) << shift
        shift += 7
        if not b & 0x80:
            return value, at


def rename_export(data, base, symbol):
    at = find(data, base, 0x80000022)[0]
    trie, size = struct.unpack("<II", data[at + 40 : at + 48])
    trie += base

    def walk(node, prefix):
        info, p = uleb(data, trie + node)
        p += info
        children = data[p]
        p += 1
        for _ in range(children):
            end = data.index(b"\0", p)
            label = data[p:end].decode()
            child, p = uleb(data, end + 1)
            name = prefix + label
            if name == symbol:
                info, q = uleb(data, trie + child)
                if data[q + info]:
                    raise SystemExit("patch_macho.py: %s's node has children" % symbol)
                return end - 1
            if symbol.startswith(name):
                found = walk(child, name)
                if found is not None:
                    return found
        return None

    last = walk(0, "")
    if last is None:
        raise SystemExit("patch_macho.py: no export %s" % symbol)
    data[last] = ord("Y") if data[last] != ord("Y") else ord("Z")


def set_bind_ordinal(data, base, arg):
    stream, ordinal = arg.split(":")
    ordinal = int(ordinal)
    at = find(data, base, 0x80000022)[0]
    field = {"bind": 16, "lazy": 32}[stream]
    offset, size = struct.unpack("<II", data[at + field : at + field + 8])
    p, end = base + offset, base + offset + size
    while p < end:
        op = data[p] & 0xF0
        if op in (0x10, 0x30):
            data[p] = (0x30 | (ordinal & 0xF)) if ordinal <= 0 else (0x10 | ordinal)
            return
        p += 1
        if op == 0x40:
            p = data.index(b"\0", p) + 1
        elif op in (0x20, 0x60, 0x70, 0x80, 0xA0, 0xC0):
            for _ in range(2 if op == 0xC0 else 1):
                _, p = uleb(data, p)
    raise SystemExit("patch_macho.py: no one-byte ordinal opcode in the %s stream" % stream)


def weak_name(data, base, symbol):
    """The offset of the weak-bind stream's set-symbol opcode naming symbol."""
    at = find(data, base, 0x80000022)[0]
    offset, size = struct.unpack("<II", data[at + 24 : at + 32])
    p, end = base + offset, base + offset + size
    while p < end:
        op = data[p] & 0xF0
        p += 1
        if op == 0x40:
            stop = data.index(b"\0", p)
            if data[p:stop] == symbol.encode():
                return p - 1
            p = stop + 1
        elif op in (0x20, 0x60, 0x70, 0x80, 0xA0, 0xC0):
            for _ in range(2 if op == 0xC0 else 1):
                _, p = uleb(data, p)
    raise SystemExit("patch_macho.py: no weak-bind name %s" % symbol)


def append_command(data, base, blob):
    """Writes a load command into the spare space after the load commands and counts it."""
    _, end = commands(data, base)
    if data[end : end + len(blob)].strip(b"\0"):
        raise SystemExit("patch_macho.py: no spare space after the load commands")
    data[end : end + len(blob)] = blob
    ncmds, sizeofcmds = struct.unpack("<II", data[base + 16 : base + 24])
    data[base + 16 : base + 24] = struct.pack("<II", ncmds + 1, sizeofcmds + len(blob))


# Each change is a function (data, index, base, size, arg) that edits data in place: index, base and
# size are the slice's entry in the universal header, its offset and its size.
def fat_field(field):
    def change(data, index, base, size, arg):
        data[8 + 20 * index + field : 12 + 20 * index + field] = struct.pack(">I", int(arg, 0))

    return change


def header_field(at, add=False):
    def change(data, index, base, size, arg):
        value = int(arg, 0) + (struct.unpack("<I", data[base + at : base + at + 4])[0] if add else 0)
        data[base + at : base + at + 4] = struct.pack("<I", value)

    return change


def drop(data, index, base, size, arg):
    keep = [i for i in range(2) if i != index][0]
    entry = data[8 + 20 * keep : 28 + 20 * keep]
    data[4:8] = struct.pack(">I", 1)
    data[8:48] = entry + b"\0" * 20


def append(data, index, base, size, arg):
    data += b"\1"


def swap(data, index, base, size, arg):
    first, second = data[8:16], data[28:36]
    data[8:16], data[28:36] = second, first


def truncate(data, index, base, size, arg):
    n = int(arg, 0)
    data[8 + 20 * index + 12 : 8 + 20 * index + 16] = struct.pack(">I", n)
    if base + size == len(data):
        del data[base + n :]
    else:
        data[base + n : base + size] = bytes(size - n)


def gap(data, index, base, size, arg):
    ends = sorted((o, o + z) for _, o, z in slices(data).values())
    if ends[0][1] >= ends[1][0]:
        raise SystemExit("patch_macho.py: no gap between the slices")
    data[ends[0][1]] = 1


def gap_header(data, index, base, size, arg):
    data[8 + 20 * len(slices(data)) + 4] = 1


def cmdsize(data, index, base, size, arg):
    data[base + 36 : base + 40] = struct.pack("<I", int(arg, 0))


def lastcmd_grow(data, index, base, size, arg):
    _, at, cmd_size = commands(data, base)[0][-1]
    data[at + 4 : at + 8] = struct.pack("<I", cmd_size + int(arg, 0))
    sizeofcmds = struct.unpack("<I", data[base + 20 : base + 24])[0]
    data[base + 20 : base + 24] = struct.pack("<I", sizeofcmds + int(arg, 0))


def add_command(data, index, base, size, arg):
    append_command(data, base, struct.pack("<II", int(arg, 0), 8))


def retag(data, index, base, size, arg):
    old, new = (int(x, 0) for x in arg.split(":"))
    at = find(data, base, old)[0]
    data[at : at + 4] = struct.pack("<I", new)


def rename_dylib(data, index, base, size, arg):
    at = find(data, base, 0xC)[0]
    cmd_size, name_at = struct.unpack("<II", data[at + 4 : at + 12])
    room = cmd_size - name_at
    new = arg.encode()
    if len(new) >= room:
        raise SystemExit("patch_macho.py: %s does not fit" % arg)
    data[at + name_at : at + cmd_size] = new + b"\0" * (room - len(new))


def dylib_offset(data, index, base, size, arg):
    at = find(data, base, 0xC)[0]
    data[at + 8 : at + 12] = struct.pack("<I", int(arg, 0))


def dylib_fill(data, index, base, size, arg):
    at = find(data, base, 0xC)[0]
    cmd_size, name_at = struct.unpack("<II", data[at + 4 : at + 12])
    data[at + name_at : at + cmd_size] = b"x" * (cmd_size - name_at)


def rpath(data, index, base, size, arg):
    path = arg.encode() + b"\0"
    cmd_size = (12 + len(path) + 7) // 8 * 8
    append_command(
        data, base, struct.pack("<III", 0x8000001C, cmd_size, 12) + path + b"\0" * (cmd_size - 12 - len(path))
    )


def minos(data, index, base, size, arg):
    at = find(data, base, 0x32)[0]
    major, minor, patch = (int(x) for x in arg.split("."))
    data[at + 12 : at + 16] = struct.pack("<I", major << 16 | minor << 8 | patch)


def platform(data, index, base, size, arg):
    at = find(data, base, 0x32)[0]
    data[at + 8 : at + 12] = struct.pack("<I", int(arg, 0))


def weak_rename(data, index, base, size, arg):
    last = weak_name(data, base, arg) + len(arg)
    data[last] = ord("Y") if data[last] != ord("Y") else ord("Z")


def weak_flag(data, index, base, size, arg):
    data[weak_name(data, base, arg)] |= 0x8


CHANGES = {
    "drop": drop,
    "append": append,
    "subtype": fat_field(4),
    "offset": fat_field(8),
    "size": fat_field(12),
    "cputype": fat_field(0),
    "align": fat_field(16),
    "swap": swap,
    "truncate": truncate,
    "gap": gap,
    "gap-header": gap_header,
    "filetype": header_field(12),
    "header-subtype": header_field(8),
    "magic": header_field(0),
    "flags": header_field(24),
    "sizeofcmds": header_field(20, add=True),
    "ncmds": header_field(16, add=True),
    "cmdsize": cmdsize,
    "lastcmd-grow": lastcmd_grow,
    "add-command": add_command,
    "retag": retag,
    "dylib": rename_dylib,
    "dylib-offset": dylib_offset,
    "dylib-fill": dylib_fill,
    "rpath": rpath,
    "minos": minos,
    "platform": platform,
    "export": lambda data, index, base, size, arg: rename_export(data, base, arg),
    "bind-ordinal": lambda data, index, base, size, arg: set_bind_ordinal(data, base, arg),
    "weak-name": weak_rename,
    "weak-flag": weak_flag,
}


def main(argv):
    if len(argv) < 4 or argv[3] not in CHANGES:
        sys.stderr.write(__doc__)
        return 2
    data = bytearray(open(argv[1], "rb").read())
    arch = argv[4] if len(argv) > 4 else "x86_64"
    index, base, size = slices(data)[ARCH[arch]]
    CHANGES[argv[3]](data, index, base, size, argv[5] if len(argv) > 5 else None)
    open(argv[2], "wb").write(data)
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv))
