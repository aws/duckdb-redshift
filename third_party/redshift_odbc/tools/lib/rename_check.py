"""Checks a renamed Windows set against its originals, before a lock names the set.

    python3 -I rename_check.py <rows> <original-dir> <renamed-dir>

Written apart from the transform (rename_pin.py and delvewheel): it reads the rows only for the
file hashes, the generated names and the name seed, which each generated name must derive from,
and judges each renamed file from the two images alone. <original-dir> holds what rename_pin.py's
<set-dir> held: the set's files under the MSI's names and the lock's three notices. For
every driver and library row, <original-dir>/<name> must have the source-sha256 and, under its
rename row's name, <renamed-dir>/<generated>.dll the sha256. Every such row needs a rename row, to a
distinct name 16 characters long or, for krb5_64 and wshelp64 alone, as long as its original base
name. A renamed file
has its original's length and differs from it only in:

  - its import descriptors' name fields and DLL names: the same imports in the same order, each set
    member's under its generated name, the new names written over the old ones or over zero bytes
    of a section's padding past its original virtual size, inside its new one;
  - each whole string that named a set member or a DLL policy/windows/unshipped-names.txt lists,
    now that member's generated name or, for an unshipped DLL, as many '|' characters;
    a member's bare base name names it only for krb5_64 and wshelp64, and any other is unchanged
    and kept by its policy/windows/kept-names.txt row, or fails, as does a stale row;
  - the VirtualSize of a section given a new import name, grown to that name's end within its raw
    size;
  - the checksum: still 0 where it was 0, otherwise the renamed bytes' own.

An unshipped name must become exactly as many '|' characters, as the pin writes it, which no load
can open: a ':', '/' or '\\' would still make a path. A string still holding an original name there
fails. The checker then searches the renamed bytes itself, sharing no string finder with the pin:
ignoring case, in ASCII and UTF-16LE at every offset, for each member's file name, the self-names
and the unshipped names. An occurrence outside pe.names_allowed, the import and delay-import names
and the rewritten strings fails. The file also holds no bound imports, and no delay import or
forwarder naming a set member, and no DependentLoadFlags other than 0 or
LOAD_LIBRARY_SEARCH_DLL_LOAD_DIR with at most LOAD_LIBRARY_SEARCH_SYSTEM32.

Once every file passes, it recomputes the name seed itself, apart from the pin's Plan, and the
name-seed row must equal it: the SHA-256 of the source manifest ("<sha256>  <size>  <name>" for
each file of <original-dir>, the notices among them, sorted by name) and the transform's identity,
which is each rename-requirements.txt line's name, version and SHA-256, sorted by name, the pin's
and the applier's VERSIONs, and the place and original bytes of each string the rename rewrites
(the sites this checker finds, file by file in name order). A missing notice, or a .dll there that
no row names, fails first. So a lock whose names all derive from a seed of other files, tools or
versions fails here.
On the first failure it prints "<rule>: <reason>" and exits 1.
Standard library only, Python 3.6 and later.
"""

import base64
import hashlib
import importlib.util
import os
import re
import struct
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
UNSHIPPED = os.path.join(HERE, "..", "..", "policy", "windows", "unshipped-names.txt")
REQUIREMENTS = os.path.join(HERE, "rename-requirements.txt")
# A requirements line as the seed covers it: the name, the version and the SHA-256; the URL is
# where the pin fetches the wheel, which the SHA-256 already pins.
REQUIREMENT = re.compile(r"([a-z][a-z0-9-]*)==([0-9][0-9a-z.]*) --hash=sha256:([0-9a-f]{64})"
                         r"(?: --url=https://[!-~]+)?\Z")
DLL_LOAD_DIR, SYSTEM32 = 0x100, 0x800


def sibling(name):
    """A module of this directory, by path, since python3 -I puts no directory on sys.path. No
    bytecode is written next to it, so the tools never change the repository."""
    sys.dont_write_bytecode = True
    spec = importlib.util.spec_from_file_location("redshift_odbc_" + name, os.path.join(HERE, name + ".py"))
    module = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(module)
    return module


rename_apply = sibling("rename_apply")
pe = sibling("pe")
Failure, base = rename_apply.Failure, rename_apply.base
lock = sibling("lock")
SELF_NAMED = lock.SELF_NAMED
PIN = os.path.join(HERE, "rename_pin.py")


def within(position, ranges):
    return any(a <= position < b for a, b in ranges)


