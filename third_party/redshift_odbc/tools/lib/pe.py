"""Reads the resource directory of a PE32+ x86-64 image, where a Windows build would embed the
driver set as RCDATA resources.

    python3 -I pe.py rcdata <image>

rcdata prints one line per RCDATA resource (type 10), "<name> <language> <size>", in the order the
directory lists them: the name is its number, or its UTF-16 string as a JSON string (in double
quotes, with anything outside printable ASCII escaped and a lone surrogate as U+FFFD), and the
language its number. It prints nothing for an image with no resource directory or no RCDATA
resource, which is what a bundle that embeds no driver set has. Resources of other types (a
manifest, a version) are read past and not printed.

This reader reads only a PE32+ image for x86-64, the form every windows_amd64 file takes, and
refuses any other, naming it: a file that is not a PE image, a PE32 (32-bit) image, or one for
another machine. Every structure it reads must lie inside the file. Those are the root directory,
each type's name directory and, below RCDATA, each name's language directory and data entry, each
inside the resource directory that the image's data directory names, and each RCDATA resource's
data, inside a section. A numeric ID (a type, a name or a language) with any of its reserved bits
(16 to 30) set is refused. RCDATA has three levels (type, name, language), no more, and the
directories it reads hold at most 65,536 entries in all. A file over 1 GiB is refused from its size,
unread. The bytes that decoded names take are not summed, though many names can share one string:
the input is our own build's output. Nothing in the file is executed.

On failure it prints one line, "<rule>: <reason>", to stderr and exits 1. Standard library only,
Python 3.6 and later.

The rename tools (rename_pin.py, rename_check.py) import it for the rest of what they read: the
import, delay-import and export directories, the load configuration's DependentLoadFlags, the header
checksum, the places a DLL's own names are expected, which they skip (names_allowed: the export
name, the version resource, the certificate table, the PDB path and the imported functions'
hint/name entries), and the whole NUL-terminated ASCII and UTF-16LE strings an image holds
(whole_strings). Every table and name lies inside its section's bytes in the file. A DLL name or
forwarder is at most 256 bytes, and an exported or imported function's name at most 4,096, as a C++
decorated name can be longer. The Windows loading policy (windows_policy.py) also reads the exported
names (export_names), the imported functions' hint/name entries (hint_names), the CodeView record
that holds the PDB path (pdb_ranges) and the version resource's strings, OriginalFilename among them
(version_strings). A delay-import descriptor that uses virtual addresses rather than relative ones,
a CodeView record that is not RSDS or is over 4,121 bytes, and a version resource that is not a
VS_VERSIONINFO, are refused by name.
"""

import collections
import json
import os
import re
import struct
import sys

MAX_FILE = 1 << 30
MAX_ENTRIES = 1 << 16
MACHINE_AMD64, MAGIC_PE32_PLUS, MAGIC_PE32 = 0x8664, 0x20B, 0x10B
RT_RCDATA, RT_VERSION = 10, 16
# The data directories' indexes: the export, import, resource and certificate tables, the load
# configuration, bound imports and delay imports.
EXPORT, IMPORT, RESOURCE_DIRECTORY, SECURITY, LOAD_CONFIG, BOUND_IMPORT, DELAY_IMPORT = 0, 1, 2, 4, 10, 11, 13
DEBUG, CLR = 6, 14  # the debug directory and the .NET runtime header
CHECKSUM_AT, RVA_COUNT_AT, DATA_DIRECTORIES_AT = 64, 108, 112  # offsets in the PE32+ optional header
HIGH_BIT = 0x80000000
# A numeric ID is 16 bits; the bits between it and HIGH_BIT are reserved, so a set one is a form
# this reader does not read.
RESERVED_ID_BITS = 0x7FFF0000
MAX_NAME = 256
MAX_EXPORT_NAME = 4096  # a C++ function's decorated name can be longer than a DLL's
DEPENDENT_LOAD_FLAGS_AT = 78  # in IMAGE_LOAD_CONFIG_DIRECTORY64
ORDINAL_FLAG = 1 << 63  # an import thunk naming its function by ordinal, with no hint/name entry
CODEVIEW, DEBUG_ENTRY = 2, 28  # the debug type of the PDB path's record, and a debug entry's size
MAX_CODEVIEW = 24 + MAX_EXPORT_NAME + 1  # RSDS, a GUID and an age, then the path and its NUL
# A section header: its file offset, its VirtualSize field's being header + 8.
Section = collections.namedtuple("Section", "header address virtual_size raw_size raw_at")
Import = collections.namedtuple("Import", "descriptor name_at name")


