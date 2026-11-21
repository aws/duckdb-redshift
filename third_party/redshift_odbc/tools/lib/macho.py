"""The Mach-O rules of macOS verification.

    python3 -I macho.py policy <dylib> <policy-dir> <interface-header>
    python3 -I macho.py thin <dylib> <out-dir>
    python3 -I macho.py exports <dylib>

The library is the universal file the signed package holds, which the lock pins by SHA-256; its own
code signature is not checked again. This tool reads only the forms the locked release uses, and
refuses every other as drift, which a reviewed change adds: a slice that has chained fixups, a
separate export trie, LC_VERSION_MIN_MACOSX, LC_DYLD_INFO (not _ONLY) or a library load command
other than LC_LOAD_DYLIB fails at load-commands, naming the form; a bind with the weak-import flag
fails at namespace (weak-binds in the weak-bind opcodes); an export other than a regular, weak, or
weak thread-local one fails at exports.

policy reads the universal library and checks each slice against the macOS policy in <policy-dir>
(policy/macos/), each failing with its rule's name:
  slices            a universal (fat) file of at most 1 GiB with exactly an x86_64 and an arm64
                    slice, each the whole architecture (no arm64e or x86_64h) in both the universal
                    header and the slice's own Mach-O header, inside the file, aligned (at most
                    2^15), not empty, not overlapping, with only zero padding between them and
                    nothing after the last; each slice a 64-bit Mach-O dynamic library whose at most
                    4096 load commands, each at least 8 bytes and a multiple of 8, fill exactly the
                    size its header gives, and each command whose fields this tool reads at least as
                    long as those fields;
  load-commands     every load command is one this tool recognises, and none is a form it does not
                    read (above);
  namespace         the header's flags name two-level namespaces (MH_TWOLEVEL) and not
                    MH_FORCE_FLAT; the slice has exactly one LC_DYLD_INFO_ONLY, the one command dyld
                    reads its binds from; and every library ordinal the bind and lazy-bind opcodes
                    name is one of the libraries the slice's LC_LOAD_DYLIB commands load (1 to their
                    number), not the library itself (0), the host executable (-1), a flat
                    lookup (-2) or a weak lookup (-3), since a two-level image can still bind single
                    symbols those ways (ld's -undefined dynamic_lookup); a bind whose entry sets no
                    ordinal, which dyld binds to the library itself, fails too. The bind opcodes are
                    read to their first done opcode, where dyld stops, and the lazy-bind opcodes
                    through, one entry after another. So every non-weak bind and lazy bind goes, by
                    ordinal, to one of the slice's own libraries
  weak-binds        the names of the weak-bind opcodes, up to their first done opcode. dyld resolves
                    a weak bind by name across the process's images, the host's included, so each
                    such name, compared exactly as the slice spells it, must be one the slice's
                    export trie holds (which the exports rule pins to expected-exports.txt, so a row
                    there also lets the slice weak-bind the name) or one it also binds by ordinal to
                    one of its libraries. Any other name would be a lookup by name alone, which
                    namespace refuses. A weak-bind entry flagged as the slice's own non-weak
                    definition, which would take the place of other images' weak definitions,
                    fails too; 2.3.0.0 has none. Weak definitions still coalesce by name, as for
                    any C++ library: an accepted residual, which the driver's macOS tests check;
                    and a weak lookup in the bind and lazy-bind opcodes is still refused
                    (namespace).
                    policy prints each slice's count of weak-bound names, each counted once
  linked-libraries  every LC_LOAD_DYLIB names a library in allowed-load-dylibs.txt
  search-path       no LC_RPATH
  min-os            one LC_BUILD_VERSION, for macOS, whose minimum is at most min-os.txt's driver row
  exports           every name of the export trie has a leading underscore, and the names without it
                    equal the slice's rows of expected-exports.txt, both ways, so each row stands
                    for exactly one name of the trie
  entry-points      every name in the header's REDSHIFT_ODBC_ENTRY_POINTS is in expected-exports.txt
                    for both slices
thin writes the two slices, after the slices rule, to <out-dir>/x86_64 and <out-dir>/arm64, which
must not exist yet, for the environment-name rule (lib/env_names.py), which verification runs on
each as its own platform.
exports prints the body of expected-exports.txt for the library, which is how that file is
generated on a driver update: every exported name without its leading underscore, sorted, alone when
both slices export it and followed by its slice when one does. The exports rule compares the same
sets.
Nothing in the file is executed.

On failure it prints one line, "<arch>: <rule>: <reason>" or "<rule>: <reason>", to stderr and
exits 1. Standard library only, Python 3.6 and later.
"""

