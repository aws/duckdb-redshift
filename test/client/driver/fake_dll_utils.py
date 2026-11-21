"""A stand-in for delvewheel's delvewheel/_dll_utils.py, which rename_test.sh packs into a wheel at
test time, so the pin's own steps run on a host without the pinned wheels. replace_needed renames
an image's imports as delvewheel does when an old name has no room for the new one: it writes the
new name after the content of the section that holds the import directory, points the descriptor
at it and grows that section's virtual size. The pinned routine also clears DependentLoadFlags and
rewrites SizeOfInitializedData and SizeOfImage, which this leaves as they are; rename_check.py
refuses a change to any of them. With RS_FAKE_DELVEWHEEL_GROW set it also appends a byte, as
delvewheel does when it must add room, and with RS_FAKE_DELVEWHEEL_CLEAR_FLAGS set it clears
DependentLoadFlags as the pinned routine does. Standard library only.
"""

import os
import struct


def replace_needed(lib_path, old_deps, name_map, strip, verbose, test):
    with open(lib_path, "rb") as f:
        data = bytearray(f.read())
    pe = struct.unpack_from("<I", data, 0x3C)[0]
    sections, optional_size = struct.unpack_from("<H", data, pe + 6)[0], struct.unpack_from("<H", data, pe + 20)[0]
    # make_pe.py puts the import directory and its names in .rdata, the last section.
    header = pe + 24 + optional_size + 40 * (sections - 1)
    start, raw_at = struct.unpack_from("<I", data, header + 12)[0], struct.unpack_from("<I", data, header + 20)[0]
    imports = struct.unpack_from("<I", data, pe + 24 + 112 + 8)[0]
    wanted = {name.lower(): name_map[name] for name in old_deps}
    descriptor = raw_at + imports - start
    while any(data[descriptor : descriptor + 20]):
        name_at = struct.unpack_from("<I", data, descriptor + 12)[0] - start + raw_at
        name = bytes(data[name_at : data.index(b"\0", name_at)]).decode("ascii")
        if name.lower() in wanted:
            (virtual_size,) = struct.unpack_from("<I", data, header + 8)
            place, new = virtual_size + virtual_size % 2, wanted[name.lower()].encode("ascii") + b"\0"
            data[raw_at + place : raw_at + place + len(new)] = new
            struct.pack_into("<I", data, descriptor + 12, start + place)
            struct.pack_into("<I", data, header + 8, place + len(new))
        descriptor += 20
    load_config = struct.unpack_from("<I", data, pe + 24 + 112 + 8 * 10)[0]
    if load_config and os.environ.get("RS_FAKE_DELVEWHEEL_CLEAR_FLAGS"):
        struct.pack_into("<H", data, raw_at + load_config - start + 78, 0)  # DependentLoadFlags
    if os.environ.get("RS_FAKE_DELVEWHEEL_GROW"):
        data.append(0)
    with open(lib_path, "wb") as f:
        f.write(data)
