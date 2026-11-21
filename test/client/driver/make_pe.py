"""Writes a tiny PE32+ image with a resource directory for the tool tests, from byte templates: no
compiler, no resource compiler and no checked-in binary.

    python3 -I make_pe.py <out.dll> '<json spec>'

The image is an MZ header, the PE headers and a section, .rsrc, which holds the resource
directory: its directories, then its data entries, its names and its data. Every key of the spec
is optional:

  machine    the COFF header's machine (default 0x8664, x86-64)
  magic      the optional header's magic (default 0x20b, PE32+)
  resources  [[type, name, language, text], ...]: each resource, its type and name a number or a
             string, and its text "@<path>" for that file's bytes (default: none, and then no
             resource directory at all)
  many       adds this many RCDATA resources, numbered from 1, or with `named`, string names "R1",
             "R2", ..., each one byte long
  trailing   text appended after the image, as DuckDB appends an extension's metadata
  directories
             the optional header's number of data directories (default 16)
  optional_size
             the optional header's size, the section table following it (default 240)
  virtual_size
             the section's virtual size (default: the resource directory's size)
  sections_before
             this many empty sections, at most 2, listed before .rsrc, and one more after it
  directory_size
             the resource directory's size in the data directory (default: the section's)
  data_address, data_size
             the first data entry's address and size (default: its resource's)
  break      a list of: "no-pe" (the MZ header points at no PE signature), "truncated" (the file
             ends inside the COFF header), "entries-past" (the root directory claims entries past
             the resource directory), "section-cut" (the file ends one byte before the section's
             end), "fourth-level" (the first language entry points at a directory),
             "entry-outside" (it points past the resource directory), "subdir-outside" (the
             first type points past it), "type-data" (the first type points at a data entry),
             "name-outside" (the first type's first name points past it), "name-data" (that
             name points at a data entry), "name-long" (the first string name's length runs past
             the resource directory) and "name-surrogate" (that name starts with a lone surrogate)
  rdata      a second section, .rdata, for the rename and policy tests: {"imports": [dll, ...],
             "imported_functions": [name, ...] each import's hint/name entries (default ["f"]),
             "imported_ordinals": n more entries by ordinal, "raw_thunks": [n, ...] more entries
             as given, "no_name_table": true (each
             descriptor's import name table field 0), "delay_imports": [dll, ...],
             "delay_functions": [name, ...] their import name tables' entries (default none),
             "delay_va": true (descriptors in the virtual-address form), "export_name": dll,
             "forwarders": ["dll.function", ...], "functions": n plain exports without a name,
             "exported": [name, ...] plain exports with one ("ordinal_past": true points their
             ordinals past the functions), "unused_slots": n export address table entries of 0,
             "functions_at" or "names_at": n the export address or name table's address instead,
             "names_count": n the export directory's NumberOfNames instead,
             "export_name_at": n the export name's,
             "load_flags": n, "load_config_size": n, "load_config_at": n (the load configuration's
             address instead), "bound_import": true, "strings": [["a" or
             "u", text], ...] (each whole, ASCII or UTF-16LE; "h", its text the hex of bytes
             written as given), "pdb": path (a CodeView debug
             record naming it; "pdb_type": n another debug type, "pdb_pointer": n its file
             offset, "debug_size": n the debug directory's size), "clr": true (a .NET runtime
             header), "renamed": {dll: new}, which writes each new import name after the
             section's content and points its descriptor there, as delvewheel does
             ("renamed_over": i writes it over the i-th string instead), "virtual_size": n}
  version    {key: value, ...}: a version resource (type 16) holding a VS_VERSIONINFO with these
             strings in one StringFileInfo table, after the other resources ("version_unpadded":
             true leaves each string's padding out of its length, as linkers do)
  checksum   the header checksum: a number (default 0), or "valid" for the image's own
             checksum
  certificate
             text appended as the certificate table, which the security directory names
Standard library only, Python 3.6 and later.
"""

import collections
import json
import struct
import sys