import os
import re
import struct
import sys

FAT_MAGIC, MH_MAGIC_64, MH_DYLIB = 0xCAFEBABE, 0xFEEDFACF, 6
ARCHS = {0x01000007: ("x86_64", 3), 0x0100000C: ("arm64", 0)}  # cputype: name, the "all" subtype
MAX_FILE = 1 << 30
LOADS = {0xC: "LC_LOAD_DYLIB"}  # the one library load command 2.3.0.0 uses
LC_RPATH, LC_BUILD_VERSION, LC_DYLD_INFO_ONLY = 0x8000001C, 0x32, 0x80000022
# The size each command this tool reads fields of must have at least, so no field is read from
# the next command: dyld_info's 48, build_version's 24 and a dylib command's 24.
MIN_CMDSIZE = {LC_DYLD_INFO_ONLY: 48, LC_BUILD_VERSION: 24}
MIN_CMDSIZE.update((c, 24) for c in LOADS)
# Forms dyld reads that 2.3.0.0 does not use: each fails, naming it, until a reviewed change adds
# its rules (Apple's linker writes chained fixups by default for a macOS 12 minimum).
NOT_READ = {0x80000034: "chained fixups (LC_DYLD_CHAINED_FIXUPS)",
            0x80000033: "a separate export trie (LC_DYLD_EXPORTS_TRIE)",
            0x24: "LC_VERSION_MIN_MACOSX", 0x22: "LC_DYLD_INFO, not LC_DYLD_INFO_ONLY",
            0x80000018: "LC_LOAD_WEAK_DYLIB", 0x8000001F: "LC_REEXPORT_DYLIB", 0x80000023: "LC_LOAD_UPWARD_DYLIB",
            0x20: "LC_LAZY_LOAD_DYLIB"}


class Failure(Exception):
    pass


def u32(data, at, big=False):
    if at < 0 or at + 4 > len(data):
        raise Failure("a field lies outside its structure")
    return struct.unpack(">I" if big else "<I", data[at:at + 4])[0]


class Slice(object):
    """One architecture of the universal file: its bytes and its load commands."""

    def __init__(self, arch, data):
        self.arch, self.data = arch, data
        if len(data) < 32:
            raise Failure("slices: the %s slice is too small for a Mach-O header" % arch)
        magic, cputype, subtype, filetype, ncmds, sizeofcmds, self.flags, _ = struct.unpack("<IiiIIIII", data[:32])
        if magic != MH_MAGIC_64 or filetype != MH_DYLIB:
            raise Failure("slices: the %s slice is not a 64-bit Mach-O dynamic library" % arch)
        name, all_subtype = ARCHS.get(cputype & 0xFFFFFFFF, ("", None))
        if name != arch:
            raise Failure("slices: the %s slice's header names another architecture" % arch)
        # The slice's own header is what dyld runs, so it too must name the whole architecture (the
        # high byte holds capability bits).
        if subtype & 0x00FFFFFF != all_subtype:
            raise Failure("slices: the %s slice's header is the subtype %d, not the whole architecture"
                          % (arch, subtype & 0x00FFFFFF))
        if 32 + sizeofcmds > len(data) or ncmds > 4096:
            raise Failure("slices: the %s slice's load commands run past the slice" % arch)
        self.commands, at = [], 32
        for _ in range(ncmds):
            if at + 8 > 32 + sizeofcmds:
                raise Failure("slices: the %s slice's load commands run past their size" % arch)
            cmd, size = struct.unpack("<II", data[at:at + 8])
            if size < 8 or size % 8 or at + size > 32 + sizeofcmds:
                raise Failure("slices: a %s load command has a size that does not fit" % arch)
            if size < MIN_CMDSIZE.get(cmd, 8):
                raise Failure("slices: a %s load command 0x%x is %d bytes, shorter than its %d bytes of fields"
                              % (arch, cmd, size, MIN_CMDSIZE[cmd]))
            self.commands.append((cmd, at, size))
            at += size
        if at != 32 + sizeofcmds:
            raise Failure("slices: the %s slice's load commands do not fill the size its header gives" % arch)

    def find(self, cmd):
        return [(at, size) for c, at, size in self.commands if c == cmd]