class Failure(Exception):
    pass


def unpack(data, fmt, at, what):
    size = struct.calcsize(fmt)
    if at < 0 or at + size > len(data):
        raise Failure("image: %s lies outside the file" % what)
    return struct.unpack_from(fmt, data, at)


class Image(object):
    """A PE32+ x86-64 image: its bytes, its sections and its resource directory's place."""

    def __init__(self, data):
        self.data = data
        if data[:2] != b"MZ":
            raise Failure("image: not a PE image (no MZ header)")
        (pe,) = unpack(data, "<I", 0x3C, "the PE header's offset")
        if data[pe:pe + 4] != b"PE\0\0":
            raise Failure("image: not a PE image (no PE signature at the offset the MZ header gives)")
        machine, sections, _, _, _, optional_size, _ = unpack(data, "<HHIIIHH", pe + 4, "the COFF header")
        optional = pe + 24
        (magic,) = unpack(data, "<H", optional, "the optional header")
        if magic == MAGIC_PE32:
            raise Failure("image: a PE32 (32-bit) image, not PE32+")
        if magic != MAGIC_PE32_PLUS:
            raise Failure("image: the optional header's magic is 0x%x, not PE32+ (0x20b)" % magic)
        if machine != MACHINE_AMD64:
            raise Failure("image: built for machine 0x%x, not x86-64 (0x8664)" % machine)
        (count,) = unpack(data, "<I", optional + RVA_COUNT_AT, "the number of data directories")
        self.checksum_at = optional + CHECKSUM_AT
        (self.checksum,) = unpack(data, "<I", self.checksum_at, "the optional header")
        self.directories = []
        for i in range(min(count, 16)):
            if optional_size < DATA_DIRECTORIES_AT + 8 * (i + 1):
                break
            self.directories.append(unpack(data, "<II", optional + DATA_DIRECTORIES_AT + 8 * i, "the data directories"))
        self.resource_directory = self.directory(RESOURCE_DIRECTORY)
        self.sections = []
        for i in range(sections):
            header = optional + optional_size + 40 * i
            _, virtual_size, address, raw_size, raw_at = unpack(data, "<8sIIII", header, "the section table")
            self.sections.append(Section(header, address, virtual_size, raw_size, raw_at))

    def directory(self, index):
        """(address, size) of the data directory at `index`, (0, 0) when the image has none."""
        return self.directories[index] if index < len(self.directories) else (0, 0)

    def offset(self, address, size, what, rule="resources"):
        """The file offset of `size` bytes at the relative virtual address `address`."""
        for section in self.sections:
            start, raw_size = section.address, section.raw_size
            if start <= address and address + size <= start + max(section.virtual_size, raw_size):
                if address + size > start + raw_size or section.raw_at + raw_size > len(self.data):
                    raise Failure("%s: %s lies outside the bytes its section holds in the file" % (rule, what))
                return section.raw_at + address - start
        raise Failure("%s: %s lies outside every section" % (rule, what))

    def name_at(self, address, what, limit=MAX_NAME, rule="imports"):
        """(file offset, bytes) of the NUL-terminated name at `address`, at most `limit` bytes; a
        name it cannot read fails under `rule`."""
        at = self.offset(address, 1, what, rule)
        section = next(s for s in self.sections if s.raw_at <= at < s.raw_at + s.raw_size)
        end = self.data.find(b"\0", at, min(at + limit + 1, section.raw_at + section.raw_size))
        if end < 0:
            raise Failure("%s: %s has no NUL within %d bytes in its section" % (rule, what, limit))
        return at, self.data[at:end]

    def table(self, index, width, what):
        """The file offsets of a zero-terminated array of `width`-byte entries at directory `index`."""
        address, _ = self.directory(index)
        found = []
        while address:
            at = self.offset(address + width * len(found), width, what, "imports")
            if not any(self.data[at:at + width]):
                return found
            found.append(at)
        return found


