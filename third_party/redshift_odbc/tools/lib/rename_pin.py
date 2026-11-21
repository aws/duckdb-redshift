"""Computes the Windows set's rename at pin time and prints its lock rows.

    python3 -I rename_pin.py names <set-dir>
    python3 -I rename_pin.py pin <set-dir> <driver-name>
    python3 -I rename_pin.py fetch-tools

<set-dir> holds the DLLs msi.py check extracts from the MSI, under the MSI's names, and the lock's
notices (LICENSE, NOTICE and THIRD_PARTY_LICENSES): the seed and set hash cover every file, so a
set without the notices gives other rows than the lock's. Every .dll gets a generated base
name: the start of the lowercase base32 (a-z2-7) of SHA-256(name seed, its lowercase original base
name), 16 characters, except krb5_64 at 7 and wshelp64 at 8 (lock.SELF_NAMED), which look
themselves up by name, so that string is rewritten in place; one naming another member fails. The
name seed is the SHA-256 of the source manifest ("<sha256>  <size>  <name>" per file, sorted) and the
transform's identity: each rename-requirements.txt line's name, version and SHA-256 (so a pefile
bump changes every name, as a delvewheel one does), this step's and the applier's versions, and each
string patch's file, offset and original bytes. A change to any file or to the transform therefore
changes every name. A whole string that is a member's bare base name, without
.dll, names it only for krb5_64 and wshelp64; any other is left as it is where
policy/windows/kept-names.txt keeps it, and refuses the pin otherwise (rename_apply.split_kept).

names prints the name-seed row and one rename row per DLL. pin rewrites each DLL: its imports of set
members by delvewheel's routine (only for a DLL that has them), each whole string naming a set
member to that member's generated name, each naming a DLL policy/windows/unshipped-names.txt lists
to '|' characters, and its checksum, kept 0 when 0 and recomputed otherwise. It prints the
name-seed, set-hash (the settled manifest of the renamed set), driver or library, rename and patch
rows (a row per run of changed bytes, runs under 8 bytes apart joined), which rename_apply.py
reproduces the set from. A longer file from the routine fails: a patch holds same-length changes.

The tools come from RS_RENAME_TOOLS (default build/rename-tools), each as <sha256>.whl; fetch-tools
fills it from rename-requirements.txt's pinned URLs with download.sh, whatever RS_DRIVER_MIRROR
says, and checks each wheel by its pinned SHA-256. It keeps a cached wheel that has that SHA-256 and
refuses one that does not. pin imports a private copy of the checked bytes. pin exits 3, naming the
tool, when one is missing or not yet pinned, so a test can skip; any other failure prints
"<rule>: <reason>" and exits 1. Python 3.8 and later.
"""

import base64
import hashlib
import importlib.util
import os
import re
import struct
import subprocess
import sys
import tempfile

VERSION = 1  # part of the transform's identity
HERE = os.path.dirname(os.path.abspath(__file__))
REQUIREMENTS = os.path.join(HERE, "rename-requirements.txt")
UNSHIPPED = os.path.join(HERE, "..", "..", "policy", "windows", "unshipped-names.txt")
REQUIREMENT = re.compile(r"([a-z][a-z0-9-]*)==([0-9][0-9a-z.]*)"
                         r"(?: --hash=sha256:([0-9a-f]{64})(?: --url=(https://[!-~]+))?)?\Z")
DOWNLOAD = os.path.join(HERE, "download.sh")
MAX_WHEEL_BYTES = 1 << 20  # each pinned wheel is under 100 KB


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
Failure = rename_apply.Failure
sha256, read_file, base = rename_apply.sha256, rename_apply.read_file, rename_apply.base


class Missing(Failure):
    """A pinned tool that is not in the cache, or not pinned by SHA-256 yet."""