SECTION_ADDRESS, SECTION_FILE, EMPTY_SECTIONS, RDATA_ADDRESS = 0x1000, 0x200, 0x10000, 0x8000
RVA_COUNT_AT, DATA_DIRECTORIES_AT = 108, 112  # offsets in the PE32+ optional header, as pe.py's


def payload(text):
    """A resource's bytes: the file's that "@<path>" names, else the text's."""
    if isinstance(text, bytes):
        return text
    if text.startswith("@"):
        with open(text[1:], "rb") as f:
            return f.read()
    return text.encode()


def resource_section(resources, spec):
    breaks = spec.get("break", [])
    # {type: {name: [(language, text)]}}, each level in first-seen order
    tree = collections.OrderedDict()
    for kind, name, language, text in resources:
        tree.setdefault(kind, collections.OrderedDict()).setdefault(name, []).append((language, text))
    types = [(kind, list(names.items())) for kind, names in tree.items()]
    # Directory offsets: the root, each type's, then each name's.
    directories = [len(types)] + [len(names) for _, names in types] + [len(langs) for _, n in types for _, langs in n]
    offsets, at = [], 0
    for count in directories:
        offsets.append(at)
        at += 16 + 8 * count
    leaves = [(l, payload(text)) for _, names in types for _, langs in names for l, text in langs]
    data_entries = at
    at += 16 * len(leaves)
    strings = {}
    for label in [t for t, _ in types] + [n for _, names in types for n, _ in names]:
        if isinstance(label, str) and label not in strings:
            strings[label] = at
            at += 2 + 2 * len(label)
    blobs = []
    for _, text in leaves:
        at = (at + 7) & ~7
        blobs.append(at)
        at += len(text)
    section = bytearray(at)

    def label_field(label):
        return strings[label] | 0x80000000 if isinstance(label, str) else label

    def directory(index, entries):
        struct.pack_into(
            "<IIHHHH",
            section,
            offsets[index],
            0,
            0,
            0,
            0,
            sum(isinstance(k, str) for k, _ in entries),
            sum(not isinstance(k, str) for k, _ in entries),
        )
        for i, (key, target) in enumerate(entries):
            struct.pack_into("<II", section, offsets[index] + 16 + 8 * i, label_field(key), target)

    name_dir = 1 + len(types)
    leaf = 0
    directory(0, [(t, offsets[1 + i] | 0x80000000) for i, (t, _) in enumerate(types)])
    for i, (_, names) in enumerate(types):
        directory(1 + i, [(n, offsets[name_dir + j] | 0x80000000) for j, (n, _) in enumerate(names)])
        for _, langs in names:
            directory(name_dir, [(l, data_entries + 16 * (leaf + k)) for k, (l, _) in enumerate(langs)])
            name_dir += 1
            leaf += len(langs)
    for k, (_, text) in enumerate(leaves):
        struct.pack_into("<IIII", section, data_entries + 16 * k, SECTION_ADDRESS + blobs[k], len(text), 0, 0)
        section[blobs[k] : blobs[k] + len(text)] = text
    for label, where in strings.items():
        section[where : where + 2 + 2 * len(label)] = struct.pack("<H", len(label)) + label.encode("utf-16-le")
    if "entries-past" in breaks:
        struct.pack_into("<H", section, 14, 1000)
    struct.pack_into("<I", section, data_entries, spec.get("data_address", SECTION_ADDRESS + blobs[0]))
    struct.pack_into("<I", section, data_entries + 4, spec.get("data_size", len(leaves[0][1])))
    # Each directory entry is 8 bytes, (label, target); a break overwrites one field of the first
    # type's, the first name's or the first language's entry.
    first_type_target = 16 + 4
    first_name_label, first_name_target = offsets[1] + 16, offsets[1] + 16 + 4
    first_language_target = offsets[1 + len(types)] + 16 + 4
    if "subdir-outside" in breaks:
        struct.pack_into("<I", section, first_type_target, 0x7FFFFFF0 | 0x80000000)
    if "type-data" in breaks:
        struct.pack_into("<I", section, first_type_target, offsets[1])
    if "name-outside" in breaks:
        struct.pack_into("<I", section, first_name_label, 0x7FFFFFF0 | 0x80000000)
    if "name-data" in breaks:
        struct.pack_into("<I", section, first_name_target, offsets[1 + len(types)])
    if "name-long" in breaks:
        struct.pack_into("<H", section, min(strings.values()), 0xFFFF)
    if "name-surrogate" in breaks:
        struct.pack_into("<H", section, min(strings.values()) + 2, 0xD800)
    if "entry-outside" in breaks:
        struct.pack_into("<I", section, first_language_target, 0x7FFFFFF0)
    if "fourth-level" in breaks:
        struct.pack_into(
            "<I",
            section,
            first_language_target,
            struct.unpack_from("<I", section, first_language_target)[0] | 0x80000000,
        )
    return bytes(section)