def universal(path):
    """The slices of the universal file at path, as {arch: Slice}."""
    try:
        with open(path, "rb") as f:
            data = f.read(MAX_FILE + 1)
    except OSError as e:
        raise Failure("slices: cannot read %s: %s" % (path, e.strerror))
    if len(data) > MAX_FILE or len(data) < 8 or u32(data, 0, True) != FAT_MAGIC:
        raise Failure("slices: the library is not a universal Mach-O file")
    count = u32(data, 4, True)
    if count != 2:
        raise Failure("slices: the library has %d slices; it must have exactly x86_64 and arm64" % count)
    covered, entries = [(0, 8 + 20 * count)], {}
    for i in range(count):
        cputype, subtype, offset, size, align = struct.unpack(">IIIII", data[8 + 20 * i:28 + 20 * i])
        name, all_subtype = ARCHS.get(cputype, (None, None))
        if name is None or name in entries:
            raise Failure("slices: the library's slices are not exactly x86_64 and arm64")
        if subtype & 0x00FFFFFF != all_subtype:
            raise Failure("slices: the %s slice is the subtype %d, not the whole architecture" % (name, subtype & 0x00FFFFFF))
        if align > 15 or offset % (1 << align) or offset + size > len(data) or size == 0:
            raise Failure("slices: the %s slice lies outside the file or is not aligned" % name)
        covered.append((offset, offset + size))
        entries[name] = (offset, size)
    covered.sort()
    for (_, end), (start, _) in zip(covered, covered[1:]):
        if start < end:
            raise Failure("slices: the library's slices overlap")
    # Nothing outside the header and the slices: only zero padding between them, nothing after.
    gaps = [(end, start) for (_, end), (start, _) in zip(covered, covered[1:])]
    if any(data[s:e].strip(b"\0") for s, e in gaps):
        raise Failure("slices: the library holds bytes other than zero between its slices")
    if covered[-1][1] != len(data):
        raise Failure("slices: the library holds bytes after its last slice")
    return {name: Slice(name, data[entries[name][0]:sum(entries[name])]) for name in ("x86_64", "arm64")}


# Every load command this tool can judge; any other fails, so a new kind is a reviewed change.
RECOGNISED = dict(LOADS, **{
    "0x19": "LC_SEGMENT_64", "0x2": "LC_SYMTAB", "0xb": "LC_DYSYMTAB", "0xd": "LC_ID_DYLIB",
    "0x80000022": "LC_DYLD_INFO_ONLY", "0x1b": "LC_UUID", "0x32": "LC_BUILD_VERSION",
    "0x2a": "LC_SOURCE_VERSION", "0x26": "LC_FUNCTION_STARTS", "0x29": "LC_DATA_IN_CODE", "0x1d": "LC_CODE_SIGNATURE",
    "0x8000001c": "LC_RPATH"})