def requirements():
    """{name: (version, sha256 or None, URL or None)} of rename-requirements.txt, which must pin
    delvewheel."""
    pins = {}
    for line in read_file(REQUIREMENTS, "tools").decode("ascii", "replace").splitlines():
        line = line.split("#", 1)[0].strip()
        if line:
            match = REQUIREMENT.match(line)
            if not match:
                raise Failure("tools: %s: %r is not <name>==<version>"
                              " [--hash=sha256:<hex> [--url=<https URL>]]" % (REQUIREMENTS, line))
            pins[match.group(1)] = match.group(2, 3, 4)
    if "delvewheel" not in pins:
        raise Failure("tools: %s pins no delvewheel" % REQUIREMENTS)
    return pins


def tools_dir():
    return os.environ.get("RS_RENAME_TOOLS") or os.path.normpath(os.path.join(HERE, "../../../../build/rename-tools"))


# The private copies of the checked wheels, kept until the process exits.
PRIVATE = []


def load_tools():
    """delvewheel's import-table routine, from the pinned wheels in the cache. Each wheel is read
    once, checked by its SHA-256 and imported from a private copy of those bytes, so a cache another
    process can write cannot swap the bytes between the check and the import."""
    directory, wheels = tools_dir(), []
    private = tempfile.TemporaryDirectory(prefix="rename-tools-")
    PRIVATE.append(private)
    for name, (version, digest, _) in sorted(requirements().items()):
        if digest is None:
            raise Missing("tools: %s==%s has no --hash=sha256 pin in %s" % (name, version, REQUIREMENTS))
        wheel = os.path.join(directory, digest + ".whl")
        if not os.path.isfile(wheel):
            raise Missing("tools: %s==%s is not in %s (rename_pin.py fetch-tools fetches it)"
                          % (name, version, directory))
        data = read_file(wheel, "tools")
        if sha256(data) != digest:
            raise Failure("tools: %s is not the pinned %s==%s" % (wheel, name, version))
        copy = os.path.join(private.name, digest + ".whl")
        with open(copy, "xb") as f:
            f.write(data)
        wheels.append((name, version, wheel, copy))
    try:
        import importlib.metadata
    except ImportError:
        raise Missing("tools: rename_pin.py pin needs Python 3.8 or later")
    for name, version, wheel, copy in wheels:
        found = [d.version for d in importlib.metadata.distributions(name=name, path=[copy])]
        if found != [version]:
            raise Failure("tools: %s holds %s %s, not the pinned %s" % (wheel, name, found, version))
        sys.path.insert(0, copy)
    from delvewheel import _dll_utils
    return _dll_utils.replace_needed


def fetch_tools():
    """Puts each pinned wheel in the cache as <sha256>.whl, downloaded from its pinned URL and
    checked by its SHA-256. A cached wheel with another SHA-256 is refused, not replaced."""
    directory = tools_dir()
    pins = sorted(requirements().items())
    for name, (version, digest, url) in pins:
        if digest is None:
            raise Failure("tools: %s==%s has no --hash=sha256 pin in %s" % (name, version, REQUIREMENTS))
        if url is None:
            raise Failure("tools: %s==%s has no --url pin in %s" % (name, version, REQUIREMENTS))
    try:
        os.makedirs(directory, exist_ok=True)
    except OSError as e:
        raise Failure("tools: cannot create %s: %s" % (directory, e))
    for name, (version, digest, url) in pins:
        wheel = os.path.join(directory, digest + ".whl")
        if os.path.isfile(wheel):
            if sha256(read_file(wheel, "tools")) != digest:
                raise Failure("tools: %s is not the pinned %s==%s; remove it to fetch it again"
                              % (wheel, name, version))
            continue
        with tempfile.TemporaryDirectory() as tmp:
            body = os.path.join(tmp, "wheel")
            # A missing bash fails as one line, as every other error here does, not a traceback.
            try:
                status = subprocess.call(["bash", DOWNLOAD, url, body, str(MAX_WHEEL_BYTES)])
            except OSError as e:
                raise Failure("tools: cannot run %s: %s" % (DOWNLOAD, e))
            if status != 0:
                raise Failure("tools: downloading %s==%s from %s failed" % (name, version, url))
            data = read_file(body, "tools")
        if sha256(data) != digest:
            raise Failure("tools: %s is not the pinned %s==%s: its SHA-256 is %s"
                          % (url, name, version, sha256(data)))
        # Created afresh, never through a file or link already at that name. A failed create,
        # write or replace leaves no .part behind and fails as one line, not a traceback.
        part = None
        try:
            part = tempfile.NamedTemporaryFile(dir=directory, prefix=digest + ".", suffix=".part",
                                               delete=False)
            with part:
                part.write(data)
            os.replace(part.name, wheel)
        except OSError as e:
            if part is not None and os.path.lexists(part.name):
                os.unlink(part.name)
            raise Failure("tools: writing %s failed: %s" % (wheel, e))


