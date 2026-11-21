"""Writes a tiny universal Mach-O dynamic library for the tool tests, from byte templates: no
compiler and no checked-in binary.

    python3 -I make_macho.py <out.dylib> '<json spec>'

Each slice (x86_64 and arm64) is a header, its load commands and a __TEXT segment holding
__cstring, then a __LINKEDIT segment holding the export trie and the bind streams. Every key of the
spec is optional:

  archs     the slices, in order (default ["x86_64", "arm64"])
  exports   the exported symbols, without the leading underscore (default: none)
  arm64_exports
            the arm64 slice's exported symbols instead
  strings   C strings in __TEXT,__cstring (default: none)
  arm64_strings
            the arm64 slice's C strings instead
  minos     LC_BUILD_VERSION's minimum macOS (default "11.0")
  version_command
            "build" (one LC_BUILD_VERSION, the default) or "two" (two LC_BUILD_VERSION)
  dylibs    [[command, path], ...]: libraries to load, command one of LC_LOAD_DYLIB,
            LC_LOAD_WEAK_DYLIB, LC_REEXPORT_DYLIB, LC_LOAD_UPWARD_DYLIB, LC_LAZY_LOAD_DYLIB
            (default [["LC_LOAD_DYLIB", "/usr/lib/libSystem.B.dylib"]])
  rpath     adds an LC_RPATH with this path
  command   adds a load command with this number and no payload (one the tools refuse, such as
            LC_DYLD_CHAINED_FIXUPS, is written so)
  trie      "info" (the export trie and binds in LC_DYLD_INFO_ONLY, the default), "old-info" (in
            LC_DYLD_INFO) or "none" (no LC_DYLD_INFO(_ONLY) at all)
  trie_hex  the export trie's bytes, in hex, instead of the one built from exports
  trie_size the export trie's size in its load command (default: its length)
  trie_loop the root's first edge leads back to the root
  trie_out  the root's first edge leads past the end of the trie
  export_flags  the flags of every export (default 0)
  bind_hex, lazy_bind_hex, weak_bind_hex
            the bind, lazy-bind and weak-bind opcode streams, in hex, in __LINKEDIT after the
            export trie and named by LC_DYLD_INFO(_ONLY) (default: none); "bind_size" and
            "weak_bind_size" set the bind and weak-bind streams' sizes in the command instead
  second_info
            true: the weak-bind stream is named instead by a second LC_DYLD_INFO_ONLY, which holds
            nothing else
Standard library only, Python 3.6 and later.
"""

import json
import os
import struct
import sys

CPU = {"x86_64": (0x01000007, 3), "arm64": (0x0100000C, 0)}
DYLIB_COMMANDS = {
    "LC_LOAD_DYLIB": 0xC,
    "LC_LOAD_WEAK_DYLIB": 0x80000018,
    "LC_REEXPORT_DYLIB": 0x8000001F,
    "LC_LOAD_UPWARD_DYLIB": 0x80000023,
    "LC_LAZY_LOAD_DYLIB": 0x20,
}
TEXT_SIZE = 0x4000
CSTRING_AT = 0x1000


def uleb(value):
    out = bytearray()
    while True:
        byte = value & 0x7F
        value >>= 7
        out.append(byte | (0x80 if value else 0))
        if not value:
            return bytes(out)


def trie(symbols, flags=0, loop=False, out=False):
    """An export trie: a radix tree over the names, each a regular export at address 0x1000."""

    def build(names, prefix):
        node = {"terminal": prefix in names, "edges": []}
        rest = sorted(n[len(prefix) :] for n in names if n.startswith(prefix) and n != prefix)
        while rest:
            first = rest[0][0]
            group = [r for r in rest if r[0] == first]
            common = os.path.commonprefix(group)
            node["edges"].append((common, build(names, prefix + common)))
            rest = [r for r in rest if r[0] != first]
        return node

    root = build(set(symbols), "")
    nodes = []

    def flatten(node):
        nodes.append(node)
        for _, child in node["edges"]:
            flatten(child)

    flatten(root)
    offsets = [0] * len(nodes)
    index = {id(node): i for i, node in enumerate(nodes)}
    for _ in range(8):  # child offsets are ULEB128, so lay out until the sizes settle
        blobs, at = [], 0
        for i, node in enumerate(nodes):
            offsets[i] = at
            info = uleb(flags) + uleb(0x1000) if node["terminal"] else b""
            body = uleb(len(info)) + info + bytes([len(node["edges"])])
            for e, (label, child) in enumerate(node["edges"]):
                target = offsets[index[id(child)]]
                if i == 0 and e == 0 and (loop or out):
                    target = 0 if loop else 0x3FFF
                body += label.encode() + b"\0" + uleb(target)
            blobs.append(body)
            at += len(body)
    return b"".join(blobs)


def command(cmd, payload):
    size = (8 + len(payload) + 7) // 8 * 8
    return struct.pack("<II", cmd, size) + payload + b"\0" * (size - 8 - len(payload))


def dylib(cmd, path):
    return command(cmd, struct.pack("<IIII", 24, 2, 0x10000, 0x10000) + path.encode() + b"\0")


def version(text):
    parts = [int(p) for p in text.split(".")] + [0, 0]
    return parts[0] << 16 | parts[1] << 8 | parts[2]