def rcdata(image):
    """(name, language, size, file offset) of each RCDATA resource of the image."""
    return resources(image, RT_RCDATA)


def resources(image, kind):
    """(name, language, size, file offset) of each resource of type `kind`."""
    address, size = image.resource_directory
    if address == 0:
        return []
    base = image.offset(address, size, "the resource directory")
    end = base + size
    seen = 0

    def entries(at, what):
        """The (name, target) entries of the directory at `at`, from the resource directory's start."""
        if at < 0 or base + at + 16 > end:
            raise Failure("resources: %s lies outside the resource directory" % what)
        nonlocal seen
        named, numbered = struct.unpack_from("<HH", image.data, base + at + 12)
        seen += named + numbered
        if seen > MAX_ENTRIES:
            raise Failure("resources: the directory has more than %d entries" % MAX_ENTRIES)
        first = base + at + 16
        if first + 8 * (named + numbered) > end:
            raise Failure("resources: %s's entries run past the resource directory" % what)
        return [struct.unpack_from("<II", image.data, first + 8 * i) for i in range(named + numbered)]

    def numeric(field):
        if not field & HIGH_BIT and field & RESERVED_ID_BITS:
            raise Failure("resources: a numeric ID has reserved bits set")

    def name(field):
        numeric(field)
        if not field & HIGH_BIT:
            return str(field)
        at = field & ~HIGH_BIT
        if base + at + 2 > end:
            raise Failure("resources: a name lies outside the resource directory")
        (length,) = struct.unpack_from("<H", image.data, base + at)
        if base + at + 2 + 2 * length > end:
            raise Failure("resources: a name runs past the resource directory")
        text = image.data[base + at + 2:base + at + 2 + 2 * length].decode("utf-16-le", "replace")
        return json.dumps(text)  # quoted, with anything outside printable ASCII escaped

    def subdirectory(target, what):
        if not target & HIGH_BIT:
            raise Failure("resources: %s is a data entry, where the directory needs a level below it" % what)
        return target & ~HIGH_BIT

    kind_name = {RT_RCDATA: "RCDATA", RT_VERSION: "VERSION"}[kind]
    found = []
    for type_field, type_target in entries(0, "the root directory"):
        numeric(type_field)
        names = entries(subdirectory(type_target, "a type"), "a type's directory")
        if type_field != kind:
            continue
        for name_field, name_target in names:
            label = name(name_field)
            for language, data_entry in entries(subdirectory(name_target, "a name"), "a name's directory"):
                numeric(language)
                if data_entry & HIGH_BIT:
                    raise Failure("resources: %s %s has a fourth directory level" % (kind_name, label))
                if base + data_entry + 16 > end:
                    raise Failure("resources: %s %s's data entry lies outside the resource directory"
                                  % (kind_name, label))
                data_address, data_size = struct.unpack_from("<II", image.data, base + data_entry)
                at = image.offset(data_address, data_size, "%s %s's data" % (kind_name, label))
                found.append((label, language, data_size, at))
    return found


def imports(image):
    """An Import (descriptor offset, name offset, name) per import descriptor, in table order."""
    return [Import(at, *image.name_at(unpack(image.data, "<I", at + 12, "an import")[0], "an import's DLL name"))
            for at in image.table(IMPORT, 20, "the import directory")]