class Plan(object):
    """The set's files, each DLL's image and string patch sites, the name seed and generated names."""

    def __init__(self, set_dir):
        name_problem = sibling("lock").name_problem
        self.files = {}
        try:
            names = sorted(os.listdir(set_dir))
        except OSError as e:
            raise Failure("set: cannot list %s: %s" % (set_dir, e.strerror))
        for name in names:
            path = os.path.join(set_dir, name)
            if name_problem(name) or not os.path.isfile(path):
                raise Failure("set: %s" % (name_problem(name) or "%s is not a regular file" % path))
            self.files[name] = read_file(path, "set")
        self.dlls = [n for n in self.files if n.lower().endswith(".dll")]
        if not self.dlls:
            raise Failure("set: %s holds no .dll" % set_dir)
        # Each name a string may hold, lowercase, to the DLL it names (None for one not shipped).
        self.members = {}
        for name in self.dlls:
            self.members[base(name).lower()] = self.members[name.lower()] = name
        for name in rename_apply.policy_names(UNSHIPPED):
            self.members[name] = self.members[name + ".dll"] = None
        self.images = {n: pe.Image(self.files[n]) for n in self.dlls}
        kept, self.sites = rename_apply.kept_names(), {}
        for n in self.dlls:
            found = pe.named_strings(self.images[n], self.members)
            self.sites[n], _ = rename_apply.split_kept(n, self.files[n], found, self.members.get,
                                                       kept)
        held = {text.lower() for n in self.dlls for _, _, _, text in self.sites[n]}
        self_named, lengths = sibling("lock").SELF_NAMED, {}
        for name in self.dlls:
            lengths[name] = self_named.get(name.lower(), 16)  # the lengths the lock requires
            if {name.lower(), base(name).lower()} & held and lengths[name] != len(base(name)):
                raise Failure("names: a set file holds the name %s, which a generated name must then replace "
                              "at its %d characters, and only krb5_64 and wshelp64 keep their length"
                              % (name, len(base(name))))
        manifest = "".join("%s  %d  %s\n" % (sha256(self.files[n]), len(self.files[n]), n) for n in self.files)
        identity = "".join("tool %s %s %s\n" % (name, row[0], row[1] or "unpinned")
                           for name, row in sorted(requirements().items()))
        identity += "pin %d\napply %d\n" % (VERSION, rename_apply.VERSION)
        identity += "".join("string %s %d %s\n" % (n, at, self.files[n][at:at + size].hex())
                            for n in self.dlls for at, size, _, _ in self.sites[n])
        self.seed = sha256((manifest + identity).encode("ascii"))
        self.generated = {}
        for name in self.dlls:
            digest = hashlib.sha256((self.seed + " " + base(name).lower()).encode("ascii")).digest()
            self.generated[name] = base64.b32encode(digest).decode("ascii").lower()[:lengths[name]]
        if len(set(self.generated.values())) != len(self.dlls):
            raise Failure("names: two generated names collide: two files whose names differ only in case, "
                          "or a hash collision, which a changed transform version resolves")

    def import_map(self, name):
        """{import name: the generated file name} for each set member `name` imports."""
        imported = [i.name.decode("ascii", "replace") for i in pe.imports(self.images[name])]
        return {i: self.generated[self.members[i.lower()]] + ".dll" for i in imported if self.members.get(i.lower())}


