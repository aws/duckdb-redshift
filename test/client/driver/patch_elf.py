"""Copies a 64-bit little-endian ELF file with one field changed, for the ELF policy tests.

    python3 -I patch_elf.py <in> <out> <change>...

Changes (numbers may be decimal or 0x hex):
    byte <offset> <value>          set one byte
    u32 <offset> <value>           set the 4-byte little-endian value at <offset>
    phdr-type <from> <to>          retype the first program header of type <from>
    dyn-tag <from> <to>            retag the first dynamic entry with tag <from>
    dyn-val <tag> <value>          set the value of the first dynamic entry with <tag>
    dyn-clear <tag> <mask>         clear <mask>'s bits in the value of the first entry with <tag>
    dyn-tag-all <from> <to>        retag every dynamic entry with tag <from>
    phdr-add <type> <field> <n>    add <n> (maybe negative) to the offset, vaddr, filesz, memsz or
                                   align of the first program header of type <type>
    phdr-set <type> <field> <n>    set that field of the first program header of type <type> to <n>
    shdr-add <name> <field> <n>    add <n> to the addr, offset or size of section <name>
    shdr-set <name> <field> <n>    set the name (sh_name, an offset into the section-name
                                   strings), type, flags, addr, offset, size, link or info of
                                   section <name>
    dynsym <name> <field> <n>      set the other (st_other, one byte), shndx (st_shndx,
                                   two bytes) or value of the .dynsym symbol <name>, found by its
                                   name in .dynsym's linked strings
    replace <text> <text>          replace every occurrence of a byte string of the same length
Fails when a change finds nothing to change. Standard library only.
"""

import struct
import sys


def phdrs(data):
    off, size, count = struct.unpack_from("<Q", data, 0x20)[0], 56, struct.unpack_from("<H", data, 0x38)[0]
    return [off + size * i for i in range(count)]


def dynamic(data):
    for p in phdrs(data):
        kind, _, off, _, _, filesz = struct.unpack_from("<IIQQQQ", data, p)
        if kind == 2:  # PT_DYNAMIC
            return [off + 16 * i for i in range(filesz // 16)]
    raise SystemExit("no PT_DYNAMIC")


def shdr(data, section):
    off, size, count, names = (
        struct.unpack_from(f, data, o)[0] for f, o in (("<Q", 0x28), ("<H", 0x3A), ("<H", 0x3C), ("<H", 0x3E))
    )
    if isinstance(section, int):
        if 0 <= section < count:
            return off + size * section
        raise SystemExit("no section %d" % section)
    strings = struct.unpack_from("<Q", data, shdr(data, names) + 24)[0]
    for i in range(count):
        s = off + size * i
        start = strings + struct.unpack_from("<I", data, s)[0]
        if data[start : data.index(b"\0", start)] == section.encode():
            return s
    raise SystemExit("no section %s" % section)


def dynsym(data, name):
    table = shdr(data, ".dynsym")
    off, size, link = (struct.unpack_from(f, data, table + o)[0] for f, o in (("<Q", 24), ("<Q", 32), ("<I", 40)))
    strings = struct.unpack_from("<Q", data, shdr(data, link) + 24)[0]
    for e in range(off, off + size, 24):
        start = strings + struct.unpack_from("<I", data, e)[0]
        if data[start : data.index(b"\0", start)] == name.encode():
            return e
    raise SystemExit("no symbol %s" % name)


def add(data, at, n, fmt="<Q"):
    bits = 8 * struct.calcsize(fmt)
    struct.pack_into(fmt, data, at, (struct.unpack_from(fmt, data, at)[0] + n) % 2**bits)


PHDR_FIELDS = {"offset": 8, "vaddr": 16, "filesz": 32, "memsz": 40, "align": 48}
SHDR_FIELDS = {
    "name": (0, "<I"),
    "type": (4, "<I"),
    "flags": (8, "<Q"),
    "addr": (16, "<Q"),
    "offset": (24, "<Q"),
    "size": (32, "<Q"),
    "link": (40, "<I"),
    "info": (44, "<I"),
}
SYM_FIELDS = {"other": (5, "<B"), "shndx": (6, "<H"), "value": (8, "<Q")}


def first(entries, data, tag, fmt="<q"):
    for e in entries:
        if struct.unpack_from(fmt, data, e)[0] == tag:
            return e
    raise SystemExit("nothing with %d" % tag)


def main(argv):
    data = bytearray(open(argv[1], "rb").read())
    args = argv[3:]
    while args:
        op = args.pop(0)
        if op == "replace":
            a, b = args.pop(0).encode(), args.pop(0).encode()
            if len(a) != len(b) or a not in data:
                raise SystemExit("cannot replace %r" % a)
            data[:] = data.replace(a, b)
            continue
        if op in ("phdr-add", "phdr-set", "shdr-add", "shdr-set", "dynsym"):
            what, field, n = args.pop(0), args.pop(0), int(args.pop(0), 0)
            if op == "dynsym":
                at, fmt = SYM_FIELDS[field]
                struct.pack_into(fmt, data, dynsym(data, what) + at, n)
                continue
            if op in ("shdr-add", "shdr-set"):
                at, fmt = SHDR_FIELDS[field]
                at += shdr(data, what)
                if op == "shdr-add":
                    add(data, at, n, fmt)
                else:
                    struct.pack_into(fmt, data, at, n % 2 ** (8 * struct.calcsize(fmt)))
                continue
            at = first(phdrs(data), data, int(what, 0), "<I") + PHDR_FIELDS[field]
            if op == "phdr-add":
                add(data, at, n)
            else:
                struct.pack_into("<Q", data, at, n % 2**64)
            continue
        x, y = int(args.pop(0), 0), int(args.pop(0), 0)
        if op == "byte":
            data[x] = y
        elif op == "u32":
            struct.pack_into("<I", data, x, y)
        elif op == "phdr-type":
            struct.pack_into("<I", data, first(phdrs(data), data, x, "<I"), y)
        elif op == "dyn-tag":
            struct.pack_into("<q", data, first(dynamic(data), data, x), y)
        elif op == "dyn-val":
            struct.pack_into("<Q", data, first(dynamic(data), data, x) + 8, y)
        elif op == "dyn-tag-all":
            entries = [e for e in dynamic(data) if struct.unpack_from("<q", data, e)[0] == x]
            if not entries:
                raise SystemExit("nothing with %d" % x)
            for e in entries:
                struct.pack_into("<q", data, e, y)
        elif op == "dyn-clear":
            e = first(dynamic(data), data, x) + 8
            struct.pack_into("<Q", data, e, struct.unpack_from("<Q", data, e)[0] & ~y)
        else:
            raise SystemExit("unknown change %s" % op)
    open(argv[2], "wb").write(data)


if __name__ == "__main__":
    main(sys.argv)