def segment(name, vmaddr, vmsize, fileoff, filesize, sections=()):
    payload = struct.pack("<16sQQQQiiII", name.encode(), vmaddr, vmsize, fileoff, filesize, 5, 5, len(sections), 0)
    for sect, seg, addr, size, offset in sections:
        payload += struct.pack(
            "<16s16sQQIIIIIIII", sect.encode(), seg.encode(), addr, size, offset, 0, 0, 0, 0, 0, 0, 0
        )
    return command(0x19, payload)


def slice_bytes(arch, spec):
    cputype, subtype = CPU[arch]
    names = spec.get("arm64_exports") if arch == "arm64" and "arm64_exports" in spec else spec.get("exports")
    exports = (
        trie(
            ["_" + e for e in names],
            spec.get("export_flags", 0),
            spec.get("trie_loop", False),
            spec.get("trie_out", False),
        )
        if names
        else b"\0\0"
    )
    if "trie_hex" in spec:
        exports = bytes.fromhex(spec["trie_hex"])
    trie_length = spec.get("trie_size", len(exports))
    form = spec.get("trie", "info")
    strings = spec["arm64_strings"] if arch == "arm64" and "arm64_strings" in spec else spec.get("strings", [])
    cstrings = b"".join(s.encode() + b"\0" for s in strings)
    sections = [("__cstring", "__TEXT", CSTRING_AT, len(cstrings), CSTRING_AT)]
    linkedit_at = TEXT_SIZE
    trie_size = (len(exports) + 7) // 8 * 8
    # Bind streams follow the trie, each 8-aligned: (offset, bytes) by name.
    tables, at = {}, linkedit_at + trie_size
    pieces = [(name, bytes.fromhex(spec.get(name + "_hex", ""))) for name in ("bind", "lazy_bind", "weak_bind")]
    for name, blob in pieces:
        tables[name] = (at if blob else 0, blob)
        at += (len(blob) + 7) // 8 * 8
    linkedit_size = at - linkedit_at
    cmds = [
        segment("__TEXT", 0, TEXT_SIZE, 0, TEXT_SIZE, sections),
        segment("__LINKEDIT", TEXT_SIZE, linkedit_size, linkedit_at, linkedit_size),
        dylib(0xD, "@rpath/librsodbc64.dylib"),
    ]
    weak_bind = (tables["weak_bind"][0], spec.get("weak_bind_size", len(tables["weak_bind"][1])))
    second = spec.get("second_info")
    if form in ("info", "old-info"):
        cmds.append(
            command(
                0x22 if form == "old-info" else 0x80000022,
                struct.pack(
                    "<10I",
                    0,
                    0,
                    tables["bind"][0],
                    spec.get("bind_size", len(tables["bind"][1])),
                    *((0, 0) if second else weak_bind),
                    tables["lazy_bind"][0],
                    len(tables["lazy_bind"][1]),
                    linkedit_at,
                    trie_length,
                ),
            )
        )
    if second:
        cmds.append(command(0x80000022, struct.pack("<10I", 0, 0, 0, 0, *weak_bind, 0, 0, 0, 0)))
    minos = version(spec.get("minos", "11.0"))
    for _ in range(2 if spec.get("version_command") == "two" else 1):
        cmds.append(command(0x32, struct.pack("<IIII", 1, minos, version("15.0"), 0)))
    for name, path in spec.get("dylibs", [["LC_LOAD_DYLIB", "/usr/lib/libSystem.B.dylib"]]):
        cmds.append(dylib(DYLIB_COMMANDS[name], path))
    if "rpath" in spec:
        cmds.append(command(0x8000001C, struct.pack("<I", 12) + spec["rpath"].encode() + b"\0"))
    if "command" in spec:
        cmds.append(command(int(spec["command"]), b""))
    body = b"".join(cmds)
    header = struct.pack("<IiiIIIII", 0xFEEDFACF, cputype, subtype, 6, len(cmds), len(body), 0x100085, 0)
    data = bytearray(header + body)
    if len(data) > CSTRING_AT:
        raise SystemExit("make_macho.py: the load commands do not fit before __cstring")
    data += b"\0" * (CSTRING_AT - len(data)) + cstrings
    data += b"\0" * (TEXT_SIZE - len(data)) + exports + b"\0" * (trie_size - len(exports))
    for name, _ in pieces:
        offset, blob = tables[name]
        if blob:
            data += b"\0" * (offset - len(data)) + blob
    data += b"\0" * (linkedit_at + linkedit_size - len(data))
    return bytes(data)


def main(argv):
    if len(argv) != 3:
        sys.stderr.write("usage: make_macho.py <out.dylib> '<json spec>'\n")
        return 2
    spec = json.loads(argv[2])
    archs = spec.get("archs", ["x86_64", "arm64"])
    slices = [slice_bytes(a, spec) for a in archs]
    header = struct.pack(">II", 0xCAFEBABE, len(archs))
    at, layout = 0x4000, []
    for arch, data in zip(archs, slices):
        layout.append((arch, at, len(data)))
        header += struct.pack(">IIIII", CPU[arch][0], CPU[arch][1], at, len(data), 14)
        at = (at + len(data) + 0x3FFF) // 0x4000 * 0x4000
    out = bytearray(header + b"\0" * (0x4000 - len(header)))
    for (arch, offset, size), data in zip(layout, slices):
        out += b"\0" * (offset - len(out)) + data
    with open(argv[1], "wb") as f:
        f.write(out)
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv))