RECOGNISED = {(int(k, 16) if isinstance(k, str) else k): v for k, v in RECOGNISED.items()}
PLATFORM_MACOS = 1
MH_TWOLEVEL, MH_FORCE_FLAT = 0x80, 0x100
# The export flags 2.3.0.0's tries hold: a regular export, a weak one, and a weak thread-local one.
# Any other (absolute, re-exported from another library, a resolver stub) fails.
EXPORT_FLAGS = (0, 4, 5)
MAX_TRIE_DEPTH = 1024


def rows(path):
    """(tokens, line_no) of each line of a policy file, without its comment."""
    try:
        with open(path, encoding="ascii") as f:
            out = []
            for n, line in enumerate(f, 1):
                tokens = []
                for token in line.split():
                    if token.startswith("#"):
                        break
                    tokens.append(token)
                if tokens:
                    out.append((tokens, n))
            return out
    except (OSError, UnicodeDecodeError) as e:
        raise Failure("policy: cannot read %s: %s" % (path, e))


def version_number(text, where):
    """A version as LC_BUILD_VERSION encodes it: 16 bits of major, 8 of minor, 8 of patch."""
    parts = text.split(".")
    if not 2 <= len(parts) <= 3 or not all(p.isdigit() and len(p) <= 5 for p in parts):
        raise Failure("policy: %s: %r is not a version such as 11.0" % (where, text))
    parts = [int(p) for p in parts] + [0]
    if parts[0] > 0xFFFF or parts[1] > 0xFF or parts[2] > 0xFF:
        raise Failure("policy: %s: %r is not a version such as 11.0 (at most 65535.255.255)" % (where, text))
    return parts[0] << 16 | parts[1] << 8 | parts[2]


def version_text(value):
    return "%d.%d.%d" % (value >> 16, (value >> 8) & 0xFF, value & 0xFF)


def entry_points(header):
    """SQL<name> for each X(<name>) of the header's REDSHIFT_ODBC_ENTRY_POINTS(X)."""
    try:
        with open(header, encoding="utf-8") as f:
            text = f.read()
    except (OSError, UnicodeDecodeError) as e:
        raise Failure("entry-points: cannot read the interface header %s: %s" % (header, e))
    match = re.search(r"#define REDSHIFT_ODBC_ENTRY_POINTS\(X\)((?:[^\n]*\\\n)*[^\n]*)", text)
    names = re.findall(r"X\(([A-Za-z0-9_]+)\)", match.group(1)) if match else []
    if not names:
        raise Failure("entry-points: %s has no REDSHIFT_ODBC_ENTRY_POINTS(X) list" % header)
    return {"SQL" + n for n in names}


def uleb(data, at, end, where="the export trie"):
    value = shift = 0
    while True:
        if at >= end or shift > 63:
            raise Failure("a number in %s runs past it" % where)
        b = data[at]
        at += 1
        value |= (b & 0x7F) << shift
        shift += 7
        if not b & 0x80:
            return value, at


def info_only(s):
    """The slice's one LC_DYLD_INFO_ONLY: its (rebase, bind, weak-bind, lazy-bind, export) (offset,
    size) pairs."""
    commands = s.find(LC_DYLD_INFO_ONLY)
    if len(commands) != 1:
        raise Failure("%s: namespace: the slice has %d LC_DYLD_INFO_ONLY commands; dyld reads its binds from one"
                      % (s.arch, len(commands)))
    at = commands[0][0]
    return [struct.unpack("<II", s.data[at + 8 + 8 * i:at + 16 + 8 * i]) for i in range(5)]


