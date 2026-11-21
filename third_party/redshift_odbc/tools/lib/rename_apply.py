"""Applies the lock's recorded rename to the Windows set: no third-party code.

    python3 -I rename_apply.py <rows> <set-dir> <out-dir>

<rows> is a lock, or the rows rename_pin.py printed. Only its windows_amd64 rows are read:

    windows_amd64  driver|library  <name>  <sha256>  <source-sha256>  [<msi-url> <msi-sha256> <path>]
    windows_amd64  rename  <name>  <generated name>.dll
    windows_amd64  patch   <name>  <offset>  <original bytes, hex>  <new bytes, hex>
    windows_amd64  name-seed|set-hash  <sha256>
    windows_amd64  prerequisite  ...                     (read past)

For each file with a rename row, it reads <set-dir>/<name>, checks its SHA-256 against the
source-sha256, checks the original bytes at every patch and writes the new ones, checks the result's
SHA-256 against the sha256, and writes it to <out-dir>/<generated name>.dll. <out-dir> must not
exist; on any failure nothing is left in it. Each field has one spelling: hashes and bytes in
lowercase hex, offsets in decimal without a leading zero, a patch's two byte strings of one length,
patches in increasing order without overlap, and a generated name of 16, 7 or 8 characters from
a-z2-7, then .dll, as the lock writes it. On failure it prints one line, "<rule>: <reason>", and
exits 1. Standard library only, Python 3.6 and later.
"""

import hashlib
import importlib.util
import os
import re
import shutil
import sys

VERSION = 1  # part of the transform's identity, so a change to what this applies changes every name
PLATFORM = "windows_amd64"
MAX_FILE = 1 << 30
SHA256 = re.compile(r"[0-9a-f]{64}\Z")
HEX = re.compile(r"(?:[0-9a-f]{2})+\Z")
OFFSET = re.compile(r"(?:0|[1-9][0-9]{0,9})\Z")
GENERATED = re.compile(r"([a-z2-7]+)\.dll\Z")
GENERATED_LENGTHS = (16, 7, 8)
HERE = os.path.dirname(os.path.abspath(__file__))
KEPT = os.path.join(HERE, "..", "..", "policy", "windows", "kept-names.txt")


class Failure(Exception):
    pass


def sibling(name):
    """A module of this directory, by path, since python3 -I puts no directory on sys.path. No
    bytecode is written next to it, so the tools never change the repository."""
    sys.dont_write_bytecode = True
    spec = importlib.util.spec_from_file_location("redshift_odbc_" + name, os.path.join(HERE, name + ".py"))
    module = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(module)
    return module


def sha256(data):
    return hashlib.sha256(data).hexdigest()


def read_file(path, what):
    try:
        with open(path, "rb") as f:
            if os.fstat(f.fileno()).st_size > MAX_FILE:
                raise Failure("%s: %s is larger than 1 GiB" % (what, path))
            return f.read(MAX_FILE)
    except OSError as e:
        raise Failure("%s: cannot read %s: %s" % (what, path, e.strerror))


def policy_names(path):
    """The lowercase names a policy file lists, one per line, '#' starting a comment."""
    names = []
    for line in read_file(path, "policy").decode("ascii", "replace").splitlines():
        line = line.split("#", 1)[0].strip()
        if line:
            if not re.match(r"[a-z0-9_]{1,64}\Z", line):
                raise Failure("policy: %s lists %r, not a lowercase base name" % (path, line))
            names.append(line)
    return names


def kept_names(path=KEPT):
    """{(file, offset): (sha256, text)} of policy/windows/kept-names.txt, one row per line, '#'
    starting a comment: <file> <file sha256> <offset> <text> <reason>, the reason not empty, and the
    file named in lower case, as the MSI names the set's files, so a row cannot miss its file."""
    rows, name_problem = {}, sibling("lock").name_problem
    text = read_file(path, "policy").decode("ascii", "replace")
    for number, line in enumerate(text.splitlines(), 1):
        fields = line.split("#", 1)[0].split(None, 4)
        if not fields:
            continue
        if (len(fields) != 5 or name_problem(fields[0]) or fields[0] != fields[0].lower() or
                not SHA256.match(fields[1]) or not OFFSET.match(fields[2]) or
                (fields[0], int(fields[2])) in rows):
            raise Failure("policy: %s:%d is not <file> <sha256> <offset> <text> <reason>, once "
                          "per file and offset" % (path, number))
        rows[(fields[0], int(fields[2]))] = (fields[1], fields[3])
    return rows


def split_kept(name, data, sites, member_of, kept):
    """(rewritten, kept) of a DLL's whole-string sites (pe.named_strings). A string that is a set
    member's bare base name names that member only for a self-named one (lock.SELF_NAMED), which
    looks itself up by it; any other is left unchanged, and only where kept-names.txt keeps it for
    this file's SHA-256, offset and text. Without its row the string refuses, and a row for this
    file that keeps no such string refuses too, so a stale list fails rather than passes."""
    digest, self_named = sha256(data), sibling("lock").SELF_NAMED
    rewritten, keep = [], []
    for site in sites:
        at, _, _, text = site
        member = member_of(text.lower())
        if member is None or text.lower().endswith(".dll") or member.lower() in self_named:
            rewritten.append(site)
        elif kept.get((name, at)) == (digest, text):
            keep.append(site)
        else:
            raise Failure("kept-names: %s holds %r at %d, the bare name of the set member %s, "
                          "which policy/windows/kept-names.txt does not keep for this file"
                          % (name, text, at, member))
    for (file, at), (sha, text) in sorted(kept.items()):
        if file == name and (sha, at) not in [(digest, s[0]) for s in keep]:
            raise Failure("kept-names: the row for %s at %d (%s, SHA-256 %s) keeps no bare member "
                          "name of that file, whose SHA-256 is %s" % (file, at, text, sha, digest))
    return rewritten, keep