def version_info(strings, unpadded=False):
    """A VS_VERSIONINFO holding a VS_FIXEDFILEINFO and `strings` in one StringFileInfo table."""

    def node(key, value, children=b"", text=False):
        """A node padded to 32 bits: a text value's length counts its UTF-16 words. Unpadded, a
        leaf's length stops at its value's end, so the next sibling must be aligned."""
        head = key.encode("utf-16-le") + b"\0\0"
        head += bytes(-(6 + len(head)) % 4)
        body = value + bytes(-len(value) % 4) + children
        value_length = len(value) // (2 if text else 1)
        length = 6 + len(head) + (len(value) if unpadded and not children else len(body))
        block = struct.pack("<HHH", length, value_length, text) + head + body
        return block + bytes(-len(block) % 4)

    table = b"".join(node(k, (v + "\0").encode("utf-16-le"), text=True) for k, v in strings.items())
    fixed = struct.pack("<13I", 0xFEEF04BD, 0x10000, *([0] * 11))
    string_info = node("StringFileInfo", b"", node("040904b0", b"", table, True), True)
    return node("VS_VERSION_INFO", fixed, string_info)


def rdata_section(spec, file_at, address=RDATA_ADDRESS):
    """(section bytes, its virtual size, its raw size, {data directory index: (address, size)}), for
    a section at file offset `file_at`."""
    content, directories = bytearray(16), {}

    def put(data, align=2):
        content.extend(bytes(-len(content) % align))
        content.extend(data)
        return address + len(content) - len(data)

    def string(kind, text):
        if kind == "h":
            return bytes.fromhex(text)
        return text.encode("utf-16-le") + b"\0\0" if kind == "u" else text.encode() + b"\0"

    strings = [put(b"\0" + string(k, t)) + 1 for k, t in spec.get("strings", [])]

    def hint_names(functions):
        return [put(b"\0\0" + f.encode() + b"\0") for f in functions]

    def thunk_table(hints, ordinals=0):
        """An import name table: an entry per hint/name entry, then `ordinals` by ordinal, then the
        spec's raw entries."""
        entries = hints + [(1 << 63) | (k + 1) for k in range(ordinals)] + spec.get("raw_thunks", []) + [0]
        return put(struct.pack("<%dQ" % len(entries), *entries), 8)

    imports = spec.get("imports", [])
    if imports:
        names = [put(name.encode() + b"\0") for name in imports]
        hints = hint_names(spec.get("imported_functions", ["f"]))  # shared by every import
        thunks = [thunk_table(hints, spec.get("imported_ordinals", 0)) for _ in imports]
        # Without an import name table, the loader reads the names from the address table.
        original = [0 if spec.get("no_name_table") else t for t in thunks]
        descriptors = [struct.pack("<5I", o, 0, 0, n, t) for o, t, n in zip(original, thunks, names)]
        table = put(b"".join(descriptors) + bytes(20), 4)
        directories[1] = (table, 20 * (len(imports) + 1))
    delayed = spec.get("delay_imports", [])
    if delayed:
        names = [put(name.encode() + b"\0") for name in delayed]
        functions = spec.get("delay_functions")
        name_table = thunk_table(hint_names(functions)) if functions is not None else 0
        attributes = 0 if spec.get("delay_va") else 1
        descriptors = [struct.pack("<8I", attributes, n, 0, 0, name_table, 0, 0, 0) for n in names]
        table = put(b"".join(descriptors) + bytes(32), 4)
        directories[13] = (table, 32 * (len(delayed) + 1))
    if "export_name" in spec:
        name, forwarders = put(spec["export_name"].encode() + b"\0"), spec.get("forwarders", [])
        exported = spec.get("exported", [])
        plain = spec.get("functions", 0) + len(exported)
        count = len(forwarders) + plain + spec.get("unused_slots", 0)
        directory = put(bytes(40 + 4 * count), 4)
        # Each plain function's address is the export name's, outside the directory.
        targets = [put(f.encode() + b"\0", 1) for f in forwarders] + [name] * plain
        targets += [0] * spec.get("unused_slots", 0)  # an address of 0 is an unused ordinal
        at = directory - address
        name_table = ordinal_table = 0
        if exported:  # the named functions follow the forwarders
            name_rvas = [put(e.encode() + b"\0", 1) for e in exported]
            name_table = put(struct.pack("<%dI" % len(exported), *name_rvas), 4)
            indexes = range(len(forwarders), len(forwarders) + len(exported))
            if spec.get("ordinal_past"):
                indexes = [count] * len(exported)
            ordinal_table = put(struct.pack("<%dH" % len(exported), *indexes), 2)
        struct.pack_into(
            "<7I",
            content,
            at + 12,
            spec.get("export_name_at", name),
            1,
            count,
            spec.get("names_count", len(exported)),
            spec.get("functions_at", directory + 40),
            spec.get("names_at", name_table),
            ordinal_table,
        )
        struct.pack_into("<%dI" % count, content, at + 40, *targets)
        directories[0] = (directory, address + len(content) - directory)
    if "pdb" in spec:
        record = put(b"RSDS" + bytes(20) + spec["pdb"].encode() + b"\0", 4)
        size = len(spec["pdb"]) + 25
        pointer = spec.get("pdb_pointer", file_at + record - address)
        kind = spec.get("pdb_type", 2)  # 2 is CodeView
        entry = put(struct.pack("<IIHHIIII", 0, 0, 0, 0, kind, size, record, pointer), 4)
        directories[6] = (entry, spec.get("debug_size", 28))
    if spec.get("clr"):
        directories[14] = (put(bytes(72), 4), 72)
    if "load_flags" in spec:
        size = spec.get("load_config_size", 148)
        at = put(struct.pack("<I74xH68x", size, spec["load_flags"]), 8)
        directories[10] = (spec.get("load_config_at", at), size)
    if spec.get("bound_import"):
        directories[11] = (put(bytes(16), 4), 16)
    raw_size = (len(content) + 0x100 + 0x1FF) & ~0x1FF  # room after the content for new names
    for old, new in spec.get("renamed", {}).items():
        descriptor = directories[1][0] - address + 20 * imports.index(old)
        if "renamed_over" in spec:  # over the bytes of that string, not after the content
            where = strings[spec["renamed_over"]]
            content[where - address : where - address + len(new) + 1] = new.encode() + b"\0"
        else:
            where = put(new.encode() + b"\0")
        struct.pack_into("<I", content, descriptor + 12, where)
    virtual_size = spec.get("virtual_size", len(content))
    return bytes(content) + bytes(raw_size - len(content)), virtual_size, raw_size, directories