class Names(object):
    """Every name of the set, lowercase: each DLL's original and generated, with and without .dll,
    and the unshipped DLLs'."""

    def __init__(self, rows):
        self.generated = {}
        for name in rows.files:
            if name not in rows.renames:
                raise Failure("rename: %s has no rename row" % name)
            new = rows.renames[name]
            # Only a self-named DLL keeps its own length, as the lock's rule says (lock.SELF_NAMED).
            length = SELF_NAMED.get(name.lower(), 16)
            if len(new) != length:
                raise Failure("rename: %s's generated name %s is not %d characters" % (name, new, length))
            self.generated[name.lower()] = new
        if len(set(self.generated.values())) != len(self.generated):
            raise Failure("rename: two files have one generated name")
        # Each generated name is the start of base32(SHA-256(name seed, lowercase base name)), so
        # no name the rows give comes from another seed; main then recomputes the seed itself.
        seed = rows.hashes.get("name-seed")
        if seed is None:
            raise Failure("rename: the rows have no name-seed row, which the generated names "
                          "derive from")
        for name in rows.files:
            new = rows.renames[name]
            digest = hashlib.sha256((seed + " " + base(name).lower()).encode("ascii")).digest()
            if base64.b32encode(digest).decode("ascii").lower()[:len(new)] != new:
                raise Failure("rename: %s's generated name %s does not derive from the name-seed "
                              "row" % (name, new))
        self.originals = {}  # each original name, with and without .dll, to its DLL (None: not shipped)
        for name in rows.files:
            self.originals[name.lower()] = self.originals[base(name).lower()] = name.lower()
        unshipped = rename_apply.policy_names(UNSHIPPED)
        for name in unshipped:
            self.originals[name] = self.originals[name + ".dll"] = None
        # What the checker's own search looks for: a self-name names its DLL bare, too.
        self.searched = {n.lower() for n in rows.files} | {n + ".dll" for n in unshipped} | set(unshipped)
        self.searched |= {base(n).lower() for n in rows.files if n.lower() in SELF_NAMED}
        generated = set(self.generated.values())
        self.members = set(self.originals) | generated | {g + ".dll" for g in generated}
        self.kept = rename_apply.kept_names()

    def member(self, dll_name):
        return dll_name.decode("ascii", "replace").lower() in self.members

    def renamed_import(self, imported):
        """The name an import must have after the rename: a set member's generated file name."""
        text = imported.name.decode("ascii", "replace").lower()
        return self.generated[self.originals[text]] + ".dll" if self.originals.get(text) else text


def changes(old, new):
    """Each offset where the two equal-length byte strings differ, skipping equal 4 KiB blocks."""
    for start in range(0, len(old), 4096):
        if old[start:start + 4096] != new[start:start + 4096]:
            for at in range(start, min(start + 4096, len(old))):
                if old[at] != new[at]:
                    yield at


def occurrences(data, needles):
    """(offset, needle) of each needle in `data`, ignoring case, in ASCII and UTF-16LE, at every
    offset and whatever bytes surround it."""
    lowered = data.lower()  # only A-Z change, and each needle's UTF-16LE high bytes are zero
    for needle in sorted(needles):
        for encoded in (needle.encode("ascii"), needle.encode("utf-16-le")):
            at = lowered.find(encoded)
            while at >= 0:
                yield at, needle
                at = lowered.find(encoded, at + 1)


def rewritten_sites(name, old, sites, names):
    """The sites (pe.named_strings) the rename must rewrite. A member's bare base name names it
    only for a self-named DLL; any other must be kept as it is by its kept-names.txt row, for this
    file's SHA-256, offset and text, and a row of this file's that keeps no such string fails."""
    digest, rewritten, kept = rename_apply.sha256(old), [], []
    for site in sites:
        at, _, _, text = site
        member = names.originals[text.lower()]
        if member is None or text.lower().endswith(".dll") or member in SELF_NAMED:
            rewritten.append(site)
        elif names.kept.get((name, at)) == (digest, text):
            kept.append(at)
        else:
            raise Failure("kept-names: %s holds %r at %d, the bare name of the set member %s, which "
                          "policy/windows/kept-names.txt does not keep for this file" % (name, text, at, member))
    for (file, at), (sha, text) in sorted(names.kept.items()):
        if file == name and (sha != digest or at not in kept):
            raise Failure("kept-names: the row for %s at %d (%s, SHA-256 %s) keeps no bare member name of "
                          "that file, whose SHA-256 is %s" % (file, at, text, sha, digest))
    return rewritten