def base(name):
    """A set file's base name: its name without .dll."""
    return name[:-len(".dll")]


def generated_base(name):
    """The base of a rename row's <generated name>.dll, or None when the row's name is not one."""
    match = GENERATED.match(name)
    return match.group(1) if match and len(match.group(1)) in GENERATED_LENGTHS else None


class Rows(object):
    """The windows_amd64 rows: files {name: (kind, sha256, source-sha256)}, renames {name: generated
    base name}, patches {name: [(offset, original, new)]}, and the name seed and set hash."""

    def __init__(self, text):
        self.files, self.renames, self.patches, self.hashes = {}, {}, {}, {}
        name_problem = sibling("lock").name_problem
        for number, line in enumerate(text.splitlines(), 1):
            fields = line.split("#", 1)[0].split()
            if not fields or fields[0] != PLATFORM:
                continue
            try:
                self.row(fields[1:], name_problem)
            except Failure as e:
                raise Failure("rows: line %d: %s" % (number, e))
        for name in list(self.renames) + list(self.patches):
            if name not in self.files:
                raise Failure("rows: %s is renamed or patched but has no driver or library row" % name)

    def row(self, fields, name_problem):
        kind, rest = (fields[0], fields[1:]) if fields else ("", [])
        if kind == "prerequisite":
            return
        if kind in ("name-seed", "set-hash"):
            if len(rest) != 1 or not SHA256.match(rest[0]) or kind in self.hashes:
                raise Failure("%s takes one SHA-256 of 64 lowercase hex characters, once" % kind)
            self.hashes[kind] = rest[0]
            return
        if kind not in ("driver", "library", "rename", "patch"):
            raise Failure("unknown row kind %r" % kind)
        if not rest or name_problem(rest[0]):
            raise Failure(name_problem(rest[0]) if rest else "%s row has no file name" % kind)
        name, rest = rest[0], rest[1:]
        if kind in ("driver", "library"):
            if len(rest) not in (2, 5) or not all(SHA256.match(h) for h in rest[:2]) or name in self.files:
                raise Failure("%s %s takes a sha256 and a source-sha256 (64 lowercase hex characters), once"
                              % (kind, name))
            self.files[name] = (kind, rest[0], rest[1])
        elif kind == "rename":
            if len(rest) != 1 or name in self.renames:
                raise Failure("rename %s takes one generated name, once" % name)
            base = generated_base(rest[0])
            if base is None:
                raise Failure("generated name %r is not 16, 7 or 8 characters from a-z2-7, "
                              "then .dll" % rest[0])
            self.renames[name] = base
        else:
            if len(rest) != 3 or not OFFSET.match(rest[0]) or not all(HEX.match(b) for b in rest[1:]):
                raise Failure("patch %s takes a decimal offset and two non-empty lowercase hex byte strings" % name)
            offset, original, new = int(rest[0]), bytes.fromhex(rest[1]), bytes.fromhex(rest[2])
            if len(original) != len(new):
                raise Failure("patch %s at %d changes %d bytes into %d" % (name, offset, len(original), len(new)))
            earlier = self.patches.setdefault(name, [])
            if earlier and earlier[-1][0] + len(earlier[-1][1]) > offset:
                raise Failure("patch %s at %d overlaps or precedes the patch before it" % (name, offset))
            earlier.append((offset, original, new))


def apply(rows, name, original):
    """The renamed bytes of `name`, checked against the rows before and after."""
    _, want, source = rows.files[name]
    if sha256(original) != source:
        raise Failure("rename: %s's SHA-256 is %s, not the source-sha256 %s" % (name, sha256(original), source))
    data = bytearray(original)
    for offset, old, new in rows.patches.get(name, []):
        if data[offset:offset + len(old)] != old:
            raise Failure("patch: %s at %d does not hold the recorded original bytes %s" % (name, offset, old.hex()))
        data[offset:offset + len(new)] = new
    if sha256(data) != want:
        raise Failure("rename: %s's renamed SHA-256 is %s, not the sha256 %s" % (name, sha256(data), want))
    return bytes(data)


def main(argv):
    if len(argv) != 4:
        sys.stderr.write("usage: rename_apply.py <rows> <set-dir> <out-dir>\n")
        return 2
    _, rows_path, set_dir, out_dir = argv
    try:
        rows = Rows(read_file(rows_path, "rows").decode("utf-8", "replace"))
        renamed = [(rows.renames[name] + ".dll", apply(rows, name, read_file(os.path.join(set_dir, name), "rename")))
                   for name in sorted(rows.renames)]
        try:
            os.mkdir(out_dir)
        except OSError as e:
            raise Failure("rename: cannot create %s: %s" % (out_dir, e.strerror))
        try:
            for new_name, data in renamed:
                with open(os.path.join(out_dir, new_name), "xb") as f:
                    f.write(data)
        except OSError as e:
            shutil.rmtree(out_dir, ignore_errors=True)
            raise Failure("rename: cannot write into %s: %s" % (out_dir, e.strerror))
    except Failure as e:
        sys.stderr.write("%s\n" % e)
        return 1
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv))