def pe_checksum(data, at):
    words = bytearray(data) + bytes(len(data) % 2)
    words[at : at + 4] = bytes(4)
    total = sum(struct.unpack("<%dH" % (len(words) // 2), bytes(words)))
    while total > 0xFFFF:
        total = (total & 0xFFFF) + (total >> 16)
    return total + len(data)


def section_header(name, virtual_size, address, raw_size, raw_at):
    return struct.pack("<8sIIIIIIHHI", name, virtual_size, address, raw_size, raw_at, 0, 0, 0, 0, 0x40000040)


def image(spec):
    breaks = spec.get("break", [])
    many = [[10, ("R%d" % (i + 1)) if spec.get("named") else i + 1, 0, "x"] for i in range(spec.get("many", 0))]
    resources = spec.get("resources", []) + many
    if "version" in spec:
        resources.append([16, 1, 1033, version_info(spec["version"], spec.get("version_unpadded", False))])
    section = resource_section(resources, spec) if resources else b""
    raw_size = (len(section) + 0x1FF) & ~0x1FF
    optional = bytearray(240)
    struct.pack_into("<H", optional, 0, spec.get("magic", 0x20B))
    struct.pack_into("<I", optional, RVA_COUNT_AT, spec.get("directories", 16))
    if resources:
        size = spec.get("directory_size", len(section))
        struct.pack_into("<II", optional, DATA_DIRECTORIES_AT + 8 * 2, SECTION_ADDRESS, size)
    optional = optional[: spec.get("optional_size", len(optional))]
    # Empty sections with no bytes in the file, at addresses past .rsrc's, as .text or .reloc.
    before = spec.get("sections_before", 0)
    empty = [section_header(b".text", 0x1000, EMPTY_SECTIONS + 0x1000 * i, 0, 0) for i in range(before + 1)]
    rsrc = section_header(b".rsrc", spec.get("virtual_size", len(section)), SECTION_ADDRESS, raw_size, SECTION_FILE)
    headers = empty[:before] + [rsrc] + empty[before:] if before else [rsrc]
    rdata, directories = b"", {}
    if "rdata" in spec:
        rdata_at = SECTION_FILE + raw_size
        # After .rsrc's last page, so a large resource set (the notices) never overlaps it.
        rdata_address = max(RDATA_ADDRESS, (SECTION_ADDRESS + len(section) + 0xFFF) & ~0xFFF)
        rdata, virtual_size, rdata_raw, directories = rdata_section(spec["rdata"], rdata_at, rdata_address)
        headers.append(section_header(b".rdata", virtual_size, rdata_address, rdata_raw, SECTION_FILE + raw_size))
        # SizeOfImage and SizeOfInitializedData as a linker writes them, which delvewheel's pefile
        # recomputes: the end of the last section, page-aligned, and the sections' raw sizes.
        ends = [rdata_address + virtual_size]
        ends += [EMPTY_SECTIONS + 0x1000 * (before + 1)] if before else []
        struct.pack_into("<I", optional, 8, raw_size + rdata_raw)
        # The fields pefile, which delvewheel uses, reads: image base, alignments, versions and sizes.
        struct.pack_into(
            "<QIIHHHHHHIII",
            optional,
            24,
            0x180000000,
            0x1000,
            0x200,
            6,
            0,
            0,
            0,
            6,
            0,
            0,
            (max(ends) + 0xFFF) & ~0xFFF,
            SECTION_FILE,
        )
        struct.pack_into("<HH", optional, 68, 2, 0x160)
    for index, (address, size) in directories.items():
        struct.pack_into("<II", optional, DATA_DIRECTORIES_AT + 8 * index, address, size)
    coff = struct.pack("<HHIIIHH", spec.get("machine", 0x8664), len(headers), 0, 0, 0, len(optional), 0x2022)
    pe = 0x40 if "no-pe" not in breaks else 0x80
    out = bytearray(b"MZ" + bytes(0x3A) + struct.pack("<I", pe))
    out += b"PE\0\0" + coff + bytes(optional) + b"".join(headers)
    out += bytes(SECTION_FILE - len(out)) + section + bytes(raw_size - len(section)) + rdata
    if "certificate" in spec:
        struct.pack_into("<II", out, 0x40 + 24 + DATA_DIRECTORIES_AT + 8 * 4, len(out), len(spec["certificate"]))
        out += spec["certificate"].encode()
    checksum = spec.get("checksum", 0)
    struct.pack_into("<I", out, 0x40 + 24 + 64, pe_checksum(out, 0x40 + 24 + 64) if checksum == "valid" else checksum)
    if "truncated" in breaks:
        out = out[:0x50]
    if "section-cut" in breaks:
        out = out[:-1]
    return bytes(out) + spec.get("trailing", "").encode()


def main(argv):
    if len(argv) != 3:
        sys.stderr.write("usage: make_pe.py <out.dll> '<json spec>'\n")
        return 2
    with open(argv[1], "wb") as f:
        f.write(image(json.loads(argv[2])))
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv))
