"""Which of the driver's weak-bound names a process bound outside the driver, and to which image
(macos-hosts.sh's test of what weak-definition coalescing reaches).

    python3 -I macos_bindings.py slots <driver> <arm64|x86_64>
    python3 -I macos_bindings.py summary <driver> <arm64|x86_64> <reached> [<name>...]

slots prints, for each entry of the slice's weak-bind opcodes (to their first done opcode, as dyld reads them),
the slot's offset from the image's header, in hex, and the name, one `<offset> <name>` line each. The slice and its
load commands are read with the macOS verification's own reader (third_party/redshift_odbc/tools/lib/macho.py);
the opcodes are walked here, as that reader's bind_stream, which verify-release.sh's macOS checks run, keeps each
bind's name and library but not where it binds. inputs/weak_slots.c reads those slots in a process that has loaded
the driver, when it exits, and writes `<name> <image>` for each: where dyld's coalescing left that bind. dyld's
DYLD_PRINT_BINDINGS does not show this: it logs a weak bind before coalescing moves it.

summary reads that file and prints one line, how many of the slice's weak-bound names it has and how many went
outside the driver, to which image; then, for each name given, `<name> -> <image>`. Exits 1, saying so, when the
file names none of them (a host that ignored DYLD_INSERT_LIBRARIES, being hardened without
allow-dyld-environment-variables), 2 on a usage error.
"""

import os
import struct
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
sys.dont_write_bytecode = True  # no __pycache__ in third_party/redshift_odbc/tools/lib
sys.path.insert(0, os.path.join(HERE, "..", "..", "..", "third_party", "redshift_odbc", "tools", "lib"))
import macho  # noqa: E402

LC_SEGMENT_64 = 0x19


def segments(s):
    """Each LC_SEGMENT_64's vmaddr, in load-command order (a bind's segment index), and __TEXT's, from the load
    commands macho.Slice has read and bounded."""
    out, text = [], 0
    for at, size in s.find(LC_SEGMENT_64):
        if size < 72:
            raise macho.Failure("an LC_SEGMENT_64 is %d bytes, shorter than its fields" % size)
        name, vmaddr = struct.unpack_from("<16sQ", s.data, at + 8)
        out.append(vmaddr)
        if name.rstrip(b"\0") == b"__TEXT":
            text = vmaddr
    return out, text


def weak_slots(path, arch):
    """(offset from the header, name) for each weak bind of the slice."""
    s = macho.universal(path)[arch]
    vmaddrs, text = segments(s)
    offset, size = macho.info_only(s)[2]
    data, at, end = s.data, offset, offset + size
    if end > len(data):
        raise macho.Failure("the weak-bind opcodes lie outside the slice")
    where = "the weak-bind opcode stream"
    out, state = [], {"name": None, "segment": 0, "address": 0}
    mask = (1 << 64) - 1

    def bind(step):
        if state["segment"] >= len(vmaddrs):
            raise macho.Failure("a weak bind names segment %d of %d" % (state["segment"], len(vmaddrs)))
        out.append(((vmaddrs[state["segment"]] + state["address"] - text) & mask, state["name"]))
        state["address"] = (state["address"] + 8 + step) & mask

    while at < end:
        op, imm = data[at] & 0xF0, data[at] & 0x0F
        at += 1
        if op == 0x00:
            break
        if op == 0x40:
            stop = data.find(b"\0", at, end)
            if stop < 0:
                raise macho.Failure("a symbol name runs past the weak-bind stream")
            state["name"], at = data[at:stop].decode("ascii"), stop + 1
        elif op in (0x10, 0x30, 0x50):
            # A library ordinal or special ordinal, or a bind type, in the immediate: no operand, and no slot moves.
            pass
        elif op in (0x20, 0x60):
            # A library ordinal (ULEB) or an addend (SLEB), read alike, as only its length matters.
            _, at = macho.uleb(data, at, end, where)
        elif op == 0x70:
            state["segment"] = imm
            state["address"], at = macho.uleb(data, at, end, where)
        elif op == 0x80:
            step, at = macho.uleb(data, at, end, where)
            state["address"] = (state["address"] + step) & mask
        elif op == 0x90:
            bind(0)
        elif op == 0xA0:
            step, at = macho.uleb(data, at, end, where)
            bind(step)
        elif op == 0xB0:
            bind(imm * 8)
        elif op == 0xC0:
            count, at = macho.uleb(data, at, end, where)
            skip, at = macho.uleb(data, at, end, where)
            for _ in range(count):
                bind(skip)
        else:
            raise macho.Failure("weak-bind opcode 0x%02x is not one this tool reads" % op)
    return out


def summary(driver, arch, reached_file, wanted):
    leaf = os.path.basename(driver)
    weak = {name for _, name in weak_slots(driver, arch)}
    reached = {}
    if os.path.exists(reached_file):
        for line in open(reached_file, encoding="utf-8", errors="replace"):
            fields = line.split()
            if len(fields) == 2 and fields[0] in weak:
                reached.setdefault(fields[0], set()).add(fields[1])
    if not reached:
        print("no weak-bound name of the driver's %d was read: the host did not load the reader" % len(weak))
        return 1
    outside = {}
    for name, images in reached.items():
        for image in images - {leaf}:
            outside.setdefault(image, []).append(name)
    parts = ["%s %d" % (image, len(names)) for image, names in sorted(outside.items())]
    print(
        "%d weak-bound names, %d read; outside the driver: %s"
        % (len(weak), len(reached), ", ".join(parts) if parts else "none")
    )
    # Type information (__ZTI) and type names (__ZTS) are read-only data a type's identity rests on, which every
    # image's copy coalescing joins as one; every other name is listed whole.
    for image, names in sorted(outside.items()):
        rtti = [name for name in names if name.startswith(("__ZTI", "__ZTS"))]
        rest = sorted(set(names) - set(rtti))
        print("  %s: %d type information or type names%s" % (image, len(rtti), "; " + " ".join(rest) if rest else ""))
    for name in wanted:
        print("%s -> %s" % (name, " ".join(sorted(reached.get(name, {"not read"})))))
    return 0


def main(argv):
    try:
        if len(argv) == 4 and argv[1] == "slots" and argv[3] in ("arm64", "x86_64"):
            for offset, name in weak_slots(argv[2], argv[3]):
                print("%x %s" % (offset, name))
            return 0
        if len(argv) >= 5 and argv[1] == "summary" and argv[3] in ("arm64", "x86_64"):
            return summary(argv[2], argv[3], argv[4], argv[5:])
    except macho.Failure as e:
        print("macos_bindings.py: %s" % e)
        return 1
    sys.stderr.write(
        "usage: macos_bindings.py slots <driver> <arm64|x86_64>\n"
        "       macos_bindings.py summary <driver> <arm64|x86_64> <reached> [<name>...]\n"
    )
    return 2


if __name__ == "__main__":
    sys.exit(main(sys.argv))