def delay_imports(image):
    """(name offset, name) of each delay-import descriptor's DLL name."""
    names = []
    for at in image.table(DELAY_IMPORT, 32, "the delay-import directory"):
        # Every 64-bit linker writes the relative form, attributes bit 0.
        attributes, name = struct.unpack_from("<II", image.data, at)
        if not attributes & 1:
            raise Failure("imports: a delay-import descriptor uses virtual addresses (attributes "
                          "bit 0 is clear), a form this reader does not read")
        names.append(image.name_at(name, "a delay import's DLL name"))
    return names


def hint_names(image):
    """The file ranges of the hint/name entries the import and delay-import name tables point at:
    each imported function's hint and NUL-terminated name. An entry by ordinal has none."""
    tables = []
    for at in image.table(IMPORT, 20, "the import directory"):
        original, _, _, _, first = struct.unpack_from("<IIIII", image.data, at)
        tables.append(original or first)  # with no import name table, the address table holds it
    tables += [struct.unpack_from("<I", image.data, at + 16)[0]
               for at in image.table(DELAY_IMPORT, 32, "the delay-import directory")]
    ranges = []
    for address in (t for t in tables if t):
        for i in range(MAX_ENTRIES + 1):
            if i == MAX_ENTRIES:
                raise Failure("imports: an import name table has more than %d entries"
                              % MAX_ENTRIES)
            at = image.offset(address + 8 * i, 8, "an import name table", "imports")
            (thunk,) = struct.unpack_from("<Q", image.data, at)
            if not thunk:
                break
            if thunk & ORDINAL_FLAG:
                continue
            if thunk >> 31:
                raise Failure("imports: an import name table entry 0x%x is neither an ordinal nor "
                              "a hint/name address" % thunk)
            hint_at = image.offset(thunk, 2, "a hint/name entry", "imports")
            name_at, name = image.name_at(thunk + 2, "an imported function", MAX_EXPORT_NAME)
            ranges.append((hint_at, name_at + len(name) + 1))
    return ranges


def export_names(image):
    """Each name the export directory lists, and "@<ordinal>" for each export with an address and
    no name, which a caller can still reach with GetProcAddress."""
    address, _ = image.directory(EXPORT)
    if not address:
        return []
    at = image.offset(address, 40, "the export directory", "exports")
    base, count, named, functions, names, ordinals = struct.unpack_from("<6I", image.data, at + 16)
    if count > MAX_ENTRIES or named > MAX_ENTRIES:
        raise Failure("exports: the export directory lists more than %d functions or names"
                      % MAX_ENTRIES)

    def array(fmt, address, n, what):
        if not n:
            return ()
        at = image.offset(address, n * struct.calcsize(fmt), what, "exports")
        return struct.unpack_from("<%d%s" % (n, fmt), image.data, at)

    targets = array("I", functions, count, "the export address table")
    name_rvas = array("I", names, named, "the export name table")
    indexes = array("H", ordinals, named, "the export ordinal table")
    found, with_name = [], set()
    for rva, index in zip(name_rvas, indexes):
        if index >= count:
            raise Failure("exports: an exported name's ordinal index %d is past the %d functions"
                          % (index, count))
        name = image.name_at(rva, "an exported name", MAX_EXPORT_NAME, "exports")[1]
        found.append(name.decode("ascii", "replace"))
        with_name.add(index)
    return found + ["@%d" % (base + i) for i, t in enumerate(targets) if t and i not in with_name]