def renamed(plan, name, replace_needed):
    original = plan.files[name]
    data = original
    mapping = plan.import_map(name)
    if mapping:
        with tempfile.TemporaryDirectory() as tmp:
            path = os.path.join(tmp, name)
            with open(path, "wb") as f:
                f.write(original)
            replace_needed(path, list(mapping), mapping, False, 0, [])
            data = read_file(path, "pin")
        if len(data) != len(original):
            raise Failure("pin: delvewheel changed %s's length from %d to %d bytes" % (name, len(original), len(data)))
    data = bytearray(data)
    for at, size, encoding, text in plan.sites[name]:
        member = plan.members[text.lower()]
        new = "|" * len(text) if member is None else plan.generated[member] + text[len(base(member)):]
        data[at:at + size] = new.encode(encoding)
    image = plan.images[name]
    value = 0 if image.checksum == 0 else pe.checksum(bytes(data), image.checksum_at)
    data[image.checksum_at:image.checksum_at + 4] = struct.pack("<I", value)
    return bytes(data)


def first_difference(old, new, at):
    while at < len(old) and old[at:at + 4096] == new[at:at + 4096]:
        at = min(at + 4096, len(old))
    while at < len(old) and old[at] == new[at]:
        at += 1
    return at


def patch_rows(name, old, new):
    """One patch row per run of changed bytes, runs fewer than 8 equal bytes apart joined, so a
    UTF-16LE name is one row."""
    rows, at = [], first_difference(old, new, 0)
    while at < len(old):
        stop = at
        while True:
            while stop < len(old) and old[stop] != new[stop]:
                stop += 1
            following = first_difference(old, new, stop)
            if following - stop >= 8 or following == len(old):
                break
            stop = following
        rows.append("patch  %s  %d  %s  %s" % (name, at, old[at:stop].hex(), new[at:stop].hex()))
        at = following
    return rows


def pin(set_dir, driver):
    plan = Plan(set_dir)
    if driver not in plan.dlls:
        raise Failure("pin: the driver %s is not a .dll of the set" % driver)
    replace_needed = load_tools() if any(plan.import_map(n) for n in plan.dlls) else None
    out = {n: renamed(plan, n, replace_needed) for n in plan.dlls}
    embedded = {(plan.generated[n] + ".dll" if n in out else n): out.get(n, data) for n, data in plan.files.items()}
    manifest = "".join("%s  %d  %s\n" % (sha256(embedded[n]), len(embedded[n]), n) for n in sorted(embedded))
    rows = ["name-seed  %s" % plan.seed, "set-hash  %s" % sha256(manifest.encode("ascii"))]
    for name in plan.dlls:
        kind = "driver" if name == driver else "library"
        rows.append("%s  %s  %s  %s" % (kind, name, sha256(out[name]), sha256(plan.files[name])))
        rows.append("rename  %s  %s.dll" % (name, plan.generated[name]))
        rows += patch_rows(name, plan.files[name], out[name])
    return rows


def main(argv):
    usage = "usage: rename_pin.py names <set-dir> | pin <set-dir> <driver-name> | fetch-tools\n"
    command = argv[1] if len(argv) > 1 else ""
    if (command, len(argv)) not in (("names", 3), ("pin", 4), ("fetch-tools", 2)):
        sys.stderr.write(usage)
        return 2
    try:
        if command == "fetch-tools":
            fetch_tools()
            return 0
        if command == "names":
            plan = Plan(argv[2])
            rows = ["name-seed  %s" % plan.seed]
            rows += ["rename  %s  %s.dll" % (n, plan.generated[n]) for n in plan.dlls]
        else:
            rows = pin(argv[2], argv[3])
        sys.stdout.write("".join("windows_amd64  %s\n" % row for row in rows))
    except Missing as e:
        sys.stderr.write("%s\n" % e)
        return 3
    except (Failure, pe.Failure) as e:
        sys.stderr.write("%s\n" % e)
        return 1
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv))