def export_trie(s):
    """The slice's exported symbols as the slice spells them, from its export trie."""
    start, length = info_only(s)[4]
    if not length:
        raise Failure("%s: exports: the slice has no export trie" % s.arch)
    end = start + length
    if end > len(s.data):
        raise Failure("%s: exports: the export trie lies outside the slice" % s.arch)
    names, seen = set(), set()
    pending = [(start, "", 0)]
    try:
        while pending:
            node, prefix, depth = pending.pop()
            if node in seen:
                raise Failure("the export trie loops")
            if depth > MAX_TRIE_DEPTH or not start <= node < end:
                raise Failure("the export trie is deeper than %d or points outside itself" % MAX_TRIE_DEPTH)
            seen.add(node)
            info, at = uleb(s.data, node, end)
            if info:
                flags, _ = uleb(s.data, at, end)
                if flags not in EXPORT_FLAGS:
                    raise Failure("an export has flags this tool does not know (0x%x)" % flags)
                names.add(prefix)
            at += info
            if at >= end:
                raise Failure("a node of the export trie runs past it")
            count = s.data[at]
            at += 1
            for _ in range(count):
                stop = s.data.find(b"\0", at, end)
                if stop < 0:
                    raise Failure("an edge of the export trie has no end")
                label = s.data[at:stop].decode("ascii")
                child, at = uleb(s.data, stop + 1, end)
                pending.append((start + child, prefix + label, depth + 1))
    except (Failure, UnicodeDecodeError, IndexError) as e:
        raise Failure("%s: exports: the export trie is not one this tool reads: %s" % (s.arch, e))
    return names


def exports(s):
    """The slice's exported symbols as expected-exports.txt lists them: without the leading
    underscore, which each must have, so that each row stands for exactly one name of the trie."""
    names = export_trie(s)
    bare = sorted(n for n in names if not n.startswith("_"))
    if bare:
        raise Failure("%s: exports: the slice exports names without a leading underscore, which expected-exports.txt, "
                      "listing names without it, cannot tell from the same names with one: %s"
                      % (s.arch, " ".join(bare[:20]) + (" ..." if len(bare) > 20 else "")))
    return {n[1:] for n in names}


# The library a bind names: n > 0 is the n-th library load command; these are special.
WEAK_LOOKUP = -3
SPECIAL_ORDINALS = {0: "the library itself", -1: "the host executable", -2: "a flat lookup", WEAK_LOOKUP: "a weak lookup"}
# Bind opcodes (the high nibble), by what follows the opcode byte: nothing, one ULEB or SLEB (read
# alike, as only its length matters), two, or a NUL-terminated name. The ordinal opcodes 0x10, 0x20
# and 0x30 are read apart; any other opcode, such as 0xD0 (threaded binds, arm64e), fails. Of these,
# 0x90, 0xA0, 0xB0 and 0xC0 bind, and 0x00 (done) ends the bind and weak-bind streams: dyld
# (ImageLoaderMachOCompressed's eachBind and its weak-bind iterator) and LLVM (MachOBindEntry) stop
# there, so nothing after it is read. The lazy-bind stream holds one entry per lazy pointer, each
# ended by a done opcode. dyld binds a lazy pointer by reading its entry from the entry's start, with
# no library or name set, and when it binds them all at once it reads the stream through, keeping
# the last entry's; this tool reads the whole stream and starts each entry with neither set, and a
# lazy bind before its entry sets a library fails (namespace), so each reading gives a bind the same
# library.
BIND_BARE, BIND_ONE_NUMBER, BIND_TWO_NUMBERS, BIND_NAME = (0x00, 0x50, 0x90, 0xB0), (0x60, 0x70, 0x80, 0xA0), 0xC0, 0x40
BIND_DO = (0x90, 0xA0, 0xB0, 0xC0)
# A weak-bind entry with this name flag says the slice holds a non-weak definition of the name, which
# then takes the place of every image's weak definition of it; it binds nothing. A name with the
# weak-import flag may name a library that does not define it, which dyld then binds to 0; 2.3.0.0
# has none, so it fails.
BIND_SYMBOL_FLAGS_WEAK_IMPORT, BIND_SYMBOL_FLAGS_NON_WEAK_DEFINITION = 0x1, 0x8