def check_file(name, names, old, new):
    o, r = pe.Image(old), pe.Image(new)
    if len(old) != len(new):
        raise Failure("bytes: %s's renamed file is %d bytes, not the original's %d" % (name, len(new), len(old)))
    o_imports, r_imports = pe.imports(o), pe.imports(r)
    o_names = [(i.name_at, i.name_at + len(i.name) + 1) for i in o_imports]
    r_names = [(i.name_at, i.name_at + len(i.name) + 1) for i in r_imports]
    sites = rewritten_sites(name, old, pe.named_strings(o, names.originals), names)
    allowed = [(o.checksum_at, o.checksum_at + 4)] + o_names + [(at, at + size) for at, size, _, _ in sites]
    allowed += [(i.descriptor + 12, i.descriptor + 16) for i in o_imports]
    allowed += [(s.header + 8, s.header + 12) for s in o.sections]
    # The routine moves an import name only into a section's padding, past its virtual size.
    slack = [(s.raw_at + s.virtual_size, s.raw_at + s.raw_size) for s in o.sections]
    for at in changes(old, new):
        if not within(at, allowed) and not (within(at, r_names) and old[at] == 0 and within(at, slack)):
            raise Failure("bytes: %s changed at %d, outside the import names, the set's names, a section's "
                          "size and the checksum" % (name, at))

    expected = [names.renamed_import(i) for i in o_imports]
    got = [i.name.decode("ascii", "replace").lower() for i in r_imports]
    if got != expected:
        raise Failure("imports: %s imports %s, not %s" % (name, " ".join(got), " ".join(expected)))
    for imported in r_imports:
        (address,) = struct.unpack_from("<I", new, imported.descriptor + 12)
        end = address + len(imported.name) + 1
        if not any(s.address <= address and end <= s.address + s.virtual_size for s in r.sections):
            raise Failure("imports: %s's import name %s lies past its section's virtual size"
                          % (name, imported.name.decode("ascii", "replace")))
    for at, size, encoding, text in sites:
        member = names.originals[text.lower()]
        now = new[at:at + size].decode(encoding, "replace")
        if member is None and now != "|" * len(text):
            raise Failure("strings: %s at %d: the unshipped name %s became %s, not %d '|' characters"
                          % (name, at, text, now, len(text)))
        if member is not None and now.lower() != names.generated[member] + text[len(base(member)):].lower():
            raise Failure("strings: %s at %d: the name %s became %s, not its generated name" % (name, at, text, now))
    places = pe.names_allowed(o) + o_names + r_names + [(at, at + size) for at, size, _, _ in sites]
    places += [(at, at + len(delayed) + 1) for at, delayed in pe.delay_imports(o)]
    for at, needle in occurrences(new, names.searched):
        if not within(at, places):
            raise Failure("strings: %s at %d still holds %s, where no DLL may name one"
                          % (name, at, needle))

    # The routine writes a new import name past a section's content and grows it to the name's end.
    moved = [(struct.unpack_from("<I", new, i.descriptor + 12)[0], len(i.name) + 1)
             for i, j in zip(r_imports, o_imports) if i.name_at != j.name_at]
    for before, after in zip(o.sections, r.sections):
        ends = [at + size - after.address for at, size in moved
                if after.address <= at and at + size <= after.address + after.raw_size]
        if after.virtual_size != before.virtual_size and not (
                before.virtual_size < after.virtual_size <= max(ends, default=0)):
            raise Failure("sections: %s's section at 0x%x changed its virtual size from %d to %d; "
                          "only a section given a new import name grows, to that name's end"
                          % (name, after.address, before.virtual_size, after.virtual_size))
    if o.checksum == 0 and r.checksum != 0:
        raise Failure("checksum: %s's checksum was 0 and is 0x%x" % (name, r.checksum))
    if o.checksum != 0 and r.checksum != pe.checksum(new, r.checksum_at):
        raise Failure("checksum: %s's checksum 0x%x is not its renamed bytes' 0x%x"
                      % (name, r.checksum, pe.checksum(new, r.checksum_at)))

    for _, delayed in pe.delay_imports(r):
        if names.member(delayed):
            raise Failure("imports: %s delay-loads the set member %s" % (name, delayed.decode("ascii", "replace")))
    for forwarder in pe.exports(r)[1]:
        if names.member(forwarder.rsplit(b".", 1)[0]):
            raise Failure("imports: %s forwards %s to a set member" % (name, forwarder.decode("ascii", "replace")))
    if r.directory(pe.BOUND_IMPORT)[0]:
        raise Failure("imports: %s has bound imports, which a rename leaves stale" % name)
    flags = pe.dependent_load_flags(r)
    if flags and (not flags & DLL_LOAD_DIR or flags & ~(DLL_LOAD_DIR | SYSTEM32)):
        raise Failure("load-flags: %s's DependentLoadFlags 0x%x is not LOAD_LIBRARY_SEARCH_DLL_LOAD_DIR with at "
                      "most LOAD_LIBRARY_SEARCH_SYSTEM32" % (name, flags))