def pdb_ranges(image):
    """The file ranges of the debug directory's CodeView records, which hold the PDB path."""
    address, size = image.directory(DEBUG)
    if not address:
        return []
    if size % DEBUG_ENTRY or size > DEBUG_ENTRY * MAX_ENTRIES:
        raise Failure("image: the debug directory's size %d is not a whole number of entries, at "
                      "most %d" % (size, MAX_ENTRIES))
    at = image.offset(address, size, "the debug directory", "image")
    ranges = []
    for i in range(size // DEBUG_ENTRY):
        entry = at + DEBUG_ENTRY * i
        kind, data_size, _, pointer = struct.unpack_from("<IIII", image.data, entry + 12)
        if kind == CODEVIEW and data_size:
            if pointer + data_size > len(image.data):
                raise Failure("image: a CodeView record lies outside the file")
            # The run-time name search skips this range, so it must be a PDB path's record.
            if image.data[pointer:pointer + 4] != b"RSDS" or data_size > MAX_CODEVIEW:
                raise Failure("image: a CodeView record is not an RSDS record of at most %d bytes"
                              % MAX_CODEVIEW)
            ranges.append((pointer, pointer + data_size))
    return ranges


def version_strings(image):
    """(key, value) of each string of each version resource's StringFileInfo, OriginalFilename
    among them, in the order the resources list them."""
    found = []
    for _, _, size, at in resources(image, RT_VERSION):
        node = VersionNode(image.data, at, at, at + size)
        if node.key != "VS_VERSION_INFO":
            raise Failure("version: a version resource is %r, not a VS_VERSIONINFO" % node.key)
        for info in node.children():
            for table in info.children() if info.key == "StringFileInfo" else []:
                found += [(s.key, s.value.decode("utf-16-le", "replace").rstrip("\0"))
                          for s in table.children()]
    return found


class VersionNode(object):
    """A node of a version resource that starts at `base`: its length, value length and type, its
    UTF-16 key, then its value and its children, each aligned to 32 bits from `base`, all inside
    `end`."""

    def __init__(self, data, base, at, end):
        self.data, self.base = data, base
        if at + 6 > end:
            raise Failure("version: a version resource node lies outside its parent")
        length, value_length, text = struct.unpack_from("<HHH", data, at)
        self.end = at + length
        if length < 6 or self.end > end:
            raise Failure("version: a version resource node's length %d runs past its parent"
                          % length)
        key_end = at + 6
        while key_end + 2 <= self.end and data[key_end:key_end + 2] != b"\0\0":
            key_end += 2
        if key_end + 2 > self.end:
            raise Failure("version: a version resource node's key has no NUL")
        self.key = data[at + 6:key_end].decode("utf-16-le", "replace")
        value_at = self.aligned(key_end + 2)
        value_end = value_at + value_length * (2 if text == 1 else 1)  # a text length is in words
        if value_length and value_end > self.end:  # with no value, the padding may end the node
            raise Failure("version: %s's value runs past its node" % self.key)
        self.value, self.first_child = data[value_at:value_end], self.aligned(value_end)

    def aligned(self, at):
        return self.base + ((at - self.base + 3) & ~3)

    def children(self):
        found, at = [], self.first_child
        while at + 6 <= self.end:
            child = VersionNode(self.data, self.base, at, self.end)
            found.append(child)
            at = self.aligned(child.end)
        return found


def exports(image):
    """(the export name's (offset, bytes), or None, and each forwarder string) of the image."""
    address, size = image.directory(EXPORT)
    if not address:
        return None, []
    at = image.offset(address, 40, "the export directory", "exports")
    name, _, count, _, functions = struct.unpack_from("<IIIII", image.data, at + 12)
    # Bounded before the table is unpacked, so a crafted count costs no memory (as export_names).
    if count > MAX_ENTRIES:
        raise Failure("exports: the export directory lists more than %d functions or names" % MAX_ENTRIES)
    table = image.offset(functions, 4 * count, "the export address table", "exports")
    targets = struct.unpack_from("<%dI" % count, image.data, table)
    # An export whose address lies inside the export directory is a forwarder, "<dll>.<name>".
    forwarders = [image.name_at(t, "a forwarder", rule="exports")[1]
                  for t in targets if address <= t < address + size]
    return image.name_at(name, "the export name", rule="exports"), forwarders


def dependent_load_flags(image):
    """The load configuration's DependentLoadFlags, 0 when it has none."""
    address, _ = image.directory(LOAD_CONFIG)
    if not address:
        return 0
    (size,) = struct.unpack_from("<I", image.data, image.offset(address, 4, "the load configuration", "load-flags"))
    if size < DEPENDENT_LOAD_FLAGS_AT + 2:
        return 0
    at = image.offset(address, DEPENDENT_LOAD_FLAGS_AT + 2, "the load configuration", "load-flags")
    return struct.unpack_from("<H", image.data, at + DEPENDENT_LOAD_FLAGS_AT)[0]


def checksum(data, at):
    """The PE checksum of `data` with the field at `at` read as zero: the 16-bit one's complement
    sum of its words, plus its length."""
    words = bytearray(data) + bytes(len(data) % 2)  # an odd last byte is the low byte of a last word
    words[at:at + 4] = bytes(4)
    total = sum(struct.unpack("<%dH" % (len(words) // 2), bytes(words)))
    while total > 0xFFFF:
        total = (total & 0xFFFF) + (total >> 16)
    return total + len(data)


def names_allowed(image):
    """The file ranges where a DLL names itself or is named without being loaded, the places a DLL
    may name one: the export name, the PDB path, the version resource, the certificate table and the
    imported functions' hint/name entries."""
    export, _ = exports(image)
    ranges = [(export[0], export[0] + len(export[1]))] if export else []
    ranges += [(at, at + size) for _, _, size, at in resources(image, RT_VERSION)]
    at, size = image.directory(SECURITY)  # a file offset, not an address
    return ranges + ([(at, at + size)] if at else []) + pdb_ranges(image) + hint_names(image)


def named_strings(image, names):
    """Each whole string (whole_strings) that, ignoring case, is one of `names`, outside
    names_allowed and the import and delay-import names, which the import rules check."""
    skip = names_allowed(image) + [(i.name_at, i.name_at + len(i.name)) for i in imports(image)]
    skip += [(at, at + len(name)) for at, name in delay_imports(image)]
    return [s for s in whole_strings(image.data) if s[3].lower() in names and not any(a <= s[0] < b for a, b in skip)]


ASCII = re.compile(rb"(?<![\x20-\x7e])[\x20-\x7e]{3,}(?=\x00)")
# An ASCII string's last character and NUL also read as a UTF-16 unit. Read after another
# printable byte, they end that string, so a UTF-16LE string right after it starts after them.
# After a single printable byte no UTF-16LE string starts: rename_check.py's own search fails it.
UTF16 = re.compile(rb"(?<![\x20-\x7e])(?:(?<![\x20-\x7e]\x00)|(?<=[\x20-\x7e]{2}\x00))"
                   rb"(?:[\x20-\x7e]\x00){3,}(?=\x00\x00)")


def whole_strings(data):
    """(offset, length in bytes, encoding, text) of each whole NUL-terminated printable ASCII or
    UTF-16LE string of at least 3 characters. Where bytes read either way, a printable byte and
    NUL after another printable byte end an ASCII string; rename_check.py's own search covers the
    other reading."""
    found = [(m.start(), m.end() - m.start(), "ascii", m.group().decode("ascii")) for m in ASCII.finditer(data)]
    found += [(m.start(), m.end() - m.start(), "utf-16-le", m.group().decode("utf-16-le"))
              for m in UTF16.finditer(data)]
    return sorted(found)


def main(argv):
    if len(argv) != 3 or argv[1] != "rcdata":
        sys.stderr.write("usage: pe.py rcdata <image>\n")
        return 2
    try:
        with open(argv[2], "rb") as f:
            # Refused from its size, before any byte is read, so a larger file costs no memory.
            if os.fstat(f.fileno()).st_size > MAX_FILE:
                raise Failure("image: the file is larger than 1 GiB")
            data = f.read(MAX_FILE)
        for label, language, size, _ in rcdata(Image(data)):
            print("%s %d %d" % (label, language, size))
    except Failure as e:
        sys.stderr.write("%s\n" % e)
        return 1
    except OSError as e:
        sys.stderr.write("image: cannot read %s: %s\n" % (argv[2], e.strerror))
        return 1
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv))