def bind_stream(s, name, offset, size):
    """A bind, lazy-bind or weak-bind opcode stream: the library ordinals it sets; its binds, each
    (ordinal, name), the ordinal None when the entry sets none before the bind (dyld
    then binds to the library itself); the names it sets; and those of them flagged as non-weak
    definitions. The weak-bind stream's failures are weak-binds', the others' namespace's."""
    end, at, found, binds, names, strong = offset + size, offset, [], [], set(), set()
    ordinal = symbol = None
    rule = "weak-binds" if name == "weak-bind" else "namespace"
    if end > len(s.data):
        raise Failure("%s: %s: the %s opcodes lie outside the slice" % (s.arch, rule, name))
    where = "the %s opcode stream" % name
    try:
        while at < end:
            op, imm = s.data[at] & 0xF0, s.data[at] & 0x0F
            at += 1
            if op == 0x10:
                ordinal = imm
                found.append(ordinal)
            elif op == 0x20:
                ordinal, at = uleb(s.data, at, end, where)
                found.append(ordinal)
            elif op == 0x30:
                ordinal = imm - 16 if imm else 0  # a negative number in four bits
                found.append(ordinal)
            elif op == BIND_NAME:
                stop = s.data.find(b"\0", at, end)
                if stop < 0:
                    raise Failure("a symbol name runs past the stream")
                symbol = s.data[at:stop]
                if imm & BIND_SYMBOL_FLAGS_WEAK_IMPORT:
                    raise Failure("%s has the weak-import flag, a form 2.3.0.0 does not use; a reviewed change "
                                  "adds it" % listed([symbol]))
                (strong if imm & BIND_SYMBOL_FLAGS_NON_WEAK_DEFINITION else names).add(symbol)
                at = stop + 1
            elif op in BIND_ONE_NUMBER:
                _, at = uleb(s.data, at, end, where)
            elif op == BIND_TWO_NUMBERS:
                _, at = uleb(s.data, at, end, where)
                _, at = uleb(s.data, at, end, where)
            elif op not in BIND_BARE:
                raise Failure("opcode 0x%02x is not one this tool reads" % op)
            if op in BIND_DO:
                binds.append((ordinal, symbol))
            elif op == 0x00:
                if name != "lazy-bind":
                    break
                ordinal = symbol = None
    except Failure as e:
        raise Failure("%s: %s: the %s opcodes are not ones this tool reads: %s" % (s.arch, rule, name, e))
    return found, binds, names, strong


def listed(symbols):
    """Up to 20 names, as the slice spells them, for a failure."""
    shown = [n.decode("ascii", "replace") for n in symbols[:20]]
    return " ".join(shown) + (" ..." if len(symbols) > 20 else "")


def check_namespace(s, loaded):
    """The namespace rule: two-level namespaces, and every bind and lazy bind by an ordinal that
    names one of the slice's libraries. Returns the names it binds by ordinal."""
    if not s.flags & MH_TWOLEVEL or s.flags & MH_FORCE_FLAT:
        raise Failure("%s: namespace: the slice's flags 0x%x do not name two-level namespaces, or force a flat one"
                      % (s.arch, s.flags))
    streams, by_ordinal = info_only(s), set()
    for name, (offset, size) in (("bind", streams[1]), ("lazy-bind", streams[3])):
        found, binds, _, _ = bind_stream(s, name, offset, size)
        if any(ordinal is None for ordinal, _ in binds):
            raise Failure("%s: namespace: a %s opcode binds before its entry names a library, so dyld would "
                          "bind it to the library itself (ordinal 0)" % (s.arch, name))
        for ordinal in found:
            if not 1 <= ordinal <= len(loaded):
                raise Failure("%s: namespace: a %s opcode names %s (ordinal %d), not one of the slice's libraries "
                              "(1 to %d)" % (s.arch, name, SPECIAL_ORDINALS.get(ordinal, "no library"), ordinal,
                                             len(loaded)))
        by_ordinal.update(symbol for _, symbol in binds)
    return by_ordinal