def tool_identity(path=REQUIREMENTS):
    """'tool <name> <version> <sha256>' per rename-requirements.txt line, sorted by name. A line
    without its SHA-256, or a second line for one tool, fails: no seed could cover it."""
    tools = {}
    for line in rename_apply.read_file(path, "seed").decode("ascii", "replace").splitlines():
        line = line.split("#", 1)[0].strip()
        if not line:
            continue
        match = REQUIREMENT.match(line)
        if not match or match.group(1) in tools:
            raise Failure("seed: %s: %r is not one <name>==<version> --hash=sha256:<hex> line per "
                          "tool" % (path, line))
        tools[match.group(1)] = match.group(2, 3)
    if not tools:
        raise Failure("seed: %s pins no tool" % path)
    return "".join("tool %s %s %s\n" % (name, version, digest)
                   for name, (version, digest) in sorted(tools.items()))


def pin_version(path=PIN):
    """rename_pin.py's VERSION, read from its one 'VERSION = <n>' line, so none of its code runs."""
    found = re.findall(r"^VERSION = ([0-9]+)\b", rename_apply.read_file(path, "seed").decode("ascii", "replace"),
                       re.MULTILINE)
    if len(found) != 1:
        raise Failure("seed: %s does not hold one 'VERSION = <n>' line" % path)
    return int(found[0])


def recomputed_seed(original_dir, rows, names):
    """The name seed of <original-dir>'s files and this transform, from this checker's own reading."""
    try:
        listed = sorted(os.listdir(original_dir))
    except OSError as e:
        raise Failure("seed: cannot list %s: %s" % (original_dir, e.strerror))
    for notice in lock.REQUIRED_NOTICES:
        if notice not in listed:
            raise Failure("seed: %s holds no %s, one of the notices the seed covers" % (original_dir, notice))
    manifest, strings = "", ""
    for name in listed:
        path = os.path.join(original_dir, name)
        if lock.name_problem(name) or not os.path.isfile(path):
            raise Failure("seed: %s" % (lock.name_problem(name) or "%s is not a regular file" % path))
        if name.lower().endswith(".dll") and name not in rows.files:
            raise Failure("rename: %s holds %s, which no driver or library row names" % (original_dir, name))
        data = rename_apply.read_file(path, "seed")
        manifest += "%s  %d  %s\n" % (rename_apply.sha256(data), len(data), name)
        if name in rows.files:
            sites = rewritten_sites(name, data, pe.named_strings(pe.Image(data), names.originals), names)
            strings += "".join("string %s %d %s\n" % (name, at, data[at:at + size].hex())
                               for at, size, _, _ in sites)
    identity = tool_identity() + "pin %d\napply %d\n" % (pin_version(), rename_apply.VERSION) + strings
    return rename_apply.sha256((manifest + identity).encode("ascii"))


def main(argv):
    if len(argv) != 4:
        sys.stderr.write("usage: rename_check.py <rows> <original-dir> <renamed-dir>\n")
        return 2
    _, rows_path, original_dir, renamed_dir = argv
    try:
        rows = rename_apply.Rows(rename_apply.read_file(rows_path, "rows").decode("utf-8", "replace"))
        names = Names(rows)
        for name in sorted(rows.renames):
            _, want, source = rows.files[name]
            old = rename_apply.read_file(os.path.join(original_dir, name), "rename")
            new = rename_apply.read_file(os.path.join(renamed_dir, rows.renames[name] + ".dll"), "rename")
            if rename_apply.sha256(old) != source or rename_apply.sha256(new) != want:
                raise Failure("rename: %s's original or renamed SHA-256 is not the rows' source-sha256 or "
                              "sha256" % name)
            check_file(name, names, old, new)
        seed = recomputed_seed(original_dir, rows, names)
        if seed != rows.hashes["name-seed"]:
            raise Failure("seed: the name-seed row %s is not %s, the seed of %s's files, the notices "
                          "among them, and the rename's tools, versions and rewritten strings"
                          % (rows.hashes["name-seed"], seed, original_dir))
    except (Failure, pe.Failure) as e:
        sys.stderr.write("%s\n" % e)
        return 1
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv))