def check_weak_binds(s, by_ordinal):
    """The weak-binds rule: each name bound by weak lookup is, exactly as the slice spells it, one of
    the slice's own exports (from its export trie, which the exports rule then pins to
    expected-exports.txt) or one it also binds by ordinal. Returns the names."""
    offset, size = info_only(s)[2]
    _, _, weak, strong = bind_stream(s, "weak-bind", offset, size)
    if strong:
        raise Failure("%s: weak-binds: the weak-bind opcodes flag names as the slice's own non-weak definitions, which "
                      "would take the place of every other image's weak definition of them: %s"
                      % (s.arch, listed(sorted(strong))))
    exported = {n.encode("ascii") for n in export_trie(s)}
    unbounded = sorted(n for n in weak if n not in exported and n not in by_ordinal)
    if unbounded:
        raise Failure("%s: weak-binds: the slice binds names by weak lookup that it neither exports nor binds by "
                      "ordinal to one of its libraries, so dyld would find them by name alone: %s"
                      % (s.arch, listed(unbounded)))
    return weak


def library_name(s, at, size):
    offset = u32(s.data, at + 8)
    stop = s.data.find(b"\0", at + offset, at + size)
    if offset < 24 or offset >= size or stop < 0:
        raise Failure("%s: linked-libraries: a load command's library name lies outside it" % s.arch)
    return s.data[at + offset:stop].decode("utf-8", "replace")


def read_policy(policy_dir, header):
    """policy/macos/: the allowed libraries, the min-os.txt floors and each slice's expected exports,
    which must hold the interface's entry points."""
    allowed = {t[0] for t, _ in rows(os.path.join(policy_dir, "allowed-load-dylibs.txt"))}
    floors = {}
    for tokens, n in rows(os.path.join(policy_dir, "min-os.txt")):
        if len(tokens) != 2 or tokens[0] not in ("duckdb", "driver") or tokens[0] in floors:
            raise Failure("policy: min-os.txt:%d: the file has one duckdb row and one driver row, each "
                          "<name> <version>" % n)
        floors[tokens[0]] = version_number(tokens[1], "min-os.txt:%d" % n)
    if set(floors) != {"duckdb", "driver"} or floors["driver"] < floors["duckdb"]:
        raise Failure("policy: min-os.txt needs a duckdb and a driver row, the driver row not below duckdb's")
    expected = {"x86_64": set(), "arm64": set()}
    for tokens, n in rows(os.path.join(policy_dir, "expected-exports.txt")):
        if len(tokens) > 2 or (len(tokens) == 2 and tokens[1] not in expected):
            raise Failure("policy: expected-exports.txt:%d: a row is <name> [x86_64|arm64]" % n)
        for arch in tokens[1:] or list(expected):
            expected[arch].add(tokens[0])
    missing = sorted(entry_points(header) - (expected["x86_64"] & expected["arm64"]))
    if missing:
        raise Failure("entry-points: the interface's entry points are not all in expected-exports.txt for both "
                      "slices; missing: %s" % " ".join(missing))
    return allowed, floors, expected


def check_slice(s, allowed, floors, expected):
    """One slice against the policy; returns (its minimum macOS, the number of names it binds by
    weak lookup)."""
    arch = s.arch
    for cmd, at, size in s.commands:
        if cmd in NOT_READ:
            raise Failure("%s: load-commands: the slice uses %s, which this tool does not read; a reviewed change "
                          "adds it" % (arch, NOT_READ[cmd]))
        if cmd not in RECOGNISED:
            raise Failure("%s: load-commands: the slice has a load command this tool does not recognise (0x%x)"
                          % (arch, cmd))
    loaded = [library_name(s, at, size) for cmd, at, size in s.commands if cmd in LOADS]
    weak = len(check_weak_binds(s, check_namespace(s, loaded)))
    for cmd, at, size in s.commands:
        if cmd in LOADS and library_name(s, at, size) not in allowed:
            raise Failure("%s: linked-libraries: %s loads %s, which is not in allowed-load-dylibs.txt"
                          % (arch, LOADS[cmd], library_name(s, at, size)))
        if cmd == LC_RPATH:
            raise Failure("%s: search-path: the slice has an LC_RPATH" % arch)
    versions = [(u32(s.data, at + 8), u32(s.data, at + 12)) for at, _ in s.find(LC_BUILD_VERSION)]
    if len(versions) != 1 or versions[0][0] != PLATFORM_MACOS:
        raise Failure("%s: min-os: the slice does not name exactly one minimum macOS version" % arch)
    if versions[0][1] > floors["driver"]:
        raise Failure("%s: min-os: the slice needs macOS %s, newer than %s in min-os.txt"
                      % (arch, version_text(versions[0][1]), version_text(floors["driver"])))
    got = exports(s)
    extra, absent = sorted(got - expected[arch]), sorted(expected[arch] - got)
    if extra or absent:
        raise Failure("%s: exports: the slice's exports differ from expected-exports.txt; extra: %s; missing: %s"
                      " (macho.py exports prints the whole list)"
                      % (arch, " ".join(extra[:20]) + (" ..." if len(extra) > 20 else ""),
                         " ".join(absent[:20]) + (" ..." if len(absent) > 20 else "")))
    return versions[0][1], weak


def policy(path, policy_dir, header):
    slices = universal(path)
    allowed, floors, expected = read_policy(policy_dir, header)
    found = {arch: check_slice(slices[arch], allowed, floors, expected) for arch in ("x86_64", "arm64")}
    newest = max(v for v, _ in found.values())
    weak = {arch: w for arch, (_, w) in found.items()}
    note = ""
    if newest > floors["duckdb"]:
        note = ", newer than DuckDB's %s, as min-os.txt accepts" % version_text(floors["duckdb"])
    residual = ""
    if any(weak.values()):
        residual = ("; by weak lookup, which dyld resolves by name across the process, it binds %s name%s "
                    "in x86_64 and %s in arm64" % (format(weak["x86_64"], ","), "" if weak["x86_64"] == 1 else "s",
                                                   format(weak["arm64"], ",")))
    print("the library fits the macOS policy (minimum macOS %s%s)%s" % (version_text(newest), note, residual))


def thin(path, out_dir):
    for arch, s in sorted(universal(path).items()):
        with open(os.path.join(out_dir, arch), "xb") as f:
            f.write(s.data)


def list_exports(path):
    """expected-exports.txt's body for the library: names both slices export alone, others with
    their slice, sorted."""
    slices = universal(path)
    names = {arch: exports(slices[arch]) for arch in ("x86_64", "arm64")}
    for name in sorted(names["x86_64"] | names["arm64"]):
        only = [arch for arch in ("x86_64", "arm64") if name not in names[arch]]
        print(name if not only else "%s  %s" % (name, "arm64" if only == ["x86_64"] else "x86_64"))


# command: (number of arguments after it, the function)
COMMANDS = {"policy": (3, policy), "thin": (2, thin), "exports": (1, list_exports)}


def main(argv):
    command = COMMANDS.get(argv[1] if len(argv) > 1 else "")
    if not command or len(argv) - 2 != command[0]:
        sys.stderr.write("usage: macho.py policy <dylib> <policy-dir> <interface-header>\n"
                         "       macho.py thin <dylib> <out-dir>\n"
                         "       macho.py exports <dylib>\n")
        return 2
    try:
        command[1](*argv[2:])
    except Failure as e:
        sys.stderr.write("%s\n" % e)
        return 1
    except struct.error as e:
        sys.stderr.write("slices: the library is not one this tool reads: %s\n" % e)
        return 1
    except (OSError, UnicodeDecodeError) as e:
        sys.stderr.write("cannot read or write a file: %s\n" % e)
        return 1
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv))
